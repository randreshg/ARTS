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
#include "arts/edt.h"
#include "arts/db.h"
#include "arts/utils/malloc.h"

#include <stddef.h>
#include <string.h>

#include "arts/edt_context.h" /* current_edt + run-start/end ctx hooks */
#include "arts/event.h"       /* arts_event_create (finish-scope proxy) */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"   /* outbound send helpers */
#include "arts/transport/protocol.h" /* wire packet structs */
#include "arts/utils/atomics.h"
#include "arts/utils/shared.h" /* arts_shared_ptr_t, get/release */

/* No-hint EDT placement policy: ROUNDROBIN (default) distributes execution
 * rank across all nodes when the caller expresses no placement preference
 * (NULL hint or ARTS_HINT_ANY_RANK), mirroring arts_db_create's NULL-hint
 * home distribution.  CREATOR pins to the calling rank (legacy behavior).
 * CMake sets this for every libarts compile; the fallback covers any TU
 * that pulls in edt.c outside the normal build (e.g. direct inclusion). */
#ifndef ARTS_NOHINT_EDT_ROUNDROBIN
#define ARTS_NOHINT_EDT_ROUNDROBIN 1
#endif

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#endif

#include "arts/cxl/wrapper.h"

/* Per-worker EDT-execution context (current_edt, owned-finish-events list,
 * created-DB tracking + save/restore snapshot) lives in sync/edt_context.c.
 * edt.c reads `current_edt` directly and calls the run-start/run-end context
 * hooks below via that header. */

/*
 * arts_edt_deleter — shared_t deleter.
 *
 * Runs at the cb's last release.  Mirrors the DB pattern (the DB deleter ->
 * arts_db_free): delegates to arts_edt_free, which is the canonical
 * struct-free path.
 *
 * Foreign TUs reach the same pointer via arts_edt_get_deleter().
 */
/* canonical struct-free path; sole caller is arts_edt_deleter in this TU. */
static void arts_edt_free(struct arts_edt_s *edt);

/* cb deleter (route_table deleter-by-kind for ARTS_GUID_EDT).  External
 * linkage so route_table.c can reference it directly. */
void arts_edt_deleter(void *self) { arts_edt_free((struct arts_edt_s *)self); }

/* Publish the EDT cb deleter into the route_table's per-kind table at startup
 * (decoupled registration — see arts_route_table_register_deleter). */
__attribute__((constructor)) static void arts_edt_register_cb_deleter(void) {
  arts_route_table_register_deleter(ARTS_GUID_EDT, arts_edt_deleter);
}

/* Getter for foreign TUs that compare against or install the same deleter
 * pointer. */
void (*arts_edt_get_deleter(void))(void *) { return arts_edt_deleter; }

/* The engine's create args for an EDT received off the wire: the args header,
 * the blob packet, the EDT image, contiguous.  Images that fit are composed on
 * the stack. */
#define EDT_CREATE_ARGS_INLINE 1024

void arts_edt_create_enter(const struct arts_msg_object_blob_packet_s *packet,
                           const void *edt, uint32_t edt_size) {
  struct arts_ooo_args_create_blob_s a = {
      .size = (uint32_t)sizeof(*packet) + edt_size};
  uint32_t args_size = (uint32_t)sizeof(a) + a.size;
  _Alignas(uint64_t) unsigned char inline_args[EDT_CREATE_ARGS_INLINE];
  unsigned char *args = args_size <= sizeof(inline_args)
                            ? inline_args
                            : (unsigned char *)arts_malloc(args_size);
  memcpy(args, &a, sizeof(a));
  memcpy(args + sizeof(a), packet, sizeof(*packet));
  memcpy(args + sizeof(a) + sizeof(*packet), edt, edt_size);
  arts_ooo_dispatch_or_defer_guid(packet->guid, OOO_EDT_CREATE, args,
                                  args_size);
  if (args != inline_args) {
    arts_free(args);
  }
}

