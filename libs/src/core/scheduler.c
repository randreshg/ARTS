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
#include "arts/runtime_state.h"
#include "arts/utils/malloc.h"

#include <stdlib.h>
#include <time.h>

#include "arts/compute/edt.h"
#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/counter/object_counter.h"
#include "arts/defs.h"
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/memory/db.h"
#include "arts/sync/termination.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/system/topology.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"
#include "arts/transport/socket.h"
#include "arts/utils/array_list.h"
#include "arts/utils/atomics.h"
#include "arts/utils/deque.h"

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#include "arts/gpu/gpu_stream.h"
#endif

#define PACKET_SIZE 4096
#define NETWORK_BACKOFF_INCREMENT 0
#define ARTS_CROSS_NUMA_STEAL_BACKOFF 1024U

extern unsigned int num_numa_domains;

#if defined(__APPLE__)
extern void init_per_node(unsigned int node_id, int argc, char **argv)
    __attribute__((weak_import));
extern void init_per_worker(unsigned int node_id, unsigned int worker_id,
                            int argc, char **argv)
    __attribute__((weak_import));
#else
extern void init_per_node(unsigned int node_id, int argc, char **argv)
    __attribute__((weak));
extern void init_per_worker(unsigned int node_id, unsigned int worker_id,
                            int argc, char **argv)
    __attribute__((weak));
#endif

static int arts_runtime_argc = 0;
static char **arts_runtime_argv = NULL;

static void arts_worker_try_sleep(void);

static inline void arts_runtime_idle_backoff(void) {
  for (unsigned int i = 0; i < arts_thread_info.back_off; i++) {
    ARTS_SPIN_PAUSE();
  }
  if (arts_thread_info.back_off < 4096) {
    arts_thread_info.back_off <<= 1;
  } else if (arts_node_info.idle_sleep_enabled) {
    arts_worker_try_sleep();
  }
}

static inline void arts_wake_one_worker(void) {
  if (!arts_node_info.idle_sleep_enabled)
    return;
  /* Quick check: if all workers are spinning, nobody is sleeping. */
  if (__atomic_load_n(&arts_node_info.n_spinning, __ATOMIC_RELAXED) >=
      arts_node_info.worker_thread_count)
    return;
  pthread_mutex_lock(&arts_node_info.worker_sleep_mutex);
  pthread_cond_signal(&arts_node_info.worker_sleep_cond);
  pthread_mutex_unlock(&arts_node_info.worker_sleep_mutex);
}

