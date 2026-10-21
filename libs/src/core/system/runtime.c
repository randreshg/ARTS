/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/
#include "arts/coherence/coherence.h" /* terminal quiescence sweep */
#include "arts/runtime_state.h"
#include "arts/system/schedfuzz.h"
#include "arts/utils/malloc.h"
#include "arts/utils/random.h"

#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>

#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/db.h"
#include "arts/defs.h"
#include "arts/edt.h"
#include "arts/edt_context.h" /* arts_owned_finish_cleanup, ctx tls */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/system/topology.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/transport/socket.h"
#include "arts/utils/array_list.h"
#include "arts/utils/deque.h"
#include "arts/utils/stdatomic_compat.h"

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#include "arts/gpu/gpu_stream.h"
#endif

#ifdef ARTS_USE_CXL
#include "arts/cxl/arena.h"
#endif

static int arts_runtime_argc = 0;
static char **arts_runtime_argv = NULL;

ARTS_WEAK void init_per_node(unsigned int node_id, int argc, char **argv) {
  (void)node_id;
  (void)argc;
  (void)argv;
}

ARTS_WEAK void init_per_worker(unsigned int node_id, unsigned int worker_id,
                               int argc, char **argv) {
  (void)node_id;
  (void)worker_id;
  (void)argc;
  (void)argv;
}

ARTS_WEAK void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

struct arts_runtime_shared_s arts_node_info;

void arts_runtime_node_init(struct arts_config_s *config) {
#ifdef ARTS_DEBUG_INJECTION
  arts_sched_fuzz_init();
#elif !defined(NDEBUG)
  /* A hook's variable set where the hooks are not compiled would leave the
   * run unchanged, and the run would read as one that exercised the hook. */
  static const char *const hook_vars[] = {
      "ARTS_SCHED_FUZZ", "ARTS_WRITE_COMPLETION_DELAY_MS", "ARTS_NET_MAX_MSG",
      "ARTS_REGPOOL_FORCE_FULL_NODES", "ARTS_REQUEST_DELAY_MS"};
  for (size_t i = 0; i < sizeof(hook_vars) / sizeof(hook_vars[0]); i++) {
    const char *v = getenv(hook_vars[i]);
    if (v != NULL && v[0] != '\0') {
      ARTS_ERROR("%s is set, but this build compiles no injection hooks "
                 "(configure it with -DARTS_DEBUG_INJECTION=ON)",
                 hook_vars[i]);
    }
  }
#endif
  unsigned int tc = config->thread_count;
  size_t regpool_slab_bytes = (size_t)config->regpool_slab_mb * 1024 * 1024;

  /* Registered-slab pool for DB payload buffers + fabric bring-up.  The pool
   * must be live before any worker or progress thread can allocate a
   * payload (arts_db_buf_alloc draws from it), so it is carved here — before
   * arts_thread_init spawns a single thread.
   *
   * On a multinode run the fabric comes up FIRST so the pool can register its
   * slabs against the domain net_init creates.  Receive resources are armed
   * before this rank's fabric address is ever published, so no peer can target
   * an unarmed endpoint: the pool is carved, RX landing buffers are posted, and
   * only then does the one-shot fi-address exchange ride the
   * already-established TCP mesh here on the main thread — before any
   * progress thread exists to contend for those sockets.  A single-node run
   * stays entirely fabric-free and carves the pool with a NULL (unregistered)
   * domain, identical to the plain-malloc path. */
  if (arts_global_rank_count > 1) {
    arts_net_init(config->provider, config->fabric_domain,
                  config->net_interface);
    if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(),
                           regpool_slab_bytes, 0)) {
      ARTS_ERROR("arts_runtime_node_init: registered pool init failed");
    }
    arts_net_rx_arm();
    arts_net_exchange_addresses();
    /* Providers whose one-sided immediate path can be configured out prove
     * it now, against this rank's own memory, so a misconfiguration fails
     * here by name instead of at the first DB payload (no-op elsewhere). */
    arts_net_selfcheck();
    /* Bootstrap is complete: the fi-address exchange was the last payload to
     * ride the TCP mesh.  All live traffic now flows over the fabric; the mesh
     * connections stay open as zero-traffic liveness sentinels (a peer's
     * close surfaces as HUP, probed by a progress thread) and only the
     * listeners close. */
    arts_socket_sentinel_arm();
  } else {
#ifdef ARTS_DEBUG_INJECTION
    /* A one-rank run brings no fabric up, so a fabric hook it asks for would
     * leave the run unchanged. */
    static const char *const fabric_hook_vars[] = {
        "ARTS_NET_MAX_MSG", "ARTS_WRITE_COMPLETION_DELAY_MS",
        "ARTS_REQUEST_DELAY_MS"};
    for (size_t i = 0;
         i < sizeof(fabric_hook_vars) / sizeof(fabric_hook_vars[0]); i++) {
      const char *v = getenv(fabric_hook_vars[i]);
      if (v != NULL && v[0] != '\0') {
        ARTS_ERROR("%s is set, but a one-rank run has no fabric for it to act "
                   "on",
                   fabric_hook_vars[i]);
      }
    }
#endif
    if (!arts_regpool_init(NULL, NULL, regpool_slab_bytes, 0)) {
      ARTS_ERROR("arts_runtime_node_init: registered pool init failed");
    }
  }

  /* Scheduler */
