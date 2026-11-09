/* SPDX-License-Identifier: Apache-2.0
 *
 * ooo_drain_repush — OoO drain re-push vs ooo_list Treiber under reuse.
 *
 * Properties under test (ooo.c arts_ooo_drain + dispatch_or_defer MISS path):
 *
 *  (1) "Save next BEFORE dispatch."  arts_ooo_drain reads head->next into a
 *      local BEFORE calling dispatch_or_defer, because on a MISS the dispatch
 *      RE-PUSHES the very same node (overwriting head->link.next as it links to
 *      the new chain top).  If the walk re-read head->next AFTER dispatch it
 *      would follow the re-push chain and either spin or lose the rest of the
 *      snapshot.  We drive a drain whose every node MISSes (slot value == NULL)
 *      so every node is re-pushed mid-walk, then assert the snapshot was walked
 *      to completion exactly once (each node visited once, none lost).
 *
 *  (2) Re-push-onto-same-stack ABA safety under allocator address reuse.  The
 *      ooo_list is consumed ONLY by whole-chain reverse_drain (a single
 *      atomic_exchange); there is deliberately NO pop_one.  A push only links
 *      to "whatever is on top now" and never caches head->next for a CAS, so
 *      re-pushing a node back onto the SAME stack is ABA-safe even when freed
 *      payloads' addresses are recycled.  We interleave a MISS-drain (which
 *      re-pushes) with a concurrent producer thread that pushes fresh payloads
 *      (forcing the allocator to recycle addresses of nodes that were freed on
 *      HIT in prior rounds).  Correctness criterion across the whole run: every
 *      distinct payload id is dispatched EXACTLY ONCE and the chain ends empty
 *      (no loss, no duplicate, no corruption).
 *
 *  (3) The drain on the installer's handle (arts_ooo_drain_on) walks its
 *      snapshot the same way: on a handle that is not live every node takes
 *      the engine and, the slot still empty, is re-pushed; on a live handle
 *      every node runs inline on the handle's object and its node is
 *      consumed; and once a node of the walk retires the object, every later
 *      node of the walk takes the engine and parks again.
 *
 *  (4) Debug only: arts_ooo_dispatch_on refuses an EDT or event satisfy while
 *      the running EDT has device invalidations outstanding (the API entries
 *      hold those back on the wrapper's slot first); a forked child that
 *      tries it ends by SIGABRT.
 *
 * Each payload carries a unique 32-bit id in its args blob; the dispatch
 * handler records the id into a global seen[] bitmap and flags any duplicate,
 * and any non-create handler run against an object other than the test's.
 *
 * Standalone: #includes ooo.c, links shared.c, single fixed slot, libc shims,
 * the counter increments recorded here; arts_route_table_live is the route
 * table's predicate over shared.c.
 */

#include "arts/utils/stdatomic_compat.h"
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/utils/shared.h"

/* ── Dispatch accounting: record id, flag duplicates ────────────────────── */
#define MAX_IDS 200000
static _Atomic(unsigned char) g_seen[MAX_IDS];
static _Atomic(uint64_t) g_dispatched;
static _Atomic(uint64_t) g_dup;

static int g_obj = 0x1234;
static _Atomic(uint64_t) g_wrong_item;

static void recorder(void *item, void *args) {
  if (item != &g_obj) {
    atomic_fetch_add_explicit(&g_wrong_item, 1, memory_order_relaxed);
  }
  uint32_t id = *(const uint32_t *)args;
  if (id < MAX_IDS) {
    unsigned char prev =
        atomic_fetch_add_explicit(&g_seen[id], 1, memory_order_relaxed);
    if (prev != 0) {
      atomic_fetch_add_explicit(&g_dup, 1, memory_order_relaxed);
    }
  }
  atomic_fetch_add_explicit(&g_dispatched, 1, memory_order_relaxed);
}

