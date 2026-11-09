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
#include "arts/coherence/buffer.h"    /* the teardown's landing count */
#include "arts/coherence/coherence.h" /* the teardown's quiescence walk */
#include "arts/runtime_state.h"
#include "arts/system/schedfuzz.h"
#include "arts/utils/malloc.h"
#include "arts/utils/random.h"

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/db.h"
#include "arts/defs.h"
#include "arts/edt.h"
#include "arts/edt_context.h" /* ctx tls */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
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

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_thread_stop_s *arts_thread_stop_slot;

/* A run's id: never 0, which names a rank that has not learned one. */
static uint64_t runtime_draw_run_id(void) {
  uint64_t id = 0;
  while (id == 0) {
    ssize_t got = getrandom(&id, sizeof(id), 0);
    if (got < 0 && errno != EINTR) {
      ARTS_ERROR("arts_runtime_node_init: getrandom failed: %s",
                 strerror(errno));
    }
    if (got != (ssize_t)sizeof(id)) {
      id = 0;
    }
  }
  return id;
}

void arts_runtime_node_init(struct arts_config_s *config) {
#ifdef ARTS_DEBUG_INJECTION
  arts_net_rdzv_cut_done = arts_exit_cut_raise;
  arts_sched_fuzz_init();
#elif !defined(NDEBUG)
  /* A hook's variable set where the hooks are not compiled would leave the
   * run unchanged, and the run would read as one that exercised the hook. */
  static const char *const hook_vars[] = {
      "ARTS_SCHED_FUZZ", "ARTS_WRITE_COMPLETION_DELAY_MS", "ARTS_NET_MAX_MSG",
      "ARTS_REGPOOL_FORCE_FULL_NODES", "ARTS_REQUEST_DELAY_MS",
      "ARTS_NET_RDZV_CUT", "ARTS_NET_REFUSE_EP_CLOSE"};
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

  /* Rank 0 draws the run's id; the address exchange hands it to the others. */
  uint64_t run_id = 0;
  if (arts_global_rank_id == 0) {
    run_id = runtime_draw_run_id();
    atomic_store_explicit(&arts_exit_meta.run_id, run_id, memory_order_relaxed);
  }

  /* Registered-slab pool for DB payload buffers + fabric bring-up.  The pool
   * must be live before any worker or progress thread can allocate a
   * payload (every coherence buffer draws from it), so it is carved here —
   * before arts_thread_init spawns a single thread.
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
                  config->net_interface,
                  config->launcher != NULL &&
                      strcmp(config->launcher, "local") == 0);
#if defined(ARTS_DEBUG_INJECTION) && !defined(ARTS_DEBUG_TEARDOWN)
    /* Only the teardown's endpoint close acts on this hook: the run would read
     * as one that exercised it. */
    const char *refuse_ep_close = getenv("ARTS_NET_REFUSE_EP_CLOSE");
    if (refuse_ep_close != NULL && refuse_ep_close[0] != '\0') {
      ARTS_ERROR("ARTS_NET_REFUSE_EP_CLOSE is set, but this build runs no "
                 "teardown (configure it with -DARTS_DEBUG_TEARDOWN=ON)");
    }
#endif
    if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(),
                           regpool_slab_bytes, 0)) {
      ARTS_ERROR("arts_runtime_node_init: registered pool init failed");
    }
    arts_net_rx_arm();
    arts_startup_step(ARTS_MESH_PHASE_EXCHANGE);
    run_id = arts_net_exchange_addresses(run_id);
    atomic_store_explicit(&arts_exit_meta.run_id, run_id, memory_order_relaxed);
    /* Providers whose one-sided immediate path can be configured out prove
     * it now, against this rank's own memory, so a misconfiguration fails
     * here by name instead of at the first DB payload (no-op elsewhere). */
    arts_net_selfcheck();
    /* Bootstrap is complete: the fi-address exchange was the last payload to
     * ride the TCP mesh.  All live traffic now flows over the fabric; the mesh
     * connections stay open as payload-free liveness sentinels (a peer's
     * close surfaces as HUP, probed by the progress loop and the ring-full
     * spin).  Nothing closes here: the listener closed when the formation
     * completed. */
    arts_socket_sentinel_arm();
    arts_startup_end();
  } else {
#ifdef ARTS_DEBUG_INJECTION
    /* A one-rank run brings no fabric up, so a fabric hook it asks for would
     * leave the run unchanged. */
    static const char *const fabric_hook_vars[] = {
        "ARTS_NET_MAX_MSG", "ARTS_WRITE_COMPLETION_DELAY_MS",
        "ARTS_REQUEST_DELAY_MS", "ARTS_NET_RDZV_CUT",
        "ARTS_NET_REFUSE_EP_CLOSE"};
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
   * build's arts_scheduler_loop[] dispatch table. */
  if (config->gpu > 0 && config->scheduler == 0) {
    config->scheduler = 3;
  }
#endif
  arts_node_info.scheduler = arts_scheduler_loop[config->scheduler];

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
  arts_node_info.gpu_route_table =
      config->gpu ? (arts_route_table_t **)arts_calloc(
                        config->gpu, sizeof(arts_route_table_t *))
                  : NULL;
  arts_node_info.local_spin =
      (_Atomic(bool) **)arts_calloc(tc, sizeof(_Atomic(bool) *));
  arts_node_info.thread_stop = (struct arts_thread_stop_s *)arts_calloc_aligned(
      tc, sizeof(struct arts_thread_stop_s), sizeof(struct arts_thread_stop_s));
  for (unsigned int t = 0; t < tc; t++) {
    atomic_init(&arts_node_info.thread_stop[t].state, ARTS_THREAD_RUNNING);
  }

  /* Thread counts */
  arts_node_info.worker_thread_count = config->worker_thread_count;
  arts_node_info.progress_thread_count = config->progress_thread_count;
  arts_node_info.total_thread_count = tc;

  /* Synchronization barriers.  No runtime thread exists yet. */
  atomic_init(&arts_node_info.ready_to_push, tc);
  atomic_init(&arts_node_info.ready_to_parallel_start, tc);
  atomic_init(&arts_node_info.ready_to_inspect, tc);

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
  arts_guid_node_geometry_init();
  /* Every route table exists before any thread can mint, park or take in a
   * key that maps to it. */
  arts_route_table_node_init(config->route_table_entries,
                             config->route_table_size);

  /* Performance counters */
  arts_node_info.counter_capture_interval = config->counter_capture_interval;

  arts_node_info.live_counters =
      (arts_counter_t **)arts_calloc(tc, sizeof(arts_counter_t *));

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

#ifdef ARTS_DEBUG_TEARDOWN
/* How many EDTs were left runnable in a deque at teardown.  The shutdown
 * abandoned them, and they stay as it left them, with every ref and hold they
 * took: their memory is reclaimed by the process exit, never by ending holds
 * no thread can release any more.  Every other runtime thread has stopped,
 * so the caller is the deque's only user and pops it as its owner. */
static unsigned int arts_count_abandoned_edts(struct arts_deque_s *dq) {
  unsigned int n = 0;
  while (arts_deque_pop_front(dq) != NULL) {
    n++;
  }
  return n;
}

/* Information, never a verdict: the discards reclaim ran, the descriptors
 * they ended (under the quarantine) and the landings still out after them
 * (under the landing count).  A test that cuts expectations on purpose reads
 * it; any shutdown may leave non-zero values. */
static void runtime_reclaim_report(size_t discarded, uint64_t ended) {
  long long landings = 0;
#ifdef ARTS_DEBUG_LANDING_BALANCE
  landings = (long long)arts_db_landings_outstanding();
#endif
  if (discarded != 0 || ended != 0 || landings != 0) {
    ARTS_DEBUG("TEARDOWN-INFO: reclaim discarded %zu unpaired rendezvous "
               "expectation(s) (descriptors ended %llu, landings out %lld)",
               discarded, (unsigned long long)ended, landings);
  }
}

/* The teardown proper, once every other runtime thread has stopped and the
 * deques are gone. */
static void
arts_runtime_global_cleanup(const struct arts_teardown_exempt_s *exempt,
                            unsigned int queued_edts) {
  unsigned int tc = arts_node_info.total_thread_count;
  /* Every other runtime thread has stopped, so coherence wait-structures
   * must be empty now but for what the exempt EDTs hold.  Reports (does not
   * abort); compiled out below DEBUG log level — see the check's
   * definition. */
  arts_db_debug_quiescence_check(exempt, queued_edts);
  /* Fabric teardown, phase 1, before anything below frees: every other
   * runtime thread has stored its stop, so none touches the fabric again.
   * Quiesce discards or reaps in-flight txns (their bounces return to the
   * still-live pool) and closes the endpoint and the registrations bound to
   * it; nothing this thread runs from then on sends.  Once the close
   * succeeds, no peer's one-sided write, a cut one included, lands in what
   * the sweep and the frees below return. */
  bool writes_shut_out = true;
  if (arts_global_rank_count > 1) {
    writes_shut_out = arts_net_quiesce();
  }
  /* Final-teardown fast path: every other runtime thread has stopped and the
   * process is about to exit, so running each leftover object's destructor
   * only moves bookkeeping inside heaps and pools that are themselves
   * released wholesale below (pool unmap / process exit) — on programs that
   * never destroy their objects the per-item sweep costs wall-clock time
   * proportional to the leftover population for no observable effect.  Keep
   * the full sweep only under a sanitizer: once it has run, an object kept
   * alive by a reference nobody will drop is reachable from nothing, which a
   * leak checker reports.  An EDT the shutdown abandoned (admitted, never
   * finished) is the one object the sweep leaves installed: it keeps the
   * blocks it acquired, and that residue is the process exit's to reclaim, so
   * when one stays, every route table is kept and the residue stays reachable
   * through them.  Before a coherent descriptor's install count goes, the
   * sweep hands it to its arm's rows (arts_db_sweep_open_items). */
  uint64_t abandoned_edts = 0;
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||           \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
  abandoned_edts =
      arts_route_table_node_sweep(arts_edt_admitted, arts_db_sweep_detach);
#endif

  /* Counter cleanup (reverse of arts_runtime_node_init allocation). */
  for (unsigned int t = 0; t < tc; t++) {
    if (arts_node_info.capture_arrays && arts_node_info.capture_arrays[t]) {
      for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
        if (arts_node_info.capture_arrays[t][i]) {
          arts_delete_array_list(arts_node_info.capture_arrays[t][i]);
        }
      }
      arts_free(arts_node_info.capture_arrays[t]);
    }
  }
  arts_free(arts_node_info.capture_arrays);
  arts_free(arts_node_info.live_counters);

#ifdef ARTS_USE_CXL
  /* The arenas stay: peers may still be in the store, and a run's CXL
   * allocations are released outside the runtime once it ends. */
  pthread_mutex_destroy(&arts_node_info.cxl_arena_lock);
#endif

#ifdef ARTS_USE_GPU
  /* GPU cleanup must run BEFORE route tables are freed — free_gpu_item()
     calls arts_route_table_lookup_db() for LC DB host-side metadata. */
  if (arts_node_info.gpu) {
    arts_cleanup_gpus();
  }
#endif

  if (abandoned_edts == 0) {
    arts_route_table_node_cleanup();
  }

  /* Per-thread indexed arrays */
  arts_free(arts_node_info.deque);
  arts_free(arts_node_info.progress_deque);
  arts_free(arts_node_info.gpu_deque);
  arts_free(arts_node_info.gpu_route_table);
  arts_free((void *)arts_node_info.local_spin);
  /* A stopped thread never reads its slot again. */
  arts_free(arts_node_info.thread_stop);
  arts_node_info.thread_stop = NULL;
  arts_thread_stop_slot = NULL;
  for (unsigned int i = 0; i < tc; i++) {
    arts_free(arts_node_info.keys[i]);
  }
  arts_free(arts_node_info.keys);
  arts_guid_generator_cleanup();

  /* Socket server global arrays (safe to call even for single-node) */
  arts_socket_cleanup();

  /* Fabric teardown, phase 2, after phase 1's close, so no write lands in
   * what it returns: the discard of every rendezvous expectation left
   * unpaired releases only what its context owns (the sweep above may
   * already have dropped a count a context only borrows; a discard reads
   * nothing through one), then the recv landing buffers go back; quiesce
   * dispatched no completion, so no late pairing races a discard.  Both
   * phases run before the pool cleanup, so what they return goes back to a
   * live pool and no recv buffer references a slab MR it closes. */
  if (arts_global_rank_count > 1) {
    uint64_t ended = 0;
#ifdef ARTS_DEBUG_DB_QUARANTINE
    ended = arts_db_descriptors_ended();
#endif
    size_t discarded = arts_net_reclaim();
#ifdef ARTS_DEBUG_DB_QUARANTINE
    ended = arts_db_descriptors_ended() - ended;
#endif
    runtime_reclaim_report(discarded, ended);
  }
#ifdef ARTS_DEBUG_DB_QUARANTINE
  arts_db_descriptor_quarantine_drain();
#endif

  /* Registered-slab pool and the domain: torn down last, once the endpoint
   * has closed.  With the endpoint or a registration bound to it still open,
   * a peer's write may still land in a slab, so both are left to the process
   * exit. */
  if (!writes_shut_out) {
    ARTS_WARN("registered pool, domain and fabric left unfreed: the fabric "
              "may still write");
    return;
  }
  arts_regpool_cleanup();

  /* Phase 3: the pool's slab MRs (backing both sends and the recv buffers) are
   * now closed, so the net module can close the domain and fabric — no memory
   * registration outlives its domain. */
  if (arts_global_rank_count > 1) {
    arts_net_teardown();
  }
}
#endif /* ARTS_DEBUG_TEARDOWN */

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
 * After all threads have registered (ready_to_push barrier), thread 0 passes
 * the parallel-start barrier, creates the program's entry EDT on rank 0, and
 * passes the last startup barrier before entering the main scheduler loop.
 */