#ifdef ARTS_USE_GPU
  /* GPU EDTs are pushed onto per-worker GPU deques that only a GPU scheduler
   * loop drains; under the default (CPU-only) scheduler they are never popped,
   * so any program that spawns a GPU EDT hangs.  When GPU support is enabled
   * but the scheduler is still at its default selection, promote to the GPU
   * scheduler loop.  An explicit non-default choice (e.g. a GPU backoff/demand
   * variant) is respected.  Index 3 is arts_gpu_scheduler_loop in the GPU
   * build's scheduler_loop[] dispatch table. */
  if (config->gpu > 0 && config->scheduler == 0) {
    config->scheduler = 3;
  }
#endif
  arts_node_info.scheduler = scheduler_loop[config->scheduler];

  /* Per-thread indexed arrays */
  arts_node_info.deque =
      (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) * tc);
  arts_node_info.progress_deque =
      config->progress_thread_count
          ? (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) *
                                                config->progress_thread_count)
          : NULL;
  arts_node_info.gpu_deque =
      (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) * tc);
  arts_node_info.route_table =
      (arts_route_table_t **)arts_calloc(tc, sizeof(arts_route_table_t *));
  arts_node_info.gpu_route_table =
      config->gpu ? (arts_route_table_t **)arts_calloc(
                        config->gpu, sizeof(arts_route_table_t *))
                  : NULL;
  {
    /* Config load keeps route_table_size above ARTS_REMOTE_ROUTE_SHARD_BITS,
     * so every shard hashes with a shift of at least 1. */
    unsigned int shard_shift =
        config->route_table_size - ARTS_REMOTE_ROUTE_SHARD_BITS;
    unsigned int shard_entries = 1U << shard_shift;
    for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
      arts_node_info.remote_route_table[s] =
          arts_new_route_table(shard_entries, shard_shift);
    }
  }
  arts_node_info.local_spin = (_Atomic(_Atomic(bool) *) *)arts_calloc(
      tc, sizeof(_Atomic(_Atomic(bool) *)));
  arts_node_info.thread_roles =
      (unsigned int *)arts_calloc(tc, sizeof(unsigned int));

  /* Thread counts */
  arts_node_info.worker_thread_count = config->worker_thread_count;
  arts_node_info.progress_thread_count = config->progress_thread_count;
  arts_node_info.total_thread_count = tc;

  /* Synchronization barriers.  No runtime thread exists yet. */
  atomic_init(&arts_node_info.ready_to_push, tc);
  atomic_init(&arts_node_info.ready_to_parallel_start, tc);
  atomic_init(&arts_node_info.ready_to_inspect, tc);
  atomic_init(&arts_node_info.ready_to_clean, tc);

  /* Seed at our own rank so different ranks pick different first targets;
   * across ranks the round-robin then walks the cluster evenly instead of
   * hammering rank 0. */
  atomic_init(&arts_node_info.db_rr_route, arts_global_rank_id);
  atomic_init(&arts_node_info.edt_rr_route, arts_global_rank_id);

  /* GPU config */
  arts_node_info.gpu = config->gpu;
  arts_node_info.gpu_route_table_size = config->gpu_route_table_size;
  arts_node_info.gpu_route_table_entries = config->gpu_route_table_entries;
  arts_node_info.gpu_locality = config->gpu_locality;
  arts_node_info.gpu_fit = config->gpu_fit;
  arts_node_info.gpu_lc_sync = config->gpu_lc_sync;
  arts_node_info.gpu_max_edts = config->gpu_max_edts;
  arts_node_info.gpu_max_memory = config->gpu_max_memory;
  arts_node_info.gpu_p2p = config->gpu_p2p;
  arts_node_info.gpu_buff_on = config->gpu_buff_on;
  arts_node_info.free_db_after_gpu_run = config->free_db_after_gpu_run;
  arts_node_info.run_gpu_gc_idle = config->run_gpu_gc_idle;
  arts_node_info.run_gpu_gc_pre_edt = config->run_gpu_gc_pre_edt;
  arts_node_info.delete_zeros_gpu_gc = config->delete_zeros_gpu_gc;