static void arts_worker_try_sleep(void) {
  /* Only sleep if there are enough spinners remaining. */
  unsigned int cur = __atomic_load_n(&arts_node_info.n_spinning, __ATOMIC_ACQUIRE);
  while (cur > arts_node_info.max_spinners) {
    if (__atomic_compare_exchange_n(&arts_node_info.n_spinning, &cur, cur - 1,
                                    /*weak=*/1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      goto do_sleep;
  }
  return;

do_sleep:
  pthread_mutex_lock(&arts_node_info.worker_sleep_mutex);
  /* Re-check alive under the lock to avoid sleeping past shutdown. */
  if (arts_thread_info.alive) {
    arts_thread_info.is_sleeping = true;
    pthread_cond_wait(&arts_node_info.worker_sleep_cond,
                      &arts_node_info.worker_sleep_mutex);
    arts_thread_info.is_sleeping = false;
  }
  pthread_mutex_unlock(&arts_node_info.worker_sleep_mutex);
  __atomic_add_fetch(&arts_node_info.n_spinning, 1, __ATOMIC_ACQ_REL);
  arts_thread_info.back_off = 1;
}

unsigned int arts_pick_worker_for_numa(unsigned int preferred_numa_id,
                                       const unsigned int *thread_numa_ids,
                                       unsigned int worker_count,
                                       unsigned int start) {
  if (!worker_count)
    return ARTS_INVALID_WORKER_ID;

  unsigned int start_idx = start % worker_count;
  if (thread_numa_ids) {
    unsigned int local_count = 0;
    for (unsigned int i = 0; i < worker_count; i++) {
      if (thread_numa_ids[i] == preferred_numa_id)
        local_count++;
    }
    if (local_count) {
      unsigned int local_target = start % local_count;
      unsigned int local_seen = 0;
      for (unsigned int i = 0; i < worker_count; i++) {
        if (thread_numa_ids[i] != preferred_numa_id)
          continue;
        if (local_seen == local_target)
          return i;
        local_seen++;
      }
    }
  }

  return start_idx;
}

unsigned int arts_pick_worker_steal_victim(unsigned int self_thread_id,
                                           unsigned int self_numa_id,
                                           const unsigned int *thread_numa_ids,
                                           unsigned int worker_count,
                                           unsigned int start,
                                           bool allow_cross_numa) {
  if (worker_count <= 1)
    return ARTS_INVALID_WORKER_ID;

  unsigned int start_idx = start % worker_count;
  if (thread_numa_ids) {
    for (unsigned int i = 0; i < worker_count; i++) {
      unsigned int victim = (start_idx + i) % worker_count;
      if (victim == self_thread_id)
        continue;
      if (thread_numa_ids[victim] == self_numa_id)
        return victim;
    }
  }

  if (!allow_cross_numa)
    return ARTS_INVALID_WORKER_ID;

  for (unsigned int i = 0; i < worker_count; i++) {
    unsigned int victim = (start_idx + i) % worker_count;
    if (victim != self_thread_id)
      return victim;
  }

  return ARTS_INVALID_WORKER_ID;
}

unsigned int arts_pick_ready_worker(unsigned int preferred_numa_id,
                                    arts_guid_t ready_guid) {
  unsigned int worker_count = arts_node_info.worker_thread_count;
  if (!worker_count)
    return ARTS_INVALID_WORKER_ID;

  unsigned int seed =
      (unsigned int)(ready_guid ^ (ready_guid >> 32) ^ arts_thread_info.thread_id);
  unsigned int rr =
      __atomic_fetch_add(&arts_node_info.ready_rr_counter, 1,
                         __ATOMIC_RELAXED);
  unsigned int start = seed + rr;
  return arts_pick_worker_for_numa(preferred_numa_id,
                                   arts_node_info.thread_numa_ids,
                                   worker_count, start);
}

static inline unsigned int
arts_pick_ready_worker_for_edt(const struct arts_edt_s *edt) {
  return arts_pick_ready_worker(
      edt ? edt->numa_domain : arts_thread_info.numa_domain_id,
      edt ? edt->current_edt : 0);
}

static inline bool arts_thread_owns_worker_deque(unsigned int worker) {
  return arts_thread_info.role == ARTS_ROLE_WORKER &&
         arts_thread_info.thread_id == worker && arts_thread_info.my_deque;
}

static inline void arts_enqueue_ready_inbox(unsigned int target_worker,
                                            struct arts_edt_s *edt) {
  struct arts_ready_edt_node_s *node =
      (struct arts_ready_edt_node_s *)arts_malloc(sizeof(*node));
  node->edt = edt;
  node->next = NULL;

  pthread_mutex_lock(&arts_node_info.ready_inbox_locks[target_worker]);
  struct arts_ready_edt_node_s *tail =
      arts_node_info.ready_inbox_tails[target_worker];
  if (tail)
    tail->next = node;
  else
    arts_node_info.ready_inbox_heads[target_worker] = node;
  arts_node_info.ready_inbox_tails[target_worker] = node;
  pthread_mutex_unlock(&arts_node_info.ready_inbox_locks[target_worker]);
}

static inline struct arts_edt_s *arts_runtime_pop_ready_inbox(void) {
  unsigned int worker = arts_thread_info.thread_id;
  if (worker >= arts_node_info.worker_thread_count ||
      !arts_node_info.ready_inbox_heads)
    return NULL;

  pthread_mutex_lock(&arts_node_info.ready_inbox_locks[worker]);
  struct arts_ready_edt_node_s *node = arts_node_info.ready_inbox_heads[worker];
  if (node) {
    arts_node_info.ready_inbox_heads[worker] = node->next;
    if (!arts_node_info.ready_inbox_heads[worker])
      arts_node_info.ready_inbox_tails[worker] = NULL;
  }
  pthread_mutex_unlock(&arts_node_info.ready_inbox_locks[worker]);

  if (!node)
    return NULL;
  struct arts_edt_s *edt = node->edt;
  arts_free(node);
  return edt;
}

static inline struct arts_edt_s *
arts_runtime_steal_from_ready_inbox(unsigned int worker) {
  if (worker >= arts_node_info.worker_thread_count ||
      !arts_node_info.ready_inbox_heads)
    return NULL;

  pthread_mutex_lock(&arts_node_info.ready_inbox_locks[worker]);
  struct arts_ready_edt_node_s *node = arts_node_info.ready_inbox_heads[worker];
  if (node) {
    arts_node_info.ready_inbox_heads[worker] = node->next;
    if (!arts_node_info.ready_inbox_heads[worker])
      arts_node_info.ready_inbox_tails[worker] = NULL;
  }
  pthread_mutex_unlock(&arts_node_info.ready_inbox_locks[worker]);

  if (!node)
    return NULL;
  struct arts_edt_s *edt = node->edt;
  arts_free(node);
  return edt;
}

static inline unsigned int arts_enqueue_ready_cpu_edt(struct arts_edt_s *edt) {
  unsigned int target_worker = arts_pick_ready_worker_for_edt(edt);
  if (target_worker == ARTS_INVALID_WORKER_ID)
    target_worker = 0;

  if (target_worker >= arts_node_info.worker_thread_count)
    target_worker = 0;

  if (arts_thread_owns_worker_deque(target_worker))
    arts_deque_push_front(arts_thread_info.my_deque, edt, 0);
  else if (arts_node_info.ready_inbox_heads &&
           arts_node_info.ready_inbox_locks)
    arts_enqueue_ready_inbox(target_worker, edt);
  else
    arts_deque_push_front(arts_node_info.deque[target_worker], edt, 0);

  return target_worker;
}

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
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

typedef bool (*scheduler_t)(void);
#ifdef ARTS_USE_GPU
scheduler_t scheduler_loop[] = {
    (scheduler_t)arts_default_scheduler_loop,
    (scheduler_t)arts_network_before_steal_scheduler_loop,
    (scheduler_t)arts_network_first_scheduler_loop,
    (scheduler_t)arts_gpu_scheduler_loop,
    (scheduler_t)arts_gpu_scheduler_backoff_loop,
    (scheduler_t)arts_gpu_scheduler_demand_loop};
#else
scheduler_t scheduler_loop[] = {
    (scheduler_t)arts_default_scheduler_loop,
    (scheduler_t)arts_network_before_steal_scheduler_loop,
    (scheduler_t)arts_network_first_scheduler_loop};
#endif

void arts_runtime_node_init(struct arts_config_s *config) {
  unsigned int tc = config->thread_count;

  /* Scheduler */
  arts_node_info.scheduler = scheduler_loop[config->scheduler];

  /* Deque implementation selection (0=simple, 1=priority) */
  arts_deque_select(config->deque_type);

  /* Per-thread indexed arrays */
  arts_node_info.deque =
      (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) * tc);
  arts_node_info.ready_inbox_locks =
      (pthread_mutex_t *)arts_malloc(sizeof(pthread_mutex_t) * tc);
  arts_node_info.ready_inbox_heads =
      (struct arts_ready_edt_node_s **)arts_calloc(
          tc, sizeof(struct arts_ready_edt_node_s *));
  arts_node_info.ready_inbox_tails =
      (struct arts_ready_edt_node_s **)arts_calloc(
          tc, sizeof(struct arts_ready_edt_node_s *));
  for (unsigned int i = 0; i < tc; ++i)
    pthread_mutex_init(&arts_node_info.ready_inbox_locks[i], NULL);
  arts_node_info.receiver_deque =
      config->receiver_thread_count
          ? (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) *
                                                config->receiver_thread_count)
          : NULL;
  arts_node_info.gpu_deque =
      (struct arts_deque_s **)arts_malloc(sizeof(struct arts_deque_s *) * tc);
  arts_node_info.route_table =
      (arts_route_table_t **)arts_calloc(tc, sizeof(arts_route_table_t *));
  arts_node_info.gpu_route_table =
      config->gpu ? (arts_route_table_t **)arts_calloc(
                        config->gpu, sizeof(arts_route_table_t *))
                  : NULL;
  arts_node_info.remote_route_table = arts_new_route_table(
      config->route_table_entries, config->route_table_size);
  arts_node_info.thread_numa_ids =
      (unsigned int *)arts_calloc(tc, sizeof(unsigned int));
  arts_node_info.local_spin = (volatile bool **)arts_calloc(tc, sizeof(bool *));
  arts_node_info.memory_moves =
      (unsigned int **)arts_calloc(tc, sizeof(unsigned int *));
  arts_node_info.atomic_waits =
      (struct atomic_create_barrier_info_s **)arts_calloc(
          tc, sizeof(struct atomic_create_barrier_info_s *));

  /* Thread counts */
  arts_node_info.worker_thread_count = config->worker_thread_count;
  arts_node_info.sender_thread_count = config->sender_thread_count;
  arts_node_info.receiver_thread_count = config->receiver_thread_count;
  arts_node_info.total_thread_count = tc;

  /* Synchronization barriers */
  arts_node_info.ready_to_push = tc;
  arts_node_info.ready_to_parallel_start = tc;
  arts_node_info.ready_to_inspect = tc;
  arts_node_info.ready_to_execute = tc;
  arts_node_info.ready_to_network =
      config->sender_thread_count + config->receiver_thread_count;
  arts_node_info.ready_to_clean = tc;

  /* Locks and shutdown coordination */
  arts_node_info.send_lock = 0U;
  arts_node_info.recv_lock = 0U;
  arts_node_info.steal_request_lock = 1U;
  arts_node_info.shutdown_count = arts_global_rank_count - 1;
  arts_node_info.ready_to_shutdown = arts_global_rank_count - 1;
  arts_node_info.auto_shutdown_guid = config->auto_shutdown ? 1 : NULL_GUID;

  /* Network buffer */
  arts_node_info.buf = (char *)arts_malloc(PACKET_SIZE);
  arts_node_info.packet_size = PACKET_SIZE;

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

  /* Worker idle-sleep */
  arts_node_info.idle_sleep_enabled = config->idle_sleep_enabled;
  if (arts_node_info.idle_sleep_enabled) {
    pthread_mutex_init(&arts_node_info.worker_sleep_mutex, NULL);
    pthread_cond_init(&arts_node_info.worker_sleep_cond, NULL);
    arts_node_info.n_spinning = config->worker_thread_count;
    unsigned int quarter = config->worker_thread_count / 4;
    arts_node_info.max_spinners = quarter < 1 ? 1 : (quarter > 16 ? 16 : quarter);
  }

  /* GUID generation */
  arts_node_info.keys = (uint64_t **)arts_calloc(tc, sizeof(uint64_t *));
  arts_node_info.global_guid_thread_id =
      (uint64_t *)arts_calloc(tc, sizeof(uint64_t));

  /* Performance counters */
  arts_node_info.counter_folder = config->counter_folder;
  arts_node_info.counter_capture_interval = config->counter_capture_interval;

  arts_node_info.live_counters =
      (arts_counter_t **)arts_calloc(tc, sizeof(arts_counter_t *));

  arts_node_info.saved_counters =
      (arts_counter_t **)arts_calloc(tc, sizeof(arts_counter_t *));
  for (unsigned int t = 0; t < tc; t++) {
    arts_node_info.saved_counters[t] = (arts_counter_t *)arts_calloc(
        NUM_COUNTER_TYPES, sizeof(arts_counter_t));
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

  /* Object counter storage (per-arts_id tracking) */
  arts_object_alloc_node_storage(tc);

#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu) {
    arts_node_init_gpus();
  }
#endif
}

