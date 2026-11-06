/* SPDX-License-Identifier: Apache-2.0
 *
 * net_gate_drop — the send gate of the libfabric transport (transport/net.c).
 * Once the gate is closed a send is dropped with its completion-gated free
 * run, a one-sided PUT the provider's ceiling cut into pieces stops at its
 * first dropped piece, and a send held in the ring-full spin stands down at
 * the spin's next pass.  The SHUTDOWN post asks the mesh about, and injects
 * to, only the peers this rank contacted.
 *
 *   (a) After arts_net_close_gate, arts_net_send_core runs a payload's free
 *       once and posts and queues nothing, with or without a payload.
 *   (b) A 64 KiB PUT at a 4 KiB ceiling (16 pieces) on a thread whose ring
 *       is 3 slots short of a data send's cap: pieces 1-3 queue and piece 4
 *       spins.  Another thread closes the gate: the PUT returns false and its
 *       local-done hook has not run.  Completing the ring's entries, as the
 *       quiesce does, runs the hook exactly once, at the last queued piece,
 *       and frees the piece group.  (b') The same 15 slots short: the dropped
 *       piece is the last one, and nothing is subtracted.
 *   (c) A send spinning on a full ring returns false within 1 s of the
 *       gate's close, its free run once.
 *   (p) The post skips this rank and every peer it never contacted without
 *       asking the mesh, and a contacted peer the mesh reports gone without
 *       injecting.
 *   (f) A send spinning on a full ring while every pass's sentinel probe is
 *       due: from the third probe on the sentinel reports a closed peer, and
 *       the spin tries to take the shutdown point once, with origin
 *       BACKPRESSURE, and returns false within 1 s, its free run once.
 *
 * Nothing reaches the provider and no fabric comes up: each sending thread
 * holds its own ring's drain token and the main thread holds the CQ token,
 * so a spin can neither post a queued entry nor reap.  Standalone: #includes
 * net.c (its statics) with libc-backed runtime shims and recording mesh
 * shims, a sentinel probe and check the test switches on, and a take of the
 * shutdown point that records its origin and closes the gate, as the point's
 * first taker does on a multi-rank run, and returns false, as for a point
 * another thread took, so the caller returns to its spin.  Under a sanitizer
 * a report here is a defect.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/shutdown.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

/* ---- runtime externs the module (and regpool) reference ------------------ */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  exit(rc ? rc : 1);
}

/* The piece group of the PUT under test: the first allocation of the
 * group's size after arming is watched, and its frees are counted. */
static size_t g_watch_size;
static _Atomic(bool) g_watch_armed;
static _Atomic(void *) g_watch_ptr;
static _Atomic(int) g_watch_frees;

void *arts_malloc(size_t size) {
  void *p = malloc(size);
  bool armed = true;
  if (size == g_watch_size && atomic_compare_exchange_strong_explicit(
                                  &g_watch_armed, &armed, false,
                                  memory_order_relaxed, memory_order_relaxed)) {
    atomic_store_explicit(&g_watch_ptr, p, memory_order_relaxed);
  }
  return p;
}
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void *arts_calloc_aligned(size_t n, size_t size, size_t align) {
  void *p = NULL;
  if (posix_memalign(&p, align, n * size) != 0) {
    return NULL;
  }
  memset(p, 0, n * size);
  return p;
}
void arts_free(void *ptr) {
  if (ptr != NULL &&
      ptr == atomic_load_explicit(&g_watch_ptr, memory_order_relaxed)) {
    atomic_fetch_add_explicit(&g_watch_frees, 1, memory_order_relaxed);
  }
  free(ptr);
}
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif

void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* The mesh: the ranks the post asked about, and the one it reports gone. */
static _Atomic(int) g_gone_asked[4];
static unsigned int g_gone_rank = ~0u;

bool arts_socket_armed(void) { return true; }
bool arts_socket_peer_gone(unsigned int rank) {
  if (rank < 4) {
    atomic_fetch_add_explicit(&g_gone_asked[rank], 1, memory_order_relaxed);
  }
  return rank == g_gone_rank;
}

/* Compile the module under test (its statics: g_net, the rings, net_put). */
#include "../../libs/src/core/transport/net.c"

#include "counter_stubs.h"

