/* SPDX-License-Identifier: Apache-2.0
 *
 * event_channel_gate — the channel event's gate (event_channel.c) driven
 * without a runtime.
 *
 * The unit includes the gate and supplies its four hooks: the signal records
 * each pair in bookkeeping that only the drain's holder touches, plainly, so
 * ThreadSanitizer judges whether the word's RMWs hand the drain from one
 * holder to the next; a free is free(); the fuzz sites are this file's own
 * stalls; and the limit hook counts its hits and returns.
 *
 *  (a) A dependence producer stopped inside its push's link window while
 *      another dependence is counted: (a1) behind that dependence, so the
 *      drainer needs the link the stopped producer stores into it; (a2) in
 *      front of it, so the drainer needs the queue's first link.  The
 *      satisfy that claims the drain neither returns nor delivers a pair
 *      until the producer is released; then the pairs come out in exchange
 *      order.
 *  (b) A dependence whose delivery satisfies the same channel: the nested add
 *      finds the drain held and leaves, the holder serves it, and no drain
 *      runs inside another.
 *  (c) Satisfies and dependences from several threads, with widened windows
 *      at the three fuzz sites and one dependence in 16 nested: each
 *      producer's satisfies and dependences are consumed in its own order,
 *      and every node is delivered exactly once or left queued.
 *  (d) After each case the word, at rest, counts exactly the nodes left on
 *      each queue: the check the deleter asserts.
 *  (e) A field at 2^31 - 1: one more add reaches the limit hook exactly once
 *      and leaves the other field unchanged.
 */

#include "arts/event.h"
#include "arts/system/threads.h"
#include "arts/utils/mpsc.h"
#include "arts/utils/stdatomic_compat.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NTHREADS 4
#if defined(__SANITIZE_THREAD__)
#define STORM_OPS 20000
#else
#define STORM_OPS 100000
#endif
#define NESTED_TID NTHREADS /* the pseudo-producer of nested satisfies */
#define STALL_CHECK_MS 100

static int failed;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL event_channel_gate: " __VA_ARGS__);                         \
      printf("\n");                                                            \
      failed = 1;                                                              \
    }                                                                          \
  } while (0)

/* ---- the gate's hooks ---------------------------------------------------- */

static void event_signal_one(struct arts_event_dep_s *dep, arts_guid_t data);
static void unit_fuzz(int site);
static void unit_limit(uint64_t cur);
static inline void event_node_free(arts_lf_link_t *node) { free(node); }
#define EVENT_CHANNEL_FUZZ(site) unit_fuzz(site)
#define EVENT_CHANNEL_LIMIT(cur) unit_limit(cur)

#include "event_channel.c"

/* A node's identity: its producer and that producer's sequence on its queue,
 * carried in `target`; a dependence's `slot` says whether its delivery
 * satisfies the channel again. */
#define ID(tid, seq) (((uint64_t)(tid) << 32) | (uint32_t)(seq))
#define ID_TID(id) ((uint32_t)((id) >> 32))
#define ID_SEQ(id) ((uint32_t)(id))

static struct arts_event_s ch;

/* Written only by the thread that holds the drain. */
static uint32_t next_data_seq[NTHREADS + 1];
static uint32_t next_dep_seq[NTHREADS];
static uint32_t nested_issued;
static uint64_t pairs;
static int signal_depth;
#define LOG_MAX 8
static uint64_t log_data[LOG_MAX], log_dep[LOG_MAX];
static bool check_fifo; /* set while no thread runs */

/* What a thread outside the drain may observe of it. */
static _Atomic(uint64_t) delivered;

static struct arts_event_dep_s *node_new(uint64_t id, uint32_t nested) {
  struct arts_event_dep_s *n = calloc(1, sizeof(*n));
  if (!n) {
    printf("FAIL event_channel_gate: out of memory\n");
    exit(1);
  }
  n->target = (arts_guid_t)id;
  n->slot = nested;
  return n;
}

static void produce_sat(uint64_t id) {
  channel_produce(&ch, &ch.channel.data_queue, node_new(id, 0), CH_SAT_ONE);
}