void arts_handler_edt_create(void *i, void *a) { recorder(i, a); }
void arts_handler_event_create(void *i, void *a) { recorder(i, a); }
void arts_handler_db_create(void *i, void *a) { recorder(i, a); }
void arts_handler_event_satisfy_slot(void *i, void *a) { recorder(i, a); }
void arts_handler_edt_satisfy_slot(void *i, void *a) { recorder(i, a); }
void arts_handler_event_add_dependence(void *i, void *a) { recorder(i, a); }
void arts_handler_edt_destroy(void *i, void *a) { recorder(i, a); }
void arts_handler_db_destroy(void *i, void *a) { recorder(i, a); }
void arts_db_acquire_replay_dep(void *i, void *a) { recorder(i, a); }
void arts_handler_db_snapshot_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_publish(void *i, void *a) { recorder(i, a); }
#if defined(ARTS_VALIDITY_EXCL)
void arts_handler_db_excl_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_excl_release(void *i, void *a) { recorder(i, a); }
#elif defined(ARTS_VALIDITY_INV)
void arts_handler_db_grant_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_inv_request(void *i, void *a) { recorder(i, a); }
#ifdef ARTS_RETENTION_PURGE
void arts_handler_db_grant_return(void *i, void *a) { recorder(i, a); }
#endif
#elif defined(ARTS_VALIDITY_FLUSH)
void arts_handler_db_fetch_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_flush_announce(void *i, void *a) { recorder(i, a); }
#elif defined(ARTS_VALIDITY_VAL)
void arts_handler_db_grant_request(void *i, void *a) { recorder(i, a); }
#ifdef ARTS_RETENTION_PURGE
void arts_handler_db_grant_return(void *i, void *a) { recorder(i, a); }
#endif
#endif

/* ── Single fixed slot ────────────────────────────────────────────────── */
static arts_route_item_t g_slot;
void arts_route_table_reserve_or_lookup(arts_guid_t key,
                                        arts_route_item_t **out) {
  (void)key;
  *out = &g_slot;
}

void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }
#ifdef ARTS_DEBUG_INJECTION
void arts_sched_fuzz_point(void) {}
#endif
bool arts_route_table_live(arts_shared_ptr_t h, arts_guid_t key) {
  return h != NULL && arts_shared_home(h) != NULL &&
         arts_shared_tag(h) == (uint64_t)key;
}

/* Part 3's destroy body retires the object it runs on, as a real destroy
 * does: the cb's tag cleared and the value taken out of the slot. */
static arts_shared_ptr_t g_p3_cb;
void arts_handler_event_destroy(void *i, void *a) {
  recorder(i, a);
  if (g_p3_cb != NULL) {
    arts_shared_set_tag(g_p3_cb, 0);
    arts_shared_ptr_t old =
        arts_shared_slot_exchange(&g_slot.value, (arts_shared_ptr_t)NULL);
    if (old != NULL) {
      arts_shared_release(&old);
    }
  }
}
bool arts_route_table_set_destroyed_item(arts_guid_t key,
                                         arts_shared_ptr_t cb) {
  (void)key;
  (void)cb;
  return false;
}
void arts_route_table_delete_unpublished(arts_guid_t key, void *obj) {
  (void)key;
  (void)obj;
}

#include "../../libs/src/core/ooo.c"

/* The running EDT, read by the engine's Debug force-defer check: none. */
ARTS_THREAD_LOCAL_IE struct arts_edt_s *arts_current_edt;

/* The engine's counter increments, recorded so part 3 can count the pins a
 * drain spends where the tree captures NUM_ROUTE_PIN. */
ARTS_THREAD_LOCAL_IE arts_counter_t
    arts_thread_local_counters[NUM_COUNTER_TYPES];
void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  atomic_fetch_add_explicit(&counter->count, num, memory_order_relaxed);
}
static uint64_t pins_now(void) {
  return atomic_load_explicit(&arts_thread_local_counters[NUM_ROUTE_PIN].count,
                              memory_order_relaxed);
}

