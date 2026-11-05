/* SPDX-License-Identifier: Apache-2.0
 *
 * net_rdzv_discard — the teardown's end of the rendezvous pairing table of
 * the libfabric transport (transport/net.c): arts_net_quiesce closes the
 * endpoint and runs no callback; arts_net_reclaim then runs the discard
 * callback of every expectation whose data never landed, once, with the
 * expectation's argument, and runs no callback for anything else.
 *
 * Each expectation's context owns a landing (a buffer of the registered pool)
 * and a counted handle (a control block whose object's deleter counts its
 * runs); the test drops its own count of each handle before the teardown, so
 * the context's is the last.  Both callbacks release the two; the pairing
 * callback also counts a pairing, the discard a discard.
 *
 *   1. Unpaired expectations (sixteen, with the table compiled to one slot at
 *      level 0 and two below, so most park below level 0): before reclaim no
 *      callback has run and no handle is freed, quiesce included; reclaim
 *      runs each discard exactly once, which returns the landing to the
 *      still-live pool (a free after the pool's cleanup is fatal) and drops
 *      the handle's last count, so each deleter runs exactly then; no pairing
 *      callback runs; reclaim reports sixteen discards.
 *   2. An expectation whose data arrived before the teardown: its pairing
 *      callback ran once, then, and its discard never runs.
 *   3. An expectation registered with no discard (its argument owns nothing)
 *      and a data arrival no expectation met: reclaim runs nothing for
 *      either.
 *   4. When the endpoint did not close (the test sets the record a failed
 *      close leaves), reclaim runs no discard (it reports none) and keeps the
 *      table and the recv buffers; with the record cleared, a second reclaim
 *      does the work.
 *
 * Standalone: #includes net.c (its statics) and shared.c with the shims
 * net_rendezvous uses, and links the real registered pool; the fabric comes
 * up on tcp, so the teardown runs whole, endpoint close included, before the
 * pool's cleanup.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* Every txid collides at level 0; deeper levels separate txids by hash. */
#define ARTS_NET_RDZV_LEVEL0_LOG2 0
#define ARTS_NET_RDZV_LEVEL_LOG2 1
#include "../../libs/src/core/transport/net.c"
#include "../../libs/src/core/utils/shared.c"

#include "counter_stubs.h"
#include "net_runtime_stubs.h"

/* ------------------------------------------------------------------------- */

#define UNPAIRED 16
#define LANDING_LEN 4096u

/* One expectation's context.  Records live for the whole test, so the oracle
 * reads them after the callbacks ran; a callback releases the landing and the
 * handle and clears them (arts_shared_release sets the handle it drops to
 * NULL, which the oracle's NULL check reads). */
struct ctx {
  void *landing;
  arts_shared_ptr_t handle;
  int paired;
  int discarded;
};

static struct ctx g_unpaired[UNPAIRED];
static struct ctx g_paired;
static struct ctx g_nothing; /* registered with no discard */
static int g_deleted;
static int g_landings_returned;
static int g_failures;

#define FAIL(...)                                                              \
  do {                                                                         \
    printf("FAIL net_rdzv_discard: " __VA_ARGS__);                             \
    g_failures++;                                                              \
  } while (0)

static void counting_deleter(void *obj) {
  g_deleted++;
  free(obj);
}

static void release_owned(struct ctx *c) {
  if (c->landing != NULL) {
    arts_regpool_free(c->landing);
    c->landing = NULL;
    g_landings_returned++;
  }
  arts_shared_release(&c->handle);
}

static void on_data(void *arg) {
  struct ctx *c = (struct ctx *)arg;
  c->paired++;
  release_owned(c);
}

static void on_discard(void *arg) {
  struct ctx *c = (struct ctx *)arg;
  c->discarded++;
  release_owned(c);
}

/* Fills `c` with a landing and a handle whose last count is the context's. */
static void ctx_init(struct ctx *c) {
  c->landing = arts_regpool_alloc_aligned(LANDING_LEN, 64);
  if (c->landing == NULL) {
    printf("FAIL net_rdzv_discard: landing allocation failed\n");
    exit(1);
  }
  arts_shared_ptr_t mine =
      arts_shared_make(malloc(sizeof(int)), counting_deleter);
  c->handle = arts_shared_copy(mine);
  arts_shared_release(&mine);
}

static unsigned levels_now(void) {
  unsigned n = 0;
  for (struct net_rdzv_level_s *lv = g_rdzv_level0; lv != NULL;
       lv = atomic_load_explicit(&lv->next, memory_order_acquire)) {
    n++;
  }
  return n;
}