static void produce_dep(uint64_t id, uint32_t nested) {
  channel_produce(&ch, &ch.channel.dep_queue, node_new(id, nested), CH_DEP_ONE);
}

static void event_signal_one(struct arts_event_dep_s *dep, arts_guid_t data) {
  CHECK(signal_depth == 0, "a drain ran inside another drain's delivery");
  signal_depth++;
  uint64_t d = (uint64_t)data, p = (uint64_t)dep->target;
  if (pairs < LOG_MAX) {
    log_data[pairs] = d;
    log_dep[pairs] = p;
  }
  if (check_fifo) {
    CHECK(ID_SEQ(d) == next_data_seq[ID_TID(d)],
          "producer %u's satisfy %u delivered, expected %u", ID_TID(d),
          ID_SEQ(d), next_data_seq[ID_TID(d)]);
    next_data_seq[ID_TID(d)] = ID_SEQ(d) + 1;
    CHECK(ID_SEQ(p) == next_dep_seq[ID_TID(p)],
          "producer %u's dependence %u served, expected %u", ID_TID(p),
          ID_SEQ(p), next_dep_seq[ID_TID(p)]);
    next_dep_seq[ID_TID(p)] = ID_SEQ(p) + 1;
  }
  pairs++;
  atomic_fetch_add_explicit(&delivered, 1u, memory_order_relaxed);
  if (dep->slot) {
    produce_sat(ID(NESTED_TID, nested_issued++));
  }
  signal_depth--;
}

/* ---- fuzz sites ---------------------------------------------------------- */

static _Thread_local bool tl_stall_link; /* stop inside the link window */
static _Thread_local uint64_t tl_rng;    /* widen windows when non-zero */
static _Atomic(int) stalled, release_stall;

static uint32_t rnd(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return (uint32_t)*s;
}

static void unit_fuzz(int site) {
  if (site == EVENT_CHANNEL_FUZZ_LINK && tl_stall_link) {
    atomic_store_explicit(&stalled, 1, memory_order_release);
    while (!atomic_load_explicit(&release_stall, memory_order_acquire)) {
      arts_runtime_idle_pause();
    }
    return;
  }
  if (tl_rng != 0 && rnd(&tl_rng) % 10u == 0u) {
    for (uint32_t i = 1u << (rnd(&tl_rng) % 11u); i > 0; i--) {
      arts_runtime_idle_pause();
    }
  }
}

static uint32_t limit_hits;
static uint64_t limit_cur;

static void unit_limit(uint64_t cur) {
  limit_hits++;
  limit_cur = cur;
}

/* ---- helpers ------------------------------------------------------------- */

static void channel_reset(void) {
  atomic_init(&ch.channel.state, 0u);
  arts_mpsc_init(&ch.channel.data_queue);
  arts_mpsc_init(&ch.channel.dep_queue);
  memset(next_data_seq, 0, sizeof(next_data_seq));
  memset(next_dep_seq, 0, sizeof(next_dep_seq));
  nested_issued = 0;
  pairs = 0;
  signal_depth = 0;
  atomic_store_explicit(&delivered, 0u, memory_order_relaxed);
}

static uint64_t queue_free(arts_mpsc_t *q) {
  uint64_t n = 0;
  arts_lf_link_t *chain = arts_mpsc_drain_remaining(q);
  while (chain) {
    arts_lf_link_t *next =
        atomic_load_explicit(&chain->next, memory_order_relaxed);
    free(chain);
    chain = next;
    n++;
  }
  return n;
}