void arts_thread_zero_node_start(int argc, char **argv, arts_edt_t entry) {
  ARTS_INFO("Thread 0: starting node initialization");

  // PERIODIC counter capture starts AFTER the barriers below (see
  // arts_counter_capture_start), when progress threads are running; only its
  // time-sync request leaves before the last one.
  //
  // End-to-end marker (rank 0): take the start stamp here — initialization is
  // complete and the application, the main EDT, is about to run — so the
  // whole application span is bracketed.  This is a self-contained, env-gated
  // wall-clock marker that matches the reference runtimes' span definition
  // for a fair comparison.
  if (!arts_global_rank_id && arts_node_info.e2e_marker_enabled) {
    arts_node_info.e2e_start_stamp = arts_get_time_stamp();
  }
  /* Every thread has registered (ready_to_push), the start stamp is taken,
   * and no EDT of this rank has run yet. */
  arts_exit_publish_node_ready();

#ifdef ARTS_USE_GPU
  arts_init_per_gpu_wrapper(argc, argv);
#endif

  arts_runtime_barrier(&arts_node_info.ready_to_parallel_start);
  if (!arts_global_rank_id) {
    ARTS_INFO("Thread 0: scheduling main_edt on rank 0 (argc=%d)", argc);
    uint64_t main_args[2] = {(uint64_t)argc, (uint64_t)argv};
    arts_edt_hint_t main_hint = ARTS_EDT_HINT_DEFAULTS;
    arts_edt_create(entry, 2, main_args, 0, &main_hint);
  }

  // No EDT of this rank runs before this barrier releases the other threads.
  arts_counter_capture_sync_begin();
  arts_runtime_barrier(&arts_node_info.ready_to_inspect);

  // Its wait for the time-sync response needs this rank's progress threads,
  // which the barrier above released.
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
  arts_thread_stop_slot = &arts_node_info.thread_stop[thread->id];
  atomic_store_explicit(&arts_thread_info.alive, true, memory_order_relaxed);
  arts_node_info.local_spin[thread->id] = &arts_thread_info.alive;
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
    arts_runtime_barrier(&arts_node_info.ready_to_inspect);
  }
  arts_thread_random_init(arts_global_rank_id, thread->id);
}