/* ------------------------------------------------------------------------- */

_Static_assert(sizeof(struct net_put_group_s) != sizeof(struct net_txn_s),
               "the watched group must be told apart from a txn by size");

#define DATA_CAP (ARTS_NET_RING_CAP - ARTS_NET_RING_RESERVED)
#define PUT_LEN (64u * 1024u)
#define CEILING 4096u

static _Atomic(int) g_failures;
#define FAIL(...)                                                              \
  do {                                                                         \
    printf("FAIL net_gate_drop: " __VA_ARGS__);                                \
    atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);           \
  } while (0)

static fi_addr_t g_peers[4];

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(unsigned ms) {
  struct timespec ts = {.tv_sec = ms / 1000u,
                        .tv_nsec = (long)(ms % 1000u) * 1000000L};
  nanosleep(&ts, NULL);
}

/* The sentinel probe, off but in (f): then every probe is due, and the check
 * reports a closed peer from call g_sentinel_true_from on. */
static _Atomic(bool) g_probe_on;
static int g_sentinel_true_from;
static _Atomic(int) g_sentinel_calls;
static _Atomic(uint64_t) g_sentinel_true_ns;

bool arts_socket_probe_due(uint64_t *next_ns) {
  (void)next_ns;
  return atomic_load_explicit(&g_probe_on, memory_order_relaxed);
}

bool arts_socket_sentinel_check(void) {
  int n =
      atomic_fetch_add_explicit(&g_sentinel_calls, 1, memory_order_relaxed) + 1;
  if (n < g_sentinel_true_from) {
    return false;
  }
  uint64_t none = 0;
  atomic_compare_exchange_strong_explicit(&g_sentinel_true_ns, &none, now_ns(),
                                          memory_order_relaxed,
                                          memory_order_relaxed);
  return true;
}

static _Atomic(int) g_entries;
static _Atomic(int) g_entry_origin;

bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)rc;
  atomic_store_explicit(&g_entry_origin, (int)origin, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_entries, 1, memory_order_relaxed);
  arts_net_close_gate();
  return false;
}

_Noreturn void arts_exit_resume(void) {
  printf("FAIL net_gate_drop: the end resumed after a take that failed\n");
  fflush(stdout);
  _exit(1);
}

bool arts_shutdown_taken(void) {
  return atomic_load_explicit(&g_entries, memory_order_relaxed) != 0;
}

static void count_run(void *arg) {
  atomic_fetch_add_explicit((_Atomic(int) *)arg, 1, memory_order_relaxed);
}

static void gate_open(void) {
  atomic_store_explicit(&g_net.quiescing, false, memory_order_relaxed);
}

/* This thread's ring, its drain token held and `n` inert txns queued in the
 * given class. */
static struct net_ring_s *ring_hold_fill(uint32_t n, bool is_ack) {
  struct net_ring_s *r = net_get_ring();
  atomic_store_explicit(&r->draining, 1, memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    if (!net_ring_try_enqueue(
            r, net_txn_new(0, NULL, 0, NULL, NULL, NULL, NULL), is_ack)) {
      FAIL("the ring took %u of %u inert txns\n", i, n);
      break;
    }
  }
  return r;
}

static uint32_t ring_used(struct net_ring_s *r) {
  return atomic_load_explicit(&r->tail, memory_order_acquire) -
         atomic_load_explicit(&r->head, memory_order_acquire);
}

/* Complete one queued entry from the head, as the quiesce does. */
static bool ring_complete_one(struct net_ring_s *r) {
  uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  if (h == t) {
    return false;
  }
  net_txn_complete(r->slots[h % ARTS_NET_RING_CAP]);
  atomic_store_explicit(&r->head, h + 1, memory_order_release);
  return true;
}

static void ring_release(struct net_ring_s *r) {
  while (ring_complete_one(r)) {
  }
  atomic_store_explicit(&r->draining, 0, memory_order_release);
}

/* ---- (a) ----------------------------------------------------------------- */

static char g_a_payload[64];
static _Atomic(int) g_a_frees;

static void a_free(void *p) {
  if (p == g_a_payload) {
    atomic_fetch_add_explicit(&g_a_frees, 1, memory_order_relaxed);
  }
}