void arts_runtime_global_cleanup() {
  arts_counter_capture_stop();
  // Write all counter outputs (thread, node, and cluster levels)
  unsigned int tc = arts_node_info.total_thread_count;
  for (unsigned int t = 0; t < tc; t++) {
    arts_counter_write(arts_node_info.counter_folder, arts_global_rank_id, t);
  }
  // Write object counter output (per-arts_id tracking)
  arts_object_write_node(arts_node_info.counter_folder, arts_global_rank_id,
                         tc);
  arts_clean_up_dbs();

  /* Counter cleanup (reverse of arts_runtime_node_init allocation) */
  for (unsigned int t = 0; t < tc; t++) {
    if (arts_node_info.capture_arrays && arts_node_info.capture_arrays[t]) {
      for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
        if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC &&
            arts_node_info.capture_arrays[t][i]) {
          arts_delete_array_list(arts_node_info.capture_arrays[t][i]);
          arts_node_info.capture_arrays[t][i] = NULL;
        } else if (arts_counter_mode_array[i] != ARTS_COUNTER_MODE_PERIODIC &&
                   arts_node_info.capture_arrays[t][i]) {
          ARTS_WARN("Ignoring non-periodic capture array pointer during "
                    "cleanup (rank=%u thread=%u counter=%s ptr=%p)",
                    arts_global_rank_id, t, arts_counter_names[i],
                    (void *)arts_node_info.capture_arrays[t][i]);
          arts_node_info.capture_arrays[t][i] = NULL;
        }
      }
      arts_free(arts_node_info.capture_arrays[t]);
      arts_node_info.capture_arrays[t] = NULL;
    }
    if (arts_node_info.saved_counters) {
      arts_free(arts_node_info.saved_counters[t]);
      arts_node_info.saved_counters[t] = NULL;
    }
  }
  arts_free(arts_node_info.capture_arrays);
  arts_node_info.capture_arrays = NULL;
  arts_free(arts_node_info.saved_counters);
  arts_node_info.saved_counters = NULL;
  arts_free(arts_node_info.live_counters);
  arts_node_info.live_counters = NULL;

  /* Object counter cleanup */
  arts_object_cleanup_node_storage(tc);

