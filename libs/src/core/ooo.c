/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

#include "arts/ooo.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h> /* memcpy (OoO payload alloc) */

#include "arts.h"
#include "arts/coherence/coherence.h" /* arts_handler_db_acquire */
#include "arts/coherence/handlers.h"  /* coherence wire handlers + replay */
#include "arts/db.h"                  /* arts_db_acquire_all */
#include "arts/edt.h"   /* arts_handler_edt_satisfy_slot[_ptr], arts_get_depv */
#include "arts/event.h" /* arts_handler_event_satisfy_slot / add_dependence */
#include "arts/gas/route_table.h"
#include "arts/system/print.h" /* ARTS_WARN / ARTS_INFO */
#include "arts/system/schedfuzz.h"
#include "arts/utils/lockfree_lifo.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"
#include "arts/counter/Preamble.h"

/* ===========================================================================
 * OoO engine — unified dispatch_or_defer.
 *
 * One Treiber stack (ooo_list) per route_table slot accumulates deferred
 * operations that arrived before their target object was installed.  Every
 * deferred operation is one arts_ooo_payload_s node (link first, kind tag,
 * trailing args blob).
 *
 * Two polarities, one engine:
 *   - Create body (OOO_*_CREATE) runs while the slot is empty: it installs
 *     the object, which drains the slot.  While the slot is occupied the
 *     create parks; the occupant's destroy re-drives it and it installs as
 *     the GUID's next generation.  Several parked creates of one GUID are the
 *     undefined concurrent-create shape: each destroy admits one of them and
 *     the rest re-park, in no promised order.
 *   - Non-create handler (Cat B/C) receives an already-acquired, valid item
 *     from dispatch_or_defer and operates on it — no lookup/acquire/push in
 *     the handler body.  The g_ooo_table[kind] entries are these handler
 *     bodies, defined in each subsystem TU.
 *
 * Concurrency of the non-create polarity (lock-free, per-call acquire):
 *   - A producer's dispatch_or_defer reloads slot.value every call.  HIT
 *     (value != NULL) → run the handler with a ref pinned across the call.
 *     MISS (value == NULL) → push the payload; then re-check value and key
 *     and, if an installer raced in or the slot was returned, drain (so the
 *     node is not stranded).  A payload whose slot no longer names its GUID
 *     follows the GUID to the slot that names it now; it is never dropped.
 *   - A producer only ever pushes while value == NULL.  Once value is
 *     installed, every producer HITs and dispatches inline (never pushes), so
 *     no push races a create handler's drain.  Pre-install pushes are caught
 *     by the install's drain snapshot; a push that loses that race triggers
 *     its own drain via the post-push re-check.  No drain lock is needed.
 *   - drain takes ONE reverse_drain snapshot and walks it once.  A node that
 *     MISSes mid-walk (a destroy earlier in the same walk NULLed the slot)
 *     re-pushes onto a fresh chain to await the next install (labeled-GUID
 *     reuse) — it is not re-walked in this pass, so no spin.
 * ===========================================================================*/

/* ===== g_ooo_table — kind → replay handler ================================
 * Each handler replays the operation against the now-installed target by
 * re-issuing the original entry (internal_signal_edt, arts_event_satisfy_slot,
 * arts_handler_db_*, ...).  The entry's own lookup HITs during a drain
 * (drain runs post-install), takes its inline hit path, and does NOT re-enter
 * dispatch_or_defer — so re-issue is one level deep, never recursive.  `item`
 * (the acquired object) is passed through for the one handler (db_acquire)
 * that consumes it directly. */

/* EDT satisfy + destroy handlers live in edt.c
 * (arts_handler_edt_satisfy_slot[_ptr], arts_handler_edt_destroy) — pure cores
 * that write the acquired EDT's dep slot / detach the slot on destroy. */

/* Event satisfy / add-dependence / destroy handlers live in event.c
 * (arts_handler_event_satisfy_slot / arts_handler_event_add_dependence /
 * arts_handler_event_destroy) — pure cores that operate on the acquired
 * event. */

/* The OoO replay of a deferred local DB→EDT dependency is the coherence acquire
 * handler itself: arts_handler_db_acquire(item = installed db_s, args). The
 * drain only fires when the slot value is non-NULL (DB installed), so `item` is
 * always a valid db_s; the handler re-attempts the single dep through the
 * proper coherence path (writer_count / buffer-ref handling) and self-accounts
 * / parks. No whole-driver re-run. */