static void noop_deleter(void *o) { (void)o; }

/* ───────────────────────────────────────────────────────────────────────
 * PART 1 — deterministic single-thread "save next before dispatch".
 *
 * Pre-fill the chain with M payloads while value == NULL.  Call arts_ooo_drain
 * once: every node MISSes (value still NULL) so every node is re-pushed.  The
 * snapshot must be walked to completion — exactly M distinct ids deferred, and
 * after the drain the chain still holds exactly M nodes (the re-pushed ones).
 * Then publish value and drain again: now every node HITs and dispatches once.
 * Verify each of the M ids dispatched exactly once.
 * ──────────────────────────────────────────────────────────────────────── */
static int part1_save_next(void) {
  const int M = 1000;
  atomic_store_explicit(&g_slot.value, (arts_shared_slot_t){0},
                        memory_order_relaxed);
  arts_lf_stack_init(&g_slot.ooo_list);
  for (int id = 0; id < M; id++) {
    atomic_store_explicit(&g_seen[id], 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
  atomic_store_explicit(&g_dup, 0, memory_order_relaxed);

  /* Defer M payloads (all MISS — value NULL — so they push, no dispatch). */
  for (uint32_t id = 0; id < (uint32_t)M; id++) {
    arts_ooo_dispatch_or_defer(
        &g_slot, NULL, OOO_EVENT_SATISFY_SLOT,
        atomic_load_explicit(&g_slot.key, memory_order_acquire), &id,
        sizeof(id));
  }
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 0) {
    (void)fprintf(stderr, "FAIL part1: pre-install defer dispatched early\n");
    return 1;
  }

  /* Drain with value STILL NULL: every node MISSes mid-walk and is re-pushed.
   * The walk must save next-before-dispatch and complete the snapshot. */
  arts_ooo_drain(&g_slot);
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 0) {
    (void)fprintf(stderr, "FAIL part1: MISS-drain dispatched %" PRIu64 "\n",
                  atomic_load_explicit(&g_dispatched, memory_order_relaxed));
    return 1;
  }
  /* Count re-pushed nodes by detaching (then put them back via reverse so a
   * subsequent drain can dispatch them). */
  arts_lf_link_t *chain = arts_lf_stack_reverse_drain(&g_slot.ooo_list);
  int repushed = 0;
  for (arts_lf_link_t *p = chain; p;) {
    arts_lf_link_t *nx = atomic_load_explicit(&p->next, memory_order_relaxed);
    repushed++;
    p = nx;
  }
  if (repushed != M) {
    (void)fprintf(stderr,
                  "FAIL part1: re-pushed %d nodes after MISS-drain (want %d) — "
                  "snapshot truncated/looped (save-next regression)\n",
                  repushed, M);
    return 1;
  }
  /* Re-attach the detached chain so we can dispatch it now. */
  for (arts_lf_link_t *p = chain; p;) {
    arts_lf_link_t *nx = atomic_load_explicit(&p->next, memory_order_relaxed);
    arts_lf_stack_push(&g_slot.ooo_list, p);
    p = nx;
  }

  /* Publish value and drain: every node HITs and dispatches once. */
  arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
  arts_shared_slot_store(&g_slot.value, cb);
  arts_ooo_drain(&g_slot);

  uint64_t got = atomic_load_explicit(&g_dispatched, memory_order_relaxed);
  if (got != (uint64_t)M) {
    (void)fprintf(stderr,
                  "FAIL part1: HIT-drain dispatched %" PRIu64 " (want %d)\n",
                  got, M);
    return 1;
  }
  if (atomic_load_explicit(&g_dup, memory_order_relaxed) != 0) {
    (void)fprintf(stderr, "FAIL part1: duplicate dispatch detected\n");
    return 1;
  }
  for (int id = 0; id < M; id++) {
    if (atomic_load_explicit(&g_seen[id], memory_order_relaxed) != 1) {
      (void)fprintf(stderr, "FAIL part1: id %d seen %u times (want 1)\n", id,
                    atomic_load_explicit(&g_seen[id], memory_order_relaxed));
      return 1;
    }
  }
  arts_shared_ptr_t old =
      arts_shared_slot_exchange(&g_slot.value, (arts_shared_ptr_t)NULL);
  if (old) {
    arts_shared_release(&old);
  }
  printf("[part1] PASS  save-next-before-dispatch: %d nodes re-pushed + "
         "dispatched once\n",
         M);
  return 0;
}