#ifdef ARTS_USE_GPU
  /* GPU cleanup must run BEFORE route tables are freed — free_gpu_item()
     calls arts_route_table_lookup_db() for LC DB host-side metadata. */
  if (arts_node_info.gpu) {
    arts_cleanup_gpus();
  }
#endif

  /* Route table cleanup (after entries cleaned by arts_clean_up_dbs) */
  for (unsigned int i = 0; i < tc; i++) {
    arts_delete_route_table(arts_node_info.route_table[i]);
  }
  arts_free(arts_node_info.route_table);
  arts_delete_route_table(arts_node_info.remote_route_table);

  if (arts_node_info.ready_inbox_heads) {
    for (unsigned int i = 0; i < tc; ++i) {
      struct arts_ready_edt_node_s *node = arts_node_info.ready_inbox_heads[i];
      while (node) {
        struct arts_ready_edt_node_s *next = node->next;
        arts_free(node);
        node = next;
      }
      pthread_mutex_destroy(&arts_node_info.ready_inbox_locks[i]);
    }
  }

  /* Per-thread indexed arrays */
  arts_free(arts_node_info.deque);
  arts_free(arts_node_info.ready_inbox_locks);
  arts_free(arts_node_info.ready_inbox_heads);
  arts_free(arts_node_info.ready_inbox_tails);
  arts_free(arts_node_info.receiver_deque);
  arts_free(arts_node_info.gpu_deque);
  arts_free(arts_node_info.gpu_route_table);
  arts_free(arts_node_info.thread_numa_ids);
  arts_free((void *)arts_node_info.local_spin);
  arts_free(arts_node_info.memory_moves);
  arts_free(arts_node_info.atomic_waits);
  arts_free(arts_node_info.buf);
  for (unsigned int i = 0; i < tc; i++) {
    arts_free(arts_node_info.keys[i]);
  }
  arts_free(arts_node_info.keys);
  arts_free(arts_node_info.global_guid_thread_id);

  /* Worker idle-sleep cleanup */
  if (arts_node_info.idle_sleep_enabled) {
    pthread_mutex_destroy(&arts_node_info.worker_sleep_mutex);
    pthread_cond_destroy(&arts_node_info.worker_sleep_cond);
  }

  /* Network outbound queues and sequence tracking arrays */
  arts_server_cleanup();

  /* Socket server global arrays (safe to call even for single-node) */
  arts_ll_server_cleanup();
}

/*
 * arts_thread_zero_node_start — Thread 0 (master) startup sequence.
 *
 * After all threads have registered (ready_to_push barrier), thread 0:
 *   1. Enables global GUID generation.
 *   2. Creates the shutdown epoch (termination detection).
 *   3. Schedules main_edt on rank 0 (if defined by the application).
 *   4. Waits for all threads through a series of barriers before entering
 *      the main scheduler loop.
 */