/* The coherence replay bodies are the wire handlers themselves
 * (arts_handler_db_grant_request / _snapshot_request / _publish /
 * _destroy) — pure (item, args) Cat-B bodies defined in the coherence TUs.
 * Each model's enum (and this table) carries only that model's OOO_DB_* kinds,
 * so a build references only the bodies it actually defines:
 *   - GRANT_REQUEST: VAL and INV only (grant.c).
 *   - PUBLISH: WT only; WB's enum omits it (WB fatals on the wire).
 *   - GRANT_INVALIDATE: WT only.  WT can see a GRANT/INVALIDATE
 * reorder (or a before-create race) that lands INVALIDATE before the cache
 * installs, so it defers + replays here.  The WB write policy never defers
 * INVALIDATE (home publishes the rw_holder target only after that rank's
 * cache install, so the dispatcher/self-send call the body directly). */

/* Event/EDT destroy replay bodies are the wire handlers themselves
 * (arts_handler_event_destroy / arts_handler_edt_destroy) — pure (item, args)
 * Cat-B bodies defined in event.c / edt.c.  The item is installed (drain runs
 * post-install) and ref-pinned by dispatch_or_defer; the body performs the
 * destroy action (route_table_set_destroyed) directly on it. */

/* Mirrors the per-model ooo_kind enum: the model-agnostic slots are always
 * present, and each build's OOO_DB_* arm initializes only that model's kinds
 * (each kind ↔ its handler 1:1). */
static const arts_ooo_handler_fn_t g_ooo_table[OOO_KIND_COUNT] = {
    [OOO_EDT_CREATE] = arts_handler_edt_create,
    [OOO_EVENT_CREATE] = arts_handler_event_create,
    [OOO_DB_CREATE] = arts_handler_db_create,
    [OOO_EVENT_SATISFY_SLOT] = arts_handler_event_satisfy_slot,
    [OOO_EDT_SATISFY_SLOT] = arts_handler_edt_satisfy_slot,
    [OOO_EVENT_ADD_DEPENDENCE] = arts_handler_event_add_dependence,
    [OOO_EDT_DESTROY] = arts_handler_edt_destroy,
    [OOO_EVENT_DESTROY] = arts_handler_event_destroy,
    [OOO_DB_DESTROY] = arts_handler_db_destroy,
#if defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_PURGE)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_EXCL_REQUEST] = arts_handler_db_excl_request,
    [OOO_DB_EXCL_RELEASE] = arts_handler_db_excl_release,
#elif defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_EXCL_REQUEST] = arts_handler_db_excl_request,
#elif defined(ARTS_PROTOCOL_INV)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_GRANT_REQUEST] = arts_handler_db_grant_request,
    [OOO_DB_INV_REQUEST] = arts_handler_db_inv_request,
    [OOO_DB_PUBLISH] = arts_handler_db_publish,
#ifdef ARTS_RELEASE_PURGE
    [OOO_DB_GRANT_RETURN] = arts_handler_db_grant_return,
#endif
#ifdef ARTS_WRITE_POLICY_WB
    [OOO_DB_INV_REDIRECT] = arts_handler_db_inv_redirect,
#endif
#elif defined(ARTS_WRITE_POLICY_WT)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_SNAPSHOT_REQUEST] = arts_handler_db_snapshot_request,
    [OOO_DB_GRANT_REQUEST] = arts_handler_db_grant_request,
    [OOO_DB_PUBLISH] = arts_handler_db_publish,
#ifdef ARTS_RELEASE_PURGE
    [OOO_DB_GRANT_RETURN] = arts_handler_db_grant_return,
#endif
#elif defined(ARTS_WRITE_POLICY_WB)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_SNAPSHOT_REQUEST] = arts_handler_db_snapshot_request,
    [OOO_DB_GRANT_REQUEST] = arts_handler_db_grant_request,
#elif defined(ARTS_PROTOCOL_FLUSH)
    [OOO_DB_ACQUIRE] = arts_db_acquire_replay_dep,
    [OOO_DB_FETCH_REQUEST] = arts_handler_db_fetch_request,
    [OOO_DB_FLUSH_ANNOUNCE] = arts_handler_db_flush_announce,
#endif
};

/* ===== payload alloc ====================================================== */