/* ───────────────────────────────────────────────────────────────────────
 * PART 2 — concurrent re-push vs producer push under address reuse.
 *
 * One "miss-drainer" thread repeatedly drains the slot while value == NULL
 * (each drain re-pushes whatever it snapshots).  N producer threads push fresh
 * payloads concurrently.  After producers finish, value is published and a
 * final drain dispatches everything.  Across the whole run every distinct id
 * must dispatch EXACTLY once (re-push must not lose or duplicate a node, and
 * recycled addresses must not corrupt the Treiber chain).
 * ──────────────────────────────────────────────────────────────────────── */
#define P2_PRODUCERS 6
#define P2_PER_PRODUCER 20000

static _Atomic(int) g_p2_gate;
static _Atomic(int) g_p2_producers_done;
static _Atomic(int) g_drainer_stop;

typedef struct {
  uint32_t base; /* first id this producer emits */
  int count;
} p2_producer_ctx_t;

static void *p2_producer_fn(void *vp) {
  p2_producer_ctx_t *c = (p2_producer_ctx_t *)vp;
  while (atomic_load_explicit(&g_p2_gate, memory_order_acquire) == 0) {
  }
  for (int k = 0; k < c->count; k++) {
    uint32_t id = c->base + (uint32_t)k;
    arts_ooo_dispatch_or_defer(
        &g_slot, NULL, OOO_EVENT_SATISFY_SLOT,
        atomic_load_explicit(&g_slot.key, memory_order_acquire), &id,
        sizeof(id));
  }
  atomic_fetch_add_explicit(&g_p2_producers_done, 1, memory_order_release);
  return NULL;
}

/* Continuously drain while value == NULL — forces concurrent re-push of any
 * snapshot it grabs (every node MISSes since value stays NULL until the end).
 */
static void *p2_drainer_fn(void *vp) {
  (void)vp;
  while (atomic_load_explicit(&g_p2_gate, memory_order_acquire) == 0) {
  }
  while (atomic_load_explicit(&g_drainer_stop, memory_order_acquire) == 0) {
    arts_ooo_drain(&g_slot);
  }
  return NULL;
}