void arts_runtime_thread_park(void) {
  arts_cleanup_edt_tls();
  arts_db_resume_list_free();
  /* Release: this thread's last accesses, its steals among them, happen
   * before what the stopper does once it acquires the store.  The thread
   * stays, so its thread-local storage, which a stopper may still reach
   * through local_spin, outlives every reader. */
  atomic_store_explicit(&arts_thread_stop_slot->state, ARTS_THREAD_STOPPED,
                        memory_order_release);
  for (;;) {
    (void)pause();
  }
}

#ifdef ARTS_DEBUG_TEARDOWN
struct arts_stop_result_s {
  unsigned int running; /* threads still RUNNING when the poll gave up */
  unsigned int unsafe;  /* threads whose slot read STOPPED_UNSAFE */
};

/* Stops every runtime thread but the one whose slot is `self` (NULL on a
 * thread the runtime did not create) and polls their slots with acquire
 * loads for at most two seconds.  Zero in both counts means every other
 * runtime thread has stopped, and its last access to shared runtime state
 * happens before the return.  Only after node_ready was acquired, which
 * publishes every thread's local_spin entry. */
static struct arts_stop_result_s
arts_runtime_stop_threads(const struct arts_thread_stop_s *self) {
  const unsigned int tc = arts_node_info.total_thread_count;
  for (unsigned int i = 0; i < tc; i++) {
    if (&arts_node_info.thread_stop[i] == self) {
      continue;
    }
    _Atomic(bool) *flag = arts_node_info.local_spin[i];
    /* Release: a thread that sees the stop sees what preceded it here. */
    atomic_store_explicit(flag, false, memory_order_release);
  }
  const uint64_t budget_ns = 2000000000ULL;
  const uint64_t begin = arts_get_time_stamp();
  struct arts_stop_result_s r;
  for (;;) {
    r.running = 0;
    r.unsafe = 0;
    for (unsigned int i = 0; i < tc; i++) {
      if (&arts_node_info.thread_stop[i] == self) {
        continue;
      }
      unsigned int st = atomic_load_explicit(
          &arts_node_info.thread_stop[i].state, memory_order_acquire);
      if (st == ARTS_THREAD_RUNNING) {
        r.running++;
      } else if (st == ARTS_THREAD_STOPPED_UNSAFE) {
        r.unsafe++;
      }
    }
    if (r.running == 0 || arts_get_time_stamp() - begin >= budget_ns) {
      return r;
    }
    const struct timespec poll = {.tv_sec = 0, .tv_nsec = 1000000L};
    (void)nanosleep(&poll, NULL);
  }
}

