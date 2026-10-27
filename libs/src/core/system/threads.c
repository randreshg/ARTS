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
#include "arts/system/threads.h"
#include "arts/counter/Preamble.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <unistd.h>
#ifndef __APPLE__
#include <sched.h>
#endif

#include "arts/counter/counter.h"
#include "arts/memory/regpool.h" /* the pool learns this rank's NUMA nodes */
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/topology.h" /* arts_get_thread_mask + the NUMA facts */
#include "arts/transport/dispatcher.h"
#include "arts/utils/malloc.h"

unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;
unsigned int arts_global_master_rank_id;

struct arts_config_s *arts_thread_config;

struct thread_mask_s *arts_thread_masks;
pthread_t *arts_thread_handles;

void *arts_thread_loop(void *data) {
  struct thread_mask_s *thread = (struct thread_mask_s *)data;
  arts_runtime_private_init(thread, arts_thread_config);
  arts_runtime_loop();
  arts_runtime_private_cleanup();

  // Save final counter values to saved_counters before thread exits
  // This must happen after cleanup but before thread terminates
  uint64_t *saved = arts_node_info.saved_counters[thread->id];
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    saved[i] = atomic_load_explicit(&arts_thread_local_counters[i].count,
                                    memory_order_relaxed);
  }

  return NULL;
  // pthread_exit(NULL);
}

/*
 * arts_thread_main_join — Main thread (thread 0) entry after initialization.
 *
 * Runs the runtime loop until alive==false, then cleans up and joins all
 * other pthreads.  Counter data is saved before joining to ensure no
 * data loss.
 */
void arts_thread_main_join() {
  ARTS_DEBUG("arts_thread_main_join: main thread entering runtime_loop");
  arts_runtime_loop();
  ARTS_DEBUG("arts_thread_main_join: main thread exited runtime_loop, joining "
             "%u threads",
             arts_node_info.total_thread_count - 1);

  /* End-to-end marker (rank 0): e2e_start_stamp is taken at the application
   * start (runtime.c) and e2e_end_stamp by the shutdown entry's CAS winner
   * (shutdown.c), so the span is printed once that entry has finished, and
   * ahead of any teardown step that could hang.  Print it on stderr, gated by
   * $ARTS_E2E_MARKER, in the same "[E2E] <ns>" form the reference runtimes
   * use, so the harness parses all three runtimes identically.  A shutdown
   * recognised before the application started has no span, so nothing is
   * printed for it. */
  arts_runtime_wait_shutdown_entry();
  if (!arts_global_rank_id && arts_node_info.e2e_marker_enabled &&
      arts_node_info.e2e_end_stamp != 0 &&
      arts_node_info.e2e_end_stamp >= arts_node_info.e2e_start_stamp) {
    fprintf(stderr, "[E2E] %lu\n",
            (unsigned long)(arts_node_info.e2e_end_stamp -
                            arts_node_info.e2e_start_stamp));
    fflush(stderr);
  }

  /* Stop the progress threads.  They poll the fabric without ever blocking,
   * so clearing alive is enough to end their loops; no socket needs closing
   * to wake them. */
  arts_runtime_stop_network();

  /* The capture thread reads every runtime thread's counters in that
   * thread's own storage.  No thread leaves the runtime before it passes the
   * teardown barrier, and none passes it before this thread arrives, so
   * joining the capture thread first keeps every thread it reads alive and
   * orders its last access before each thread saves its counts. */
  arts_counter_capture_stop();
  arts_runtime_private_cleanup();

  // Save main thread's final counter values before joining other threads
  uint64_t *saved = arts_node_info.saved_counters[0];
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    saved[i] = atomic_load_explicit(&arts_thread_local_counters[i].count,
                                    memory_order_relaxed);
  }

  // File-based counter aggregation: no socket synchronization needed
  // Each node writes its own JSON file independently, master polls filesystem
  // Join ALL threads (workers and progress threads) with timeout + cancel
  // escalation, so a single stuck thread never wedges the entire process.
  //
  // Budget:
  //   JOIN_DEADLINE_MS   = 1500  per-thread cooperative join deadline
  //   CANCEL_DEADLINE_MS = 500   post-cancel grace window
  //
  // Escalation: timed join → pthread_cancel → timed join → give up and
  // move on (we are about to exit the process anyway).
  {
    const long join_deadline_ns = 1500L * 1000000L;  /* 1.5 s */
    const long cancel_deadline_ns = 500L * 1000000L; /* 0.5 s */
    for (unsigned int i = 1; i < arts_node_info.total_thread_count; i++) {
      struct timespec deadline;
      (void)clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += join_deadline_ns;
      while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
      }
      int rc = pthread_timedjoin_np(arts_thread_handles[i], NULL, &deadline);
      if (rc == 0) {
        continue;
      }
      ARTS_WARN("arts_thread_main_join: thread %u did not join within "
                "%ld ms (rc=%d), cancelling",
                i, join_deadline_ns / 1000000L, rc);
      pthread_cancel(arts_thread_handles[i]);
      atomic_fetch_add_explicit(&arts_shutdown_abandon.cancelled_threads, 1u,
                                memory_order_relaxed);
      (void)clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += cancel_deadline_ns;
      while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
      }
      rc = pthread_timedjoin_np(arts_thread_handles[i], NULL, &deadline);
      if (rc != 0) {
        ARTS_WARN("arts_thread_main_join: thread %u did not join after "
                  "cancel (rc=%d); leaking and continuing",
                  i, rc);
        if (arts_shutdown_abandon.unjoined == NULL) {
          arts_shutdown_abandon.unjoined = (bool *)arts_calloc(
              arts_node_info.total_thread_count, sizeof(bool));
        }
        arts_shutdown_abandon.unjoined[i] = true;
        atomic_fetch_add_explicit(&arts_shutdown_abandon.unjoined_threads, 1u,
                                  memory_order_relaxed);
      }
    }
  }
  arts_runtime_global_cleanup();
  // arts_free(args);
  /* A thread never joined reads its own entry after the teardown barrier, so
   * the table is left to the process exit. */
  if (arts_shutdown_abandon.unjoined == NULL) {
    arts_free(arts_thread_masks);
  }
  arts_free(arts_shutdown_abandon.unjoined);
  arts_shutdown_abandon.unjoined = NULL;
  arts_free(arts_thread_handles);
}