static int part2_concurrent_repush(void) {
  const int TOTAL = P2_PRODUCERS * P2_PER_PRODUCER;
  if (TOTAL > MAX_IDS) {
    (void)fprintf(stderr, "FAIL part2: TOTAL %d > MAX_IDS\n", TOTAL);
    return 1;
  }
  atomic_store_explicit(&g_slot.value, (arts_shared_slot_t){0},
                        memory_order_relaxed);
  arts_lf_stack_init(&g_slot.ooo_list);
  for (int id = 0; id < TOTAL; id++) {
    atomic_store_explicit(&g_seen[id], 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
  atomic_store_explicit(&g_dup, 0, memory_order_relaxed);
  atomic_init(&g_p2_gate, 0);
  atomic_store_explicit(&g_p2_producers_done, 0, memory_order_relaxed);
  atomic_store_explicit(&g_drainer_stop, 0, memory_order_relaxed);

  pthread_t prod[P2_PRODUCERS];
  p2_producer_ctx_t pctx[P2_PRODUCERS];
  for (int i = 0; i < P2_PRODUCERS; i++) {
    pctx[i].base = (uint32_t)(i * P2_PER_PRODUCER);
    pctx[i].count = P2_PER_PRODUCER;
    if (pthread_create(&prod[i], NULL, p2_producer_fn, &pctx[i]) != 0) {
      (void)fprintf(stderr, "FAIL part2: pthread_create producer %d\n", i);
      return 1;
    }
  }
  pthread_t drainer;
  if (pthread_create(&drainer, NULL, p2_drainer_fn, NULL) != 0) {
    (void)fprintf(stderr, "FAIL part2: pthread_create drainer\n");
    return 1;
  }

  atomic_store_explicit(&g_p2_gate, 1, memory_order_release);

  for (int i = 0; i < P2_PRODUCERS; i++) {
    pthread_join(prod[i], NULL);
  }
  /* Stop the miss-drainer (it has been re-pushing snapshots all along). */
  atomic_store_explicit(&g_drainer_stop, 1, memory_order_release);
  pthread_join(drainer, NULL);

  /* Nothing should have dispatched yet (value stayed NULL → all MISS). */
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 0) {
    (void)fprintf(stderr,
                  "FAIL part2: %" PRIu64
                  " dispatched while value NULL (impossible on MISS path)\n",
                  atomic_load_explicit(&g_dispatched, memory_order_relaxed));
    return 1;
  }

  /* Publish value and drain everything that survived the concurrent re-pushes.
   * A few final drains in case a node was re-pushed onto a fresh chain by the
   * last MISS-drain. */
  arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
  arts_shared_slot_store(&g_slot.value, cb);
  for (int i = 0; i < 4; i++) {
    arts_ooo_drain(&g_slot);
  }

  uint64_t got = atomic_load_explicit(&g_dispatched, memory_order_relaxed);
  uint64_t dup = atomic_load_explicit(&g_dup, memory_order_relaxed);
  if (got != (uint64_t)TOTAL) {
    (void)fprintf(stderr,
                  "FAIL part2: dispatched %" PRIu64 " (want %d) — node lost or "
                  "duplicated under concurrent re-push\n",
                  got, TOTAL);
    return 1;
  }
  if (dup != 0) {
    (void)fprintf(stderr, "FAIL part2: %" PRIu64 " duplicate dispatches\n",
                  dup);
    return 1;
  }
  for (int id = 0; id < TOTAL; id++) {
    if (atomic_load_explicit(&g_seen[id], memory_order_relaxed) != 1) {
      (void)fprintf(stderr, "FAIL part2: id %d seen %u (want 1)\n", id,
                    atomic_load_explicit(&g_seen[id], memory_order_relaxed));
      return 1;
    }
  }
  arts_lf_link_t *leftover = arts_lf_stack_drain(&g_slot.ooo_list);
  if (leftover != NULL) {
    (void)fprintf(stderr, "FAIL part2: chain not empty after final drains\n");
    return 1;
  }
  arts_shared_ptr_t old =
      arts_shared_slot_exchange(&g_slot.value, (arts_shared_ptr_t)NULL);
  if (old) {
    arts_shared_release(&old);
  }
  printf("[part2] PASS  concurrent re-push: %d ids, each dispatched once\n",
         TOTAL);
  return 0;
}

/* ───────────────────────────────────────────────────────────────────────
 * PART 3 — the drain on the installer's handle.
 *
 * The slot names a GUID of its own.  M payloads are parked; a drain on a
 * handle with no home re-pushes them all.  The handle is then installed
 * (value, home, tag) and a drain on it runs every node inline, once each,
 * on the handle's object, pinning nothing, and leaves the chain empty.  Last,
 * a destroy node is parked ahead of M more: the drain runs the destroy
 * inline, which retires the object, and every later node of the walk is
 * re-pushed, none run.
 * ──────────────────────────────────────────────────────────────────────── */
static uint32_t chain_len(void) {
  uint32_t n = 0;
  for (arts_lf_link_t *p =
           atomic_load_explicit(&g_slot.ooo_list.head, memory_order_relaxed);
       p != NULL; p = atomic_load_explicit(&p->next, memory_order_relaxed)) {
    n++;
  }
  return n;
}

static int part3_drain_on(void) {
  const uint32_t M = 1000;
  const arts_guid_t G = (arts_guid_t)0x5a5a;
  atomic_store_explicit(&g_slot.key, G, memory_order_relaxed);
  atomic_store_explicit(&g_slot.value, (arts_shared_slot_t){0},
                        memory_order_relaxed);
  arts_lf_stack_init(&g_slot.ooo_list);
  for (uint32_t id = 0; id < 2u * M + 1u; id++) {
    atomic_store_explicit(&g_seen[id], 0, memory_order_relaxed);
  }
  atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
  atomic_store_explicit(&g_dup, 0, memory_order_relaxed);

  arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
  arts_shared_set_tag(cb, (uint64_t)G);
  arts_shared_ptr_t mine = arts_shared_copy(cb);
  for (uint32_t id = 0; id < M; id++) {
    arts_ooo_dispatch_or_defer(&g_slot, NULL, OOO_EVENT_SATISFY_SLOT, G, &id,
                               sizeof(id));
  }
  arts_ooo_drain_on(&g_slot, mine);
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 0 ||
      chain_len() != M) {
    (void)fprintf(stderr,
                  "FAIL part3: a drain on a handle with no home ran %" PRIu64
                  " nodes and left %u parked (want 0 and %u)\n",
                  atomic_load_explicit(&g_dispatched, memory_order_relaxed),
                  chain_len(), M);
    return 1;
  }

  arts_shared_slot_store(&g_slot.value, cb);
  arts_shared_set_home(cb, &g_slot);
  uint64_t pins = pins_now();
  arts_ooo_drain_on(&g_slot, mine);
  pins = pins_now() - pins;
#if ENABLE_NUM_ROUTE_PIN
  if (pins != 0u) {
    (void)fprintf(stderr,
                  "FAIL part3: a drain on a live handle pinned %" PRIu64
                  " times (want 0)\n",
                  pins);
    return 1;
  }
#endif
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != M ||
      chain_len() != 0u) {
    (void)fprintf(stderr,
                  "FAIL part3: a drain on a live handle ran %" PRIu64
                  " nodes and left %u parked (want %u and 0)\n",
                  atomic_load_explicit(&g_dispatched, memory_order_relaxed),
                  chain_len(), M);
    return 1;
  }

  uint32_t destroy_id = 2u * M;
  pins = pins_now();
  arts_ooo_dispatch_or_defer(&g_slot, NULL, OOO_EVENT_DESTROY, G, &destroy_id,
                             sizeof(destroy_id));
  pins = pins_now() - pins;
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != M + 1u) {
    (void)fprintf(stderr, "FAIL part3: the destroy did not run at once\n");
    return 1;
  }
