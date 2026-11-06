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
#ifndef ARTS_GAS_ROUTETABLE_H
#define ARTS_GAS_ROUTETABLE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts.h"
#include "arts/defs.h"
#include "arts/utils/lockfree_lifo.h" /* arts_lf_stack_t (per-slot OoO chain) + arts_lf_link_t */
#include "arts/utils/shared.h" /* arts_shared_ptr_t, arts_shared_slot_t */
#include "arts/utils/stdatomic_compat.h"

/* Convention: all shared-object access is via caller-owned cb handles
 * (lookup_* / _acquire → handle; release required). No raw no-ref peeks. */

#define COLLISION_RESOLVES 8

/* Key values that name no object.  0 = FREE (claimable).  RETIRING is the key
 * a teardown parks in the slot while it detaches the value.  It is the KEY
 * word, not a lock beside it: one CAS from the GUID to this sentinel carries
 * both halves of what a teardown needs — proof the slot still names that
 * GUID, and sole ownership of the teardown — so a loser is simply not the
 * destroyer.  The retire leaves it within a few atomics, returning the slot
 * (0) or handing the key back when it found another object; only the retirer
 * knows which, so a reservation whose probe sequence covers the slot, and a
 * retire of the same object, wait that window out.  It is not 0, so a claim
 * skips the slot, and it carries kind ARTS_GUID_RESERVED (never a valid
 * object), so it equals no real GUID and a key search stops matching at once
 * — a request arriving mid-teardown resolves to a new reservation of the key,
 * which publishes only after the retire settles. */
#define ARTS_ROUTE_KEY_RETIRING ((arts_guid_t)1)

/* Route_item: GUID slot.  Lives as long as its segment, which lives until
 * teardown.
 *
 * An all-zero item is a free slot (key 0) with an absent value (cb NULL,
 * external count 0) and an empty OoO list (head NULL).  A segment's slots are
 * zero pages that are never initialized one by one: a page of slots is
 * populated only by a write to one of its slots.
 *
 * `value` is an atomic shared-ptr cb slot (arts_shared_ptr_t).  The cb
 * carries the strong refcount, the per-object deleter and the object pointer;
 * absence is value == NULL, and a retire exchanges the value to NULL.  No
 * generation is kept: a lookup acquires a caller-owned ref via
 * arts_shared_slot_load and accepts the object only while the pinned cb's
 * tag names the key looked up, and a retire clears that tag.  A retire
 * returns the slot and re-drives its OoO chain by each payload's own GUID. */
struct arts_route_item_s {
  /* Written only by a CAS on the exact word it leaves, or by a store out of a
   * state its writer alone holds:
   *   claim: acq_rel CAS 0 or LOST(u) -> BUSY, then the BUSY owner's release
   *     store BUSY -> CLAIM(v), claim_key written before it;
   *   kill (any reserver of the key): acq_rel CAS CLAIM(v) -> LOST(v);
   *   publish (the owner or a helper): acq_rel CAS CLAIM(v) -> the GUID;
   *   free (the claim's owner): CAS LOST(v) -> 0, release, relaxed on failure;
   *   fresh publish (a create of a GUID it minted, which nothing names yet):
   *     acq_rel CAS 0 or LOST(u) -> the GUID;
   *   retire: acq_rel CAS GUID -> RETIRING, then the retirer's release store
   *     of 0 or of the GUID handed back;
   *   a mirror table's claim (arts_route_table_search_for_empty: acq_rel CAS
   *     0 -> GUID) and a mirror slot's teardown (relaxed store of 0 after the
   *     teardown's stop).
   * BUSY, CLAIM(v) and LOST(v) carry kind ARTS_GUID_RESERVED and equal no
   * GUID.  Every load that decides on the word is acquire, so what a writer
   * did to the slot before its write is visible to a reader that sees the
   * write; loads once the other runtime threads have stopped are relaxed. */
  _Atomic(arts_guid_t) key;
  /* value == NULL means "absent" — deliberately NOT distinguishing
   * "never created" from "destroyed".  The OoO engine treats both
   * uniformly and resolves by kind: a create parks while the slot is
   * occupied and runs while it is empty; a Cat B request/publish parks while
   * it is empty until the next install; a Cat C response/ack, which never
   * enters the engine, is dropped on absent.
   *
   * key == 0 means the slot is FREE.  A destroy returns the slot by zeroing
   * the key, so occupancy tracks live objects rather than every object the
   * run ever made, and a returned slot is claimed exactly like a never-used
   * one. */
  /* cb: event/db/edt (NULL = absent).  Orders: a slot's
   * (arts_shared_slot_t, shared.h). */
  _Atomic(arts_shared_slot_t) value;
  arts_lf_stack_t ooo_list; /* OoO defer chain (Treiber) */
  /* The key a live or lost claim stands for.  Written (relaxed) by the claim's
   * owner while the key word is BUSY, before its release store of CLAIM(v);
   * read (relaxed) after an acquire load of the key word that saw CLAIM(v),
   * so the key word orders it: the read returns v's key, or a later claim's
   * once v ended (every CAS naming v then fails, and skipping is safe). */
  _Atomic(arts_guid_t) claim_key;
  /* Versions of this slot's claims; written and read only by the owner of a
   * BUSY key word, ordered between successive owners by the key word (the
   * release store or RMW that frees the slot, the acq_rel CAS that claims it).
   */
  uint64_t claim_ver;
} ARTS_ALIGNED_MAX;