/* Frees every thread's deques, only after a stop that left no thread running
 * or stopped unsafe: no thief or owner can reach a deque any more.  Returns how
 * many EDTs they still held, which the shutdown abandoned. */
static unsigned int arts_runtime_delete_deques(void) {
  unsigned int abandoned = 0;
  /* An EDT still in a deque stays installed, so the deque holds no reference
   * of its own that freeing it would lose.  The pointers are cleared, so a
   * stray push or steal faults instead of writing freed memory. */
  for (unsigned int i = 0; i < arts_node_info.total_thread_count; i++) {
    if (arts_node_info.deque[i] != NULL) {
      abandoned += arts_count_abandoned_edts(arts_node_info.deque[i]);
      arts_deque_delete(arts_node_info.deque[i]);
      arts_node_info.deque[i] = NULL;
    }
    if (arts_node_info.gpu_deque[i] != NULL) {
      abandoned += arts_count_abandoned_edts(arts_node_info.gpu_deque[i]);
      arts_deque_delete(arts_node_info.gpu_deque[i]);
      arts_node_info.gpu_deque[i] = NULL;
    }
  }
  for (unsigned int p = 0; p < arts_node_info.progress_thread_count; p++) {
    arts_node_info.progress_deque[p] = NULL;
  }
  arts_thread_info.my_deque = NULL;
  arts_thread_info.my_gpu_deque = NULL;
  return abandoned;
}