/*
 * arts_edt_create_core — build an EDT (header + paramv + depv), assign its
 * GUID, copy its parameters and join its finish scope, then hand it to its
 * home: a remote home receives the serialized image; on the local home the
 * built object itself enters the OoO engine, where the OOO_EDT_CREATE body
 * installs it (or parks it behind a live occupant of the GUID).
 */
bool arts_edt_create_core(struct arts_edt_s *edt, arts_guid_kind_t guid_kind,
                          arts_guid_t *guid, unsigned int rank,
                          unsigned int edt_space, arts_edt_t func_ptr,
                          uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_guid_t hint_finish_event,
                          arts_guid_t hint_output_event, uint32_t flags) {
  if (!edt) {
    edt = (struct arts_edt_s *)arts_calloc_aligned(1, edt_space,
                                                 ARTS_CACHE_LINE_SIZE);
  }
  if (!edt) {
    ARTS_ERROR("EDT allocation failed (size=%u)", edt_space);
  }

  /* lifecycle/deleter handled by the route_table cb (deleter-by-kind) on
   * install.  The only per-object shared field is self_cb (a non-owning alias
   * to that cb): zero-initialised here (calloc / left NULL on a caller-provided
   * buffer) and armed by the create body from its install handle.  Kind comes
   * from the GUID (bits 63-62); total size from
   * arts_edt_total_size(paramc/depc) — no per-object header stores them. */
  (void)edt_space;

  if (*guid == NULL_GUID) {
    *guid = arts_guid_create_for_rank(rank, guid_kind);
  }
  edt->guid = *guid;

  edt->func_ptr = func_ptr;
  edt->depc = depc;
  edt->paramc = paramc;
  edt->depc_needed = depc;

  /* Determine finish-scope for this EDT.
   *
   * Finish scopes are created explicitly via arts_event_create(FINISH); an EDT
   * joins one by passing it in hint.finish_event, otherwise it inherits the
   * caller's ambient finish_event (transitive membership).  Either way the EDT
   * INCRs the scope at create and DECRs it on completion (in
   * arts_unset_thread_local_edt_info).  `current_edt` is the file-static
   * thread-local maintained by arts_set/unset_thread_local_edt_info — direct
   * access, no route_table lookup needed (same TU). */
  arts_guid_t parent_fe;
  if (hint_finish_event != NULL_GUID) {
#if ARTS_LOG_LEVEL >= 3
    /* The join INCR must be local: an INCR shipped to another rank's scope is
     * not ordered against the member's completion DECR. */
    if (arts_guid_get_rank(hint_finish_event) != arts_global_rank_id) {
      ARTS_ERROR("EDT create names finish scope %lu homed on rank %u; a scope "
                 "in hint.finish_event must be homed on the creating rank %u "
                 "(create it here or inherit the ambient scope)",
                 (unsigned long)hint_finish_event,
                 arts_guid_get_rank(hint_finish_event), arts_global_rank_id);
    }
#endif
    parent_fe = hint_finish_event;
  } else if (current_edt) {
    parent_fe = current_edt->finish_event;
  } else {
    parent_fe = NULL_GUID;
  }
  edt->finish_event = parent_fe;
  if (parent_fe != NULL_GUID) {
    /* Join/inherit: INCR completes before the new EDT can reach its own DECR
     * (which runs only after the EDT executes — strictly later in this
     * thread). */
    arts_event_satisfy_slot(parent_fe, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
  }
  /* Output event (per-EDT result channel): never inherited — it belongs to
   * this EDT only.  The run path satisfies it with the EDT's returned GUID
   * after the EDT's data-block releases. */
  edt->output_event = hint_output_event;
  (void)flags;

  /* Copy inline parameter values into the EDT's trailing storage.
   * Layout: [<edt header> | paramv[paramc] | depv[depc]].
   *
   * The header size depends on the EDT subtype: a GPU EDT embeds extra
   * scheduling metadata (grid/block/...) between the base header and the
   * trailing paramv region.  The paramv base must therefore use the SAME
   * subtype-aware offset that arts_get_depv uses to locate depv, otherwise
   * the copy lands on top of that metadata (corrupting grid/block) and the
   * runtime reads params from the wrong place. */
  if (paramc) {
    unsigned int offset = sizeof(struct arts_edt_s);
#ifdef ARTS_USE_GPU
    if (edt->edt_type == ARTS_EDT_GPU) {
      offset = sizeof(arts_gpu_edt_t);
    }
#endif
    ARTS_DEBUG("EDT paramv copy: edt=%p offset=%u paramc=%u depc=%u "
               "edt_space=%u dep_size=%zu",
               (void *)edt, offset, paramc, depc, edt_space,
               depc * sizeof(arts_edt_dep_t));
    char *tmp = (char *)edt + offset;
    memcpy(tmp, paramv, sizeof(uint64_t) * paramc);
  }

  ARTS_INFO("EDT create [Guid:%lu, Depc:%u, Route:%u, FuncPtr:%p]", *guid,
            edt->depc, rank, (void *)func_ptr);

  if (rank != arts_global_rank_id) {
    /* Remote EDT: serialise and send to the target node.  A task's
     * serialized form is the one control message whose size the PROGRAM
     * chooses, so the bound is stated here, where the counts that set it are
     * in scope — the transport's own ceiling would report only bytes. */
    uint64_t wire_total =
        sizeof(struct arts_msg_object_blob_packet_s) + arts_edt_total_size(edt);
    if (wire_total > ARTS_NET_MSG_MAX) {
      ARTS_ERROR("EDT[Guid:%lu] paramc=%u depc=%u serializes to %llu bytes, "
                 "over the %llu-byte control-message bound",
                 *guid, paramc, depc, (unsigned long long)wire_total,
                 (unsigned long long)ARTS_NET_MSG_MAX);
    }
    ARTS_INFO("EDT[Guid:%lu] remote move to rank %u", *guid, rank);
    arts_send_object_blob(rank, *guid, (void *)edt,
                          (unsigned int)arts_edt_total_size(edt),
                          MSG_EDT_CREATE, arts_free);
  } else {
    /* The engine installs this very object; it is the create's until then. */
    struct arts_ooo_args_create_local_s a = {
        .size = ARTS_OOO_CREATE_ADOPT, .guid = *guid, .descriptor = edt};
    arts_ooo_dispatch_or_defer_guid(*guid, OOO_EDT_CREATE, &a, sizeof(a));
  }

  INCREMENT_NUM_EDT_CREATE_BY(1);
  return true;
}

arts_guid_t arts_edt_create(arts_edt_t func_ptr, uint32_t paramc,
                            const uint64_t *paramv, uint32_t depc,
                            const arts_edt_hint_t *hint) {
  TIME_EDT_CREATE_START();

  /* Snapshot hint (NULL = ARTS_EDT_HINT_DEFAULTS).  After this all optional
   * fields are well-defined and follow the documented precedence:
   *   - if .guid != NULL_GUID, the GUID's rank field overrides .rank
   *   - if .rank == ARTS_HINT_ANY_RANK (or hint itself is NULL), the rank is
   *     policy-selected below (ARTS_NOHINT_EDT_ROUNDROBIN) rather than taken
   *     from .rank
   *   - if .finish_event == NULL_GUID, the EDT inherits the caller's ambient
   *     finish scope (handled inside arts_edt_create_core). */
  arts_edt_hint_t snap = hint
                             ? *hint
                             : (arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK,
                                                 .guid = NULL_GUID};

  arts_guid_t guid = snap.guid;
  unsigned int rank;
  if (guid != NULL_GUID) {
    rank = arts_guid_get_rank(guid);
  } else if (hint == NULL || snap.rank == ARTS_HINT_ANY_RANK) {
    /* No placement preference (NULL hint, or an explicit hint that still
     * needs other fields populated but leaves rank unpinned via
     * ARTS_HINT_ANY_RANK — e.g. the OCR shim's finish/output-event-bearing
     * hint).  ROUNDROBIN distributes execution rank across all nodes
     * (mirrors arts_db_create's NULL-hint home distribution); CREATOR
     * reproduces the legacy pin-to-creator behavior.  Both arms resolve to
     * a concrete rank — the sentinel never reaches guid encoding. */
#if ARTS_NOHINT_EDT_ROUNDROBIN
    rank = arts_atomic_fetch_add(&arts_node_info.edt_rr_route, 1U) %
           arts_global_rank_count;
#else
    rank = arts_global_rank_id;
#endif
  } else if (snap.rank != ARTS_HINT_CURRENT_RANK) {
    rank = snap.rank;
  } else {
    rank = arts_global_rank_id;
  }

  unsigned int edt_space = sizeof(struct arts_edt_s) +
                           (paramc * sizeof(uint64_t)) +
                           (depc * sizeof(arts_edt_dep_t));
  bool ok = arts_edt_create_core(
      NULL, ARTS_GUID_EDT, &guid, rank, edt_space, func_ptr, paramc, paramv,
      depc, snap.finish_event, snap.output_event, snap.flags);
  TIME_EDT_CREATE_STOP();
  return ok ? guid : NULL_GUID;
}

/* Register the running EDT's result GUID — delivered as the payload when the
 * run path satisfies the EDT's output_event after its data-block releases.
 * `current_edt` is the worker thread-local; outside a running EDT this is a
 * documented no-op. */
void arts_edt_set_result(arts_guid_t result_guid) {
  if (current_edt) {
    current_edt->output_data = result_guid;
  }
}

static void arts_edt_free(struct arts_edt_s *edt) {
  /* rw_sorted is the GUID-sorted serialized-dep order, allocated once at
   * arts_db_acquire_all entry (NULL if the EDT never reached the acquire
   * phase).  Freeing here — the single canonical struct-free — covers every
   * lifetime end (run completion, cancel, destroy) with no double-free. */
  arts_free(edt->rw_sorted);
  arts_thread_info.edt_free = 1;
  arts_free(edt);
  arts_thread_info.edt_free = 0;
}

/* Retire a completed EDT's GUID by identity: only this EDT's own cb, since
 * under label reuse the slot may already hold the next generation.  A
 * concurrent retire holding the slot is waited out rather than missed: a
 * missed retire would leave the completed EDT installed and park the next
 * create for good.  The detach drops the
 * install ref, so the deleter (arts_edt_deleter -> arts_edt_free) runs at the
 * last release; `edt` stays dereferenceable after the call only while the
 * caller holds its own reference (the run path's runnable-phase ref). */
void arts_edt_delete(struct arts_edt_s *edt) {
  ARTS_INFO("EDT delete [Guid:%lu, Depc:%u, DepcNeeded:%u] on rank %u",
            edt->guid, edt->depc, edt->depc_needed, arts_global_rank_id);
  arts_route_table_set_destroyed_item(edt->guid, edt->self_cb);
}

/* Pure Cat-B body (g_ooo_table[OOO_EDT_DESTROY]).  The EDT is installed and
 * ref-pinned by dispatch_or_defer.  OCR restricts ocrEdtDestroy to
 * pre-runnable EDTs (depc_needed > 0); destroying a runnable/queued/running EDT
 * is UB and is skipped.  The retire names the object the dispatch chose, so it
 * fails when the EDT's own completion retired it first.  A destroyed EDT never
 * completes, so the destroy that wins the retire sends the finish-scope DECR
 * its completion would have: to the local proxy when the scope is remote (its
 * fire forwards the DECR and reclaims it), else to the scope itself.  The
 * output event is left alone: the programming model does not say what a
 * destroyed EDT's output event does. */
void arts_handler_edt_destroy(void *item_v, void *args_v) {
  struct arts_edt_s *edt = (struct arts_edt_s *)item_v;
  struct arts_ooo_args_edt_destroy_s *a =
      (struct arts_ooo_args_edt_destroy_s *)args_v;
  if (edt->depc_needed == 0) {
    ARTS_INFO("EDT destroy on runnable/queued EDT [Guid:%lu] — UB; ignoring",
              a->guid);
    return;
  }
  ARTS_INFO("EDT destroy [Guid:%lu, Depc:%u, DepcNeeded:%u] on rank %u",
            edt->guid, edt->depc, edt->depc_needed, arts_global_rank_id);
  arts_guid_t finish_event = edt->finish_event;
  if (arts_ooo_retire_item(a->guid, item_v) && finish_event != NULL_GUID) {
    arts_event_satisfy_slot(finish_event, NULL_GUID,
                            ARTS_EVENT_LATCH_DECR_SLOT);
  }
}

/* Cross-rank send: forward the destroy to the EDT's home rank (symmetric with
 * arts_send_event_destroy / arts_send_db_destroy). */
void arts_send_edt_destroy(unsigned int home_rank, arts_guid_t guid) {
  struct arts_msg_guid_only_packet_s packet;
  packet.guid = guid;
  arts_fill_packet_header(&packet.header, sizeof(packet), MSG_EDT_DESTROY);
  arts_transport_send_async((int)home_rank, (char *)&packet, sizeof(packet));
}

/* arts_edt_destroy — API: destroy an EDT by GUID.
 *   home != self → MSG_EDT_DESTROY wire (handler runs on the home rank);
 *   home == self → dispatch_or_defer (run the destroy body on the live EDT, or
 *                  defer on the slot until the EDT installs — symmetric with
 * the RX path and the satisfy path's local-home branch). */
void arts_edt_destroy(arts_guid_t guid) {
  /* A GUID's home rank is authoritative; an EDT homed elsewhere is destroyed
   * at its home (the route_table entry + finish-scope accounting live there).
   */
  unsigned int home = arts_guid_get_rank(guid);
  if (home != arts_global_rank_id) {
    arts_send_edt_destroy(home, guid);
    return;
  }
  struct arts_ooo_args_edt_destroy_s a = {.guid = guid};
  arts_ooo_dispatch_or_defer_guid(guid, OOO_EDT_DESTROY, &a, sizeof(a));
}

void *arts_get_depv(void *edt_ptr) {
  struct arts_edt_s *edt = (struct arts_edt_s *)edt_ptr;
  unsigned int paramc = edt->paramc;
  if (edt->edt_type == ARTS_EDT_GPU) {
#ifdef ARTS_USE_GPU
    arts_gpu_edt_t *edtGpu = (arts_gpu_edt_t *)edt_ptr;
    return (void *)((uint64_t *)(edtGpu + 1) + paramc);
#else
    return NULL;
#endif
  }
  return (void *)((uint64_t *)(edt + 1) + paramc);
}

/* arts_get_dep_modes removed — mode now lives in arts_edt_dep_t.mode */

/*
 * arts_edt_satisfy_slot — Satisfy one dependency slot on an EDT.
 *
 * Four dispatch paths:
 *   1. GPU LC invalidation drain (current EDT has pending device-replica
 * invalidations) → force-defer on the wrapper's slot so the replay is ordered
 * after it drains.
 *   2. Local EDT found in route table → write the dep slot and
 *      atomically decrement depc_needed.  If this was the last
 *      dependency (depc_needed hits 0), call arts_handle_ready_edt.
 *   3. Local EDT NOT found (still RESERVED or not yet created) →
 *      dispatch_or_defer on its slot; replayed when the EDT installs.
 *   4. Remote EDT → forward the signal over the network.
 *
 * The OoO replay re-issues arts_edt_satisfy_slot for the target, so the inline
 * hit path above (case 2) IS the single copy of the satisfy logic — the OoO
 * handler does not duplicate it.
 */

/* Defer a satisfy on `edt_guid`'s slot (dispatch-or-defer).  A satisfy
 * carries a GUID/value reference only, so the args are fixed-size —
 * dispatch_or_defer makes its own copy. */
static void edt_defer_satisfy(arts_guid_t edt_guid, arts_guid_t data_guid,
                              uint32_t slot, arts_db_access_mode_t mode) {
  struct arts_ooo_args_edt_satisfy_s a = {
      .edt_guid = edt_guid, .data_guid = data_guid, .slot = slot, .mode = mode};
  arts_ooo_dispatch_or_defer_guid(edt_guid, OOO_EDT_SATISFY_SLOT, &a,
                                  sizeof(a));
}

/* Pure core — apply a satisfy to an already-acquired, valid EDT.  No lookup /
 * acquire / defer: the caller (arts_handler_edt_satisfy_slot via
 * dispatch_or_defer) guarantees `edt` is live.  Writes depv[slot], decrements
 * depc_needed, and schedules the EDT when the last dependency lands. */
static void edt_apply_satisfy(struct arts_edt_s *edt, uint32_t slot,
                              arts_guid_t data_guid,
                              arts_db_access_mode_t mode) {
  arts_edt_dep_t *edt_dep = (arts_edt_dep_t *)arts_get_depv(edt);
  /* (uint32_t)-1 is the "no specific slot" sentinel used by control
   * dependences (registered with slot -1): they decrement readiness without
   * writing any dependence-vector entry.  A real, in-range slot writes its
   * entry and decrements.  ANY OTHER slot is genuinely out of range — it is not
   * one of this EDT's dependences, so it must neither write past the vector nor
   * count toward readiness (else the EDT could fire before its real deps land);
   * ignore it. */
  const uint32_t NO_SLOT = (uint32_t)-1;
  bool writes_slot = (slot != NO_SLOT);
  if (writes_slot && slot >= edt->depc) {
    return;
  }
  if (writes_slot) {
    edt_dep[slot].guid = data_guid;
    /* A satisfy says which block the slot names, not that the EDT may touch
     * it; the acquire path fills ptr when the access has been granted. */
    edt_dep[slot].ptr = NULL;
    edt_dep[slot].mode = mode;
  }
  /* Decrement readiness for both a real in-range slot and the no-slot
   * sentinel; only a genuine out-of-range slot (rejected above) is skipped. */
  unsigned int res = arts_atomic_sub(&edt->depc_needed, 1U);
  ARTS_INFO("Signal EDT[Guid:%lu, Slot:%u] DB[Guid:%lu] depc_needed=%u→%u",
            edt->guid, slot, data_guid, res + 1, res);
  if (res == 0) {
    ARTS_INFO("EDT[Guid:%lu] all deps satisfied — firing", edt->guid);
    arts_handle_ready_edt(edt);
  }
}

/* Home-routed handler (OOO_EDT_SATISFY_SLOT): item is the installed EDT. */
void arts_handler_edt_satisfy_slot(void *item, void *vargs) {
  struct arts_ooo_args_edt_satisfy_s *a =
      (struct arts_ooo_args_edt_satisfy_s *)vargs;
  edt_apply_satisfy((struct arts_edt_s *)item, a->slot, a->data_guid, a->mode);
}

/* arts_edt_satisfy_slot — OCR-standard API: supply depv[slot] on an EDT.
 *   home == self → dispatch_or_defer (acquire the EDT → run the handler, or
 *                  defer on the slot until the EDT installs);
 *   home != self → MSG_EDT_SATISFY_SLOT wire (handler runs on the home rank);
 *   GPU LC (wrapper has outstanding device-replica invalidations) →
 * force-defer on the wrapper's slot; the replay re-signals this EDT after
 * drain. The satisfy logic lives once in edt_apply_satisfy (the handler);
 * this entry only routes. */
void arts_edt_satisfy_slot(arts_guid_t edt_guid, uint32_t slot,
                           arts_guid_t data_guid, arts_db_access_mode_t mode) {
  TIME_EDT_SIGNAL_START();
  INCREMENT_NUM_EDT_SIGNAL_BY(1);

  if (current_edt && current_edt->invalidate_count > 0) {
    /* GPU LC: hold the satisfy until the GPU wrapper EDT's invalidations
     * drain — force-push on the wrapper's slot so the re-signal of this EDT
     * replays only after the wrapper's invalidations drain. */
    struct arts_ooo_args_edt_satisfy_s a = {.edt_guid = edt_guid,
                                            .data_guid = data_guid,
                                            .slot = slot,
                                            .mode = mode};
    arts_ooo_push_guid(current_edt->guid, OOO_EDT_SATISFY_SLOT, &a, sizeof(a));
  } else if (arts_guid_get_rank(edt_guid) == arts_global_rank_id) {
    /* Local home: acquire-or-defer; the handler supplies the dep slot. */
    edt_defer_satisfy(edt_guid, data_guid, slot, mode);
  } else {
    /* Remote home: the satisfy message carries the reference. */
    arts_send_edt_satisfy_slot(edt_guid, data_guid, slot, mode);
  }
  TIME_EDT_SIGNAL_STOP();
}

#ifdef ARTS_USE_GPU
void arts_lc_sync(arts_guid_t edt_guid, uint32_t slot, arts_guid_t data_guid) {
  arts_edt_satisfy_slot(edt_guid, slot, data_guid, DB_MODE_LC_SYNC);
}

void arts_gpu_signal_edt_memset(arts_guid_t edt_guid, uint32_t slot,
                                arts_guid_t data_guid) {
  arts_db_access_mode_t mode = DB_MODE_MEMSET;
  arts_shared_ptr_t dh = arts_route_table_lookup_db(data_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(dh);
  if (db && db->db_type == ARTS_DB_GPU) {
    mode = DB_MODE_LC_NO_COPY;
  }
  if (db) {
    arts_shared_release(&dh);
  }
  arts_edt_satisfy_slot(edt_guid, slot, data_guid, mode);
}
#endif /* ARTS_USE_GPU */

void arts_send_object_blob(unsigned int rank, arts_guid_t guid, void *ptr,
                           unsigned int mem_size, unsigned message_type,
                           void (*free_method)(void *)) {
  TIME_REMOTE_MOVE_START();
  struct arts_msg_object_blob_packet_s packet;
  packet.guid = guid;
  arts_fill_packet_header(&packet.header, sizeof(packet) + mem_size,
                          message_type);
  arts_transport_send_payload_async_free((int)rank, (char *)&packet,
                                         sizeof(packet), (char *)ptr, 0,
                                         mem_size, free_method);
  TIME_REMOTE_MOVE_STOP();
}

/* g_ooo_table[OOO_EDT_CREATE]: the engine runs it while the GUID's slot is
 * empty (`item_v` is NULL).  A local create hands over the EDT it built, which
 * is installed as is; a received blob is copied into a fresh object.  The
 * sentinel (+1 on depc_needed) holds the EDT unrunnable from the install until
 * it is removed below, so the satisfies the install's drain replays, and any
 * that land concurrently, cannot fire it before its finish-scope field is
 * final; exactly one party then observes the 0 transition.  A lost install CAS
 * means another create filled the slot after the engine saw it empty, so this
 * one re-enters the engine and parks behind it — with the same object when it
 * was handed over.  A parked create keeps open the finish-scope INCR its
 * creating rank emitted until it installs. */
void arts_handler_edt_create(void *item_v, void *args_v) {
  (void)item_v;
  const struct arts_ooo_args_create_blob_s *a =
      (const struct arts_ooo_args_create_blob_s *)args_v;
  struct arts_edt_s *edt;
  arts_guid_t guid;
  arts_shared_ptr_t pin;
  if (a->size == ARTS_OOO_CREATE_ADOPT) {
    const struct arts_ooo_args_create_local_s *l =
        (const struct arts_ooo_args_create_local_s *)args_v;
    edt = (struct arts_edt_s *)l->descriptor;
    guid = l->guid;
    edt->depc_needed += 1;
    pin = arts_route_table_install_if_absent(edt, guid, arts_global_rank_id,
                                             false);
    if (pin == NULL) {
      edt->depc_needed -= 1;
      arts_ooo_dispatch_or_defer_guid(guid, OOO_EDT_CREATE, args_v,
                                      (uint32_t)sizeof(*l));
      return;
    }
  } else {
    const unsigned char *packet = (const unsigned char *)(a + 1);
    memcpy(&guid,
           packet + offsetof(struct arts_msg_object_blob_packet_s, guid),
           sizeof(guid));
    size_t size = a->size - sizeof(struct arts_msg_object_blob_packet_s);
    edt = (struct arts_edt_s *)arts_malloc_aligned(size, ARTS_CACHE_LINE_SIZE);
    memcpy(edt, packet + sizeof(struct arts_msg_object_blob_packet_s), size);
    edt->depc_needed += 1;
    pin = arts_route_table_install_if_absent(edt, guid, arts_global_rank_id,
                                             false);
    if (pin == NULL) {
      arts_edt_deleter(edt);
      arts_ooo_dispatch_or_defer_guid(guid, OOO_EDT_CREATE, args_v,
                                      (uint32_t)(sizeof(*a) + a->size));
      return;
    }
  }
  /* `pin` keeps the EDT alive from the install to the end of this body, even
   * when the install's drain replays a destroy that retires it.  self_cb is a
   * non-owning alias of the installed cb, armed from the handle itself (a
   * lookup could return a later generation's cb). */
  edt->self_cb = pin;
  /* A finish scope homed on another rank is joined through a local LATCH
   * proxy whose fire carries the DECR to it; one homed here is decremented
   * directly by the EDT's completion.  A retire clears the cb's tag, so an EDT
   * the install's drain already destroyed gets no proxy (it never
   * completes). */
  if (edt->finish_event != NULL_GUID &&
      arts_guid_get_rank(edt->finish_event) != arts_global_rank_id &&
      arts_shared_tag(pin) == (uint64_t)guid) {
    arts_event_hint_t proxy_hint = ARTS_EVENT_HINT_LATCH(1);
    proxy_hint.auto_destroy = true;
    arts_guid_t proxy = arts_event_create(&proxy_hint);
    arts_add_dependence(proxy, edt->finish_event, ARTS_EVENT_LATCH_DECR_SLOT,
                        DB_MODE_NULL);
    edt->finish_event = proxy;
  }
  /* arts_handle_ready_edt takes the runnable-phase ref from self_cb, which is
   * sound only while `pin` is held; nothing touches the EDT after the
   * release. */
  if (arts_atomic_sub(&edt->depc_needed, 1U) == 0) {
    arts_handle_ready_edt(edt);
  }
  arts_shared_release(&pin);
}

void arts_send_edt_satisfy_slot(arts_guid_t edt, arts_guid_t db, uint32_t slot,
                                arts_db_access_mode_t mode) {
  unsigned int rank = arts_guid_get_rank(edt);
  if (rank == arts_global_rank_id) {
    /* EDT GUID claims a local home but may have migrated — resolve the
     * true owning rank through the route table. */
    rank = arts_route_table_lookup_rank(edt);
  }
  ARTS_INFO(
      "Remote Signal from DB[Guid:%lu] to EDT[Guid:%lu, Slot:%d, Rank:%u]", db,
      edt, slot, rank);

  struct arts_msg_edt_satisfy_slot_packet_s packet;
  packet.edt = edt;
  packet.db = db;
  packet.slot = slot;
  packet.mode = mode;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          MSG_EDT_SATISFY_SLOT);
  arts_transport_send_async((int)rank, (char *)&packet, sizeof(packet));
}