void arts_thread_init(struct arts_config_s *config) {
  arts_thread_config = config;

  /* Adjust the progress-thread count now that rank_count is known: a
   * single-node run has no progress thread.  Progress threads own no
   * per-peer partition — they all reap the shared fabric completion queue —
   * so their count is not bounded by the node or port count. */
  if (arts_global_rank_count == 1) {
    config->progress_thread_count = 0;
  }
  config->worker_thread_count =
      config->thread_count - config->progress_thread_count;

  arts_thread_masks = (struct thread_mask_s *)arts_malloc(
      sizeof(*arts_thread_masks) * config->thread_count);
  /* The placement is computed BEFORE the node init that carves the memory
   * pool, so the pool can be told which NUMA nodes this rank's threads
   * actually sit on and how far apart the machine's nodes are — facts it
   * would otherwise have to rediscover, and the second of which it cannot
   * see at all.  The facts are consumed by the init; nothing keeps them. */
  {
    struct arts_numa_facts_s *facts =
        (struct arts_numa_facts_s *)arts_malloc(sizeof(*facts));
    arts_get_thread_mask(config, arts_thread_masks, facts);
    arts_regpool_set_topology(facts->node_count, facts->rank_nodes,
                              facts->distance_known ? facts->distance : NULL);
    arts_free(facts);
  }
  arts_runtime_node_init(config);
  arts_print_mask(arts_thread_masks, config->thread_count);

  arts_thread_handles =
      (pthread_t *)arts_malloc(sizeof(pthread_t) * config->thread_count);
  arts_thread_handles[0] = pthread_self();
  unsigned int thread_count = config->thread_count;

  /* Compute page-aligned stack size (0 = use default). */
  size_t stack_size = 0;
  if (config->stack_size) {
    long page_size = sysconf(_SC_PAGESIZE);
    stack_size = ((config->stack_size % page_size > 0) +
                  (config->stack_size / page_size)) *
                 (size_t)page_size;
  }

  /* Create worker and progress threads with optional pinning. */
  for (unsigned int i = 1; i < thread_count; i++) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) {
      pthread_attr_setstacksize(&attr, stack_size);
    }
#ifndef __APPLE__
    if (config->pin_threads) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(arts_thread_masks[i].pu_id, &set);
      pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &set);
    }
#endif
    /* A worker slot that was never created is fatal, not degraded service:
     * the slot is joined at shutdown and its share of the placement is
     * silently missing from the run. */
    int rc = pthread_create(&arts_thread_handles[i], &attr, &arts_thread_loop,
                            &arts_thread_masks[i]);
    if (rc != 0) {
      ARTS_ERROR("thread %u: pthread_create failed: %s", i, strerror(rc));
    }
    pthread_attr_destroy(&attr);
  }

  /* Pin main thread (thread 0). */
#ifndef __APPLE__
  if (config->pin_threads) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(arts_thread_masks[0].pu_id, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
    if (rc != 0) {
      ARTS_ERROR("thread 0: pinning to pu %u failed: %s",
                 arts_thread_masks[0].pu_id, strerror(rc));
    }
  }
#endif
  arts_runtime_private_init(&arts_thread_masks[0], config);
}