int main(void) {
  setenv("FI_PROVIDER", "tcp", 1);
  arts_net_init(NULL, NULL, NULL, false);
  if (!arts_regpool_init(arts_net_domain(), arts_net_mr_endpoint(),
                         (size_t)16 * 1024 * 1024, 0)) {
    printf("FAIL net_rdzv_discard: regpool init failed\n");
    return 1;
  }
  arts_net_rx_arm();

  /* (2) paired before the teardown. */
  ctx_init(&g_paired);
  uint64_t tp = arts_net_rdzv_txid_next();
  arts_net_rdzv_expect(tp, LANDING_LEN, on_data, on_discard, &g_paired);
  net_rdzv_data_arrived(tp);
  if (g_paired.paired != 1 || g_paired.discarded != 0 || g_deleted != 1) {
    FAIL("(2) the paired expectation's callback did not run once at its "
         "pairing\n");
  }

  /* (1) unpaired. */
  for (unsigned i = 0; i < UNPAIRED; i++) {
    ctx_init(&g_unpaired[i]);
    arts_net_rdzv_expect(arts_net_rdzv_txid_next(), LANDING_LEN, on_data,
                         on_discard, &g_unpaired[i]);
  }

  /* (3) no discard, and an arrival no expectation meets. */
  arts_net_rdzv_expect(arts_net_rdzv_txid_next(), LANDING_LEN, on_data, NULL,
                       &g_nothing);
  net_rdzv_data_arrived(arts_net_rdzv_txid_next());

  unsigned levels = levels_now();
  if (levels < 2) {
    FAIL("no expectation parked below level 0 (%u level(s))\n", levels);
  }
  for (unsigned i = 0; i < UNPAIRED; i++) {
    if (g_unpaired[i].paired != 0 || g_unpaired[i].discarded != 0) {
      FAIL("(1) expectation %u ran a callback before the teardown\n", i);
    }
  }
  if (g_deleted != 1 || g_landings_returned != 1) {
    FAIL("before the teardown: %d handle(s) freed and %d landing(s) "
         "returned, want 1 and 1\n",
         g_deleted, g_landings_returned);
  }

  if (!arts_net_quiesce() || g_net.writes_may_land) {
    FAIL("quiesce did not close the endpoint\n");
  }
  if (g_rdzv_level0 == NULL || g_deleted != 1 || g_landings_returned != 1) {
    FAIL("quiesce ran a discard\n");
  }

  /* (4) the record a failed endpoint close leaves. */
  g_net.writes_may_land = true;
  size_t kept_discards = arts_net_reclaim();
  if (kept_discards != 0 || g_rdzv_level0 == NULL || g_net.rxbuf[0] == NULL ||
      g_deleted != 1 || g_landings_returned != 1) {
    FAIL("(4) reclaim released memory the endpoint may still write\n");
  }
  g_net.writes_may_land = false;
  size_t discards = arts_net_reclaim();
  if (discards != UNPAIRED) {
    FAIL("(1) reclaim reported %zu discard(s), want %u\n", discards,
         (unsigned)UNPAIRED);
  }

  for (unsigned i = 0; i < UNPAIRED; i++) {
    struct ctx *c = &g_unpaired[i];
    if (c->discarded != 1 || c->paired != 0) {
      FAIL("(1) expectation %u: %d discard(s) and %d pairing(s), want 1 "
           "and 0\n",
           i, c->discarded, c->paired);
    }
    if (c->landing != NULL || c->handle != NULL) {
      FAIL("(1) expectation %u kept what its context owns\n", i);
    }
  }
  if (g_paired.paired != 1 || g_paired.discarded != 0) {
    FAIL("(2) the paired expectation: %d discard(s) after the teardown\n",
         g_paired.discarded);
  }
  if (g_nothing.paired != 0 || g_nothing.discarded != 0) {
    FAIL("(3) a callback ran for the expectation with no discard\n");
  }
  if (g_deleted != UNPAIRED + 1 || g_landings_returned != UNPAIRED + 1) {
    FAIL("after the teardown: %d handle(s) freed and %d landing(s) "
         "returned, want %d and %d\n",
         g_deleted, g_landings_returned, UNPAIRED + 1, UNPAIRED + 1);
  }
  if (g_rdzv_level0 != NULL || g_net.rxbuf[0] != NULL) {
    FAIL("reclaim kept the pairing table or a recv buffer\n");
  }

  arts_regpool_cleanup();
  arts_net_teardown();

  printf("INFO net_rdzv_discard: %u unpaired expectation(s) over %u "
         "level(s)\n",
         (unsigned)UNPAIRED, levels);
  if (g_failures != 0) {
    printf("FAIL net_rdzv_discard: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("PASS net_rdzv_discard\n");
  return 0;
}