typedef struct arts_route_item_s arts_route_item_t;

typedef struct arts_route_table_s arts_route_table_t;

/* How one kind of segment is made and released.  Immutable; a segment points
 * at the table of the kind that built it. */
typedef struct arts_route_segment_ops_s {
  /* A segment of `windows` (= 2^`shift`) windows that no other thread can
   * reach: header built, next NULL, slots an untouched mapping.  A key's
   * window is the top `shift` bits of its hash, so a larger shift probes past
   * the segment's slots and a smaller one never reaches its upper windows.
   * NULL, with errno kept and nothing left allocated or registered, when a
   * mapping fails. */
  arts_route_table_t *(*build)(unsigned int windows, unsigned int shift);
  /* Releases a segment no other thread can reach: a lost race's own segment,
   * or any segment at teardown. */
  void (*discard)(arts_route_table_t *segment);
} arts_route_segment_ops_t;

struct arts_route_table_s {
  arts_route_item_t *data;
  unsigned int size;
  unsigned int shift;
  /* NULL until one CAS links the next segment; never written again before
   * teardown.  Every thread that finds NULL builds its own segment and CASes
   * it in (acq_rel: the release half publishes the segment, and the acquire
   * half only completes the pair a failure order needs; failure acquire); a
   * loser discards its own, which no thread saw.  Every load is acquire: it
   * publishes the segment's header and slot mapping. */
  _Atomic(struct arts_route_table_s *) next;
  const arts_route_segment_ops_t *ops; /* published with the header */
};

/* `slots` zero items in an anonymous mapping, page-aligned, populated only
 * where written; NULL with errno kept when the mapping fails. */
arts_route_item_t *arts_route_table_try_map_slots(uint64_t slots);
/* Releases a mapping arts_route_table_try_map_slots returned. */
void arts_route_table_unmap_slots(arts_route_item_t *data, uint64_t slots);

typedef struct {
  uint64_t index;
  arts_route_table_t *table;
} arts_route_table_iterator_t;

/* A chain's first segment, built by `ops`; a failed mapping ends the process,
 * since a root has no other builder to fall back on. */
arts_route_table_t *
arts_route_table_new_root(const arts_route_segment_ops_t *ops,
                          unsigned int windows, unsigned int shift);
/* arts_route_table_new_root with the CPU segment operations, the segments of
 * a rank's route tables. */
arts_route_table_t *arts_route_table_new(unsigned int windows,
                                         unsigned int shift);

/* Builds this rank's route tables: arts_guid_table_count of them, each a root
 * of `windows` (= 2^`shift`) windows, and the placement every key's table is
 * computed from.  Once, after arts_guid_node_geometry_init and before any
 * runtime thread exists or any message is taken in. */