/* (d): the queues hold exactly what the word counts; frees them. */
static void check_rest(const char *what, uint64_t want_data,
                       uint64_t want_deps) {
  uint64_t nd = queue_free(&ch.channel.data_queue);
  uint64_t np = queue_free(&ch.channel.dep_queue);
  CHECK(nd == want_data && np == want_deps,
        "%s: %" PRIu64 "/%" PRIu64 " nodes left, expected %" PRIu64 "/%" PRIu64,
        what, nd, np, want_data, want_deps);
  CHECK(channel_quiescent(&ch, nd, np),
        "%s: the word at rest does not count the %" PRIu64 "/%" PRIu64
        " nodes left",
        what, nd, np);
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ---- (a) a producer inside its link window ------------------------------- */

static _Atomic(int) satisfier_done;

static void *stalled_dep(void *arg) {
  (void)arg;
  tl_stall_link = true;
  produce_dep(ID(1, 0), 0);
  return NULL;
}

static void *claiming_sat(void *arg) {
  (void)arg;
  produce_sat(ID(2, 0));
  atomic_store_explicit(&satisfier_done, 1, memory_order_release);
  return NULL;
}

static void wait_stalled(void) {
  while (!atomic_load_explicit(&stalled, memory_order_acquire)) {
    arts_runtime_idle_pause();
  }
}

static void case_window(bool front) {
  const char *what = front ? "(a2) front node" : "(a1) successor";
  channel_reset();
  check_fifo = false;
  atomic_store_explicit(&stalled, 0, memory_order_relaxed);
  atomic_store_explicit(&release_stall, 0, memory_order_relaxed);
  atomic_store_explicit(&satisfier_done, 0, memory_order_relaxed);
  pthread_t x, s;
  if (front) {
    /* X exchanges first, so its node is behind the stub's unstored link;
     * the counted dependence is pushed behind X. */
    pthread_create(&x, NULL, stalled_dep, NULL);
    wait_stalled();
    produce_dep(ID(0, 0), 0);
  } else {
    /* The counted dependence is linked first; X exchanges behind it. */
    produce_dep(ID(0, 0), 0);
    pthread_create(&x, NULL, stalled_dep, NULL);
    wait_stalled();
  }
  pthread_create(&s, NULL, claiming_sat, NULL);
  /* Only the satisfy's add can make the word pairable here (the stopped
   * dependence is not counted yet), so once it is the satisfier holds the
   * drain and is waiting on the unstored link. */
  while (!CH_PAIRABLE(
      atomic_load_explicit(&ch.channel.state, memory_order_acquire))) {
    arts_runtime_idle_pause();
  }
  sleep_ms(STALL_CHECK_MS);
  CHECK(!atomic_load_explicit(&satisfier_done, memory_order_acquire),
        "%s: the claiming satisfy returned while a producer was in its "
        "link window",
        what);
  CHECK(atomic_load_explicit(&delivered, memory_order_relaxed) == 0,
        "%s: a pair was delivered while the link it needs was unstored", what);
  atomic_store_explicit(&release_stall, 1, memory_order_release);
  pthread_join(x, NULL);
  pthread_join(s, NULL);
  CHECK(pairs == 1, "%s: %" PRIu64 " pairs after the release, expected 1", what,
        pairs);
  produce_sat(ID(2, 1));
  CHECK(pairs == 2, "%s: %" PRIu64 " pairs after the second satisfy", what,
        pairs);
  /* Pairing follows exchange order on each queue. */
  uint64_t first = front ? ID(1, 0) : ID(0, 0);
  uint64_t second = front ? ID(0, 0) : ID(1, 0);
  CHECK(log_data[0] == ID(2, 0) && log_dep[0] == first &&
            log_data[1] == ID(2, 1) && log_dep[1] == second,
        "%s: pairs out of exchange order", what);
  check_rest(what, 0, 0);
}

/* ---- (b) a nested satisfy of the same channel ---------------------------- */

static void case_nested(void) {
  channel_reset();
  check_fifo = true;
  /* The nested satisfy pairs with a second, already queued dependence. */
  produce_dep(ID(0, 0), 1);
  produce_dep(ID(0, 1), 0);
  produce_sat(ID(1, 0));
  CHECK(pairs == 2 && log_dep[0] == ID(0, 0) &&
            log_data[1] == ID(NESTED_TID, 0) && log_dep[1] == ID(0, 1),
        "(b) nested satisfy not served by the holder's next batch");
  check_rest("(b) nested, served", 0, 0);

  /* The nested satisfy finds nothing to pair with and stays queued. */
  channel_reset();
  produce_dep(ID(0, 0), 1);
  produce_sat(ID(1, 0));
  CHECK(pairs == 1, "(b) %" PRIu64 " pairs, expected 1", pairs);
  check_rest("(b) nested, left queued", 1, 0);
}

/* ---- (c) a storm --------------------------------------------------------- */

static uint32_t issued_data[NTHREADS], issued_dep[NTHREADS];

static void *storm_thread(void *arg) {
  int t = (int)(intptr_t)arg;
  tl_rng = 0x9E3779B97F4A7C15ull * (uint64_t)(t + 1);
  uint64_t rs = 0xD1B54A32D192ED03ull * (uint64_t)(t + 1);
  for (int i = 0; i < STORM_OPS; i++) {
    if (rnd(&rs) & 1u) {
      produce_dep(ID(t, issued_dep[t]++), rnd(&rs) % 16u == 0u);
    } else {
      produce_sat(ID(t, issued_data[t]++));
    }
  }
  return NULL;
}

static void case_storm(void) {
  channel_reset();
  check_fifo = true;
  memset(issued_data, 0, sizeof(issued_data));
  memset(issued_dep, 0, sizeof(issued_dep));
  pthread_t th[NTHREADS];
  for (int t = 0; t < NTHREADS; t++) {
    pthread_create(&th[t], NULL, storm_thread, (void *)(intptr_t)t);
  }
  for (int t = 0; t < NTHREADS; t++) {
    pthread_join(th[t], NULL);
  }
  uint64_t nd = nested_issued, np = 0;
  for (int t = 0; t < NTHREADS; t++) {
    nd += issued_data[t];
    np += issued_dep[t];
  }
  uint64_t want = nd < np ? nd : np;
  CHECK(pairs == want,
        "(c) %" PRIu64 " pairs of %" PRIu64 " satisfies and %" PRIu64
        " dependences",
        pairs, nd, np);
  check_rest("(c) storm", nd - pairs, np - pairs);
  printf("(c) %d threads: %" PRIu64 " satisfies (%u nested), %" PRIu64
         " dependences, %" PRIu64 " pairs\n",
         NTHREADS, nd, nested_issued, np, pairs);
}

/* ---- (e) the field limit ------------------------------------------------- */

static void case_limit(void) {
  const uint64_t below = ((uint64_t)1 << 31) - 1u;
  channel_reset();
  limit_hits = 0;
  atomic_store_explicit(&ch.channel.state, below, memory_order_relaxed);
  produce_sat(ID(0, 0));
  uint64_t w = atomic_load_explicit(&ch.channel.state, memory_order_relaxed);
  CHECK(limit_hits == 1 && CH_SAT(limit_cur) == (uint32_t)1u << 31 &&
            CH_DEPS(w) == 0u,
        "(e) satisfy limit: %u hits, word %#" PRIx64, limit_hits, w);
  atomic_store_explicit(&ch.channel.state, 0u, memory_order_relaxed);
  CHECK(queue_free(&ch.channel.data_queue) == 1, "(e) satisfy node lost");

  channel_reset();
  limit_hits = 0;
  atomic_store_explicit(&ch.channel.state, below << 32, memory_order_relaxed);
  produce_dep(ID(0, 0), 0);
  w = atomic_load_explicit(&ch.channel.state, memory_order_relaxed);
  CHECK(limit_hits == 1 && CH_DEPS(limit_cur) == (uint32_t)1u << 31 &&
            CH_SAT(w) == 0u,
        "(e) dependence limit: %u hits, word %#" PRIx64, limit_hits, w);
  CHECK(queue_free(&ch.channel.dep_queue) == 1, "(e) dependence node lost");
}

int main(void) {
  case_window(false);
  case_window(true);
  case_nested();
  case_storm();
  case_limit();
  if (failed) {
    return 1;
  }
  printf("PASS event_channel_gate: link-window wait, nested satisfy, %d-thread "
         "storm, rest counts, field limit\n",
         NTHREADS);
  return 0;
}
