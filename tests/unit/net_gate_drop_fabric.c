/* SPDX-License-Identifier: Apache-2.0
 *
 * net_gate_drop_fabric — a SHUTDOWN frame taken in by the libfabric
 * transport's reap (transport/net.c), over a self-only fabric.  The reap
 * recognizes the frame itself: it is never dispatched, and the reaping
 * thread takes the rank's shutdown point once it has released the completion
 * queue.
 *
 *   (d) A SHUTDOWN header and then an ordinary header-only message, injected
 *       to this rank and taken in by one arts_net_progress call: the take of
 *       the shutdown point runs with origin WIRE before the dispatcher sees
 *       the ordinary message, the dispatcher never sees the SHUTDOWN, and the
 *       reaping thread counted both frames as received.
 *   (e) A send held in the ring-full spin while a SHUTDOWN header is
 *       injected to this rank: the spin's own reap tries the take once, with
 *       origin BACKPRESSURE, and the send returns false within 1 s, its free
 *       run once.
 *
 * Standalone: #includes net.c (its statics) and links the real regpool
 * object (the receive buffers need registered memory) and libfabric.  The
 * runtime externs are libc-backed shims; the take of the shutdown point is a
 * shim that records its origin and closes the send gate, as the point's first
 * taker does on a multi-rank run, and returns false, as for a point another
 * thread took; the counters are counted per thread.  FI_PROVIDER=tcp is
 * pinned by the test's environment, whose provider progresses on its own
 * thread, so injected frames reach the completion queue while no thread of
 * this test reaps.
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

#include "arts/counter/counter.h"
#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/shutdown.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

/* ---- runtime externs the module (and regpool) reference ------------------ */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1; /* self-only fabric */
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  exit(rc ? rc : 1);
}
void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void *arts_calloc_aligned(size_t n, size_t size, size_t align) {
  void *p = NULL;
  if (posix_memalign(&p, align, n * size) != 0) {
    return NULL;
  }
  memset(p, 0, n * size);
  return p;
}
void arts_free(void *ptr) { free(ptr); }
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif

ARTS_THREAD_LOCAL_IE arts_counter_t
    arts_thread_local_counters[NUM_COUNTER_TYPES];
void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  atomic_fetch_add_explicit(&counter->count, num, memory_order_relaxed);
}

/* The order in which the entry and the dispatcher saw their frames. */
static _Atomic(int) g_seq;

static _Atomic(int) g_shutdown_dispatched;
static _Atomic(int) g_ordinary_dispatched;
static _Atomic(int) g_ordinary_seq;

void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  int seq = atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed) + 1;
  if (packet->message_type == MSG_SHUTDOWN) {
    atomic_fetch_add_explicit(&g_shutdown_dispatched, 1, memory_order_relaxed);
  } else {
    atomic_store_explicit(&g_ordinary_seq, seq, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ordinary_dispatched, 1, memory_order_relaxed);
  }
}

/* No mesh: nothing is armed, no peer reads as gone, no probe is due. */
bool arts_socket_armed(void) { return false; }
bool arts_socket_peer_gone(unsigned int rank) {
  (void)rank;
  return false;
}
bool arts_socket_probe_due(uint64_t *next_ns) {
  (void)next_ns;
  return false;
}
bool arts_socket_sentinel_check(void) { return false; }

/* Compile the module under test (its statics: g_net, the rings, the reaps). */
#include "../../libs/src/core/transport/net.c"

/* ------------------------------------------------------------------------- */

static _Atomic(int) g_entries;
static _Atomic(int) g_entry_origin;
static _Atomic(int) g_entry_seq;

bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)rc;
  int seq = atomic_fetch_add_explicit(&g_seq, 1, memory_order_relaxed) + 1;
  atomic_store_explicit(&g_entry_seq, seq, memory_order_relaxed);
  atomic_store_explicit(&g_entry_origin, (int)origin, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_entries, 1, memory_order_relaxed);
  atomic_store_explicit(&g_net.quiescing, true, memory_order_relaxed);
  return false;
}

_Noreturn void arts_exit_resume(void) {
  printf("FAIL net_gate_drop_fabric: the end resumed after a take that "
         "failed\n");
  fflush(stdout);
  _exit(1);
}

bool arts_shutdown_taken(void) {
  return atomic_load_explicit(&g_entries, memory_order_relaxed) != 0;
}

static _Atomic(int) g_failures;
#define FAIL(...)                                                              \
  do {                                                                         \
    printf("FAIL net_gate_drop_fabric: " __VA_ARGS__);                         \
    atomic_fetch_add_explicit(&g_failures, 1, memory_order_relaxed);           \
  } while (0)

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

static void records_reset(void) {
  atomic_store_explicit(&g_seq, 0, memory_order_relaxed);
  atomic_store_explicit(&g_shutdown_dispatched, 0, memory_order_relaxed);
  atomic_store_explicit(&g_ordinary_dispatched, 0, memory_order_relaxed);
  atomic_store_explicit(&g_ordinary_seq, 0, memory_order_relaxed);
  atomic_store_explicit(&g_entries, 0, memory_order_relaxed);
  atomic_store_explicit(&g_entry_origin, 0, memory_order_relaxed);
  atomic_store_explicit(&g_entry_seq, 0, memory_order_relaxed);
  atomic_store_explicit(&g_net.quiescing, false, memory_order_relaxed);
}