void arts_route_table_node_init(unsigned int windows, unsigned int shift);
/* Frees every route table of this rank and clears the placement, once no
 * thread can reach them. */
void arts_route_table_node_cleanup(void);

/* ---------------------------------------------------------------------------
 * cb-based item lifecycle API.
 *
 * The slot holds an atomic shared-ptr cb.  An object is wrapped in its cb
 * (deleter chosen by GUID kind) before its install, and the object's self_cb
 * aliases it; lookups return a caller-owned cb handle the caller releases when
 * done; destroy detaches the cb and drops the install ref (the object's
 * deleter runs once the last reader also releases, so destroy-during-use is a
 * deferred free, never a use-after-free).
 * --------------------------------------------------------------------------*/

/* Register the cb deleter for a GUID kind.  Each object type calls this once
 * at startup (from a constructor in its own TU) so route_table can pick the
 * right deleter at install without referencing the per-type deleter symbols by
 * name (which would couple arts_gas to arts_memory/arts_compute and break the
 * separately-linked CUDA library). */
void arts_route_table_register_deleter(arts_guid_kind_t kind,
                                       void (*deleter)(void *));

/* Free an object that was built for `key` but never installed, with the
 * deleter its GUID kind installs with. */
void arts_route_table_delete_unpublished(arts_guid_t key, void *obj);

/* The one install rule of the global table: an object is installed only into
 * an EMPTY slot that names its key, decided in one atom with the slot's
 * identity, and it never replaces an occupant.  An occupied slot is the
 * caller's to wait for (the OoO engine parks a create there until the occupant
 * is retired).  A won install records the slot as the cb's home
 * (arts_shared_home) after its CAS and before it drains the slot's OoO list on
 * the installed cb (arts_ooo_drain_on): a destroy parked before the install
 * can retire the object inside the install. */

/* The control block an object is installed with: the GUID kind's deleter,
 * tagged with `key`.  One ref, the caller's.  The object's self_cb aliases it
 * from here to the install, so it is made before anything can reference the
 * object through it. */
arts_shared_ptr_t arts_route_table_make_handle(void *obj, arts_guid_t key);

/* Install a control block made by arts_route_table_make_handle into `key`'s
 * slot if that slot is empty.  On a win the slot takes over the ref the
 * caller passed and the slot's OoO list is drained.  The caller holds a ref
 * of its own across the call (the install writes the cb's home after handing
 * the passed ref to the slot, and the drain runs on the cb) and for as long
 * as it reads the object.  On a loss (the slot held an object under `key`
 * when the install looked; it may be empty again by the return) nothing
 * changes and the passed ref stays the caller's. */
bool arts_route_table_install_handle_if_absent(arts_shared_ptr_t cb,
                                               arts_guid_t key);

/* install_handle_if_absent for a GUID that arts_route_table_reserve_fresh may
 * reserve.  False only when the GUID was not fresh after all. */
bool arts_route_table_install_handle_fresh(arts_shared_ptr_t cb,
                                           arts_guid_t key);

/* install_handle_if_absent into `slot`, a slot the caller already resolved
 * for `key`, attempted once: false when the slot is occupied, names `key` no
 * longer, or its value word moved after the snapshot, and then `cb` stays the
 * caller's, unpublished and with no home, for the caller to resolve `key`
 * again.  As for install_handle_if_absent, the caller holds a ref of its own
 * across the call. */
bool arts_route_table_install_handle_into(arts_route_item_t *slot,
                                          arts_shared_ptr_t cb,
                                          arts_guid_t key);

/* Find or install, from a slot one arts_route_table_reserve_or_lookup of
 * `key` returned.  `cb` comes from arts_route_table_make_handle, unpublished,
 * its one ref the caller's.  Returns a counted handle on `key`'s object,
 * never NULL: on an empty slot it installs `cb` and returns a new ref on it
 * (the result equals `cb`; the passed ref becomes the slot's); on a slot
 * holding `key`'s object it returns that occupant pinned and leaves `cb`
 * untouched, the caller's to abandon with its object; a slot that changed
 * hands is resolved again.  The install's own drain may retire `cb` before
 * the call returns; the returned handle keeps its memory. */
