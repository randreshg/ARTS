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
#ifndef ARTS_RUNTIME_TYPES_H
#define ARTS_RUNTIME_TYPES_H
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file rt.h
 * @brief Internal structures for the ARTS runtime.
 *
 * This header defines internal types used by the runtime implementation:
 * EDT and DataBlock descriptors, event structures, and termination-detection
 * state.  Public types (arts_guid_t, arts_guid_kind_t, etc.) live in arts.h.
 *
 * @note This is an internal header.  User code should include @c arts.h.
 */

#include "arts.h"

#include "arts/defs.h"
/* DB-coherence layout types (arts_db_s / arts_db_cache_s / buffer / home)
 * live in the coherence module's own header; pulled in here so every
 * includer of runtime_types.h sees the DB types. */
#include "arts/coherence/types.h"
#include "arts/utils/lockfree_lifo.h" /* arts_lf_stack_t (event simple deps) */
#include "arts/utils/mpsc.h"          /* arts_mpsc_t (event channel) */
#include "arts/utils/shared.h"        /* arts_shared_ptr_t (EDT runnable ref) */
#include "arts/utils/stdatomic_compat.h"
#include <stdbool.h>
#include <stdint.h>

/* ========================================================================= */
/** @defgroup internal_structs Internal Runtime Structures
 *  These structures are exposed for layout visibility but are managed
 *  entirely by the runtime.  User code should not manipulate them directly.
 *  @{ */

/** Internal EDT descriptor. */
struct arts_edt_s {
  arts_edt_t func_ptr; /**< User function to execute. */
  uint32_t paramc;     /**< Number of static parameters. */
  uint32_t depc;       /**< Number of dependency slots. */
  /* The EDT's own GUID.  Unlike DB/Event (always reached via a route_table
   * lookup whose key already IS the GUID), an EDT is dispatched by raw pointer
   * through the lock-free work-stealing deques — at dispatch there is no key
   * and no handler args, so the GUID must travel inside the struct.  This is a
   * load-bearing identity carrier, NOT a redundant self-GUID: do not remove. */
  arts_guid_t guid;
  arts_guid_t finish_event;          /**< LATCH event for finish-scope tracking.
                                          NULL_GUID = no finish scope.  Final
                                          before the EDT is installed: a
                                          destroy on another thread reads it
                                          from then on. */
  arts_guid_t output_event;          /**< Per-EDT result channel (OCR-style).
                                          NULL_GUID = none.  The run path
                                          satisfies this event with output_data
                                          after the EDT's data-block releases.
                                          Never inherited (unlike finish_event). */
  arts_guid_t output_data;           /**< Result GUID the body registered via
                                          arts_edt_set_result (NULL_GUID if
                                          none); payload of output_event. */
  arts_edt_types_t edt_type;         /**< EDT subtype (DEFAULT=CPU, GPU). */
  /** Dependences not yet satisfied, plus one while the create body that
   * installs the EDT runs.  Every satisfier writes its slot, then decrements
   * acq_rel: the release publishes the slot (and the create body's writes),
   * the decrement that reaches 0 acquires all of them before the acquire
   * phase.  Other reads carry only the value. */
  _Atomic(unsigned int) depc_needed;
  uint32_t rw_cursor;  /**< Index into rw_sorted up to which the EDT's
                            serialized deps have been *secured*: resolved
                            locally, or woken by the grant or reply that
                            resolves them once the slot's bytes are published
                            to the EDT. */
  uint32_t *rw_sorted; /**< Dep visit order: by GUID, then by DESCENDING mode
                            strength within one GUID, ties keeping slot order.
                            The first slot of a GUID's group owns that block's
                            single acquisition (a write acquisition subsumes a
                            read one, so the strongest mode owns it) and every
                            later slot naming it is marked an alias on that
                            acquisition — the order and the classification are
                            decided together, ONCE, when the acquire phase
                            begins (arts_dep_sort_and_classify), before
                            anything fires.
                            Which modes are serialized is the protocol's answer
                            (arts_db_acquire_is_serialized): RW on the arms that
                            make a write wait, RO only on an arm that says so;
                            aliases are on no walk at all. Consumed by
                            rw_fire_from_cursor / arts_db_rw_secure /
                            arts_db_acquire_resolved (never re-derived per dep).
                            NULL until the acquire phase begins; it shares the
                            acquire record's allocation (dep_acq) and goes with
                            it. Both are pure functions of depv (fixed once
                            ready), so the single pass is identity-preserving.
                            */
  /** The acquire phase's per-slot record (struct arts_dep_acq_s, one per
   * dependence slot), allocated zero-filled when the phase begins and freed
   * with the EDT; NULL before. */
  struct arts_dep_acq_s *dep_acq;
  /** Dependences whose data is not yet resolved at this rank, plus one per
   * frame still touching the acquire state (the walk's bias, a resume entry,
   * a replay).  Every frame decrements acq_rel after its slot work (the
   * record, buffer refs, pins, the cursor): the decrement that reaches 0
   * acquires all of it, hands the resolved slots to depv and schedules the
   * EDT.  Increments are taken while the count is above zero. */
  _Atomic(unsigned int) acquire_remaining;
  unsigned int invalidate_count; /**< Outstanding cache invalidations of a GPU
                                      wrapper EDT: set while it is private,
                                      cleared after its host function, which
                                      the stream orders after every read. */
  /* Non-owning alias to this EDT's own route-table control block, armed once
   * the EDT is installed (before it can become runnable).  An EDT is dispatched
   * by raw pointer through the lock-free deques and is held by raw pointer
   * across asynchronous coherence park/resume, so nothing on those paths can
   * recover a control block to ref-count.  This alias lets the runnable-phase
   * code take an owning ref (arts_shared_copy at the ready transition, released
   * at run completion) that keeps the EDT alive even after a concurrent destroy
   * has CAS-detached the route slot.  It holds NO strong count itself (that
   * would be a self-cycle that never frees); the cb pool guarantees the pointer
   * stays valid, and copies are only ever taken while another ref is held. */
  arts_shared_ptr_t self_cb;
} ARTS_ALIGNED_MAX;