#if ENABLE_NUM_ROUTE_PIN
  /* The engine's own dispatch does pin: the count is live. */
  if (pins != 1u) {
    (void)fprintf(stderr,
                  "FAIL part3: the engine's dispatch pinned %" PRIu64
                  " times (want 1)\n",
                  pins);
    return 1;
  }
#endif
  /* Run the destroy again from a drain: put the object back first. */
  atomic_store_explicit(&g_seen[destroy_id], 0, memory_order_relaxed);
  atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
  arts_shared_ptr_t again = arts_shared_copy(mine);
  arts_shared_ptr_t prev =
      arts_shared_slot_exchange(&g_slot.value, (arts_shared_ptr_t)NULL);
  if (prev != NULL) {
    arts_shared_release(&prev);
  }
  struct arts_ooo_payload_s *d = arts_ooo_payload_alloc(
      OOO_EVENT_DESTROY, G, &destroy_id, sizeof(destroy_id));
  arts_lf_stack_push(&g_slot.ooo_list, &d->link);
  for (uint32_t id = M; id < 2u * M; id++) {
    struct arts_ooo_payload_s *n =
        arts_ooo_payload_alloc(OOO_EVENT_SATISFY_SLOT, G, &id, sizeof(id));
    arts_lf_stack_push(&g_slot.ooo_list, &n->link);
  }
  arts_shared_slot_store(&g_slot.value, again);
  g_p3_cb = mine;
  arts_ooo_drain_on(&g_slot, mine);
  g_p3_cb = NULL;
  if (atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 1u ||
      atomic_load_explicit(&g_seen[destroy_id], memory_order_relaxed) != 1u ||
      chain_len() != M) {
    (void)fprintf(stderr,
                  "FAIL part3: after a retire in the walk %" PRIu64
                  " nodes ran and %u stayed parked (want the destroy alone, "
                  "and %u)\n",
                  atomic_load_explicit(&g_dispatched, memory_order_relaxed),
                  chain_len(), M);
    return 1;
  }
  arts_ooo_free_all(&g_slot);
  arts_shared_release(&mine);
  atomic_store_explicit(&g_slot.key, (arts_guid_t)0, memory_order_relaxed);
  printf("[part3] PASS  drain on the installer's handle: re-push, inline run, "
         "stop at a retire\n");
  return 0;
}

