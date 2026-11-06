/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_thread_init worker-count derivation after the transport cutover
 * + config-surface cleanup (libs/src/core/system/threads.c).  This TU
 * #includes threads.c so the real derivation code runs; the runtime
 * collaborators arts_thread_init calls (arts_get_thread_mask,
 * arts_runtime_node_init, arts_print_mask, private_init/loop/park,
 * arts_malloc/calloc/free, ARTS_ERROR->arts_exit) are stubbed so the unit
 * links standalone and we can inspect the derived counts WITHOUT the full
 * runtime.
 *
 * Property under test
 * -------------------
 * arts_thread_init derives the role split:
 *   rank_count == 1  =>  progress = 0, worker = thread_count.
 *   rank_count  > 1  =>  worker = thread_count - progress.
 * The subtraction is unsigned, so it relies on progress <= thread_count, which
 * the configuration loader enforces: it refuses a progress count that leaves
 * the rank no worker before any thread is derived.
 *
 * Test strategy
 * -------------
 * 1. rank_count==1 case: assert progress==0 and worker==thread_count.
 * 2. rank_count>1, progress==thread_count: a boundary the loader never lets
 *    through; the derivation alone still gives worker==0, not a wrap.
 *
 * The stubbed arts_get_thread_mask does nothing and the spawn loop is bounded
 * by thread_count (small), so the derived counts are read without starting any
 * thread.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/system/shutdown.h"
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- runtime collaborator stubs (declared by the headers threads.c pulls) -
 */

unsigned int
    arts_global_rank_id_unused; /* not used; real one is in threads.c */

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin);
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  fprintf(stderr, "arts_abort(%u)\n", (unsigned)rc);
  _exit(rc ? rc : 1);
}

void *arts_malloc(size_t n);
void *arts_malloc(size_t n) { return calloc(1, n ? n : 1); }
void *arts_calloc(size_t nmemb, size_t size);
void *arts_calloc(size_t nmemb, size_t size) {
  return calloc(nmemb ? nmemb : 1, size ? size : 1);
}
void arts_free(void *p);
void arts_free(void *p) { free(p); }

/* Pull in the real threads.c (defines arts_thread_init, the globals, etc.). */
#include "../../libs/src/core/system/threads.c"

/* ---- stubs for the symbols threads.c references at link time --------------
 */

ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
ARTS_THREAD_LOCAL_IE struct arts_thread_stop_s *arts_thread_stop_slot;
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE arts_counter_t
    arts_thread_local_counters[NUM_COUNTER_TYPES];

void arts_get_thread_mask(struct arts_config_s *config,
                          struct thread_mask_s *flat,
                          struct arts_numa_facts_s *facts) {
  /* Inert: zero the flat array so the (bounded) spawn loop reads valid pu_ids,
   * and report a one-node machine so the pool's topology hand-off has
   * something consistent to consume.  Deliberately does NOT touch
   * worker_thread_count — the derivation under test already set it. */
  if (facts != NULL) {
    memset(facts, 0, sizeof(*facts));
    facts->node_count = 1;
    facts->rank_nodes = 1;
  }
  for (unsigned int t = 0; t < config->thread_count; t++) {
    flat[t].id = t;
    flat[t].pu_id = 0;
    flat[t].role = ARTS_ROLE_WORKER;
    flat[t].group_pos = 0;
    flat[t].pin = false;
  }
}
void arts_print_mask(struct thread_mask_s *t, unsigned int n) {
  (void)t;
  (void)n;
}
void arts_runtime_node_init(struct arts_config_s *c) { (void)c; }
void arts_regpool_set_topology(unsigned int node_count, uint64_t node_set,
                               const uint32_t *distance) {
  /* No pool in this harness: the hand-off only has to link. */
  (void)node_count;
  (void)node_set;
  (void)distance;
}
void arts_runtime_private_init(struct thread_mask_s *m,
                               struct arts_config_s *c) {
  (void)m;
  (void)c;
}
int arts_runtime_loop(void) { return 0; }
/* The spawned threads end here instead of parking, so the harness can join
 * them. */
void arts_runtime_thread_park(void) { pthread_exit(NULL); }

/* ------------------------------------------------------------------------- */

static unsigned int run_derivation(unsigned int rank_count,
                                   unsigned int thread_count,
                                   unsigned int progress,
                                   unsigned int *out_progress) {
  /* Set up the global rank count the derivation reads. */
  arts_global_rank_count = rank_count;

  arts_node_info.total_thread_count = thread_count;

  struct arts_config_s c;
  memset(&c, 0, sizeof(c));
  c.thread_count = thread_count;
  c.progress_thread_count = progress;
  c.port_count = 64; /* large so max_net never trips for our small counts */
  c.stack_size = 0;
  c.pin_threads = false;

  arts_thread_init(&c);

  unsigned int worker = c.worker_thread_count;
  if (out_progress) {
    *out_progress = c.progress_thread_count;
  }

  /* Join the worker threads arts_thread_init spawned (indices 1..tc-1) so the
   * stubbed loop's threads finish, then free the globals arts_thread_init
   * allocated (normally freed by the teardown, which never runs here).
   * This keeps the test leak-clean under LSan; any leak reported is then a
   * RUNTIME leak, not a test-harness artifact.  arts_thread_handles /
   * arts_thread_masks are globals of the included threads.c. */
  for (unsigned int i = 1; i < thread_count; i++) {
    pthread_join(arts_thread_handles[i], NULL);
  }
  arts_free(arts_thread_masks);
  arts_free(arts_thread_handles);
  arts_thread_masks = NULL;
  arts_thread_handles = NULL;

  return worker;
}

int main(void) {
  /* ---- Case 1: rank_count==1 forces progress to 0, worker=thread_count ----
   */
  {
    unsigned int p = 999;
    unsigned int w = run_derivation(/*rank_count=*/1, /*tc=*/4, /*progress=*/2,
                                    &p);
    if (p != 0) {
      fprintf(stderr,
              "FAIL threads_worker_underflow: rank1 did not zero progress "
              "threads (progress=%u)\n",
              p);
      return 1;
    }
    if (w != 4) {
      fprintf(stderr,
              "FAIL threads_worker_underflow: rank1 worker=%u, want 4\n", w);
      return 1;
    }
  }

  /* ---- Case 2: rank>1, progress == thread_count => worker == 0, the
   * derivation's boundary (the configuration loader refuses the pair) ---- */
  {
    unsigned int w = run_derivation(/*rank_count=*/2, /*tc=*/4,
                                    /*progress=*/4, NULL);
    if (w != 0) {
      fprintf(stderr,
              "FAIL threads_worker_underflow: boundary worker=%u, want 0\n", w);
      return 1;
    }
  }

  printf("PASS threads_worker_underflow: rank1 zeroing + boundary verified\n");
  return 0;
}