/* The exemptions of the quiescence walk: the EDT running on the end's owner
 * and on every late entry that stopped at its safe point, which never return,
 * and the blocks those EDTs hold (their acquired slots and their created
 * blocks).  Each descriptor appears once. */
struct runtime_exempt_build_s {
  struct arts_teardown_exempt_s ex;
  const struct arts_edt_s **edts;
  const struct arts_db_s **dbs;
  unsigned int db_cap;
};

static void runtime_exempt_db(struct runtime_exempt_build_s *b,
                              const struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  for (unsigned int i = 0; i < b->ex.db_count; i++) {
    if (b->dbs[i] == db) {
      return;
    }
  }
  if (b->ex.db_count == b->db_cap) {
    unsigned int cap = b->db_cap != 0 ? 2 * b->db_cap : 16;
    const struct arts_db_s **grown =
        (const struct arts_db_s **)arts_malloc(sizeof(*grown) * cap);
    if (b->ex.db_count != 0) {
      memcpy(grown, b->dbs, sizeof(*grown) * b->ex.db_count);
    }
    arts_free(b->dbs);
    b->dbs = grown;
    b->db_cap = cap;
  }
  b->dbs[b->ex.db_count++] = db;
}

static void runtime_exempt_caller(struct runtime_exempt_build_s *b,
                                  const struct arts_edt_s *edt,
                                  const arts_vector_t *created) {
  if (edt != NULL) {
    b->edts[b->ex.edt_count++] = edt;
    if (edt->dep_acq != NULL) {
      /* A slot holds its pin until its release, which a release in the body
       * marks by the slot's NULL mode (as the epilogue reads it). */
      const arts_edt_dep_t *depv =
          (const arts_edt_dep_t *)arts_get_depv((void *)edt);
      for (uint32_t i = 0; i < edt->depc; i++) {
        arts_shared_ptr_t pin = edt->dep_acq[i].pin;
        if (depv[i].mode != DB_MODE_NULL && pin) {
          runtime_exempt_db(b, (const struct arts_db_s *)arts_shared_get(pin));
        }
      }
    }
  }
  if (created != NULL) {
    for (uint64_t i = 0; i < arts_vector_count(created); i++) {
      const struct arts_created_db_s *c =
          (const struct arts_created_db_s *)arts_vector_at(created, i);
      if (c->db) {
        runtime_exempt_db(b, (const struct arts_db_s *)arts_shared_get(c->db));
      }
    }
  }
}

