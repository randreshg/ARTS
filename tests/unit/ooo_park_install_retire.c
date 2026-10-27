/* SPDX-License-Identifier: Apache-2.0
 *
 * ooo_park_install_retire — an OoO park racing the real install and retire.
 *
 * A parker pushes its node onto the slot's ooo_list and then re-reads the
 * slot's value and key; an installer
 * (arts_route_table_install_handle_if_absent) stores the value and then drains
 * the list, and a retirer (arts_route_table_set_destroyed_if) clears the value
 * and returns the key and then re-drives the list.  Of a parker and an
 * installer or retirer, either the drain takes the node or the parker's re-read
 * sees the change and drains it itself, so every message is dispatched exactly
 * once and no node is left on any slot.
 *
 * Scenarios, each over many rounds on a fresh GUID, the racing threads
 * released together and each delayed by a random spin, with a random spin
 * between every park and its re-read:
 *
 *   A. install vs non-create parkers: PARKERS threads send a non-create
 *      message to an empty GUID while one thread installs it.  Every message
 *      runs once, against the installed object.
 *
 *   B. retire vs a create parker: the GUID holds generation 1; one thread
 *      sends a create of the GUID (parks behind generation 1) while another
 *      retires generation 1.  The create body runs once, installs generation
 *      2 into the empty slot, and never meets generation 1.
 *
 *   C. both at once: B with PARKERS non-create messages also in flight.
 *      Every message runs once, against generation 1 or generation 2.
 *
 * After each round no node is parked on any slot of any table, and each
 * generation's deleter runs once when it is retired.  The scenarios also
 * count who dispatched each parked node (the installer or retirer's drain,
 * or a parker's own drain) and require both over the run, so a green run
 * has exercised both outcomes of each pairing.
 *
 * No runtime: guid.c, route_table.c, shared.c and ooo.c are #include'd; the
 * handlers are counting stubs.
 */

#include "arts/utils/stdatomic_compat.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
void arts_abort(uint8_t code) { _exit(code ? code : 70); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_calloc_aligned(size_t n, size_t s, size_t a) {
  void *p = NULL;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n * s)) {
    return NULL;
  }
  memset(p, 0, n * s);
  return p;
}
void arts_free(void *p) { free(p); }

/* Per-thread xorshift; each thread seeds its own. */
static __thread uint64_t t_rng;
static uint32_t rnd(void) {
  uint64_t x = t_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  t_rng = x;
  return (uint32_t)(x >> 32);
}
static void spin(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; i++) {
  }
}

/* Set by OOO_AFTER_PARK: this thread has parked a node this round. */
static __thread int t_parked;
static _Atomic(uint64_t) g_parks;
static void after_park(void) {
  t_parked = 1;
  atomic_fetch_add_explicit(&g_parks, 1, memory_order_relaxed);
  if (rnd() & 1u) {
    spin(rnd() & 255u);
  }
}
#define OOO_AFTER_PARK(slot) ((void)(slot), after_park())

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/ooo.c"
#include "../../libs/src/core/utils/shared.c"

#include "counter_stubs.h"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

/* ── Round state: written by main between the two barriers of a round ──── */
static arts_guid_t g_guid;
static int *g_gen1; /* the object installed before the round (B, C) */
static int *g_gen2; /* the object the round installs */
static _Atomic(int) g_errors;
static _Atomic(uint64_t) g_dispatched; /* non-create messages run */
static _Atomic(uint64_t) g_creates;    /* create bodies run */
static _Atomic(int) g_deletes;

/* Who ran a parker's message: the installing or retiring side's drain, or
 * a parker that had parked (its own rescue drain).  The parker's message is
 * the non-create one in A and C (whose create body installs, so its drain is
 * the installing side's) and the create in B. */
enum role { ROLE_PARKER, ROLE_INSTALLER, ROLE_RETIRER, ROLE_CREATOR };
enum scenario { SC_A, SC_B, SC_C, SC_STOP };
static enum scenario g_scenario; /* written by main between barriers */
static __thread enum role t_role;
static _Atomic(uint64_t) g_by_drainer;
static _Atomic(uint64_t) g_by_rescue;