static void case_a(void) {
  struct net_ring_s *r = net_get_ring();
  uint64_t posted = atomic_load_explicit(&r->posted, memory_order_relaxed);
  arts_net_close_gate();
  struct arts_msg_header_s hdr;
  arts_fill_packet_header(&hdr, sizeof(hdr) + sizeof(g_a_payload),
                          MSG_TIME_SYNC_REQUEST);
  arts_net_send_core(0, (char *)&hdr, (unsigned)sizeof(hdr), g_a_payload, 0,
                     sizeof(g_a_payload), a_free);
  arts_fill_packet_header(&hdr, sizeof(hdr), MSG_TIME_SYNC_REQUEST);
  arts_net_send_core(0, (char *)&hdr, (unsigned)sizeof(hdr), NULL, 0, 0, NULL);
  int frees = atomic_load_explicit(&g_a_frees, memory_order_relaxed);
  if (frees != 1) {
    FAIL("(a) the dropped send's free ran %d time(s), want 1\n", frees);
  }
  if (atomic_load_explicit(&r->posted, memory_order_relaxed) != posted ||
      ring_used(r) != 0) {
    FAIL("(a) a send after the gate closed was posted or queued\n");
  }
  gate_open();
}

/* ---- (b) ----------------------------------------------------------------- */

struct put_case {
  uint32_t gap;                      /* slots short of the data cap */
  _Atomic(struct net_ring_s *) ring; /* the sender's, once filled */
  _Atomic(int) hook_runs;
  bool returned;
  int hook_runs_at_return;
};

static unsigned char g_src[PUT_LEN];

static void *put_sender(void *arg) {
  struct put_case *c = (struct put_case *)arg;
  struct net_ring_s *r = ring_hold_fill(DATA_CAP - c->gap, false);
  atomic_store_explicit(&c->ring, r, memory_order_release);
  atomic_store_explicit(&g_watch_frees, 0, memory_order_relaxed);
  atomic_store_explicit(&g_watch_ptr, NULL, memory_order_relaxed);
  atomic_store_explicit(&g_watch_armed, true, memory_order_relaxed);
  c->returned = net_put(0, 0x1000, 1, 1, g_src, PUT_LEN, count_run,
                        &c->hook_runs, /*payload=*/false);
  c->hook_runs_at_return =
      atomic_load_explicit(&c->hook_runs, memory_order_relaxed);
  /* The entries ahead of the pieces, then the queued pieces: only the last
   * queued piece's completion may run the hook. */
  for (uint32_t i = 0; i < DATA_CAP - c->gap; i++) {
    ring_complete_one(r);
  }
  for (uint32_t k = 1; k <= c->gap; k++) {
    if (!ring_complete_one(r)) {
      FAIL("(b, gap %u) piece %u was not queued\n", c->gap, k);
      break;
    }
    int runs = atomic_load_explicit(&c->hook_runs, memory_order_relaxed);
    int want = k == c->gap ? 1 : 0;
    if (runs != want) {
      FAIL("(b, gap %u) the hook ran %d time(s) after queued piece %u "
           "completed, want %d\n",
           c->gap, runs, k, want);
    }
  }
  if (ring_used(r) != 0) {
    FAIL("(b, gap %u) %u entries left after the queued pieces\n", c->gap,
         ring_used(r));
  }
  ring_release(r);
  return NULL;
}

static void case_b(uint32_t gap) {
  struct put_case c;
  memset(&c, 0, sizeof(c));
  c.gap = gap;
  pthread_t t;
  pthread_create(&t, NULL, put_sender, &c);
  /* The sender spins once pieces 1..gap filled the ring to the data cap. */
  struct net_ring_s *r = NULL;
  uint64_t deadline = now_ns() + 10ull * 1000000000ull;
  while (now_ns() < deadline) {
    r = atomic_load_explicit(&c.ring, memory_order_acquire);
    if (r != NULL && ring_used(r) == DATA_CAP) {
      break;
    }
    sleep_ms(1);
  }
  if (r == NULL || ring_used(r) != DATA_CAP) {
    FAIL("(b, gap %u) the ring never reached the data cap\n", gap);
  }
  sleep_ms(50);
  arts_net_close_gate();
  pthread_join(t, NULL);
  if (c.returned) {
    FAIL("(b, gap %u) net_put returned true after its piece was dropped\n",
         gap);
  }
  if (c.hook_runs_at_return != 0) {
    FAIL("(b, gap %u) the hook ran %d time(s) before the queued pieces "
         "completed\n",
         gap, c.hook_runs_at_return);
  }
  int runs = atomic_load_explicit(&c.hook_runs, memory_order_relaxed);
  int frees = atomic_load_explicit(&g_watch_frees, memory_order_relaxed);
  if (runs != 1 || frees != 1) {
    FAIL("(b, gap %u) the hook ran %d time(s) and the group was freed %d "
         "time(s), want 1 and 1\n",
         gap, runs, frees);
  }
  atomic_store_explicit(&g_watch_ptr, NULL, memory_order_relaxed);
  gate_open();
}