void arts_runtime_teardown_oracle(bool node_ready) {
  /* An owner that took the end in the middle of a transition never finishes
   * it, so shared state may be half-way through an update. */
  enum arts_exit_origin origin = arts_exit_meta.origin;
  if (origin == ARTS_EXIT_FAULT || origin == ARTS_EXIT_RUNTIME_ERROR ||
      origin == ARTS_EXIT_BACKPRESSURE) {
    ARTS_DEBUG("teardown skipped: origin %s", arts_exit_origin_name(origin));
    return;
  }
  if (!node_ready) {
    ARTS_DEBUG("teardown skipped: the node never became ready");
    return;
  }
  struct arts_stop_result_s stop =
      arts_runtime_stop_threads(arts_thread_stop_slot);
  if (stop.unsafe != 0 || stop.running != 0) {
    /* A thread that may still be in the middle of an update can reach
     * anything the teardown would read or free, so nothing is freed and the
     * process exit reclaims it all. */
    if (stop.unsafe != 0) {
      ARTS_DEBUG("teardown skipped: %u thread(s) stopped mid-transition",
                 stop.unsafe);
    } else {
      ARTS_DEBUG("teardown skipped: %u thread(s) still running", stop.running);
    }
    return;
  }
  const unsigned int queued_edts = arts_runtime_delete_deques();

  /* Every stop slot was acquired as STOPPED, so each late entry's fields are
   * visible here. */
  const unsigned int tc = arts_node_info.total_thread_count;
  struct runtime_exempt_build_s b;
  memset(&b, 0, sizeof(b));
  b.edts = (const struct arts_edt_s **)arts_malloc(sizeof(*b.edts) * (tc + 1));
  runtime_exempt_caller(&b, arts_current_edt, arts_get_created_db_list());
  for (unsigned int i = 0; i < tc; i++) {
    const struct arts_thread_stop_s *slot = &arts_node_info.thread_stop[i];
    if (slot != arts_thread_stop_slot) {
      runtime_exempt_caller(&b, slot->caller_edt, slot->caller_dbs);
    }
  }
  b.ex.edts = b.edts;
  b.ex.dbs = b.dbs;
  arts_runtime_global_cleanup(&b.ex, queued_edts);
  arts_free((void *)b.edts);
  arts_free((void *)b.dbs);
  arts_free(arts_thread_masks);
  arts_thread_masks = NULL;
  arts_free(arts_thread_handles);
  arts_thread_handles = NULL;
}
#endif /* ARTS_DEBUG_TEARDOWN */