static void error(const char *what) {
  if (atomic_fetch_add_explicit(&g_errors, 1, memory_order_relaxed) == 0) {
    (void)fprintf(stderr, "FAIL ooo_park_install_retire: %s\n", what);
  }
}

static void note_dispatcher(bool is_create) {
  bool parker_side =
      g_scenario == SC_B ? t_role == ROLE_CREATOR : t_role == ROLE_PARKER;
  if (is_create != (g_scenario == SC_B)) {
    return;
  }
  if (!parker_side) {
    atomic_fetch_add_explicit(&g_by_drainer, 1, memory_order_relaxed);
  } else if (t_parked) {
    atomic_fetch_add_explicit(&g_by_rescue, 1, memory_order_relaxed);
  }
}

static void test_deleter(void *obj) {
  atomic_fetch_add_explicit(&g_deletes, 1, memory_order_relaxed);
  free(obj);
}

static void noncreate(void *item, void *args) {
  (void)args;
  if (item == NULL || (item != g_gen1 && item != g_gen2)) {
    error("a message ran against an object that is not its GUID's");
  }
  note_dispatcher(false);
  atomic_fetch_add_explicit(&g_dispatched, 1, memory_order_relaxed);
}

static void unexpected(void *item, void *args) {
  (void)item;
  (void)args;
  error("a handler of a kind the test never sends ran");
}

/* The create body: installs generation 2, as a real create's body does. */
void arts_handler_event_create(void *item, void *args) {
  (void)args;
  if (item != NULL) {
    error("a create body was handed an object");
  }
  note_dispatcher(true);
  atomic_fetch_add_explicit(&g_creates, 1, memory_order_relaxed);
  arts_shared_ptr_t cb = arts_route_table_make_handle(g_gen2, g_guid);
  if (!arts_route_table_install_handle_if_absent(cb, g_guid)) {
    arts_shared_abandon(&cb);
    error("a create body ran while its GUID's slot was occupied");
  }
}

void arts_handler_event_satisfy_slot(void *i, void *a) { noncreate(i, a); }

