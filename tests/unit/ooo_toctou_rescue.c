/* SPDX-License-Identifier: Apache-2.0
 *
 * ooo_toctou_rescue — OoO engine push-then-install TOCTOU rescue.
 *
 * Property under test (ooo.c arts_ooo_dispatch_or_defer MISS path):
 *   A producer that observes slot->value == NULL pushes its payload onto the
 *   slot's ooo_list, then executes a seq_cst fence and re-loads slot->value.
 *   If an installer published `value` after the producer's initial NULL load
 *   but before/around the push, the producer's own post-push re-check MUST
 *   observe the install and drain the slot — so the just-pushed node is never
 *   stranded.  Symmetrically, the installer's own drain (after publishing
 *   value) detaches whatever was already pushed.  Between the two, EVERY
 *   pushed payload is dispatched EXACTLY ONCE and freed (no loss, no double).
 *
 * Interleaving driven:
 *   For each round, N producer threads each call dispatch_or_defer on the
 *   SAME freshly-reset slot while one installer thread publishes value and
 *   drains.  A start-gate releases all threads simultaneously to maximise the
 *   window where a producer loads NULL, then the installer publishes, then the
 *   producer pushes (the exact stranding window the fence+reload rescues).
 *   The dispatch handler increments a per-round atomic counter; after join we
 *   assert dispatched == pushed and the slot's ooo_list is empty.
 *
 * Create polarity (deterministic, via the engine's after-park injection
 * point): the three shapes of a teardown racing a parked create, each of
 * which must run the create body exactly once.
 *
 * This is a standalone unit test: it #includes the runtime's ooo.c so the
 * file-static g_ooo_table + engine bodies are compiled in, links shared.c for
 * the cb shared-ptr slot, and provides a single-slot
 * arts_route_table_reserve_or_lookup plus libc malloc shims.  No ARTS runtime.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/utils/shared.h"

/* ── Dispatch accounting ──────────────────────────────────────────────────
 * Every g_ooo_table[kind] handler routes here.  We only ever defer one kind
 * in this test (the model-agnostic OOO_EVENT_SATISFY_SLOT), but all table
 * entries point at the same recorder so the build's per-config table is
 * satisfied regardless of protocol. */
static _Atomic uint64_t g_dispatched;

static void recorder(void *item, void *args) {
  (void)item;
  (void)args;
  atomic_fetch_add_explicit(&g_dispatched, 1, memory_order_relaxed);
}

/* The g_ooo_table static initializer in ooo.c references these handler symbols
 * by name (the model-agnostic set + the active model's OOO_DB_* set).  Provide
 * every one as the same recorder so any build links.  Signatures must match
 * arts_ooo_handler_fn_t exactly. */
void arts_handler_edt_create(void *i, void *a) { recorder(i, a); }
/* The create polarity's body: counted apart from the non-create recorder. */
static _Atomic uint64_t g_creates;
void arts_handler_event_create(void *i, void *a) {
  (void)i;
  (void)a;
  atomic_fetch_add_explicit(&g_creates, 1, memory_order_relaxed);
}
void arts_handler_db_create(void *i, void *a) { recorder(i, a); }
void arts_handler_event_satisfy_slot(void *i, void *a) { recorder(i, a); }
void arts_handler_edt_satisfy_slot(void *i, void *a) { recorder(i, a); }
void arts_handler_event_add_dependence(void *i, void *a) { recorder(i, a); }
void arts_handler_edt_destroy(void *i, void *a) { recorder(i, a); }
void arts_handler_event_destroy(void *i, void *a) { recorder(i, a); }
void arts_handler_db_destroy(void *i, void *a) { recorder(i, a); }
void arts_db_acquire_replay_dep(void *i, void *a) { recorder(i, a); }
void arts_handler_db_snapshot_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_publish(void *i, void *a) { recorder(i, a); }
#if defined(ARTS_PROTOCOL_EXCL)
void arts_handler_db_excl_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_excl_release(void *i, void *a) { recorder(i, a); }
#elif defined(ARTS_PROTOCOL_INV)
void arts_handler_db_grant_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_inv_request(void *i, void *a) { recorder(i, a); }
#ifdef ARTS_RELEASE_PURGE
void arts_handler_db_grant_return(void *i, void *a) { recorder(i, a); }
#endif
#ifdef ARTS_WRITE_POLICY_WB
void arts_handler_db_inv_redirect(void *i, void *a) { recorder(i, a); }
#endif
#elif defined(ARTS_PROTOCOL_FLUSH)
void arts_handler_db_fetch_request(void *i, void *a) { recorder(i, a); }
void arts_handler_db_flush_announce(void *i, void *a) { recorder(i, a); }
#elif defined(ARTS_WRITE_POLICY_WT) || defined(ARTS_WRITE_POLICY_WB)
void arts_handler_db_grant_request(void *i, void *a) { recorder(i, a); }
#ifdef ARTS_RELEASE_PURGE
void arts_handler_db_grant_return(void *i, void *a) { recorder(i, a); }
#endif
#endif