/* ---- (c) ----------------------------------------------------------------- */

struct spin_case {
  _Atomic(bool) spinning;
  _Atomic(bool) done;
  _Atomic(uint64_t) closed_ns;
  _Atomic(int) frees;
  bool returned;
  uint64_t returned_ns;
};

static void *spin_sender(void *arg) {
  struct spin_case *c = (struct spin_case *)arg;
  struct net_ring_s *r = ring_hold_fill(ARTS_NET_RING_CAP, true);
  struct net_txn_s *txn =
      net_txn_new(0, NULL, 0, NULL, NULL, count_run, &c->frees);
  atomic_store_explicit(&c->spinning, true, memory_order_release);
  c->returned = net_submit(txn, /*is_ack=*/true);
  c->returned_ns = now_ns();
  ring_release(r);
  atomic_store_explicit(&c->done, true, memory_order_release);
  return NULL;
}

static void case_c(void) {
  struct spin_case c;
  memset(&c, 0, sizeof(c));
  pthread_t t;
  pthread_create(&t, NULL, spin_sender, &c);
  while (!atomic_load_explicit(&c.spinning, memory_order_acquire)) {
    sleep_ms(1);
  }
  sleep_ms(50);
  atomic_store_explicit(&c.closed_ns, now_ns(), memory_order_relaxed);
  arts_net_close_gate();
  pthread_join(t, NULL);
  uint64_t closed = atomic_load_explicit(&c.closed_ns, memory_order_relaxed);
  if (c.returned) {
    FAIL("(c) net_submit returned true after the spin read the gate "
         "closed\n");
  }
  if (c.returned_ns - closed >= 1000000000ull) {
    FAIL("(c) net_submit returned %.3f s after the gate closed\n",
         (double)(c.returned_ns - closed) / 1e9);
  }
  int frees = atomic_load_explicit(&c.frees, memory_order_relaxed);
  if (frees != 1) {
    FAIL("(c) the dropped send's free ran %d time(s), want 1\n", frees);
  }
  gate_open();
}

/* ---- (p) ----------------------------------------------------------------- */

static void asked_reset(void) {
  for (unsigned i = 0; i < 4; i++) {
    atomic_store_explicit(&g_gone_asked[i], 0, memory_order_relaxed);
  }
}

static void asked_expect(const char *what, int want0, int want1, int want2,
                         int want3) {
  int want[4] = {want0, want1, want2, want3};
  for (unsigned i = 0; i < 4; i++) {
    int got = atomic_load_explicit(&g_gone_asked[i], memory_order_relaxed);
    if (got != want[i]) {
      FAIL("(p) %s: the mesh was asked about rank %u %d time(s), want %d\n",
           what, i, got, want[i]);
    }
  }
}

