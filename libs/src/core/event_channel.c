/* SPDX-License-Identifier: Apache-2.0
 *
 * The channel event's gate: the g-th satisfy is delivered to the g-th
 * dependence, by exactly one drainer at a time.
 *
 * A satisfy pushes its node on data_queue and adds one to the word's `sat`
 * field; a dependence pushes on dep_queue and adds one to `deps`.  The add
 * whose result makes the word pairable (both fields non-zero) claims the
 * drain.  The drainer pops min(sat, deps) pairs of its view, signals each,
 * and subtracts them in one fetch_sub, whose result decides whether it keeps
 * the drain.  Invariant: the word is pairable exactly while one thread holds
 * the drain, so no pairable word is left with nobody draining it and no two
 * threads drain at once.  An add made during a drain only raises the fields
 * (it finds the word pairable and leaves), so the drainer's subtraction never
 * underflows, and a satisfy issued from inside the drain to the same channel
 * is served by that drain rather than recursing into it.
 *
 * Every node is counted after its push, so whenever the drainer's view counts
 * a node one is queued; a pop that still returns NULL is a producer between
 * its exchange and its link store (arts/utils/mpsc.h), and the drainer waits
 * for that one store.  Each queue is FIFO in exchange order, so the pairing
 * is the g-th exchange on one with the g-th on the other.
 *
 * event.c and a whitebox unit include this file; of the runtime it calls only
 * arts_runtime_idle_pause.  The includer defines, before the include:
 *   event_signal_one(dep, data)  deliver one pair
 *   event_node_free(node)        free a popped node
 *   EVENT_CHANNEL_FUZZ(site)     a test seam at one of the sites below
 *   EVENT_CHANNEL_LIMIT(cur)     a field of `cur`, the result of a producer's
 *                                add, has reached 2^31; fatal outside a test
 * Not a standalone TU.
 */

#include "arts/event.h"
#include "arts/system/threads.h" /* arts_runtime_idle_pause */
#include "arts/utils/mpsc.h"

#include <stdbool.h>
#include <stdint.h>

#define CH_SAT_ONE ((uint64_t)1)
#define CH_DEP_ONE ((uint64_t)1 << 32)
#define CH_SAT(w) ((uint32_t)(w))
#define CH_DEPS(w) ((uint32_t)((w) >> 32))
#define CH_PAIRABLE(w) (CH_SAT(w) != 0u && CH_DEPS(w) != 0u)
/* Each add raises a field by one, so the check at 2^31 fires long before a
 * carry could cross into the other field. */
#define CH_LIMIT (((uint64_t)1 << 31) | ((uint64_t)1 << 63))

enum {
  EVENT_CHANNEL_FUZZ_LINK,    /* inside the push's link window */
  EVENT_CHANNEL_FUZZ_COUNT,   /* pushed, not yet counted */
  EVENT_CHANNEL_FUZZ_RELEASE, /* a batch served, not yet subtracted */
};

/* The caller's count proves a node queued, so a NULL pop is a producer
 * between its exchange and its link store; wait for that store. */
static arts_lf_link_t *channel_take(arts_mpsc_t *q) {
  arts_lf_link_t *n;
  while ((n = arts_mpsc_pop(q)) == NULL) {
    arts_runtime_idle_pause();
  }
  return n;
}

static void channel_drain(struct arts_event_s *e, uint64_t cur) {
  for (;;) {
    uint32_t n = CH_SAT(cur) < CH_DEPS(cur) ? CH_SAT(cur) : CH_DEPS(cur);
    for (uint32_t i = 0; i < n; i++) {
      arts_lf_link_t *d = channel_take(&e->channel.data_queue);
      arts_lf_link_t *p = channel_take(&e->channel.dep_queue);
      event_signal_one((struct arts_event_dep_s *)p,
                       ((struct arts_event_dep_s *)d)->target);
      event_node_free(d);
      event_node_free(p);
    }
    EVENT_CHANNEL_FUZZ(EVENT_CHANNEL_FUZZ_RELEASE);
    uint64_t k = (uint64_t)n * (CH_SAT_ONE + CH_DEP_ONE);
    /* Same-word pairing with channel_produce's fetch_add: whichever RMW of
     * the word is later reads the other's write.  The adds made since this
     * drainer's view are in its result, and the drain continues iff the
     * result is still pairable; an add after it reads that result and claims
     * iff it makes the word pairable.  The release hands the queues' tails
     * and this batch's frees to that claimer; the acquire, when the drain
     * continues, takes the pushes counted since the view. */
    uint64_t was =
        atomic_fetch_sub_explicit(&e->channel.state, k, memory_order_acq_rel);
    cur = was - k;
    if (!CH_PAIRABLE(cur)) {
      return;
    }
  }
}

/* A satisfy passes data_queue and CH_SAT_ONE, a dependence dep_queue and
 * CH_DEP_ONE. */
static void channel_produce(struct arts_event_s *e, arts_mpsc_t *q,
                            struct arts_event_dep_s *node, uint64_t one) {
  arts_lf_link_t *prev = arts_mpsc_push_begin(q, &node->link);
  EVENT_CHANNEL_FUZZ(EVENT_CHANNEL_FUZZ_LINK);
  arts_mpsc_push_end(prev, &node->link);
  EVENT_CHANNEL_FUZZ(EVENT_CHANNEL_FUZZ_COUNT);
  /* Same-word pairing with channel_drain's fetch_sub: whichever RMW of the
   * word is later reads the other's write, so this add either lands in a
   * running drain, whose next view counts this node, or lands on an
   * unpairable word and claims iff its result is pairable.  The release
   * carries this node's push to the drainer that counts it; the acquire, on a
   * claim, takes the previous drainer's tails and frees. */
  uint64_t old =
      atomic_fetch_add_explicit(&e->channel.state, one, memory_order_acq_rel);
  uint64_t cur = old + one;
  if (cur & CH_LIMIT) {
    EVENT_CHANNEL_LIMIT(cur);
  }
  if (!CH_PAIRABLE(old) && CH_PAIRABLE(cur)) {
    channel_drain(e, cur);
  }
}

/* Whether a channel nobody can reach any more is at rest: no drain held and
 * each field equal to the nodes left on its queue. */
static bool channel_quiescent(struct arts_event_s *e, uint64_t ndata,
                              uint64_t ndeps) {
  /* Relaxed, weaker than the word's RMWs: the caller runs after every
   * handler of the event, ordered by its reference count. */
  uint64_t w = atomic_load_explicit(&e->channel.state, memory_order_relaxed);
  return !CH_PAIRABLE(w) && CH_SAT(w) == ndata && CH_DEPS(w) == ndeps;
}