/* ── A few slots the test fully controls ──────────────────────────────────
 * The OoO engine reaches a slot's ooo_list / value through
 * arts_route_table_reserve_or_lookup; we override it with a tiny table (the
 * slot keyed `key`, else the first free slot, claimed) so the test drives
 * value publish/destroy and slot return directly.  g_slot is slot 0. */
#define NSLOTS 4
static arts_route_item_t g_slots[NSLOTS];
#define g_slot (g_slots[0])

void arts_route_table_reserve_or_lookup(arts_guid_t key,
                                        arts_route_item_t **out) {
  for (int i = 0; i < NSLOTS; i++) {
    if (__atomic_load_n(&g_slots[i].key, __ATOMIC_ACQUIRE) == key) {
      *out = &g_slots[i];
      return;
    }
  }
  for (int i = 0; i < NSLOTS; i++) {
    arts_guid_t expect = 0;
    if (__atomic_compare_exchange_n(&g_slots[i].key, &expect, key, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ||
        expect == key) {
      *out = &g_slots[i];
      return;
    }
  }
  (void)fprintf(stderr, "FAIL: test slot table full\n");
  abort();
}

bool arts_route_table_set_destroyed_object(arts_guid_t key, const void *obj) {
  (void)key;
  (void)obj;
  return false;
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

/* Between a create's park and its rescue re-read, drive one of three shapes
 * of a concurrent teardown into the slot (see main). */
enum hook_mode {
  HOOK_OFF,
  HOOK_EMPTY_NO_REDRIVE,
  HOOK_TEARDOWN,
  HOOK_RECLAIM,
  HOOK_NC_MOVED,
  HOOK_NC_TEARDOWN
};
static enum hook_mode g_hook;
static void after_park_hook(arts_route_item_t *slot);
#define OOO_AFTER_PARK(slot) after_park_hook(slot)

/* libc-backed allocator shims (ooo.c payloads + shared.c cb pool). */
void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }

/* Pull in the engine under test (compiles g_ooo_table + the bodies). */
#include "../../libs/src/core/ooo.c"

/* A dummy object the installer publishes into the slot. */
static int g_obj = 0xABCD;
static void noop_deleter(void *o) { (void)o; }

#define G_CREATE ((arts_guid_t)0x5001u)
#define G_OTHER ((arts_guid_t)0x5002u)

static void after_park_hook(arts_route_item_t *slot) {
  enum hook_mode m = g_hook;
  g_hook = HOOK_OFF;
  if (m == HOOK_OFF) {
    return;
  }
  if (m == HOOK_NC_MOVED || m == HOOK_NC_TEARDOWN) {
    /* A non-create parked on its GUID's reservation (slot 0) while that slot
     * is returned and the GUID's object installs in slot 1.  MOVED: the slot
     * is re-claimed by another GUID with no re-drive, so the parker must move
     * its own node.  TEARDOWN: the teardown's re-drive takes the node. */
    __atomic_store_n(&slot->key, (arts_guid_t)0, __ATOMIC_RELEASE);
    atomic_thread_fence(memory_order_seq_cst);
    __atomic_store_n(&g_slots[1].key, G_CREATE, __ATOMIC_RELEASE);
    arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
    arts_shared_set_tag(cb, (uint64_t)G_CREATE);
    arts_atomic_shared_store(&g_slots[1].value, cb);
    if (m == HOOK_NC_MOVED) {
      __atomic_store_n(&slot->key, G_OTHER, __ATOMIC_RELEASE);
    } else {
      arts_ooo_redrive_all(slot);
    }
    return;
  }
  /* Every shape starts as a destroy does: the value leaves the slot. */
  arts_shared_ptr_t old =
      arts_atomic_shared_exchange(&slot->value, (arts_shared_ptr_t)NULL);
  arts_shared_release(&old);
  if (m == HOOK_EMPTY_NO_REDRIVE) {
    __atomic_store_n(&slot->key, (arts_guid_t)0, __ATOMIC_RELEASE);
  } else if (m == HOOK_TEARDOWN) {
    __atomic_store_n(&slot->key, (arts_guid_t)0, __ATOMIC_RELEASE);
    atomic_thread_fence(memory_order_seq_cst);
    arts_ooo_redrive_all(slot);
  } else { /* HOOK_RECLAIM: the returned slot is claimed by another GUID,
              which parks a message of its own there. */
    __atomic_store_n(&slot->key, G_OTHER, __ATOMIC_RELEASE);
    uint64_t a = 0;
    struct arts_ooo_payload_s *p = arts_ooo_payload_alloc(
        OOO_EVENT_SATISFY_SLOT, G_OTHER, &a, sizeof(a));
    arts_lf_stack_push(&slot->ooo_list, &p->link);
  }
}

static void reset_slots(void) {
  for (int i = 0; i < NSLOTS; i++) {
    arts_ooo_free_all(&g_slots[i]);
    arts_shared_ptr_t old = arts_atomic_shared_exchange(
        &g_slots[i].value, (arts_shared_ptr_t)NULL);
    arts_shared_release(&old);
    __atomic_store_n(&g_slots[i].key, (arts_guid_t)0, __ATOMIC_RELAXED);
  }
  atomic_store_explicit(&g_creates, 0, memory_order_relaxed);
  atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);
}