static struct arts_ooo_payload_s *
arts_ooo_payload_alloc(ooo_kind_t kind, arts_guid_t guid, const void *args,
                       uint32_t args_size) {
  struct arts_ooo_payload_s *p = (struct arts_ooo_payload_s *)arts_malloc(
      sizeof(struct arts_ooo_payload_s) + args_size);
  p->kind = kind;
  p->guid = guid;
  p->args_size = args_size;
  if (args_size > 0 && args != NULL) {
    memcpy(arts_ooo_payload_args(p), args, args_size);
  }
  return p;
}

/* ===== dispatch_or_defer ================================================== */

/* The handle pinned for the non-create handler running on this thread. */
static ARTS_THREAD_LOCAL arts_shared_ptr_t ooo_dispatched;

arts_shared_ptr_t arts_ooo_dispatched_handle(const void *item) {
  arts_shared_ptr_t h = ooo_dispatched;
  return (h != NULL && arts_shared_get(h) == item) ? h : NULL;
}

bool arts_ooo_retire_item(arts_guid_t key, const void *item) {
  arts_shared_ptr_t h = arts_ooo_dispatched_handle(item);
  if (h == NULL) {
    return arts_route_table_set_destroyed_object(key, item);
  }
  return arts_route_table_set_destroyed_item(key, h);
}

/* Runs between a park's push and its rescue re-read: a scheduling-fuzz point
 * in the runtime, and where a whitebox test that compiles this file drives a
 * concurrent install or teardown into exactly that window. */
#ifndef OOO_AFTER_PARK
#define OOO_AFTER_PARK(slot) ((void)(slot), arts_sched_fuzz_point())
#endif

static bool ooo_kind_is_create(ooo_kind_t k) {
  return k == OOO_EDT_CREATE || k == OOO_EVENT_CREATE || k == OOO_DB_CREATE;
}

/* The create polarity: run the body while the slot is empty, park while it is
 * occupied.  A parked create is dispatched by the drain that follows the
 * occupant's destroy (the teardown's re-drive), finds the slot empty, and
 * installs.  A create whose body loses the install CAS re-enters here and
 * parks behind the object that won it. */
static void ooo_dispatch_create(struct arts_route_item_s *slot,
                                struct arts_ooo_payload_s *payload,
                                ooo_kind_t kind, arts_guid_t guid,
                                const void *args, uint32_t args_size) {
  for (;;) {
    /* A returned slot answers for another GUID now (or none); the create
     * follows its own GUID to the slot that names it. */
    if (__atomic_load_n(&slot->key, __ATOMIC_ACQUIRE) != guid) {
      arts_route_table_reserve_or_lookup(guid, &slot);
      continue;
    }
    arts_shared_ptr_t h = arts_atomic_shared_load(&slot->value);
    if (h == NULL) {
      g_ooo_table[kind](NULL, (void *)args);
      if (payload != NULL) {
        arts_free(payload); /* drain context: the popped node is consumed */
      }
      return;
    }
    /* A value published under another key means the slot changed hands
     * between the key check and the load; resolve again.  This cannot repeat
     * on one slot: an object is only installed into a slot that named its key
     * at the install (install_if_absent), so a slot keyed `guid` never keeps
     * a stranger's value.  This loop terminates only because EVERY install
     * path checks the key in the same atom as the install. */
    bool ours = arts_shared_tag(h) == (uint64_t)guid;
    arts_shared_release(&h);
    if (ours) {
      break;
    }
  }

  /* Occupied — park behind the occupant. */
  if (payload == NULL) {
    payload = arts_ooo_payload_alloc(kind, guid, args, args_size);
  }
  INCREMENT_NUM_OO_ENQUEUE_BY(1);
  arts_lf_stack_push(&slot->ooo_list, &payload->link);
  OOO_AFTER_PARK(slot);

  /* TOCTOU rescue, the non-create one inverted.  The occupant's teardown
   * clears the value, returns the key, then (behind a full fence) re-drives
   * the chain; we push, then (behind a full fence) re-read the value and the
   * key.  Each stores before it loads the other's word, so at least one of us
   * sees the other: either the teardown's re-drive carries our node, or we see
   * the slot emptied or returned and drain it ourselves.  Draining — never
   * re-entering with a copy — keeps the node single-owner: whichever side
   * detaches it from the chain dispatches it, and the other finds it gone. */
  atomic_thread_fence(memory_order_seq_cst);
  arts_shared_ptr_t h = arts_atomic_shared_load(&slot->value);
  bool occupied = (h != NULL) && arts_shared_tag(h) == (uint64_t)guid;
  if (h != NULL) {
    arts_shared_release(&h);
  }
  if (!occupied || __atomic_load_n(&slot->key, __ATOMIC_ACQUIRE) != guid) {
    arts_ooo_drain(slot);
  }
}

