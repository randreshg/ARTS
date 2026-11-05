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

#include <pthread.h>
#include <unistd.h>
#ifndef __APPLE__
#include <sched.h>
#endif

#include "arts/counter/counter.h"
#include "arts/db.h"             /* arts_db_resume_list_free */
#include "arts/edt_context.h"    /* arts_cleanup_edt_tls */
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

/* A thread's final counts, read by the counter writers only once they have
 * acquired the thread's stop. */
static void arts_thread_save_counters(unsigned int thread_id) {
  uint64_t *saved = arts_node_info.saved_counters[thread_id];
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    saved[i] = atomic_load_explicit(&arts_thread_local_counters[i].count,
                                    memory_order_relaxed);
  }
}

void *arts_thread_loop(void *data) {
  struct thread_mask_s *thread = (struct thread_mask_s *)data;
  arts_runtime_private_init(thread, arts_thread_config);
  arts_runtime_loop();
  arts_thread_save_counters(thread->id);
  arts_runtime_thread_park();
}

/*
 * arts_thread_main_loop — Main thread (thread 0) entry after initialization.
 *
 * Runs the runtime loop until alive==false, then stops every other runtime
 * thread and, once all have stopped, tears the node down.  No thread is
 * joined: a stopped thread parks for the rest of the process.
 */
void arts_thread_main_loop() {
  ARTS_DEBUG("arts_thread_main_loop: main thread entering runtime_loop");
  arts_runtime_loop();
  ARTS_DEBUG("arts_thread_main_loop: main thread exited runtime_loop, "
             "stopping %u threads",
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

  /* The capture thread reads every runtime thread's counters in that
   * thread's own storage, which a stopped thread keeps, and is joined before
   * any thread's saved counts are read. */
  arts_counter_capture_stop();
  arts_cleanup_edt_tls();
  arts_db_resume_list_free();
  arts_thread_save_counters(0);

  /* A thread that leaves its loop saves its counts and parks.  The counter
   * files are written either way; a thread that did not stop is left out of
   * them. */
  struct arts_stop_result_s stop =
      arts_runtime_stop_threads(arts_thread_stop_slot);
  for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
    arts_counter_write(arts_node_info.counter_folder, arts_global_rank_id, t);
  }
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
    arts_regpool_report_if_requested();
    return;
  }
  arts_runtime_delete_deques();
  arts_runtime_global_cleanup();
  arts_free(arts_thread_masks);
  arts_thread_masks = NULL;
  arts_free(arts_thread_handles);
  arts_thread_handles = NULL;
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
     * its stop never comes at shutdown and its share of the placement is
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