#ifdef ARTS_USE_CXL
  /* CXL store: the data-block arena table and its arenas. */
  uint64_t cxl_total_devs = GET_CXL_DEV_COUNT();

  if (config->cxl_db_allocation_strategy == ARTS_CXL_DB_ALLOC_ROUND_ROBIN) {
    /* Round-robin: allocate one arena per available device. */
    unsigned int dev_count = (unsigned int)cxl_total_devs;
    if (dev_count == 0) {
      dev_count = 1; /* Fallback: at least one arena on device 0. */
    }
    if (dev_count > ARTS_CXL_MAX_DEVICES) {
      dev_count = ARTS_CXL_MAX_DEVICES;
    }
    uint64_t dev_ids[ARTS_CXL_MAX_DEVICES];
    for (unsigned int i = 0; i < dev_count; i++) {
      dev_ids[i] = (uint64_t)i;
    }
    ARTS_INFO("CXL store: %lu device(s), DB allocation round_robin across %u "
              "device(s)",
              (unsigned long)cxl_total_devs, dev_count);
    if (!arts_global_rank_id) {
      arts_node_info.cxl_arenas =
          arts_cxl_arena_table_create(dev_ids, dev_count);
    } else {
      arts_node_info.cxl_arenas = arts_cxl_arena_table_attach();
    }
    arts_node_info.cxl_db_dev_count = dev_count;
  } else {
    /* Static: allocate a single arena on the configured device. */
    uint64_t dev_id = (uint64_t)config->cxl_db_allocation_device;
    ARTS_INFO("CXL store: %lu device(s), DB allocation static on device %lu",
              (unsigned long)cxl_total_devs, (unsigned long)dev_id);
    uint64_t dev_ids[1] = {dev_id};
    if (!arts_global_rank_id) {
      arts_node_info.cxl_arenas = arts_cxl_arena_table_create(dev_ids, 1);
    } else {
      arts_node_info.cxl_arenas = arts_cxl_arena_table_attach();
    }
    arts_node_info.cxl_db_dev_count = 1;
    arts_node_info.cxl_db_static_device = (unsigned int)dev_id;
  }

  ARTS_INFO("CXL store: arena table on device %lu, region on device %lu",
            (unsigned long)GET_CXL_DEV_ID(arts_node_info.cxl_arenas),
            (unsigned long)GET_CXL_REGION_DEV_ID());
  pthread_mutex_init(&arts_node_info.cxl_arena_lock, NULL);
  atomic_init(&arts_node_info.cxl_db_rr_idx, 0U);
  if (!arts_cxl_arena_table_ready(arts_node_info.cxl_arenas)) {
    ARTS_ERROR("CXL DB arena is missing or uninitialized");
  }
