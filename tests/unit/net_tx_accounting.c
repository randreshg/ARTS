/* SPDX-License-Identifier: Apache-2.0
 *
 * net_tx_accounting — the transport's count of posted, uncompleted transmits
 * (transport/net.c): each posting thread counts its posts on its own retry
 * ring and the CQ token holder counts completions, and net_tx_outstanding
 * sums the two.  The count must never read below what a reader knows is
 * outstanding, must return to zero once completions drain, and quiesce,
 * which reads it as its reap bound, must complete every transmit before it
 * frees the rings that hold the counts.
 *
 * Several sender threads send payload messages to this process's own
 * endpoint (each one a registered bounce and a completion-gated fi_send whose
 * free hook marks the send done) while one thread reaps and dispatches under
 * the CQ token.  After each send whose ring the sender sees empty (its sends
 * are all posted), the sender reads the count and then its own sends' done
 * marks: a send whose mark is still clear was not yet counted complete in
 * the value read, so the value is at least the number of such sends.  Once
 * every message has arrived the reaper stops, and a last batch is sent with
 * nobody reaping, so quiesce starts with posts outstanding and must run every
 * remaining free hook: afterwards each send's hook ran exactly once.  Under
 * ThreadSanitizer a report here is a defect.
 *
 * Standalone: #includes net.c (its statics) and links the real regpool
 * object; runtime externs are libc-backed shims.  FI_PROVIDER=tcp is pinned
 * by the test's environment.
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
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

/* ---- runtime externs the module (and regpool) reference ------------------ */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1; /* self-only fabric */
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
struct arts_runtime_shared_s arts_node_info;
struct arts_shutdown_abandon_s arts_shutdown_abandon;
void arts_abort(uint8_t code) { exit(code ? code : 1); }
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

/* Messages dispatched by the reaper: the stubbed RX sink. */
static _Atomic(uint64_t) g_recv_count;

void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
  atomic_fetch_add_explicit(&g_recv_count, 1, memory_order_relaxed);
}

/* Compile the module under test (its statics: g_net, the rings, the reaps). */
#include "../../libs/src/core/transport/net.c"

#include "counter_stubs.h"

/* ------------------------------------------------------------------------- */

#define SENDERS 4
#define SENDS 1500
#define PAYLOAD 200
#define LAST 16 /* sent after the reaper stopped */

/* One send: its payload bytes and the mark its free hook sets.  The send
 * copies only `data` (before the post), the hook writes only `done` (after
 * the completion), so the two never meet. */
struct send_rec {
  unsigned char data[PAYLOAD];
  _Atomic(int) done;
};

static struct send_rec g_recs[SENDERS][SENDS];
static struct send_rec g_last[LAST];
/* Free hooks run, over every send: each must run exactly once. */
static _Atomic(uint64_t) g_hooks;
static _Atomic(uint64_t) g_hook_twice;
/* Set once every sender has joined and every message has arrived: the
 * reaper's stop flag.  Only its own value. */
static _Atomic(bool) g_stop;

struct sender_result {
  unsigned long samples;
  unsigned long low; /* reads below the sender's own uncounted sends */
};
static struct sender_result g_results[SENDERS];