void arts_handler_edt_create(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_create(void *i, void *a) { unexpected(i, a); }
void arts_handler_edt_satisfy_slot(void *i, void *a) { unexpected(i, a); }
void arts_handler_event_add_dependence(void *i, void *a) { unexpected(i, a); }
void arts_handler_edt_destroy(void *i, void *a) { unexpected(i, a); }
void arts_handler_event_destroy(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_destroy(void *i, void *a) { unexpected(i, a); }
void arts_db_acquire_replay_dep(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_snapshot_request(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_publish(void *i, void *a) { unexpected(i, a); }
#if defined(ARTS_PROTOCOL_EXCL)
void arts_handler_db_excl_request(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_excl_release(void *i, void *a) { unexpected(i, a); }
#elif defined(ARTS_PROTOCOL_INV)
void arts_handler_db_grant_request(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_inv_request(void *i, void *a) { unexpected(i, a); }
#ifdef ARTS_RELEASE_PURGE
void arts_handler_db_grant_return(void *i, void *a) { unexpected(i, a); }
#endif
#elif defined(ARTS_PROTOCOL_FLUSH)
void arts_handler_db_fetch_request(void *i, void *a) { unexpected(i, a); }
void arts_handler_db_flush_announce(void *i, void *a) { unexpected(i, a); }
#elif defined(ARTS_PROTOCOL_VAL)
void arts_handler_db_grant_request(void *i, void *a) { unexpected(i, a); }
#ifdef ARTS_RELEASE_PURGE
void arts_handler_db_grant_return(void *i, void *a) { unexpected(i, a); }
#endif
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif
/* ThreadSanitizer serializes the spinning threads' accesses, so a round there
 * costs far more; fewer rounds keep the test within its time budget. */
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#define ROUND_SCALE 1
#else
#define ROUND_SCALE 10
#endif

/* ── Round barrier: main and every worker pass it twice per round ──────── */
#define PARKERS 6
#define WORKERS                                                                \
  (PARKERS + 2) /* parkers, then the installer/retirer, then                   \
                   the creator */
static _Atomic(unsigned) g_arrived;
static _Atomic(unsigned) g_phase;
static void barrier(void) {
  unsigned phase = atomic_load_explicit(&g_phase, memory_order_acquire);
  if (atomic_fetch_add_explicit(&g_arrived, 1, memory_order_acq_rel) ==
      WORKERS) {
    atomic_store_explicit(&g_arrived, 0, memory_order_relaxed);
    atomic_store_explicit(&g_phase, phase + 1, memory_order_release);
    return;
  }
  while (atomic_load_explicit(&g_phase, memory_order_acquire) == phase) {
  }
}

static void send_noncreate(void) {
  uint64_t args = 0;
  arts_ooo_dispatch_or_defer_guid(g_guid, OOO_EVENT_SATISFY_SLOT, &args,
                                  sizeof(args));
}

static void *worker(void *vp) {
  int id = (int)(intptr_t)vp;
  t_rng = 0x9E3779B97F4A7C15ull * (uint64_t)(id + 1);
  for (;;) {
    barrier();
    enum scenario sc = g_scenario;
    if (sc == SC_STOP) {
      return NULL;
    }
    t_parked = 0;
    spin(rnd() & 511u);
    if (id < PARKERS) {
      t_role = ROLE_PARKER;
      if (sc != SC_B) {
        send_noncreate();
      }
    } else if (id == PARKERS) {
      if (sc == SC_A) {
        t_role = ROLE_INSTALLER;
        arts_shared_ptr_t cb = arts_route_table_make_handle(g_gen2, g_guid);
        if (!arts_route_table_install_handle_if_absent(cb, g_guid)) {
          arts_shared_abandon(&cb);
          error("the install found the empty GUID occupied");
        }
      } else {
        t_role = ROLE_RETIRER;
        arts_shared_ptr_t h1 = arts_route_table_lookup(g_guid);
        if (h1 == NULL || arts_shared_get(h1) != g_gen1 ||
            !arts_route_table_set_destroyed_if(g_guid, h1)) {
          error("the retire of generation 1 did not take it");
        }
        arts_shared_release(&h1);
      }
    } else {
      t_role = ROLE_CREATOR;
      if (sc != SC_A) {
        uint64_t args = 0;
        arts_ooo_dispatch_or_defer_guid(g_guid, OOO_EVENT_CREATE, &args,
                                        sizeof(args));
      }
    }
    barrier();
  }
}

static unsigned int parked_anywhere(void) {
  unsigned int n = arts_route_table_parked_count(arts_node_info.route_table[0]);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    n += arts_route_table_parked_count(arts_node_info.remote_route_table[s]);
  }
  return n;
}

/* Node init of a one-thread rank as the runtime runs it (only the main
 * thread mints, DB GUIDs only, so no key counters), and its route tables. */
static void rt_init(void) {
  arts_thread_info.thread_id = 0;
  arts_node_info.total_thread_count = 1;
  arts_guid_node_geometry_init();
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(1, sizeof(arts_route_table_t *));
  arts_node_info.route_table[0] = arts_new_route_table(1024, 10);
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
}

/* Retires whatever `g_guid` publishes and checks it is `want`. */
static int retire_current(const int *want) {
  arts_shared_ptr_t h = arts_route_table_lookup(g_guid);
  int ok = h != NULL && arts_shared_get(h) == want &&
           arts_route_table_set_destroyed_if(g_guid, h);
  arts_shared_release(&h);
  return ok;
}

typedef struct {
  uint64_t parks, by_drainer, by_rescue;
} tally_t;

static int run(enum scenario sc, int rounds, const char *name, tally_t *t) {
  atomic_store_explicit(&g_parks, 0, memory_order_relaxed);
  atomic_store_explicit(&g_by_drainer, 0, memory_order_relaxed);
  atomic_store_explicit(&g_by_rescue, 0, memory_order_relaxed);
  for (int r = 0; r < rounds; r++) {
    g_guid = arts_guid_reserve(ARTS_GUID_DB, 0);
    g_gen1 = NULL;
    g_gen2 = (int *)malloc(sizeof(int));
    *g_gen2 = 2;
    atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
    atomic_store_explicit(&g_creates, 0, memory_order_relaxed);
    atomic_store_explicit(&g_deletes, 0, memory_order_relaxed);
    if (sc != SC_A) {
      g_gen1 = (int *)malloc(sizeof(int));
      *g_gen1 = 1;
      arts_shared_ptr_t cb = arts_route_table_make_handle(g_gen1, g_guid);
      if (!arts_route_table_install_handle_if_absent(cb, g_guid)) {
        (void)fprintf(stderr, "FAIL %s: setup install\n", name);
        return 1;
      }
    }
    g_scenario = sc;
    barrier(); /* start */
    barrier(); /* every worker done */
    uint64_t want_msgs = sc == SC_B ? 0u : (uint64_t)PARKERS;
    uint64_t want_creates = sc == SC_A ? 0u : 1u;
    uint64_t msgs = atomic_load_explicit(&g_dispatched, memory_order_relaxed);
    uint64_t creates = atomic_load_explicit(&g_creates, memory_order_relaxed);
    unsigned int parked = parked_anywhere();
    if (msgs != want_msgs || creates != want_creates || parked != 0u) {
      (void)fprintf(stderr,
                    "FAIL %s round %d: %" PRIu64 " messages run (want %" PRIu64
                    "), %" PRIu64 " creates run (want %" PRIu64
                    "), %u nodes left parked\n",
                    name, r, msgs, want_msgs, creates, want_creates, parked);
      return 1;
    }
    if (atomic_load_explicit(&g_errors, memory_order_relaxed) != 0) {
      return 1;
    }
    if (!retire_current(g_gen2)) {
      (void)fprintf(stderr, "FAIL %s round %d: generation 2 not installed\n",
                    name, r);
      return 1;
    }
    int want_deletes = sc == SC_A ? 1 : 2;
    if (atomic_load_explicit(&g_deletes, memory_order_relaxed) !=
        want_deletes) {
      (void)fprintf(stderr, "FAIL %s round %d: %d deletes (want %d)\n", name, r,
                    atomic_load_explicit(&g_deletes, memory_order_relaxed),
                    want_deletes);
      return 1;
    }
  }
  t->parks = atomic_load_explicit(&g_parks, memory_order_relaxed);
  t->by_drainer = atomic_load_explicit(&g_by_drainer, memory_order_relaxed);
  t->by_rescue = atomic_load_explicit(&g_by_rescue, memory_order_relaxed);
  if (t->by_drainer == 0u || t->by_rescue == 0u) {
    (void)fprintf(stderr,
                  "FAIL %s: not both outcomes exercised (%" PRIu64
                  " parks, %" PRIu64 " run by the drain, %" PRIu64
                  " by a parker's own drain)\n",
                  name, t->parks, t->by_drainer, t->by_rescue);
    return 1;
  }
  return 0;
}

int main(void) {
  rt_init();
  arts_route_table_register_deleter(ARTS_GUID_DB, test_deleter);
  pthread_t tid[WORKERS];
  for (int i = 0; i < WORKERS; i++) {
    if (pthread_create(&tid[i], NULL, worker, (void *)(intptr_t)i) != 0) {
      (void)fprintf(stderr, "FAIL: pthread_create %d\n", i);
      return 1;
    }
  }
  tally_t a, b, c;
  int rc = run(SC_A, 2000 * ROUND_SCALE, "A install-vs-park", &a) ||
           run(SC_B, 4000 * ROUND_SCALE, "B retire-vs-create-park", &b) ||
           run(SC_C, 2000 * ROUND_SCALE, "C retire-create-vs-parks", &c);
  if (rc != 0) {
    _exit(1); /* workers wait at a barrier main never reaches */
  }
  g_scenario = SC_STOP;
  barrier();
  for (int i = 0; i < WORKERS; i++) {
    pthread_join(tid[i], NULL);
  }
  arts_guid_generator_cleanup();
  printf("PASS ooo_park_install_retire: A %" PRIu64 " parks (%" PRIu64
         " by the install's drain, %" PRIu64 " by a rescue), B %" PRIu64
         " (%" PRIu64 " by the retire's re-drive, %" PRIu64
         " by a rescue), C %" PRIu64 " (%" PRIu64 ", %" PRIu64 ")\n",
         a.parks, a.by_drainer, a.by_rescue, b.parks, b.by_drainer, b.by_rescue,
         c.parks, c.by_drainer, c.by_rescue);
  return 0;
}