#endif

  /* GUID generation */
  arts_node_info.keys = (uint64_t **)arts_calloc(tc, sizeof(uint64_t *));
  arts_node_info.global_guid_thread_id =
      (uint64_t *)arts_calloc(tc, sizeof(uint64_t));
  arts_guid_node_geometry_init();

  /* Performance counters */
  arts_node_info.counter_folder = config->counter_folder;
  arts_node_info.counter_capture_interval = config->counter_capture_interval;

  arts_node_info.live_counters =
      (arts_counter_t **)arts_calloc(tc, sizeof(arts_counter_t *));

  arts_node_info.saved_counters =
      (uint64_t **)arts_calloc(tc, sizeof(uint64_t *));
  for (unsigned int t = 0; t < tc; t++) {
    arts_node_info.saved_counters[t] =
        (uint64_t *)arts_calloc(NUM_COUNTER_TYPES, sizeof(uint64_t));
  }

  arts_node_info.capture_arrays =
      (arts_array_list_t ***)arts_calloc(tc, sizeof(arts_array_list_t **));
  for (unsigned int t = 0; t < tc; t++) {
    arts_node_info.capture_arrays[t] = (arts_array_list_t **)arts_calloc(
        NUM_COUNTER_TYPES, sizeof(arts_array_list_t *));
    for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
      if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC) {
        arts_node_info.capture_arrays[t][i] =
            arts_new_array_list(sizeof(arts_counter_capture_t), 16);
      }
    }
  }

#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu) {
    arts_node_init_gpus();
  }
#endif
}

struct arts_shutdown_abandon_s arts_shutdown_abandon;

/* Count the EDTs left runnable in a deque at teardown.  The shutdown
 * abandoned them, and they stay as it left them, with every ref and hold they
 * took: their memory is reclaimed by the process exit, never by ending holds
 * no thread can release any more.  Every runtime thread has left its loop,
 * but each counts its own deque at the same time as the others, so the shared
 * count is updated atomically. */
static void arts_count_abandoned_edts(struct arts_deque_s *dq) {
  while (arts_deque_pop_front(dq) != NULL) {
    atomic_fetch_add_explicit(&arts_shutdown_abandon.queued_edts, 1u,
                              memory_order_relaxed);
  }
}

void arts_runtime_global_cleanup() {
  // Write all counter outputs (thread, node, and cluster levels)
  unsigned int tc = arts_node_info.total_thread_count;
  for (unsigned int t = 0; t < tc; t++) {
    arts_counter_write(arts_node_info.counter_folder, arts_global_rank_id, t);
  }
  /* Every runtime thread has joined: coherence wait-structures must be
   * empty now.  Reports (does not abort); compiled out below DEBUG log
   * level — see the check's definition. */
  arts_db_debug_quiescence_check();
  /* Final-teardown fast path: every runtime thread has joined and the process
   * is about to exit, so running each leftover object's destructor only moves
   * bookkeeping inside heaps and pools that are themselves released wholesale
   * below (pool unmap / process exit) — on programs that never destroy their
   * objects the per-item sweep costs wall-clock time proportional to the
   * leftover population for no observable effect.  Keep the full sweep only
   * under a sanitizer: once it has run, an object kept alive by a reference
   * nobody will drop is reachable from nothing, which a leak checker reports.
   * An EDT the shutdown abandoned (admitted, never finished) is the one object
   * the sweep leaves installed: it keeps the blocks it acquired, and that
   * residue is the process exit's to reclaim, so when one stays, every route
   * table is kept and the residue stays reachable through them.  A thread the
   * shutdown gave up joining may still reach the route tables, so then they
   * are neither swept nor freed, and the process exit reclaims them. */
  uint64_t abandoned_edts = 0;
  bool tables_reachable =
      atomic_load_explicit(&arts_shutdown_abandon.unjoined_threads,
                           memory_order_relaxed) != 0;
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||           \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  if (!tables_reachable) {
    abandoned_edts = arts_clean_up_dbs(arts_edt_admitted);
  }
#endif

  /* Counter cleanup (reverse of arts_runtime_node_init allocation) */
  for (unsigned int t = 0; t < tc; t++) {
    if (arts_node_info.capture_arrays && arts_node_info.capture_arrays[t]) {
      for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
        if (arts_node_info.capture_arrays[t][i]) {
          arts_delete_array_list(arts_node_info.capture_arrays[t][i]);
        }
      }
      arts_free(arts_node_info.capture_arrays[t]);
    }
    if (arts_node_info.saved_counters) {
      arts_free(arts_node_info.saved_counters[t]);
    }
  }
  arts_free(arts_node_info.capture_arrays);
  arts_free(arts_node_info.saved_counters);
  arts_free(arts_node_info.live_counters);

#ifdef ARTS_USE_CXL
  arts_cxl_arena_table_free(arts_node_info.cxl_arenas);
  pthread_mutex_destroy(&arts_node_info.cxl_arena_lock);
#endif