arts_shared_ptr_t arts_route_table_install_or_get(arts_route_item_t *slot,
                                                  arts_shared_ptr_t cb,
                                                  arts_guid_t key);

/* The one liveness predicate: `h`'s install has recorded its home (an acquire
 * load, which orders that install before the caller's later reads, for a
 * holder whose count predates the install too) and its tag still names
 * `key`.  Live means the install happened before and no retire's tag clear
 * happened before the read: the object a resolution would return, or one a
 * retire is taking at that instant.  Not live does not mean retired: a cb
 * pinned from its slot between the install's CAS and its home store is
 * installed but not yet live.  A handle that is not live (NULL included)
 * takes the GUID path.  The caller holds a ref on `h`. */
bool arts_route_table_live(arts_shared_ptr_t h, arts_guid_t key);

/* Retires, one teardown.  Each detaches an object, drops the install ref, and
 * RETURNS the slot: the key is zeroed and the slot's parked payloads are
 * re-driven to wherever their GUIDs live now, so occupancy tracks live
 * objects.  The object's deleter runs once the last reader ref is released.
 * A retire clears the retired cb's tag, so a holder of a stale handle can tell
 * a retired object from a live one.  An installed object is never moved
 * between slots, so a live object is always in the one slot its key names.
 *
 * Both forms retire one OBJECT, named by its cb, never "whatever the slot
 * holds".  The dispatched-item form waits out another retire holding the slot
 * and retries only while its object is still installed, so neither takes a
 * generation installed after it looked.  Two retires of one object may be in
 * flight at once (a duplicate teardown notice, a completion racing a
 * destroy): exactly one of them returns true.  Waits are bounded; outliving
 * the bound is fatal. */

/* Identity form: retires only while `key`'s slot holds exactly `cb`, on which
 * the caller holds a ref; otherwise retires nothing and returns false, without
 * waiting.  For an object retiring itself (a completion): under label reuse
 * the slot may hold the key's next generation by then, which is not the
 * caller's to retire.  It reaches the slot through `cb`'s home once its
 * install has recorded it, and by a search of `key` before then. */
bool arts_route_table_set_destroyed_if(arts_guid_t key, arts_shared_ptr_t cb);

/* Dispatched-item form: the object a dispatch or a holder chose, named by
 * the cb it was installed with (an object's self_cb, or a handle a resolution
 * returned).  Precondition: `cb` was installed; a cb made and never installed
 * is tagged too, and is waited on until the bound.  As the identity form, but
 * a concurrent retire holding the slot is waited out, so the retire lands
 * unless another one retired `cb` first.  Returns false once `cb` is
 * retired. */
bool arts_route_table_set_destroyed_item(arts_guid_t key, arts_shared_ptr_t cb);

/* Type-aware safe lookups: return a caller-owned cb handle (strong ref held)
 * or NULL if the slot is absent / destroyed / a kind mismatch.  Use
 * arts_shared_get(h) for the object and arts_shared_release(&h) when done. */
arts_shared_ptr_t arts_route_table_lookup_event(arts_guid_t guid);
arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t guid);
arts_shared_ptr_t arts_route_table_lookup_edt(arts_guid_t guid);

/* Kind-agnostic handle lookup (no kind validation). */
arts_shared_ptr_t arts_route_table_lookup(arts_guid_t key);

/* `key`'s object for a flow that may already hold it: when `h` is live it is
 * borrowed under the caller's ref and *looked is NULL; otherwise (`h` NULL
 * included) *looked is arts_route_table_lookup(key), which the caller
 * releases, and the object is held by *looked (NULL on a miss).  `h` is never
 * released here. */
void *arts_route_table_borrow_or_lookup(arts_shared_ptr_t h, arts_guid_t key,
                                        arts_shared_ptr_t *looked);

/* A ref of the caller's own on `h`'s object when `h` is live (a copy), NULL
 * otherwise: for a body run inline that may drop the very ref the caller
 * holds `h` by.  The caller holds a ref on `h` and releases the result. */
