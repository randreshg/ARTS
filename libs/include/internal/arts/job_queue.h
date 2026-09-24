/* SPDX-License-Identifier: Apache-2.0
 *
 * Runtime-internal work handed to a worker.  A job is not an EDT, so it cannot
 * ride a deque, and it is not a message, so it does not belong on the
 * loopback: it is a body the runtime wants run somewhere other than where it
 * was decided. */
#ifndef ARTS_JOB_QUEUE_H
#define ARTS_JOB_QUEUE_H

#ifdef ARTS_FAM

#include "arts/defs.h"
#include "arts/utils/lockfree_lifo.h"

#include <stdbool.h>

/* The link is FIRST (the Treiber-stack contract), and the node is freed by the
 * drainer that consumed it -- never pushed back onto the same stack, which is
 * what keeps the stack ABA-free under allocator address reuse.  The job owns
 * nothing: whatever `arg` points at is released by whichever of the two hooks
 * runs.  Exactly one of them runs for every posted job: `fn` does the work,
 * `discard` releases `arg` without doing it and is what a job still queued at
 * teardown gets instead.  Both are required -- a job whose work owns nothing
 * still needs a discard that frees `arg`. */
struct arts_job_s {
  arts_lf_link_t link;
  void (*fn)(void *arg);
  void (*discard)(void *arg);
  void *arg;
};

/* One stack per thread slot, each on its own cache line: a poster touches a
 * worker's head from another thread, and two workers' heads must not share a
 * line. */
struct arts_job_queue_s {
  arts_lf_stack_t stack;
} ARTS_ALIGNED_MAX;

/* Hand a job to one of this rank's WORKERS, round-robin.  Callable from any
 * thread, a progress thread included.  The job is consumed exactly once, on a
 * worker, and may not assume WHICH one -- nor that it runs at all, since a
 * runtime torn down before it is reached discards it instead. */
void arts_job_post(void (*fn)(void *), void (*discard)(void *), void *arg);

/* Run everything handed to the calling worker; false when there was nothing.
 * The whole chain is taken in ONE exchange (FIFO within the batch), so a slot
 * has one consumer and no CAS ever reads a node's next pointer -- the stack's
 * no-ABA precondition, and the reason a one-node pop is not used here. */
bool arts_job_queue_drain(void);

/* Per-thread lifecycle, from the runtime's private init / cleanup.  Both
 * consumers free the JOBS they take; the per-thread arrays belong to the
 * runtime's global cleanup, so nothing here is named "free". */
void arts_job_queue_private_init(void);
void arts_job_queue_private_drain(void);

#endif /* ARTS_FAM */
#endif /* ARTS_JOB_QUEUE_H */