#ifdef ARTS_USE_GPU
  /* GPU cleanup must run BEFORE route tables are freed — free_gpu_item()
     calls arts_route_table_lookup_db() for LC DB host-side metadata. */
  if (arts_node_info.gpu) {
    arts_cleanup_gpus();
  }
#endif

  if (tables_reachable) {
    ARTS_WARN("route tables left unfreed: a thread never joined");
  } else if (abandoned_edts == 0) {
    for (unsigned int i = 0; i < tc; i++) {
      arts_delete_route_table(arts_node_info.route_table[i]);
    }
    arts_free(arts_node_info.route_table);
    for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
      arts_delete_route_table(arts_node_info.remote_route_table[s]);
    }
  }

  /* Per-thread indexed arrays */
  arts_free(arts_node_info.deque);
  arts_free(arts_node_info.progress_deque);
  arts_free(arts_node_info.gpu_deque);
  arts_free(arts_node_info.gpu_route_table);
  arts_free((void *)arts_node_info.local_spin);
  arts_free(arts_node_info.thread_roles);
  for (unsigned int i = 0; i < tc; i++) {
    arts_free(arts_node_info.keys[i]);
  }
  arts_free(arts_node_info.keys);
  arts_free(arts_node_info.global_guid_thread_id);
  arts_guid_generator_cleanup();

  /* Socket server global arrays (safe to call even for single-node) */
  arts_socket_cleanup();

  /* Fabric teardown is two-phase around the pool cleanup.  Phase 1 runs BEFORE
   * the pool is freed: refuse new sends, discard/reap in-flight txns (their
   * bounces return to the still-live pool) and close ep/cq/av while returning
   * the recv landing buffers to the pool — so afterward no send desc or recv
   * buffer references a slab MR the pool cleanup is about to close. */
  if (arts_global_rank_count > 1) {
    arts_net_quiesce();
  }
#ifdef ARTS_DEBUG_DB_QUARANTINE
  arts_db_descriptor_quarantine_drain();
#endif

  /* Registered-slab pool: torn down last.  Every runtime thread has joined,
   * so no thread is still allocating from it. */
  arts_regpool_cleanup();

  /* Phase 2: the pool's slab MRs (backing both sends and the recv buffers) are
   * now closed, so the net module can close the domain and fabric — no memory
   * registration outlives its domain. */
  if (arts_global_rank_count > 1) {
    arts_net_teardown();
  }
}

/* Arrive at a barrier counted down from the thread count and wait for every
 * other thread's arrival.  The decrement releases what this thread wrote
 * before it, and the acquire load after the polls, which reads the same zero
 * (nothing writes the counter after the last arrival), takes every arrival's.
 * The polls carry nothing and give up the processor after a few rounds: a
 * waiter must not starve the arrivals it waits for, of a processor when
 * threads outnumber the processors that run them, or of the word itself where
 * a race detector serializes the synchronizing accesses to one word. */
static void arts_runtime_barrier(_Atomic(unsigned int) *remaining) {
  atomic_fetch_sub_explicit(remaining, 1U, memory_order_acq_rel);
  unsigned int polls = 0;
  while (atomic_load_explicit(remaining, memory_order_relaxed) != 0U) {
    if (++polls < 64U) {
      arts_runtime_idle_pause();
    } else {
      sched_yield();
    }
  }
  (void)atomic_load_explicit(remaining, memory_order_acquire);
}

/*
 * arts_thread_zero_node_start — Thread 0 (master) startup sequence.
 *
 * After all threads have registered (ready_to_push barrier), thread 0:
 *   1. Enables global GUID generation.
 *   2. Schedules main_edt on rank 0 (if defined by the application).
 *   3. Waits for all threads through a series of barriers before entering
 *      the main scheduler loop.
 */