void arts_thread_zero_node_start(int argc, char **argv) {
  ARTS_INFO("Thread 0: starting node initialization");
  arts_runtime_argc = argc;
  arts_runtime_argv = argv;
  set_global_guid_on();
  arts_shutdown_epoch_create();

  // Note: Counter capture starts AFTER barriers below, when receiver threads
  // are running. This ensures time sync messages can be processed.
  TIME_INIT_STOP();
  TIME_TOTAL_START();

  if (init_per_node)
    init_per_node(arts_global_rank_id, argc, argv);

#ifdef ARTS_USE_GPU
  arts_init_per_gpu_wrapper(argc, argv);
#endif
  set_guid_generator_after_parallel_start();

  arts_atomic_sub(&arts_node_info.ready_to_parallel_start, 1U);
  while (arts_node_info.ready_to_parallel_start) { ARTS_SPIN_PAUSE(); }
  if (init_per_worker && arts_thread_info.role == ARTS_ROLE_WORKER)
    init_per_worker(arts_global_rank_id, arts_thread_info.group_pos, argc,
                    argv);
  arts_increment_finished_epoch_list();

  arts_atomic_sub(&arts_node_info.ready_to_inspect, 1U);
  while (arts_node_info.ready_to_inspect) { ARTS_SPIN_PAUSE(); }
  arts_atomic_sub(&arts_node_info.ready_to_execute, 1U);
  while (arts_node_info.ready_to_execute) { ARTS_SPIN_PAUSE(); }
  while (arts_node_info.ready_to_network) { ARTS_SPIN_PAUSE(); }

  if (arts_global_rank_count > 1) {
    arts_remote_eager_connect_all();
  }

  // Start counter capture AFTER all barriers, when receiver threads are in
  // their runtime loops. This ensures time sync requests can be processed.
  arts_counter_capture_start();

  if (!arts_global_rank_id) {
    ARTS_INFO("Thread 0: scheduling main_edt on rank 0 (argc=%d)", argc);
    uint64_t main_args[2] = {(uint64_t)argc, (uint64_t)argv};
    arts_hint_t main_hint = {0, 0};
    arts_edt_create(main_edt, 2, main_args, 0, &main_hint);
  }
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
    arts_node_info.route_table[thread->id] = arts_new_route_table(
        config->route_table_entries, config->route_table_size);
#ifdef ARTS_USE_GPU
    if (config->gpu) {
      arts_worker_init_gpus();
    }
#endif
  }

  if (thread->role == ARTS_ROLE_SENDER || thread->role == ARTS_ROLE_RECEIVER) {
    if (thread->role == ARTS_ROLE_SENDER) {
      unsigned int size = arts_global_rank_count * config->port_count /
                          arts_node_info.sender_thread_count;
      unsigned int rem = arts_global_rank_count * config->port_count %
                         arts_node_info.sender_thread_count;
      unsigned int start;
      if (thread->group_pos < rem) {
        start = thread->group_pos * (size + 1);
        arts_remote_set_thread_outbound_queues(start, start + size + 1);
      } else {
        start = (rem * (size + 1)) + ((thread->group_pos - rem) * size);
        arts_remote_set_thread_outbound_queues(start, start + size);
      }
      ARTS_TRACE_RDMA("sender queues thread_id=%u group=%u start=%u stop=%u",
                      thread->id, thread->group_pos, start,
                      (thread->group_pos < rem) ? start + size + 1
                                                : start + size);
    }
    if (thread->role == ARTS_ROLE_RECEIVER) {
      arts_node_info.receiver_deque[thread->group_pos] =
          arts_node_info.deque[thread->id];
      unsigned int size = (arts_global_rank_count - 1) * config->port_count /
                          arts_node_info.receiver_thread_count;
      unsigned int rem = (arts_global_rank_count - 1) * config->port_count %
                         arts_node_info.receiver_thread_count;
      unsigned int start;
      if (thread->group_pos < rem) {
        start = thread->group_pos * (size + 1);
        arts_remote_set_thread_inbound_queues(start, start + size + 1);
      } else {
        start = (rem * (size + 1)) + ((thread->group_pos - rem) * size);
        arts_remote_set_thread_inbound_queues(start, start + size);
      }
      ARTS_TRACE_RDMA("receiver queues thread_id=%u group=%u start=%u stop=%u",
                      thread->id, thread->group_pos, start,
                      (thread->group_pos < rem) ? start + size + 1
                                                : start + size);
    }
  }
  arts_node_info.local_spin[thread->id] = &arts_thread_info.alive;
  arts_thread_info.alive = true;
  arts_node_info.memory_moves[thread->id] =
      (unsigned int *)&arts_thread_info.outstanding_memory_moves;
  arts_node_info.atomic_waits[thread->id] = &arts_thread_info.atomic_wait;
  arts_thread_info.atomic_wait.wait = true;
  arts_thread_info.outstanding_memory_moves = 0;
  arts_thread_info.pu_id = thread->pu_id;
  arts_thread_info.thread_id = thread->id;
  arts_thread_info.group_pos = thread->group_pos;
  arts_thread_info.numa_domain_id = thread->numa_domain_id;
  arts_node_info.thread_numa_ids[thread->id] = thread->numa_domain_id;
  arts_thread_info.role = thread->role;
  arts_thread_info.back_off = 1;
  arts_thread_info.current_edt_guid = 0;
  arts_thread_info.local_counting = 1;
  arts_thread_info.shad_lock = 0;

  // Register thread-local counter storage with nodeInfo
  arts_node_info.live_counters[thread->id] = arts_thread_local_counters;
#if ENABLE_ARTS_ID_EDT_METRICS || ENABLE_ARTS_ID_DB_METRICS
  arts_id_init_hash_table(&arts_thread_local_arts_id_metrics);
#endif
#if ENABLE_ARTS_ID_EDT_CAPTURES
  arts_thread_local_edt_capture_list =
      arts_new_array_list(sizeof(arts_id_edt_capture_t), 1024);
#endif
#if ENABLE_ARTS_ID_DB_CAPTURES
  arts_thread_local_db_capture_list =
      arts_new_array_list(sizeof(arts_id_db_capture_t), 1024);
#endif

  arts_guid_key_generator_init();

  arts_atomic_sub(&arts_node_info.ready_to_push, 1U);
  while (arts_node_info.ready_to_push) { ARTS_SPIN_PAUSE(); }
  if (thread->id) {
    arts_atomic_sub(&arts_node_info.ready_to_parallel_start, 1U);
    while (arts_node_info.ready_to_parallel_start) { ARTS_SPIN_PAUSE(); }

    if (arts_thread_info.role == ARTS_ROLE_WORKER) {
      if (init_per_worker)
        init_per_worker(arts_global_rank_id, arts_thread_info.group_pos,
                        arts_runtime_argc, arts_runtime_argv);
      arts_increment_finished_epoch_list();
    }

    arts_atomic_sub(&arts_node_info.ready_to_inspect, 1U);
    while (arts_node_info.ready_to_inspect) { ARTS_SPIN_PAUSE(); }
    arts_atomic_sub(&arts_node_info.ready_to_execute, 1U);
    while (arts_node_info.ready_to_execute) { ARTS_SPIN_PAUSE(); }
  }
  arts_thread_info.drand_buf[0] = 1202107158 + (thread->id * 1999);
  arts_thread_info.drand_buf[1] = 0;
  arts_thread_info.drand_buf[2] = 0;
}