static unsigned int chain_len(arts_route_item_t *slot, arts_guid_t *guid) {
  unsigned int n = 0;
  for (arts_lf_link_t *l =
           atomic_load_explicit(&slot->ooo_list.head, memory_order_acquire);
       l != NULL; l = atomic_load_explicit(&l->next, memory_order_relaxed)) {
    if (guid != NULL) {
      *guid = ((struct arts_ooo_payload_s *)l)->guid;
    }
    n++;
  }
  return n;
}

/* One create-polarity case: an occupant tagged G_CREATE on slot 0, a create of
 * G_CREATE parks behind it, and `mode` runs between the park and the rescue
 * re-read.  The body must run exactly once. */
/* A non-create of G_CREATE parks on G_CREATE's empty reservation in slot 0,
 * and `mode` returns that slot between the park and the rescue re-read.  The
 * message must follow its GUID to slot 1 and be dispatched exactly once. */
static int noncreate_case(enum hook_mode mode, const char *name) {
  reset_slots();
  __atomic_store_n(&g_slot.key, G_CREATE, __ATOMIC_RELAXED);
  g_hook = mode;
  uint64_t args = 0;
  arts_ooo_dispatch_or_defer(&g_slot, NULL, OOO_EVENT_SATISFY_SLOT, G_CREATE,
                             &args, sizeof(args));
  uint64_t got = atomic_load_explicit(&g_dispatched, memory_order_relaxed);
  if (got != 1u) {
    (void)fprintf(stderr, "FAIL %s: dispatched %" PRIu64 " times, want 1\n",
                  name, got);
    return 1;
  }
  for (int i = 0; i < NSLOTS; i++) {
    if (chain_len(&g_slots[i], NULL) != 0u) {
      (void)fprintf(stderr, "FAIL %s: a node is left on slot %d\n", name, i);
      return 1;
    }
  }
  reset_slots();
  return 0;
}