void arts_thread_zero_node_start(int argc, char **argv) {
  ARTS_INFO("Thread 0: starting node initialization");
  arts_runtime_argc = argc;
  arts_runtime_argv = argv;
  set_global_guid_on();

  // PERIODIC counter capture starts AFTER the barriers below (see
  // arts_counter_capture_start), when progress threads are running.
  //
  // End-to-end marker (rank 0): take the start stamp here — initialization is
  // complete and the application is about to run (init_per_node /
  // init_per_worker, then the main EDT), so the whole application span is
  // bracketed.  This is a self-contained, env-gated wall-clock marker that
  // matches the reference runtimes' span definition for a fair comparison.
  if (!arts_global_rank_id && arts_node_info.e2e_marker_enabled) {
    arts_node_info.e2e_start_stamp = arts_get_time_stamp();
  }

  init_per_node(arts_global_rank_id, argc, argv);

#ifdef ARTS_USE_GPU
  arts_init_per_gpu_wrapper(argc, argv);
#endif
  set_guid_generator_after_parallel_start();

  arts_runtime_barrier(&arts_node_info.ready_to_parallel_start);
  if (arts_thread_info.role == ARTS_ROLE_WORKER) {
    init_per_worker(arts_global_rank_id, arts_thread_info.group_pos, argc,
                    argv);
  }
  if (!arts_global_rank_id) {
    ARTS_INFO("Thread 0: scheduling main_edt on rank 0 (argc=%d)", argc);
    uint64_t main_args[2] = {(uint64_t)argc, (uint64_t)argv};
    arts_edt_hint_t main_hint = ARTS_EDT_HINT_DEFAULTS;
    arts_edt_create(main_edt, 2, main_args, 0, &main_hint);
  }

  arts_owned_finish_cleanup();

  arts_runtime_barrier(&arts_node_info.ready_to_inspect);

  // Start counter capture AFTER all barriers, when progress threads are in
  // their runtime loops. This ensures time sync requests can be processed.
  arts_counter_capture_start();
}

void arts_runtime_private_init(struct thread_mask_s *thread,
                               struct arts_config_s *config) {
  arts_node_info.deque[thread->id] = arts_thread_info.my_deque =
      arts_deque_new(config->deque_size);
  arts_node_info.gpu_deque[thread->id] = arts_thread_info.my_gpu_deque =
      (config->gpu && thread->role == ARTS_ROLE_WORKER)
          ? arts_deque_new(config->deque_size)
          : NULL;
  /* Every thread owns a local route table at its own slot: any thread (workers
   * AND progress threads — several progress threads run
   * arts_handler_edt_create concurrently) mints GUIDs into a disjoint key
   * partition keyed by thread->id, and arts_get_route_table resolves those
   * local GUIDs to route_table[id]. */
  arts_node_info.route_table[thread->id] = arts_new_route_table(
      config->route_table_entries, config->route_table_size);
  if (thread->role == ARTS_ROLE_WORKER) {
#ifdef ARTS_USE_GPU
    if (config->gpu) {
      arts_worker_init_gpus();
    }
#endif
  }

  if (thread->role == ARTS_ROLE_PROGRESS) {
    /* Progress threads have no per-socket partition (they reap the shared
     * fabric completion queue), so there are no inbound/outbound queues to
     * carve.  They still own a deque, published here so workers can steal the
     * EDTs a progress thread creates while dispatching inbound EDT_CREATE. */
    arts_node_info.progress_deque[thread->group_pos] =
        arts_node_info.deque[thread->id];
  }
  arts_node_info.thread_roles[thread->id] = (unsigned int)thread->role;
  atomic_store_explicit(&arts_thread_info.alive, true, memory_order_relaxed);
  /* Published last: a stopper that finds the pointer reads the role above and
   * stores its false after this thread's true.  A stop that began before the
   * stopper could find the pointer is claimed here instead.  seq_cst: this
   * store and the load of shutdown_state below are one side of a
   * store->load pair whose other side is arts_enter_shutdown_state's CAS of
   * shutdown_state and its stop walk's load of the pointer
   * (arts_runtime_stop_by_role), so at least one side sees the other. */
  atomic_store_explicit(&arts_node_info.local_spin[thread->id],
                        &arts_thread_info.alive, memory_order_seq_cst);
  if (thread->role == ARTS_ROLE_WORKER &&
      atomic_load_explicit(&arts_node_info.shutdown_state,
                           memory_order_seq_cst) != 0U) {
    /* Relaxed: this thread is the only reader of its own store. */
    atomic_store_explicit(&arts_thread_info.alive, false, memory_order_relaxed);
  }
  arts_thread_info.pu_id = thread->pu_id;
  arts_thread_info.thread_id = thread->id;
  arts_thread_info.group_pos = thread->group_pos;
  arts_thread_info.numa_domain_id = thread->numa_domain_id;
  arts_thread_info.role = thread->role;

  // Register thread-local counter storage with nodeInfo
  arts_node_info.live_counters[thread->id] = arts_thread_local_counters;

  arts_guid_key_generator_init();

  arts_runtime_barrier(&arts_node_info.ready_to_push);
  if (thread->id) {
    arts_runtime_barrier(&arts_node_info.ready_to_parallel_start);

    if (arts_thread_info.role == ARTS_ROLE_WORKER) {
      init_per_worker(arts_global_rank_id, arts_thread_info.group_pos,
                      arts_runtime_argc, arts_runtime_argv);
      arts_owned_finish_cleanup();
    }

    arts_runtime_barrier(&arts_node_info.ready_to_inspect);
  }
  arts_thread_random_init(arts_global_rank_id, thread->id);
}