void arts_runtime_private_cleanup() {
  unsigned int remaining = arts_atomic_sub(&arts_node_info.ready_to_clean, 1U);
  ARTS_TRACE_RDMA("private_cleanup barrier enter remaining=%u", remaining);
  while (arts_node_info.ready_to_clean) { ARTS_SPIN_PAUSE(); }
  ARTS_TRACE_RDMA("private_cleanup barrier leave");
  arts_remote_thread_outbound_queues_cleanup();
  arts_remote_thread_inbound_queues_cleanup();
  if (arts_thread_info.my_deque) {
    arts_deque_delete(arts_thread_info.my_deque);
  }
  if (arts_thread_info.my_node_deque) {
    arts_deque_delete(arts_thread_info.my_node_deque);
  }
  if (arts_thread_info.my_gpu_deque) {
    arts_deque_delete(arts_thread_info.my_gpu_deque);
  }
  arts_cleanup_epoch_pools();
  arts_cleanup_edt_tls();
}

/*
 * arts_runtime_stop — Stop all worker/network threads.
 *
 * Called from arts_shutdown() (single-node) or from the network send thread
 * after the shutdown timeout (multi-node).
 *
 * Protocol:
 *   1. Wait for each thread to register its local_spin pointer (non-NULL
 *      means the thread has finished arts_runtime_private_init).
 *   2. Set *local_spin[i] = false, which clears arts_thread_info.alive for
 *      that thread, causing it to exit its scheduler/network loop.
 */
void arts_runtime_stop() {
  ARTS_INFO("arts_runtime_stop: stopping %u threads",
            arts_node_info.total_thread_count);
  ARTS_TRACE_RDMA("runtime_stop begin total_threads=%u",
                  arts_node_info.total_thread_count);
  unsigned int i;
  for (i = 0; i < arts_node_info.total_thread_count; i++) {
    ARTS_DEBUG("arts_runtime_stop: waiting for thread %u to register", i);
    while (!arts_node_info.local_spin[i]) { ARTS_SPIN_PAUSE(); }
    (*arts_node_info.local_spin[i]) = false;
    ARTS_TRACE_RDMA("runtime_stop signaled thread=%u", i);
    ARTS_DEBUG("arts_runtime_stop: thread %u signaled to stop", i);
  }
  /* Wake any workers sleeping on the condvar so they see alive==false. */
  if (arts_node_info.idle_sleep_enabled) {
    pthread_mutex_lock(&arts_node_info.worker_sleep_mutex);
    pthread_cond_broadcast(&arts_node_info.worker_sleep_cond);
    pthread_mutex_unlock(&arts_node_info.worker_sleep_mutex);
  }
  if (arts_global_rank_count > 1) {
    ARTS_TRACE_RDMA("runtime_stop wake receivers enter");
    arts_ll_server_wakeup_receivers();
    ARTS_TRACE_RDMA("runtime_stop wake receivers leave");
  }
  ARTS_INFO("arts_runtime_stop: all threads signaled");
  ARTS_TRACE_RDMA("runtime_stop leave");
}

void arts_handle_remote_stolen_edt(struct arts_edt_s *edt) {
  ARTS_DEBUG("Processing stolen EDT[Id:%lu, Guid:%lu] on PU %u", edt->arts_id,
             edt->current_edt, arts_thread_info.pu_id);
  ARTS_TRACE_RDMA("remote stolen ready rank=%u edt=%lu epoch=%lu",
                  arts_global_rank_id, edt ? edt->current_edt : NULL_GUID,
                  edt ? edt->epoch_guid : NULL_GUID);
  increment_queue_epoch(edt->epoch_guid);
  arts_shutdown_epoch_inc_queue();
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu &&
      (!arts_thread_info.my_deque || !arts_thread_info.my_gpu_deque))
    arts_store_new_edts(edt);
  else
#endif
  {
    if (edt->edt_type == ARTS_EDT_GPU) {
      arts_deque_push_front(arts_thread_info.my_gpu_deque, edt, 0);
    } else {
      arts_deque_push_front(arts_thread_info.my_deque, edt, 0);
    }
    arts_wake_one_worker();
  }
}

/*
 * arts_handle_ready_edt — Transition an EDT from "all deps signaled" to
 *                         "queued for execution".
 *
 * Called when depc_needed reaches 0 after the last signal or after the
 * sentinel is removed during creation.
 *
 * Two phases:
 *   Phase 1 (acquire_dbs): Re-initialize depc_needed = depc + 1 (sentinel)
 *     and attempt to acquire each DB dependency locally.  If a DB is not
 *     available, an OOO request is issued; when it resolves later, it will
 *     decrement depc_needed and potentially push the EDT to the deque.
 *   Phase 2 (sentinel removal): Atomically decrement the sentinel.  If all
 *     DBs were acquired synchronously, depc_needed hits 0 here and the EDT
 *     is pushed to the worker deque for execution.
 */