void arts_ooo_dispatch_or_defer(struct arts_route_item_s *slot,
                                struct arts_ooo_payload_s *payload,
                                ooo_kind_t kind, arts_guid_t guid,
                                const void *args, uint32_t args_size) {
  if (ooo_kind_is_create(kind)) {
    ooo_dispatch_create(slot, payload, kind, guid, args, args_size);
    return;
  }
  arts_shared_ptr_t h;
  for (;;) {
    /* Identity check before anything else: destroy returns the slot, so this
     * slot may belong to a different GUID (or none) by now.  Dispatching then
     * would run the handler against the wrong object, and parking would put
     * the payload on a stranger's list.  A GUID comes back when it is created
     * again, so the payload follows its GUID to the slot that names it now. */
    if (__atomic_load_n(&slot->key, __ATOMIC_ACQUIRE) != guid) {
      arts_route_table_reserve_or_lookup(guid, &slot);
      continue;
    }
    /* Per-call acquire: (re)load the slot value every entry so a destroy that
     * NULLed it earlier in the same drain walk is observed here. */
    h = arts_atomic_shared_load(&slot->value);
    /* Verify the VALUE, not the slot.  The check above only proves the slot
     * was ours BEFORE the load; a reclaim between the two would hand us the
     * next owner's object, and dispatching this payload against it would run
     * a handler over storage of a different kind.  Re-reading the slot's key
     * is not a proof — the word is not monotone, so it can match again around
     * a stranger's value.  The pinned cb's tag is: it names the key the object
     * was published under, and the ref from the load keeps that cb from being
     * recycled underneath the comparison.  A mismatch means the slot changed
     * hands, so the payload resolves its GUID again. */
    if (h != NULL && arts_shared_tag(h) != (uint64_t)guid) {
      arts_shared_release(&h);
      arts_route_table_reserve_or_lookup(guid, &slot);
      continue;
    }
    break;
  }
  if (h) {
    void *item = arts_shared_get(h);
    /* Ref pinned across the whole handler call — a concurrent destroy's
     * exchange-to-NULL drops only the install ref; `h` keeps the object alive
     * until we release below.  The handle is published to the handler's
     * thread (nested dispatches stack), so a body can retire the object it was
     * dispatched on by identity. */
    arts_shared_ptr_t outer = ooo_dispatched;
    ooo_dispatched = h;
    g_ooo_table[kind](item, (void *)args);
    ooo_dispatched = outer;
    arts_shared_release(&h);
    if (payload != NULL) {
      arts_free(payload); /* drain context: the popped node is consumed */
    }
    return;
  }

  /* Miss — defer. */
  if (payload == NULL) {
    payload =
        arts_ooo_payload_alloc(kind, guid, args, args_size); /* fresh entry */
  }
  /* else: drain re-entry — reuse the same payload (no alloc/free).
   *
   * The ooo_list is consumed ONLY by whole-chain reverse_drain (a single
   * atomic_exchange of the head); there is deliberately NO single-node pop.
   * That is what makes re-pushing a node back onto the same stack ABA-safe
   * under allocator address reuse — a push only links to "whatever is on top
   * now" and never caches a head->next for a CAS.  Do NOT add a single-node
   * pop on this stack. */
  INCREMENT_NUM_OO_ENQUEUE_BY(1);
  arts_lf_stack_push(&slot->ooo_list, &payload->link);
  OOO_AFTER_PARK(slot);

  /* TOCTOU rescue, against an install and against a teardown.  An installer
   * publishes the value and then, behind a full fence, detaches the chain; a
   * teardown clears the value, returns the key and then, behind a full fence,
   * detaches the chain.
   * We push and then, behind a full fence, re-read the value and the key.
   * Each side stores before it loads the other's word, so at least one sees
   * the other: either the installer's drain or the teardown's re-drive carries
   * our node, or we see the value published or the key moved and drain the
   * slot ourselves.  Draining — never re-entering with a copy — keeps the node
   * single-owner: whichever side detaches it dispatches it (to the installed
   * object, or on to the slot its GUID names now), and the other finds it
   * gone. */
  atomic_thread_fence(memory_order_seq_cst);
  h = arts_atomic_shared_load(&slot->value);
  bool installed = (h != NULL);
  if (h) {
    arts_shared_release(&h);
  }
  if (installed || __atomic_load_n(&slot->key, __ATOMIC_ACQUIRE) != guid) {
    arts_ooo_drain(slot);
  }
}