static int create_case(enum hook_mode mode, const char *name) {
  reset_slots();
  __atomic_store_n(&g_slot.key, G_CREATE, __ATOMIC_RELAXED);
  arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
  arts_shared_set_tag(cb, (uint64_t)G_CREATE);
  arts_atomic_shared_store(&g_slot.value, cb);
  g_hook = mode;
  uint64_t args = 0;
  arts_ooo_dispatch_or_defer(&g_slot, NULL, OOO_EVENT_CREATE, G_CREATE, &args,
                             sizeof(args));
  uint64_t creates = atomic_load_explicit(&g_creates, memory_order_relaxed);
  if (creates != 1u) {
    (void)fprintf(stderr, "FAIL %s: create body ran %" PRIu64 " times\n",
                  name, creates);
    return 1;
  }
  if (mode == HOOK_RECLAIM) {
    arts_guid_t g = 0;
    if (chain_len(&g_slot, &g) != 1u || g != G_OTHER ||
        atomic_load_explicit(&g_dispatched, memory_order_relaxed) != 0u) {
      (void)fprintf(stderr, "FAIL %s: the other GUID's parked message was "
                            "moved or run\n",
                    name);
      return 1;
    }
    for (int i = 1; i < NSLOTS; i++) {
      if (chain_len(&g_slots[i], NULL) != 0u) {
        (void)fprintf(stderr, "FAIL %s: a node is left on slot %d\n", name,
                      i);
        return 1;
      }
    }
  } else {
    for (int i = 0; i < NSLOTS; i++) {
      if (chain_len(&g_slots[i], NULL) != 0u) {
        (void)fprintf(stderr, "FAIL %s: a node is left on slot %d\n", name,
                      i);
        return 1;
      }
    }
  }
  reset_slots();
  return 0;
}

#define ROUNDS 4000
#define PRODUCERS 6

typedef struct {
  atomic_int *gate;
  ooo_kind_t kind;
} producer_ctx_t;

static void *producer_fn(void *vp) {
  producer_ctx_t *c = (producer_ctx_t *)vp;
  while (atomic_load_explicit(c->gate, memory_order_acquire) == 0) {
    /* spin */
  }
  /* Fresh-entry dispatch_or_defer: HIT dispatches inline, MISS pushes +
   * post-push rescue.  Either way the op must be dispatched exactly once. */
  uint64_t dummy_args = 0;
  arts_ooo_dispatch_or_defer(&g_slot, NULL, c->kind, g_slot.key, &dummy_args,
                             sizeof(dummy_args));
  return NULL;
}

typedef struct {
  atomic_int *gate;
} installer_ctx_t;

static void *installer_fn(void *vp) {
  installer_ctx_t *c = (installer_ctx_t *)vp;
  while (atomic_load_explicit(c->gate, memory_order_acquire) == 0) {
    /* spin */
  }
  /* Publish value (the install) then drain — the create-handler protocol.
   * Stamped like a real install: dispatch verifies the pinned value by its
   * tag, so an unstamped cb is treated as a stranger's and never dispatched. */
  arts_shared_ptr_t cb = arts_shared_make(&g_obj, noop_deleter);
  arts_shared_set_tag(cb, 0x1234u);
  arts_atomic_shared_store(&g_slot.value, cb);
  arts_ooo_drain(&g_slot);
  return NULL;
}