void arts_handle_ready_edt(struct arts_edt_s *edt) {
  ARTS_INFO("EDT[Guid:%lu, Id:%lu] ready — entering acquire_dbs "
            "(depc=%u)",
            edt->current_edt, edt->arts_id, edt->depc);
  acquire_dbs(edt);
  unsigned int remaining = arts_atomic_sub(&edt->depc_needed, 1U);
  ARTS_INFO("EDT[Guid:%lu] acquire_dbs done, sentinel removed: "
            "depc_needed=%u",
            edt->current_edt, remaining);
  if (remaining == 0) {
    INCREMENT_NUM_EDT_ACQUIRE_BY(1);
    increment_queue_epoch(edt->epoch_guid);
    arts_shutdown_epoch_inc_queue();
#ifdef ARTS_USE_GPU
    if (arts_node_info.gpu &&
        (!arts_thread_info.my_deque || !arts_thread_info.my_gpu_deque)) {
      if (!arts_thread_info.my_deque) {
        /* CUDA callback thread: new_edts/new_edt_lock set from closure */
        arts_store_new_edts(edt);
      } else {
        /* Non-worker thread (sender/receiver): push to a worker deque */
        if (edt->edt_type == ARTS_EDT_GPU) {
          arts_deque_push_front(arts_node_info.gpu_deque[0], edt, 0);
        } else {
          arts_enqueue_ready_cpu_edt(edt);
        }
        arts_wake_one_worker();
      }
    } else
#endif
    {
      if (edt->edt_type == ARTS_EDT_GPU) {
        ARTS_INFO("EDT[Guid:%lu] pushed to GPU deque", edt->current_edt);
        arts_deque_push_front(arts_thread_info.my_gpu_deque, edt, 0);
      } else {
        unsigned int target_worker = arts_enqueue_ready_cpu_edt(edt);
        ARTS_INFO("EDT[Guid:%lu] pushed to worker deque %u (preferred NUMA %u)",
                  edt->current_edt, target_worker, edt->numa_domain);
      }
      arts_wake_one_worker();
    }
  } else {
    ARTS_DEBUG("EDT[Guid:%lu] waiting for %u more DB acquisitions",
               edt->current_edt, remaining);
  }
}

void arts_run_edt(struct arts_edt_s *edt) {
  uint32_t depc = edt->depc;
  arts_edt_dep_t *depv =
      (arts_edt_dep_t *)(((uint64_t *)(edt + 1)) + edt->paramc);

  arts_edt_t func = edt->func_ptr;
  uint32_t paramc = edt->paramc;
  const uint64_t *paramv = (uint64_t *)(edt + 1);

  ARTS_INFO("Running EDT[Id:%lu, Guid:%lu, Deps: %u, Params: %u, "
            "DepvPtr: %p]",
            edt->arts_id, edt->current_edt, depc, paramc, depv);
  prep_dbs(depc, depv, false);

  arts_set_thread_local_edt_info(edt);

  TIME_EDT_EXEC_START();
#if ENABLE_TIME_EDT_EXEC || ARTS_OBJECT_EDT_TABLE_ENABLED ||                   \
    ARTS_OBJECT_EDT_TRACE_ENABLED
  struct timespec start_time;
  struct timespec end_time;
  (void)clock_gettime(CLOCK_MONOTONIC, &start_time);
  func(paramc, paramv, depc, depv);
  (void)clock_gettime(CLOCK_MONOTONIC, &end_time);
  TIME_EDT_EXEC_STOP();

  // Record per-object EDT metrics
  uint64_t exec_ns = ((end_time.tv_sec - start_time.tv_sec) * 1000000000ULL) +
                     (end_time.tv_nsec - start_time.tv_nsec);
  arts_object_record_edt(edt->arts_id, exec_ns, 0);
  arts_object_trace_edt(edt->arts_id, exec_ns, 0);
#else
  func(paramc, paramv, depc, depv);
  TIME_EDT_EXEC_STOP();
  uint64_t exec_ns = 0;
#endif

  INCREMENT_NUM_EDT_FINISH_BY(1);

  /* Release DBs BEFORE signaling epoch completion. This ensures remote
   * DB updates (arts_remote_update_db) are queued to the sender thread
   * before the epoch-done message. TCP FIFO ordering then guarantees
   * the DB data arrives at the owner before the epoch-done signal,
   * preventing stale reads in the next epoch. */
  release_dbs(depc, depv, false);
  arts_release_created_dbs();

  arts_unset_thread_local_edt_info();

  // This is for a synchronous path
  if (edt->output_buffer != NULL_GUID) {
    arts_set_buffer(edt->output_buffer, arts_calloc(1, sizeof(unsigned int)),
                    sizeof(unsigned int));
  }

  ARTS_INFO("EDT[Guid:%lu, Id:%lu] finished (exec_ns=%lu)", edt->current_edt,
            edt->arts_id, exec_ns);
  arts_edt_delete(edt);
  DEC_OUTSTANDING_EDTS(1);
  ARTS_DEBUG("EDT completed, outstanding_edts decremented");
}

inline struct arts_edt_s *arts_runtime_steal_from_network() {
  struct arts_edt_s *edt = NULL;
  if (arts_global_rank_count > 1) {
    unsigned int index = arts_thread_info.thread_id;
    for (unsigned int i = 0; i < arts_node_info.receiver_thread_count; i++) {
      index = (index + 1) % arts_node_info.receiver_thread_count;
      if ((edt = (struct arts_edt_s *)arts_deque_pop_back(
               arts_node_info.receiver_deque[index])) != NULL) {
        break;
      }
    }
  }
  return edt;
}

#define HALF_STEAL_MAX 32

static inline struct arts_edt_s *
arts_try_steal_from_worker(unsigned int steal_loc) {
  void *stolen[HALF_STEAL_MAX];
  unsigned int count = arts_deque_simple_pop_back_half(
      arts_node_info.deque[steal_loc], stolen, HALF_STEAL_MAX);
  if (count == 0) {
    struct arts_edt_s *edt = arts_runtime_steal_from_ready_inbox(steal_loc);
    if (edt)
      INCREMENT_NUM_STEAL_SUCCESS_BY(1);
    return edt;
  }

  INCREMENT_NUM_STEAL_SUCCESS_BY(1);
  struct arts_edt_s *edt = (struct arts_edt_s *)stolen[0];
  /* Push remaining stolen items to our own deque. */
  for (unsigned int i = 1; i < count; i++) {
    arts_deque_push_front(arts_thread_info.my_deque, stolen[i], 0);
  }
  if (count > 1)
    arts_wake_one_worker();
  return edt;
}