static void send_done(void *arg) {
  struct send_rec *r = (struct send_rec *)arg;
  if (atomic_exchange_explicit(&r->done, 1, memory_order_release) != 0) {
    atomic_fetch_add_explicit(&g_hook_twice, 1, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&g_hooks, 1, memory_order_relaxed);
}

static void *sender(void *arg) {
  unsigned id = (unsigned)(uintptr_t)arg;
  struct sender_result *res = &g_results[id];
  struct arts_msg_header_s hdr;
  arts_fill_packet_header(&hdr, sizeof(hdr) + PAYLOAD, MSG_TIME_SYNC_REQUEST);
  unsigned lo = 0; /* every send below lo is marked done */
  for (unsigned i = 0; i < SENDS; i++) {
    struct send_rec *r = &g_recs[id][i];
    memset(r->data, (int)(i & 0xffu), sizeof(r->data));
    arts_net_send_core(0, (char *)&hdr, (unsigned)sizeof(hdr), (char *)r, 0,
                       PAYLOAD, send_done);
    /* A sender's own ring is empty only once every send it queued has been
     * posted (the acquire of head orders the poster's count before the
     * read). */
    struct net_ring_s *ring = t_ring;
    if (atomic_load_explicit(&ring->head, memory_order_acquire) !=
        atomic_load_explicit(&ring->tail, memory_order_relaxed)) {
      continue;
    }
    uint64_t outstanding = net_tx_outstanding();
    while (lo <= i &&
           atomic_load_explicit(&g_recs[id][lo].done, memory_order_acquire)) {
      lo++;
    }
    uint64_t mine = 0;
    for (unsigned k = lo; k <= i; k++) {
      if (!atomic_load_explicit(&g_recs[id][k].done, memory_order_acquire)) {
        mine++;
      }
    }
    res->samples++;
    if (outstanding < mine) {
      res->low++;
      printf("FAIL net_tx_accounting: sender %u read %llu outstanding with "
             "%llu of its own sends uncompleted\n",
             id, (unsigned long long)outstanding, (unsigned long long)mine);
    }
  }
  return NULL;
}

static void *reaper(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    arts_net_progress();
  }
  return NULL;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void) {
  setenv("FI_PROVIDER", "tcp", 1);
  arts_net_init(NULL, NULL, NULL, false);
  if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(),
                         (size_t)32 * 1024 * 1024, 0)) {
    printf("FAIL net_tx_accounting: regpool init failed\n");
    return 1;
  }
  arts_net_rx_arm();
  unsigned char addr[ARTS_NET_ADDR_MAX];
  unsigned alen = arts_net_own_address(addr, sizeof(addr));
  arts_net_av_insert_table(addr, alen, 1);

  int failures = 0;
  pthread_t rt;
  pthread_t st[SENDERS];
  pthread_create(&rt, NULL, reaper, NULL);
  for (unsigned i = 0; i < SENDERS; i++) {
    pthread_create(&st[i], NULL, sender, (void *)(uintptr_t)i);
  }
  for (unsigned i = 0; i < SENDERS; i++) {
    pthread_join(st[i], NULL);
  }
  const uint64_t total = (uint64_t)SENDERS * SENDS;
  uint64_t deadline = now_ns() + 30ull * 1000000000ull;
  while (atomic_load_explicit(&g_recv_count, memory_order_relaxed) < total &&
         now_ns() < deadline) {
    usleep(1000);
  }
  atomic_store_explicit(&g_stop, true, memory_order_relaxed);
  pthread_join(rt, NULL);
  uint64_t arrived = atomic_load_explicit(&g_recv_count, memory_order_relaxed);
  if (arrived != total) {
    printf("FAIL net_tx_accounting: %llu of %llu messages arrived\n",
           (unsigned long long)arrived, (unsigned long long)total);
    failures++;
  }
  unsigned long samples = 0, low = 0;
  for (unsigned i = 0; i < SENDERS; i++) {
    samples += g_results[i].samples;
    low += g_results[i].low;
  }
  if (samples == 0) {
    printf("FAIL net_tx_accounting: no sender ever saw its ring empty\n");
    failures++;
  }
  if (low != 0) {
    failures++;
  }
  /* Nobody reaps from here on: these posts stay outstanding until quiesce's
   * bounded reap, which must complete every one before it frees the rings
   * that count them. */
  struct arts_msg_header_s hdr;
  arts_fill_packet_header(&hdr, sizeof(hdr) + PAYLOAD, MSG_TIME_SYNC_REQUEST);
  for (unsigned i = 0; i < LAST; i++) {
    memset(g_last[i].data, 0x5a, sizeof(g_last[i].data));
    arts_net_send_core(0, (char *)&hdr, (unsigned)sizeof(hdr),
                       (char *)&g_last[i], 0, PAYLOAD, send_done);
  }
  uint64_t at_quiesce = net_tx_outstanding();
  arts_net_quiesce();
  uint64_t hooks = atomic_load_explicit(&g_hooks, memory_order_relaxed);
  uint64_t twice = atomic_load_explicit(&g_hook_twice, memory_order_relaxed);
  if (hooks != total + LAST || twice != 0) {
    printf("FAIL net_tx_accounting: %llu free hooks ran for %llu sends (%llu "
           "twice) after quiesce\n",
           (unsigned long long)hooks, (unsigned long long)(total + LAST),
           (unsigned long long)twice);
    failures++;
  }
  arts_regpool_cleanup();
  arts_net_teardown();
  printf("INFO net_tx_accounting: %lu samples, %llu outstanding at quiesce\n",
         samples, (unsigned long long)at_quiesce);
  if (failures != 0) {
    printf("FAIL net_tx_accounting: %d failure(s)\n", failures);
    return 1;
  }
  printf("PASS net_tx_accounting\n");
  return 0;
}