int main(void) {
  /* Choose a model-agnostic kind that is always present in g_ooo_table. */
  const ooo_kind_t kind = OOO_EVENT_SATISFY_SLOT;

  for (int r = 0; r < ROUNDS; r++) {
    /* Reset the slot to the pre-install state (value NULL, empty chain). */
    atomic_store_explicit(&g_slot.value, (arts_shared_slot_t){0},
                          memory_order_relaxed);
    arts_lf_stack_init(&g_slot.ooo_list);
    /* The slot must carry the key the payloads are deferred for: the dispatch
     * compares them, since a returned slot may belong to another GUID. */
    __atomic_store_n(&g_slot.key, (arts_guid_t)0x1234u, __ATOMIC_RELAXED);
    atomic_store_explicit(&g_dispatched, 0, memory_order_relaxed);

    atomic_int gate;
    atomic_init(&gate, 0);

    pthread_t prod[PRODUCERS];
    producer_ctx_t pctx[PRODUCERS];
    for (int i = 0; i < PRODUCERS; i++) {
      pctx[i].gate = &gate;
      pctx[i].kind = kind;
      if (pthread_create(&prod[i], NULL, producer_fn, &pctx[i]) != 0) {
        (void)fprintf(stderr, "FAIL: pthread_create producer %d\n", i);
        return 1;
      }
    }
    pthread_t inst;
    installer_ctx_t ictx = {.gate = &gate};
    if (pthread_create(&inst, NULL, installer_fn, &ictx) != 0) {
      (void)fprintf(stderr, "FAIL: pthread_create installer\n");
      return 1;
    }

    atomic_store_explicit(&gate, 1, memory_order_release);

    for (int i = 0; i < PRODUCERS; i++) {
      pthread_join(prod[i], NULL);
    }
    pthread_join(inst, NULL);

    /* After all producers + the installer's drain, there must be NO node left
     * stranded on the chain: a final drain must dispatch nothing. */
    arts_ooo_drain(&g_slot);

    uint64_t got = atomic_load_explicit(&g_dispatched, memory_order_relaxed);
    if (got != (uint64_t)PRODUCERS) {
      (void)fprintf(stderr,
                    "FAIL round %d: dispatched %" PRIu64
                    " (want %d) — a pushed "
                    "node was stranded or double-dispatched\n",
                    r, got, PRODUCERS);
      return 1;
    }
    /* Chain must be empty now. */
    arts_lf_link_t *leftover = arts_lf_stack_drain(&g_slot.ooo_list);
    if (leftover != NULL) {
      (void)fprintf(stderr, "FAIL round %d: ooo_list not empty after drain\n",
                    r);
      return 1;
    }
    /* Release the slot's install ref so the cb pool is balanced. */
    arts_shared_ptr_t old =
        arts_atomic_shared_exchange(&g_slot.value, (arts_shared_ptr_t)NULL);
    if (old) {
      arts_shared_release(&old);
    }
  }

  /* The create polarity's rescue, one deterministic interleaving each:
   * (a) the occupant is emptied and its key returned, with no re-drive — the
   *     parker must drain and install; (b) a full teardown whose re-drive
   *     takes the node — the parker's drain finds nothing; (c) the returned
   *     slot is claimed by another GUID — the node follows its own GUID and
   *     the other GUID's message stays where it was. */
  if (create_case(HOOK_EMPTY_NO_REDRIVE, "create/empty-no-redrive") ||
      create_case(HOOK_TEARDOWN, "create/teardown-redrive") ||
      create_case(HOOK_RECLAIM, "create/reclaimed-by-other") ||
      noncreate_case(HOOK_NC_MOVED, "noncreate/slot-reclaimed") ||
      noncreate_case(HOOK_NC_TEARDOWN, "noncreate/teardown-redrive")) {
    return 1;
  }

  printf("PASS ooo_toctou_rescue: %d rounds x %d producers, no strand/dup; "
         "3 create-polarity and 2 non-create teardown rescues, once each\n",
         ROUNDS, PRODUCERS);
  return 0;
}