/* Total allocation size of an EDT = struct + trailing [paramv | depv].
 * Computed from paramc/depc; no separate object header stores it. */
static inline uint64_t arts_edt_total_size(const struct arts_edt_s *edt) {
  return sizeof(struct arts_edt_s) +
         ((uint64_t)edt->paramc * sizeof(uint64_t)) +
         ((uint64_t)edt->depc * sizeof(arts_edt_dep_t));
}

/** Forward-declared dep node (definition in arts/event.h). */
struct arts_event_dep_s;

/** Internal event descriptor — single generic type, hint-driven behavior.
 *  The `is_channel` discriminator selects which union arm is active.
 *
 *  Non-CHANNEL semantics (`is_channel == 0`):
 *    - The latch decrements per LATCH_DECR satisfy and increments per
 *      LATCH_INCR satisfy; the event fires when a decrement brings it to
 *      exactly 0, never at creation and never on an increment.
 *    - Fire is a state transition (moves `state` to FIRED, publishes `data`,
 *      drains `deps_stack`): the event lingers to serve late binders until it
 *      is reclaimed, by `arts_event_destroy`, at the fire when `auto_destroy`,
 *      or when a `counted` `nb_deps` reaches zero.  Over-satisfy past the fire
 *      is silently absorbed.
 *
 *  CHANNEL semantics (`is_channel == 1`):
 *    - One packed word counts the unpaired satisfies and dependences; a
 *      pairable word (both counts non-zero) is a drain in progress.
 *    - The drainer pops one node from each queue per pair, signals, and
 *      subtracts the pairs it served.
 *    - No auto-destroy; only explicit `arts_event_destroy`. */
struct arts_event_s {
  uint8_t is_channel; /* discriminator: 0=simple, 1=channel */

  union {
    struct {
      /* The whole destroy predicate in ONE word — see event_arbiter.c.  Kept
       * apart, its axes are a Dekker pair: each participant writes one and
       * reads the other, and without a StoreLoad fence on both sides they miss
       * each other and the event is never reclaimed.  Packed, one CAS moves an
       * axis and observes the rest, and the decision is bound to the atom that
       * causes it. */
      /* [destroyed:1|fire_st:2|nb_deps:29|latch:32].  Committed only by a CAS
       * (acq_rel, from an acquire load) through event_compute_next.  The
       * FIRING->FIRED CAS releases `data`, which the IDLE->FIRING winner
       * writes in between; an acquire load that reads FIRED takes it. */
      _Atomic(uint64_t) state;
      /* Immutable after creation, so they race with nothing and enter the
       * decider as arguments rather than living in the word. */
      uint8_t counted;      /* nb_deps is a declared consumer count */
      uint8_t auto_destroy; /* destroy on fire, consumers ignored */
      uint8_t discard_data; /* publish no payload */
      arts_guid_t data;           /* last satisfy data; late binders read */
      arts_lf_stack_t deps_stack; /* Treiber stack of pending consumers */
    } simple;
    struct {
      /* [deps:32 | sat:32]: the unpaired dependences and satisfies, each
       * counted after its node is pushed.  While the event is live only
       * acq_rel RMWs touch it: a producer's fetch_add claims the drain when
       * its result makes the word pairable (both fields non-zero); the
       * drainer's fetch_sub of the pairs it served keeps the drain iff its
       * result is still pairable.  So a pairable word is exactly a drain in
       * progress, and this RMW chain orders every counted node's push, and
       * the queues' consumer-owned tails, before the next drainer's pops.
       * The deleter's Debug check reads it relaxed, at strong == 0, ordered
       * after every handler by the reference count. */
      _Atomic(uint64_t) state;
      arts_mpsc_t data_queue; /* satisfy FIFO */
      arts_mpsc_t dep_queue;  /* dependence FIFO */
    } channel;
  };
} ARTS_ALIGNED_MAX;

/** @} */ /* end internal_structs */

/* ========================================================================= */
#ifdef __cplusplus
}
#endif

#endif /* ARTS_RUNTIME_TYPES_H */