/*
 * arts_runtime_loop — Main per-thread dispatch loop.
 *
 * Each thread enters exactly one of two roles:
 *   - progress: Reaps fabric completions (dispatching inbound messages);
 *               multi-node only.
 *   - worker:   Runs the selected scheduler loop until alive==false.
 *
 * On single-node configurations, all threads are workers (no progress thread).
 * The loop exits when the teardown's stop clears alive for this thread, and
 * only then: a tree without the teardown runs it until the process ends.
 */
int arts_runtime_loop() {
  /* Every data block is created inside an EDT, whose epilogue releases the
   * creator holds still on the thread's created-DB list, so no thread enters
   * its loop holding one. */
  assert(arts_vector_count(arts_get_created_db_list()) == 0 &&
         "a creator hold outside any EDT");
  ARTS_DEBUG("Thread %u entering runtime_loop (role=%d)",
             arts_thread_info.thread_id, arts_thread_info.role);
  switch (arts_thread_info.role) {
  case ARTS_ROLE_PROGRESS: {
    uint64_t probe_ns = 0;
    while (
        atomic_load_explicit(&arts_thread_info.alive, memory_order_acquire)) {
      /* A SHUTDOWN this thread reaped ends the rank here, once the rest of
       * that burst was dispatched. */
      arts_exit_poll();
      /* Busy-poll by design: the thread budget dedicates a PU to every
       * progress thread (startup aborts on PU oversubscription when the
       * topology mask is built), so idle waiting spins with the architectural
       * pause hint instead of sleeping — a sleeping poll would put a fixed
       * per-hop latency floor under every message an idle rank receives,
       * which multiplies across latency-bound message chains. */
      if (!arts_net_progress()) {
        arts_runtime_idle_pause();
      }
      /* Peer-liveness probe: the fabric is connectionless, so a peer that
       * ends without a SHUTDOWN reaching this rank produces no fabric event,
       * and the TCP liveness sentinels carry that signal instead.  Their
       * poll is a syscall, so it runs on a clock cadence rather than on
       * every iteration, which keeps the detection latency independent of
       * how many iterations run. */
      if (arts_socket_probe_due(&probe_ns) && arts_socket_sentinel_check() &&
          arts_exit_take(0, ARTS_EXIT_SENTINEL)) {
        arts_exit_resume();
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