static int load(_Atomic(int) *v) {
  return atomic_load_explicit(v, memory_order_relaxed);
}

/* A header-only frame of `type` to this rank, past the provider's transient
 * refusals while the loopback connection comes up. */
static bool inject_self(unsigned type) {
  struct arts_msg_header_s hdr;
  arts_fill_packet_header(&hdr, sizeof(hdr), type);
  uint64_t deadline = now_ns() + 5ull * 1000000000ull;
  for (;;) {
    ssize_t rc = fi_inject(g_net.ep, &hdr, sizeof(hdr), g_net.peers[0]);
    if (rc == 0) {
      return true;
    }
    if (rc != -FI_EAGAIN || now_ns() > deadline) {
      FAIL("fi_inject of a type-%u header to this rank: %s\n", type,
           fi_strerror((int)-rc));
      return false;
    }
    sleep_ms(1);
  }
}

static uint64_t received(arts_counter_type_t c) {
  return atomic_load_explicit(&arts_thread_local_counters[c].count,
                              memory_order_relaxed);
}

/* ---- (d) ----------------------------------------------------------------- */

/* Reap and dispatch until both frames of an attempt were handled, or 5 s. */
static void drain_attempt(void) {
  uint64_t deadline = now_ns() + 5ull * 1000000000ull;
  while ((load(&g_entries) < 1 || load(&g_ordinary_dispatched) < 1) &&
         load(&g_shutdown_dispatched) == 0 && now_ns() < deadline) {
    arts_net_progress();
  }
}

static void case_d(void) {
  /* The provider lands both frames in the completion queue on its own
   * thread; a frame that lands after the call's read takes another attempt
   * with a longer wait. */
  for (unsigned attempt = 0; attempt < 5; attempt++) {
    records_reset();
    if (!inject_self(MSG_SHUTDOWN) || !inject_self(MSG_TIME_SYNC_REQUEST)) {
      return;
    }
    sleep_ms(20u << attempt);
    uint64_t num0 = received(NUM_REMOTE_RECEIVE);
    uint64_t bytes0 = received(BYTES_REMOTE_RECEIVED);
    arts_net_progress();
    uint64_t num = received(NUM_REMOTE_RECEIVE) - num0;
    uint64_t bytes = received(BYTES_REMOTE_RECEIVED) - bytes0;
    if (load(&g_shutdown_dispatched) != 0) {
      FAIL("(d) the dispatcher saw MSG_SHUTDOWN %d time(s)\n",
           load(&g_shutdown_dispatched));
      return;
    }
    if (load(&g_entries) != 1 || load(&g_ordinary_dispatched) != 1) {
      printf("NOTE net_gate_drop_fabric: (d) attempt %u: the call took in "
             "%d entry and %d ordinary message; waiting longer\n",
             attempt, load(&g_entries), load(&g_ordinary_dispatched));
      drain_attempt();
      if (load(&g_shutdown_dispatched) != 0) {
        FAIL("(d) the dispatcher saw MSG_SHUTDOWN %d time(s)\n",
             load(&g_shutdown_dispatched));
        return;
      }
      if (load(&g_entries) != 1 || load(&g_ordinary_dispatched) != 1) {
        FAIL("(d) attempt %u: %d entry and %d ordinary message within 5 s, "
             "want 1 and 1\n",
             attempt, load(&g_entries), load(&g_ordinary_dispatched));
        return;
      }
      continue;
    }
    if (load(&g_entry_origin) != (int)ARTS_EXIT_WIRE) {
      FAIL("(d) the entry ran with origin %s, want WIRE\n",
           arts_exit_origin_name((enum arts_exit_origin)load(&g_entry_origin)));
    }
    if (load(&g_entry_seq) > load(&g_ordinary_seq)) {
      FAIL("(d) the dispatcher saw the ordinary message before the entry "
           "ran\n");
    }
#if ENABLE_NUM_REMOTE_RECEIVE
    if (num != 2) {
      FAIL("(d) the reaping thread counted %llu received message(s), want "
           "2\n",
           (unsigned long long)num);
    }
#endif
#if ENABLE_BYTES_REMOTE_RECEIVED
    if (bytes != 2 * sizeof(struct arts_msg_header_s)) {
      FAIL("(d) the reaping thread counted %llu received byte(s), want "
           "%zu\n",
           (unsigned long long)bytes, 2 * sizeof(struct arts_msg_header_s));
    }
#endif
    (void)num;
    (void)bytes;
    return;
  }
  FAIL("(d) the two frames never reached one progress call's burst\n");
}

/* ---- (e) ----------------------------------------------------------------- */

static void count_run(void *arg) {
  atomic_fetch_add_explicit((_Atomic(int) *)arg, 1, memory_order_relaxed);
}