static void case_p(void) {
  /* Rank 1 of 4.  No endpoint: an inject the post must not make would
   * fault. */
  arts_global_rank_id = 1;
  arts_global_rank_count = 4;
  _Atomic(uint8_t) contacted[4];
  for (unsigned i = 0; i < 4; i++) {
    atomic_init(&contacted[i], 0);
  }
  _Atomic(uint8_t) *saved = g_net.contacted;
  g_net.contacted = contacted;
  struct fid_ep *ep = g_net.ep;
  g_net.ep = NULL;

  asked_reset();
  if (arts_net_post_shutdown() != 0) {
    FAIL("(p) a rank that contacted nobody posted\n");
  }
  asked_expect("nobody contacted", 0, 0, 0, 0);

  atomic_store_explicit(&contacted[1], 1, memory_order_relaxed);
  asked_reset();
  if (arts_net_post_shutdown() != 0) {
    FAIL("(p) the post went to this rank\n");
  }
  asked_expect("only this rank contacted", 0, 0, 0, 0);

  atomic_store_explicit(&contacted[2], 1, memory_order_relaxed);
  g_gone_rank = 2;
  asked_reset();
  if (arts_net_post_shutdown() != 0) {
    FAIL("(p) the post went to a peer the mesh reports gone\n");
  }
  asked_expect("a gone peer contacted", 0, 0, 1, 0);

  g_gone_rank = ~0u;
  g_net.ep = ep;
  g_net.contacted = saved;
  arts_global_rank_id = 0;
  arts_global_rank_count = 1;
}

/* ---- (f) ----------------------------------------------------------------- */

/* False when the sender never left the spin, which it is left in. */
static bool case_f(void) {
  struct spin_case c;
  memset(&c, 0, sizeof(c));
  g_sentinel_true_from = 3;
  atomic_store_explicit(&g_probe_on, true, memory_order_relaxed);
  pthread_t t;
  pthread_create(&t, NULL, spin_sender, &c);
  uint64_t deadline = now_ns() + 3ull * 1000000000ull;
  while (!atomic_load_explicit(&c.done, memory_order_acquire)) {
    if (now_ns() > deadline) {
      FAIL("(f) net_submit still spins 3 s after it began, after %d "
           "sentinel check(s)\n",
           atomic_load_explicit(&g_sentinel_calls, memory_order_relaxed));
      return false;
    }
    sleep_ms(1);
  }
  pthread_join(t, NULL);
  uint64_t seen =
      atomic_load_explicit(&g_sentinel_true_ns, memory_order_relaxed);
  int calls = atomic_load_explicit(&g_sentinel_calls, memory_order_relaxed);
  int entries = atomic_load_explicit(&g_entries, memory_order_relaxed);
  int origin = atomic_load_explicit(&g_entry_origin, memory_order_relaxed);
  int frees = atomic_load_explicit(&c.frees, memory_order_relaxed);
  if (c.returned) {
    FAIL("(f) net_submit returned true after its spin saw a closed peer\n");
  }
  if (seen == 0 || calls < g_sentinel_true_from) {
    FAIL("(f) the spin made %d sentinel check(s), want at least %d\n", calls,
         g_sentinel_true_from);
  } else if (c.returned_ns - seen >= 1000000000ull) {
    FAIL("(f) net_submit returned %.3f s after the closed peer was seen\n",
         (double)(c.returned_ns - seen) / 1e9);
  }
  if (entries != 1 || origin != (int)ARTS_EXIT_BACKPRESSURE) {
    FAIL("(f) %d take(s), the last with origin %s; want 1, BACKPRESSURE\n",
         entries, arts_exit_origin_name((enum arts_exit_origin)origin));
  }
  if (frees != 1) {
    FAIL("(f) the dropped send's free ran %d time(s), want 1\n", frees);
  }
  atomic_store_explicit(&g_probe_on, false, memory_order_relaxed);
  gate_open();
  return true;
}

int main(void) {
  g_watch_size = sizeof(struct net_put_group_s);
  g_net.peers = g_peers;
  g_net.peer_count = 1;
  g_net.max_msg = CEILING;
  g_net.waw_ordered = true;
  /* No reap: a spin finds the completion queue's token taken. */
  atomic_store_explicit(&g_net_cq_busy, 1, memory_order_relaxed);

  case_a();
  case_b(3);
  case_b(15);
  case_c();
  case_p();
  if (!case_f()) {
    /* The sender is still in the spin: end without exit handlers. */
    printf("FAIL net_gate_drop: %d failure(s)\n",
           atomic_load_explicit(&g_failures, memory_order_relaxed));
    fflush(stdout);
    _exit(1);
  }

  int failures = atomic_load_explicit(&g_failures, memory_order_relaxed);
  if (failures != 0) {
    printf("FAIL net_gate_drop: %d failure(s)\n", failures);
    return 1;
  }
  printf("PASS net_gate_drop\n");
  return 0;
}