/* A thread calls this only after a stop ended its loop, and every stop
 * follows the CAS of an entry whose winner never waits on teardown, so the
 * wait ends. */
void arts_runtime_wait_shutdown_entry(void) {
  while (atomic_load_explicit(&arts_node_info.shutdown_entry_done,
                              memory_order_acquire) == 0U) {
    sched_yield();
  }
}

void arts_runtime_private_cleanup() {
  /* Teardown ends what the shutdown entry's stop walk and drain use: every
   * thread's TLS stop flag, the stop arrays and the network. */
  arts_runtime_wait_shutdown_entry();
  arts_runtime_barrier(&arts_node_info.ready_to_clean);
  /* The ready_to_clean barrier above guarantees every thread has left its
   * scheduler loop, so no concurrent push/steal touches these deques, and
   * orders every thief's last access before the delete.  An EDT still in one
   * stays installed, so the deque holds no reference of its own that freeing
   * it would lose.  The pointers are cleared so that the thread owns no deque
   * from here on. */
  if (arts_thread_info.my_deque) {
    arts_count_abandoned_edts(arts_thread_info.my_deque);
    arts_deque_delete(arts_thread_info.my_deque);
    arts_thread_info.my_deque = NULL;
  }
  if (arts_thread_info.my_gpu_deque) {
    arts_count_abandoned_edts(arts_thread_info.my_gpu_deque);
    arts_deque_delete(arts_thread_info.my_gpu_deque);
    arts_thread_info.my_gpu_deque = NULL;
  }
  arts_cleanup_edt_tls();
  arts_db_resume_list_free();
}

/*
 * Walk the thread table and clear alive for every thread whose role matches
 * `role_mask`: wait for each thread to register its local_spin pointer
 * (non-NULL once it has finished arts_runtime_private_init; its role is
 * readable from then on), then, if the role matches, set *local_spin[i] =
 * false, which makes it leave its scheduler/progress loop.  The registration
 * spin is bounded to avoid an infinite busy-wait if a thread crashed during
 * init.
 */
static void arts_runtime_stop_by_role(unsigned int role_mask,
                                      const char *role_label) {
  const unsigned int max_spin = 10000000; /* ~sub-second upper bound */
  unsigned int i;
  for (i = 0; i < arts_node_info.total_thread_count; i++) {
    _Atomic(bool) *flag;
    unsigned int spin = 0;
    /* seq_cst: the other half of this side of the stop-registration pair
     * (see arts_runtime_private_init), after the shutdown CAS. */
    while ((flag = atomic_load_explicit(&arts_node_info.local_spin[i],
                                        memory_order_seq_cst)) == NULL) {
      if (++spin >= max_spin) {
        ARTS_WARN("arts_runtime_stop_%s: thread %u has not registered its "
                  "stop flag; a worker that registers later stops itself, "
                  "and the teardown stops a progress thread",
                  role_label, i);
        goto next;
      }
    }
    if ((1U << arts_node_info.thread_roles[i]) & role_mask) {
      /* Release: a thread that sees the stop sees what preceded it here. */
      atomic_store_explicit(flag, false, memory_order_release);
      ARTS_DEBUG("arts_runtime_stop_%s: thread %u signaled to stop", role_label,
                 i);
    }
  next:
    continue;
  }
}

/*
 * arts_runtime_stop_workers — clear alive on worker threads only.
 * Progress threads stay alive so they can keep handling shutdown-related
 * traffic.
 */
void arts_runtime_stop_workers() {
  ARTS_INFO("arts_runtime_stop_workers");
  arts_runtime_stop_by_role(1U << ARTS_ROLE_WORKER, "workers");
}

