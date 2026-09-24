/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/job_queue.h"

#ifdef ARTS_FAM

#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

#include <stdatomic.h>

void arts_job_post(void (*fn)(void *), void (*discard)(void *), void *arg) {
  struct arts_job_s *j = (struct arts_job_s *)arts_malloc(sizeof(*j));
  j->fn = fn;
  j->discard = discard;
  j->arg = arg;
  unsigned int k = arts_atomic_fetch_add(&arts_node_info.job_rr, 1U) %
                   arts_node_info.worker_thread_count;
  arts_lf_stack_push(
      &arts_node_info.job_queue[arts_node_info.worker_ids[k]].stack, &j->link);
}

/* Take the slot's whole chain in one exchange and consume it, running each
 * job or discarding it.  The caller owns this slot's consumer end, so the
 * chain cannot be taken from under it and every node is freed here. */
static void job_queue_consume(arts_lf_stack_t *s, bool run) {
  arts_lf_link_t *head = arts_lf_stack_reverse_drain(s);
  while (head != NULL) {
    arts_lf_link_t *next =
        atomic_load_explicit(&head->next, memory_order_relaxed);
    struct arts_job_s *j = (struct arts_job_s *)head;
    if (run) {
      j->fn(j->arg);
    } else {
      __atomic_fetch_add(&arts_shutdown_abandon.jobs, 1u, __ATOMIC_RELAXED);
      j->discard(j->arg);
    }
    arts_free(j);
    head = next;
  }
}

bool arts_job_queue_drain(void) {
  if (arts_thread_info.role != ARTS_ROLE_WORKER) {
    return false; /* only a worker owns a slot's consumer end */
  }
  arts_lf_stack_t *s =
      &arts_node_info.job_queue[arts_thread_info.thread_id].stack;
  /* Read-only empty gate, as on the self-loopback: every scheduler iteration
   * reaches this, so the nothing-to-do case must not write the head line.  An
   * unconditional exchange takes it exclusive on every pass and ping-pongs it
   * against a poster on another core even while no job exists.  Non-empty here
   * means the exchange below returns at least that node: this slot has one
   * consumer, and nothing but a push can change the head. */
  if (arts_lf_stack_empty(s)) {
    return false;
  }
  job_queue_consume(s, /*run=*/true);
  return true;
}

void arts_job_queue_private_init(void) {
  arts_lf_stack_init(
      &arts_node_info.job_queue[arts_thread_info.thread_id].stack);
  if (arts_thread_info.role == ARTS_ROLE_WORKER) {
    arts_node_info.worker_ids[arts_thread_info.group_pos] =
        arts_thread_info.thread_id;
  }
}

void arts_job_queue_private_drain(void) {
  /* After the cleanup barrier every thread has left its loop, so each slot
   * still has exactly one consumer and no poster remains.  Whatever is left is
   * DISCARDED, never run: the work a queued job stands for belongs to a turn
   * that is already lost, and doing it here would register state the edge that
   * would release it can no longer reach.  Discarding releases what each job
   * holds and leaves the rest of the teardown to report the lost turn.
   *
   * Every thread drains its OWN slot, whatever its role: at this point a slot
   * has one consumer by construction, so a slot that was never posted to costs
   * one empty exchange and nothing is left behind. */
  job_queue_consume(&arts_node_info.job_queue[arts_thread_info.thread_id].stack,
                    /*run=*/false);
}

#else
/* The facility exists only where a runtime-internal job has a poster; ISO C
 * requires a translation unit to declare something. */
typedef int arts_job_queue_unused_t;
#endif /* ARTS_FAM */