/* ───────────────────────────────────────────────────────────────────────
 * PART 4 — the force-defer guard of arts_ooo_dispatch_on (Debug only).
 * ──────────────────────────────────────────────────────────────────────── */
static int part4_force_defer_guard(void) {
#ifndef NDEBUG
  const ooo_kind_t held_back[] = {OOO_EDT_SATISFY_SLOT, OOO_EVENT_SATISFY_SLOT};
  for (size_t k = 0; k < sizeof(held_back) / sizeof(held_back[0]); k++) {
    pid_t pid = fork();
    if (pid < 0) {
      (void)fprintf(stderr, "FAIL part4: fork\n");
      return 1;
    }
    if (pid == 0) {
      /* The expected assertion's message is not this test's output. */
      (void)freopen("/dev/null", "w", stderr);
      static struct arts_edt_s wrapper;
      wrapper.invalidate_count = 1;
      arts_current_edt = &wrapper;
      /* Without the refusal the call resolves its GUID through this file's
       * single slot for ever; the alarm ends it as a failure instead. */
      (void)alarm(5);
      uint32_t id = 0;
      arts_ooo_dispatch_on(NULL, held_back[k], (arts_guid_t)0x77, &id,
                           sizeof(id));
      _exit(0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
      (void)fprintf(stderr, "FAIL part4: waitpid\n");
      return 1;
    }
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGABRT) {
      (void)fprintf(stderr,
                    "FAIL part4: dispatch_on of kind %d under outstanding "
                    "invalidations did not abort\n",
                    (int)held_back[k]);
      return 1;
    }
  }
  printf("[part4] PASS  dispatch_on refuses a held-back satisfy\n");
#endif
  return 0;
}

int main(void) {
  if (part1_save_next() != 0) {
    return 1;
  }
  if (part2_concurrent_repush() != 0) {
    return 1;
  }
  if (part3_drain_on() != 0) {
    return 1;
  }
  if (part4_force_defer_guard() != 0) {
    return 1;
  }
  if (atomic_load_explicit(&g_wrong_item, memory_order_relaxed) != 0) {
    (void)fprintf(stderr,
                  "FAIL: %" PRIu64 " handler(s) ran on an object "
                  "that is not the slot's\n",
                  atomic_load_explicit(&g_wrong_item, memory_order_relaxed));
    return 1;
  }
  printf("PASS ooo_drain_repush: save-next + re-push ABA + drain on the "
         "installer's handle all clean\n");
  return 0;
}