inline struct arts_edt_s *arts_runtime_steal_from_worker() {
  unsigned int worker_count = arts_node_info.worker_thread_count;
  if (worker_count <= 1)
    return NULL;

  INCREMENT_NUM_STEAL_ATTEMPT_BY(1);
  unsigned int self = arts_thread_info.thread_id;
  unsigned int self_numa = arts_thread_info.numa_domain_id;
  unsigned int start =
      (unsigned int)jrand48(arts_thread_info.drand_buf) % worker_count;
  const unsigned int *thread_numa_ids = arts_node_info.thread_numa_ids;
  bool allow_cross_numa =
      arts_thread_info.back_off >= ARTS_CROSS_NUMA_STEAL_BACKOFF;

  for (unsigned int pass = 0; pass < 2; ++pass) {
    if (pass == 1 && !allow_cross_numa)
      break;

    for (unsigned int i = 0; i < worker_count; ++i) {
      unsigned int victim = (start + i) % worker_count;
      if (victim == self)
        continue;

      bool same_numa =
          thread_numa_ids && thread_numa_ids[victim] == self_numa;
      if ((pass == 0 && !same_numa) || (pass == 1 && same_numa))
        continue;

      struct arts_edt_s *edt = arts_try_steal_from_worker(victim);
      if (edt)
        return edt;
    }

    if (!thread_numa_ids)
      break;
  }
  return NULL;
}

bool arts_network_first_scheduler_loop() {
  struct arts_edt_s *edt_found;
  if (!(edt_found = arts_runtime_steal_from_network())) {
    if (!(edt_found = arts_runtime_pop_ready_inbox())) {
      if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
                arts_thread_info.my_node_deque))) {
        if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
                  arts_thread_info.my_deque))) {
          edt_found = arts_runtime_steal_from_worker();
        }
      }
    }
  }
  if (edt_found) {
    arts_thread_info.back_off = 1;
    arts_run_edt(edt_found);
    return true;
  }
  arts_runtime_idle_backoff();
  return false;
}

bool arts_network_before_steal_scheduler_loop() {
  struct arts_edt_s *edt_found;
  if (!(edt_found = arts_runtime_pop_ready_inbox())) {
    if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
              arts_thread_info.my_node_deque))) {
      if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
                arts_thread_info.my_deque))) {
        if (!(edt_found = arts_runtime_steal_from_network())) {
          edt_found = arts_runtime_steal_from_worker();
        }
      }
    }
  }

  if (edt_found) {
    arts_thread_info.back_off = 1;
    arts_run_edt(edt_found);
    return true;
  }
  arts_runtime_idle_backoff();
  return false;
}

struct arts_edt_s *arts_find_edt() {
  struct arts_edt_s *edt_found = NULL;
  if (!(edt_found = arts_runtime_pop_ready_inbox())) {
    if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
              arts_thread_info.my_deque))) {
      if (!edt_found) {
        if (!(edt_found = arts_runtime_steal_from_worker())) {
          edt_found = arts_runtime_steal_from_network();
        }
      }
    }
  }
  return edt_found;
}

bool arts_default_scheduler_loop() {
  struct arts_edt_s *edt_found = NULL;
  edt_found = arts_find_edt();

  if (edt_found) {
    arts_thread_info.back_off = 1;
    arts_run_edt(edt_found);
    // arts_wake_up_context();
    return true;
  }
  CHECK_OUTSTANDING_EDTS(10000000);
  arts_runtime_idle_backoff();
  return false;
}

/*
 * arts_runtime_loop — Main per-thread dispatch loop.
 *
 * Each thread enters exactly one of three roles:
 *   - network_receive: Polls for incoming messages (multi-node only).
 *   - network_send:    Drains outbound queues; triggers arts_runtime_stop()
 *                      when shutdown timeout elapses.
 *   - worker:          Runs the selected scheduler loop until alive==false.
 *
 * On single-node configurations, all threads are workers (no network threads).
 * The loop exits when arts_runtime_stop() sets alive=false for this thread.
 */
int arts_runtime_loop() {
  ARTS_DEBUG("Thread %u entering runtime_loop (role=%d)",
             arts_thread_info.thread_id, arts_thread_info.role);
  ARTS_TRACE_RDMA("runtime_loop enter role=%d ready_network=%u",
                  arts_thread_info.role, arts_node_info.ready_to_network);
  switch (arts_thread_info.role) {
  case ARTS_ROLE_RECEIVER:
    arts_atomic_sub(&arts_node_info.ready_to_network, 1U);
    ARTS_TRACE_RDMA("receiver runtime ready ready_network=%u",
                    arts_node_info.ready_to_network);
    while (arts_thread_info.alive) {
      arts_server_try_to_receive(&arts_node_info.buf,
                                 &arts_node_info.packet_size,
                                 &arts_node_info.steal_request_lock);
    }
    break;
  case ARTS_ROLE_SENDER:
    arts_atomic_sub(&arts_node_info.ready_to_network, 1U);
    ARTS_TRACE_RDMA("sender runtime ready ready_network=%u",
                    arts_node_info.ready_to_network);
    while (arts_thread_info.alive) {
      arts_remote_async_send();
    }
    break;
  case ARTS_ROLE_WORKER:
    while (arts_thread_info.alive) {
      arts_node_info.scheduler();
    }
    break;
  default:
    break;
  }
  ARTS_TRACE_RDMA("runtime_loop exit role=%d", arts_thread_info.role);
  ARTS_DEBUG("Thread %u exiting runtime_loop", arts_thread_info.thread_id);
  return 0;
}