/*
 * arts_runtime_stop_network — clear alive on the progress threads.
 * Call AFTER arts_runtime_stop_workers and the associated worker loop
 * exits, once no more network traffic is expected.
 */
void arts_runtime_stop_network() {
  ARTS_INFO("arts_runtime_stop_network");
  arts_runtime_stop_by_role(1U << ARTS_ROLE_PROGRESS, "progress");
}

void arts_runtime_stop() {
  /* Stop every thread, progress threads included, in one call: the liveness
   * sentinel's path once a peer has closed its connection.  Nothing orders
   * any rank's close after every other rank has dispatched the initiator's
   * MSG_SHUTDOWN (the initiator's drain waits only for its local sends to
   * complete, and a receiver tears down without waiting for anyone), so an
   * orderly teardown takes this path too and joins the same shutdown. */
  ARTS_INFO("arts_runtime_stop: stopping %u threads",
            arts_node_info.total_thread_count);
  arts_runtime_stop_by_role(0xFFFFFFFFU, "all");
  ARTS_INFO("arts_runtime_stop: all threads signaled");
}

/*
 * arts_runtime_loop — Main per-thread dispatch loop.
 *
 * Each thread enters exactly one of two roles:
 *   - progress: Reaps fabric completions (dispatching inbound messages);
 *               multi-node only.
 *   - worker:   Runs the selected scheduler loop until alive==false.
 *
 * On single-node configurations, all threads are workers (no progress thread).
 * The loop exits when arts_runtime_stop() sets alive=false for this thread.
 */
int arts_runtime_loop() {
  /* Release whatever the startup hooks created on THIS thread, before it does
   * anything else.  The created-DB list is thread-local, so only this thread
   * can drain it, and a hold left on it is a data block nobody can ever
   * acquire.  Draining at hook return instead would deadlock: during the
   * hooks no thread anywhere is here yet, so a cross-rank create's blocking
   * publish waits on an acknowledgement no peer can produce.  Here the whole
   * cluster's progress plane is live.
   *
   * Placement is exact: BEFORE the role switch, so it is a no-op on a
   * progress thread (which runs no hooks, hence the assert), and BEFORE the
   * liveness test, so a fast first task finishing on another thread cannot
   * end this thread's loop before it has drained. */
  assert((arts_thread_info.role != ARTS_ROLE_PROGRESS ||
          arts_vector_count(arts_get_created_db_list()) == 0) &&
         "only threads that run the startup hooks create data blocks there");
  arts_release_created_dbs();
  ARTS_DEBUG("Thread %u entering runtime_loop (role=%d)",
             arts_thread_info.thread_id, arts_thread_info.role);
  switch (arts_thread_info.role) {
  case ARTS_ROLE_PROGRESS: {
    unsigned int sentinel_spin = 0;
    while (
        atomic_load_explicit(&arts_thread_info.alive, memory_order_acquire)) {
      /* Busy-poll by design: the thread budget dedicates a PU to every
       * progress thread (startup aborts on PU oversubscription when the
       * topology mask is built), so idle waiting spins with the architectural
       * pause hint instead of sleeping — a sleeping poll would put a fixed
       * per-hop latency floor under every message an idle rank receives,
       * which multiplies across latency-bound message chains. */
      if (!arts_net_progress()) {
        arts_runtime_idle_pause();
      }
      /* Peer-liveness probe: the fabric is connectionless, so a peer that dies
       * without the shutdown handshake produces no fabric event — the TCP
       * liveness sentinels carry that signal instead.  poll() is a syscall, so
       * throttle it to every N loop iterations (an idle iteration is now
       * nanoseconds, not a sleep); N trades a few ms of dead-peer detection
       * latency for staying off the syscall path — ample for liveness. */
      if (++sentinel_spin >= 16384u) {
        sentinel_spin = 0;
        arts_socket_sentinel_check();
      }
    }
    break;
  }
  case ARTS_ROLE_WORKER:
    while (
        atomic_load_explicit(&arts_thread_info.alive, memory_order_acquire)) {
      arts_node_info.scheduler();
    }
    break;
  default:
    break;
  }
  ARTS_DEBUG("Thread %u exiting runtime_loop", arts_thread_info.thread_id);
  return 0;
}