void arts_ooo_dispatch_or_defer_guid(arts_guid_t guid, ooo_kind_t kind,
                                     const void *args, uint32_t args_size) {
  arts_route_item_t *slot;
  arts_route_table_reserve_or_lookup(guid, &slot);
  arts_ooo_dispatch_or_defer(slot, NULL, kind, guid, args, args_size);
}

void arts_ooo_push_guid(arts_guid_t guid, ooo_kind_t kind, const void *args,
                        uint32_t args_size) {
  arts_route_item_t *slot;
  arts_route_table_reserve_or_lookup(guid, &slot);
  struct arts_ooo_payload_s *payload =
      arts_ooo_payload_alloc(kind, guid, args, args_size);
  INCREMENT_NUM_OO_ENQUEUE_BY(1);
  arts_lf_stack_push(&slot->ooo_list, &payload->link);
}

/* ===== drain ============================================================== */

void arts_ooo_drain(struct arts_route_item_s *slot) {
  /* One snapshot, walked once.  Re-pushed misses land on a fresh chain and
   * await the next install's drain. */
  arts_lf_link_t *head = arts_lf_stack_reverse_drain(&slot->ooo_list);
  while (head != NULL) {
    /* Save next first: dispatch_or_defer may re-push this node (re-setting its
     * link->next) on a miss. */
    arts_lf_link_t *next =
        atomic_load_explicit(&head->next, memory_order_relaxed);
    struct arts_ooo_payload_s *payload = (struct arts_ooo_payload_s *)head;
    arts_ooo_dispatch_or_defer(slot, payload, payload->kind, payload->guid,
                               arts_ooo_payload_args(payload),
                               payload->args_size);
    head = next;
  }
}

void arts_ooo_drain_guid(arts_guid_t guid) {
  arts_route_item_t *slot;
  arts_route_table_reserve_or_lookup(guid, &slot);
  arts_ooo_drain(slot);
}

/* Take the whole chain off a slot that is no longer bound to the GUIDs on it,
 * and send each node back through resolution so it lands wherever its own GUID
 * lives now.  Used by a teardown after it returns the slot: a node parked an
 * instant too late must not be stranded on a slot another identity is about to
 * claim.  A node whose GUID is simply gone re-defers on a fresh reservation and
 * is reaped at shutdown, the same as any request for an object that never
 * arrives. */
void arts_ooo_redrive_all(struct arts_route_item_s *slot) {
  arts_lf_link_t *head = arts_lf_stack_reverse_drain(&slot->ooo_list);
  while (head != NULL) {
    arts_lf_link_t *next =
        atomic_load_explicit(&head->next, memory_order_relaxed);
    struct arts_ooo_payload_s *payload = (struct arts_ooo_payload_s *)head;
    arts_route_item_t *target;
    arts_route_table_reserve_or_lookup(payload->guid, &target);
    arts_ooo_dispatch_or_defer(target, payload, payload->kind, payload->guid,
                               arts_ooo_payload_args(payload),
                               payload->args_size);
    head = next;
  }
}

void arts_ooo_free_all(struct arts_route_item_s *slot) {
  arts_lf_link_t *head = arts_lf_stack_reverse_drain(&slot->ooo_list);
  while (head != NULL) {
    arts_lf_link_t *next =
        atomic_load_explicit(&head->next, memory_order_relaxed);
    struct arts_ooo_payload_s *payload = (struct arts_ooo_payload_s *)head;
    /* A parked create in the adopt shape owns the object it never
     * installed. */
    if (ooo_kind_is_create(payload->kind) &&
        payload->args_size == sizeof(struct arts_ooo_args_create_local_s)) {
      const struct arts_ooo_args_create_local_s *a =
          (const struct arts_ooo_args_create_local_s *)arts_ooo_payload_args(
              payload);
      if (a->size == ARTS_OOO_CREATE_ADOPT) {
        if (payload->kind == OOO_DB_CREATE) {
          arts_shared_ptr_t cb = (arts_shared_ptr_t)a->descriptor;
          arts_shared_release(&cb);
        } else {
          arts_route_table_delete_unpublished(a->guid, a->descriptor);
        }
      }
    }
    arts_free(payload);
    head = next;
  }
}