struct spin_case {
  _Atomic(bool) spinning;
  _Atomic(bool) done;
  _Atomic(int) frees;
  bool returned;
  uint64_t returned_ns;
};

/* This thread's ring, its drain token held and filled to capacity with
 * inert txns, so a send of either class spins and nothing it queued posts. */
static struct net_ring_s *ring_hold_full(void) {
  struct net_ring_s *r = net_get_ring();
  atomic_store_explicit(&r->draining, 1, memory_order_relaxed);
  for (uint32_t i = 0; i < ARTS_NET_RING_CAP; i++) {
    if (!net_ring_try_enqueue(
            r, net_txn_new(0, NULL, 0, NULL, NULL, NULL, NULL), true)) {
      FAIL("(e) the ring took %u of %u inert txns\n", i, ARTS_NET_RING_CAP);
      break;
    }
  }
  return r;
}

static void ring_release(struct net_ring_s *r) {
  for (;;) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (h == t) {
      break;
    }
    net_txn_complete(r->slots[h % ARTS_NET_RING_CAP]);
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
  }
  atomic_store_explicit(&r->draining, 0, memory_order_release);
}

static void *spin_sender(void *arg) {
  struct spin_case *c = (struct spin_case *)arg;
  struct net_ring_s *r = ring_hold_full();
  struct net_txn_s *txn =
      net_txn_new(0, NULL, 0, NULL, NULL, count_run, &c->frees);
  atomic_store_explicit(&c->spinning, true, memory_order_release);
  c->returned = net_submit(txn, /*is_ack=*/true);
  c->returned_ns = now_ns();
  ring_release(r);
  atomic_store_explicit(&c->done, true, memory_order_release);
  return NULL;
}

/* False when the sender never left the spin, which it is left in. */
static bool case_e(void) {
  records_reset();
  struct spin_case c;
  memset(&c, 0, sizeof(c));
  pthread_t t;
  pthread_create(&t, NULL, spin_sender, &c);
  while (!atomic_load_explicit(&c.spinning, memory_order_acquire)) {
    sleep_ms(1);
  }
  sleep_ms(50);
  if (load(&g_entries) != 0) {
    FAIL("(e) the spin entered the shutdown before any SHUTDOWN arrived\n");
  }
  uint64_t injected = now_ns();
  if (!inject_self(MSG_SHUTDOWN)) {
    return false;
  }
  uint64_t deadline = injected + 3ull * 1000000000ull;
  while (!atomic_load_explicit(&c.done, memory_order_acquire)) {
    if (now_ns() > deadline) {
      FAIL("(e) net_submit still spins 3 s after the SHUTDOWN was "
           "injected (%d entry seen)\n",
           load(&g_entries));
      return false;
    }
    sleep_ms(1);
  }
  pthread_join(t, NULL);
  if (c.returned) {
    FAIL("(e) net_submit returned true after its spin took in a "
         "SHUTDOWN\n");
  }
  if (c.returned_ns - injected >= 1000000000ull) {
    FAIL("(e) net_submit returned %.3f s after the SHUTDOWN was injected\n",
         (double)(c.returned_ns - injected) / 1e9);
  }
  if (load(&g_entries) != 1 ||
      load(&g_entry_origin) != (int)ARTS_EXIT_BACKPRESSURE) {
    FAIL("(e) %d entry, the last with origin %s; want 1, BACKPRESSURE\n",
         load(&g_entries),
         arts_exit_origin_name((enum arts_exit_origin)load(&g_entry_origin)));
  }
  if (load(&g_shutdown_dispatched) != 0) {
    FAIL("(e) the dispatcher saw MSG_SHUTDOWN\n");
  }
  int frees = atomic_load_explicit(&c.frees, memory_order_relaxed);
  if (frees != 1) {
    FAIL("(e) the dropped send's free ran %d time(s), want 1\n", frees);
  }
  return true;
}

int main(void) {
  setenv("FI_PROVIDER", "tcp", 1);
  arts_net_init(NULL, NULL, NULL, false);
  if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(),
                         (size_t)32 * 1024 * 1024, 0)) {
    printf("FAIL net_gate_drop_fabric: regpool init failed\n");
    return 1;
  }
  arts_net_rx_arm();
  unsigned char addr[ARTS_NET_ADDR_MAX];
  unsigned alen = arts_net_own_address(addr, sizeof(addr));
  arts_net_av_insert_table(addr, alen, 1);

  case_d();
  if (!case_e()) {
    /* A sender may still be in the spin: end without the provider's exit
     * handlers, which it would race. */
    printf("FAIL net_gate_drop_fabric: %d failure(s)\n",
           atomic_load_explicit(&g_failures, memory_order_relaxed));
    fflush(stdout);
    _exit(1);
  }

  arts_net_quiesce();
  arts_net_reclaim();
  arts_regpool_cleanup();
  arts_net_teardown();

  int failures = atomic_load_explicit(&g_failures, memory_order_relaxed);
  if (failures != 0) {
    printf("FAIL net_gate_drop_fabric: %d failure(s)\n", failures);
    return 1;
  }
  printf("PASS net_gate_drop_fabric\n");
  return 0;
}