arts_shared_ptr_t arts_route_table_copy_live(arts_shared_ptr_t h,
                                             arts_guid_t key);

arts_route_item_t *
arts_route_table_search_for_key(arts_route_table_t *route_table,
                                arts_guid_t key);

/* Linearly scan for an empty slot and CAS-claim it for `key` (or return the
 * already-claimed slot for `key`).  Operates on the GIVEN table, and is only
 * sound for a table no retire returns slots of (the GPU mirror tables): the
 * global table reserves through arts_route_table_reserve_or_lookup. */
arts_route_item_t *
arts_route_table_search_for_empty(arts_route_table_t *route_table,
                                  arts_guid_t key);

/* Acquire of a caller-located slot's published object; returns a
 * caller-owned handle (NULL if empty); release via arts_shared_release.  It
 * takes no key bracket, so it is for a mirror table's slots, which stay bound
 * to their key for the table's life, never for the global table. */
arts_shared_ptr_t arts_route_item_acquire(arts_route_item_t *item);

/* Publish `obj` into THIS slot's cb with an explicit deleter (CAS into an
 * empty value).  It installs into a caller-located slot of a non-global mirror
 * table (the GPU per-device route tables), whose slots stay bound to their key
 * for the table's life; never point it at the global table.  Pass a NULL
 * deleter for a persistent payload the cb never frees.  Returns true iff this
 * caller won the install; on loss the slot's existing cb is kept and `obj` is
 * left untouched. */
bool arts_route_item_install_data(arts_route_item_t *item, void *obj,
                                  void (*deleter)(void *));

/* Slot reserve or lookup — returns the route_item that publishes `key`,
 * reserving one (value == NULL) if none does.  No reservation waits on
 * another; the one wait is a retire's window.  The OoO engine (arts/ooo.h)
 * uses this to reach a slot's ooo_list. */
void arts_route_table_reserve_or_lookup(arts_guid_t key,
                                        arts_route_item_t **out);

/* Reserve the slot of a GUID this thread minted itself (for this rank, or a
 * data block's GUID from this rank's slice of another home's space) inside a
 * create that has not returned, and that nothing has named to another thread:
 * no other reservation of it can exist, so the first free slot of its probe
 * sequence publishes it at once, with no claim, fence or settle, and the
 * reservation never waits.  The caller must not name the GUID anywhere (a
 * message, a parameter, a shared word) before this returns; a Debug build
 * ends the process when another reservation of it is visible. */
void arts_route_table_reserve_fresh(arts_guid_t key, arts_route_item_t **out);

void arts_route_table_iterator_reset(arts_route_table_iterator_t *iter,
                                     arts_route_table_t *table);
arts_route_item_t *arts_route_table_iterate(arts_route_table_iterator_t *iter);

/* Retire every object installed in `route_table` for which `keep` (NULL:
 * none) answers false, free every slot's parked payloads, and return how many
 * objects stayed installed.  Each object it retires is handed to `detach`
 * (NULL: none) once, after its cb's tag is cleared and before the install's
 * count is dropped, so the object outlives the call.  Only meaningful once no
 * thread can reach the table. */
uint64_t arts_route_table_sweep(arts_route_table_t *route_table,
                                bool (*keep)(arts_shared_ptr_t),
                                void (*detach)(arts_shared_ptr_t));
/* Payloads parked on the OoO lists of every slot of `route_table` (its whole
 * segment chain), read in place without consuming them.  Only meaningful once
 * no thread can push or drain. */
unsigned int arts_route_table_parked_count(arts_route_table_t *route_table);
/* Frees a chain no thread can reach any more, with its parked payloads. */
void arts_route_table_delete(arts_route_table_t *route_table);
#ifdef ARTS_DEBUG_TEARDOWN
/* arts_route_table_sweep over every route table of this rank. */
uint64_t arts_route_table_node_sweep(bool (*keep)(arts_shared_ptr_t),
                                     void (*detach)(arts_shared_ptr_t));
#endif

#ifdef __cplusplus
}
#endif

#endif
