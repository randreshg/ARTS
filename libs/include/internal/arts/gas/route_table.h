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
#include "arts/utils/shared.h" /* arts_shared_ptr_t, arts_atomic_shared_ptr_t */

/* Convention: all shared-object access is via caller-owned cb handles
 * (lookup_* / _acquire → handle; release required). No raw no-ref peeks. */

#define COLLISION_RESOLVES 8

/* Key values that name no object.  0 = FREE (claimable).  RETIRING is the key
 * a teardown parks in the slot while it detaches the value.  It is the KEY
 * word, not a lock beside it: one CAS from the GUID to this sentinel carries
 * both halves of what a teardown needs — proof the slot still names that
 * GUID, and sole ownership of the teardown — so there is nothing to hold and
 * nothing to wait on, and a loser is simply not the destroyer.  It is not 0,
 * so a claim CAS skips the slot, and it carries kind ARTS_GUID_RESERVED (never
 * a valid object), so it equals no real GUID and a key search stops matching
 * at once — a request arriving mid-teardown resolves to a fresh reservation.
 * That reservation publishes only after the retire settles, because an
 * identity retire that finds another object hands the key back. */
#define ARTS_ROUTE_KEY_RETIRING ((arts_guid_t)1)
/* Number of independent shards for the remote_route_table.  Must be a
 * power of 2 so (key & (N-1)) is the shard selector. */
#define ARTS_REMOTE_ROUTE_SHARDS 8

/* Route_item: GUID slot.  Permanent (init-array, never freed).
 *
 * `value` is an atomic shared-ptr cb slot (arts_shared_ptr_t).  The cb
 * carries the strong refcount, the per-object deleter and the object pointer;
 * absence is value == NULL, and a retire exchanges the value to NULL.  No
 * generation is kept: a lookup acquires a caller-owned ref via
 * arts_atomic_shared_load and accepts the object only while the pinned cb's
 * tag names the key looked up, and a retire clears that tag.  A retire
 * returns the slot and re-drives its OoO chain by each payload's own GUID. */
struct arts_route_item_s {
  arts_guid_t key;
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
  arts_atomic_shared_ptr_t value; /* cb: event/db/edt (NULL = absent) */
  arts_lf_stack_t ooo_list; /* OoO defer chain (Treiber) */
} ARTS_ALIGNED_MAX;

typedef struct arts_route_item_s arts_route_item_t;

typedef struct arts_route_table_s arts_route_table_t;

typedef arts_route_table_t *(*new_route_table_t)(unsigned int route_table_size,
                                                 unsigned int shift);

struct arts_route_table_s {
  arts_route_item_t *data;
  unsigned int size;
  unsigned int shift;
  /* The growable segment chain.  Reads use an acquire-load and growth uses a
   * single NULL->segment CAS (see route_table.c): the chain only ever grows
   * (a published segment is never unlinked before teardown), so the link is
   * monotonic and ABA-free — no lock is needed to traverse or extend it. */
  struct arts_route_table_s *next;
  new_route_table_t newFunc;
};

typedef struct {
  uint64_t index;
  arts_route_table_t *table;
} arts_route_table_iterator_t;

arts_route_table_t *arts_new_route_table(unsigned int route_table_size,
                                         unsigned int shift);

/* ---------------------------------------------------------------------------
 * cb-based item lifecycle API.
 *
 * The slot holds an atomic shared-ptr cb.  Install wraps the object in a cb
 * (deleter chosen by GUID kind); lookups return a caller-owned cb handle the
 * caller releases when done; destroy detaches the cb and drops the install
 * ref (the object's deleter runs once the last reader also releases, so
 * destroy-during-use is a deferred free, never a use-after-free).
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
 * is retired).
 *
 * install_if_absent: install `obj` under `key` if the slot is empty, then
 * drain the slot's OoO list.  On a win the slot owns the object through its
 * cb and the call returns a handle holding one strong ref, which the caller
 * owns and must release: the drain may already have retired the object (a
 * destroy parked before the install runs inside it), and that ref keeps the
 * caller's reads valid.  On a loss (the slot holds an object under `key`) it
 * returns NULL, abandons its cb without running the deleter, and `obj` stays
 * the caller's. */
arts_shared_ptr_t arts_route_table_install_if_absent(void *obj,
                                                     arts_guid_t key,
                                                     unsigned int rank,
                                                     bool used);

/* The control block install_if_absent would wrap `obj` in: the GUID kind's
 * deleter, tagged with `key`.  One ref, the caller's.  For an object that must
 * be referenced before it is installed. */
arts_shared_ptr_t arts_route_table_make_handle(void *obj, arts_guid_t key);

/* install_if_absent for a control block made by arts_route_table_make_handle.
 * On a win the slot takes over the ref the caller passed and the slot's OoO
 * list is drained; the caller keeps whatever other refs it holds, and must
 * hold one of its own if it reads the object after the call.  On a loss
 * nothing changes and the passed ref stays the caller's. */
bool arts_route_table_install_handle_if_absent(arts_shared_ptr_t cb,
                                               arts_guid_t key);

int arts_route_table_lookup_rank(arts_guid_t key);

/* Retires, one teardown.  Each detaches an object, drops the install ref, and
 * RETURNS the slot: the key is zeroed and the slot's parked payloads are
 * re-driven to wherever their GUIDs live now, so occupancy tracks live
 * objects.  The object's deleter runs once the last reader ref is released.
 * A retire clears the retired cb's tag, so a holder of a stale handle can tell
 * a retired object from a live one.  An installed object is never moved
 * between slots, so a live object is always in the one slot its key names.
 *
 * Every form retires one OBJECT, never "whatever the slot holds".  The
 * dispatched-item and object forms wait out another retire holding the slot
 * and retry only while their object is still installed, so none of them takes
 * a generation installed after it looked.  Two retires of one object may be
 * in flight at once (a duplicate teardown notice, a completion racing a
 * destroy): exactly one of them returns true.  A slot being retired hides its
 * key from a search, and when that retire hands the key back (it named an
 * object the slot no longer held) the slot's occupant is still the key's; the
 * object form therefore looks through a RETIRING slot of the key's window
 * rather than miss it.  Waits are bounded; outliving the bound is fatal. */

/* Identity form: retires only while `key`'s slot holds exactly `cb`, on which
 * the caller holds a ref; otherwise retires nothing and returns false, without
 * waiting.  For an object retiring itself (a completion): under label reuse
 * the slot may hold the key's next generation by then, which is not the
 * caller's to retire. */
bool arts_route_table_set_destroyed_if(arts_guid_t key, arts_shared_ptr_t cb);

/* Dispatched-item form: the object a DISPATCH chose.  As the identity form,
 * but a concurrent retire holding the slot is waited out, so the retire lands
 * unless another one retired `cb` first.  Returns false once `cb` is
 * retired. */
bool arts_route_table_set_destroyed_item(arts_guid_t key, arts_shared_ptr_t cb);

/* Object form: the object at `obj`, for a caller that has the object but no
 * handle on it: pins whatever `key` currently publishes, looking through a
 * retire in flight, and retires it by the dispatched-item form only if it is
 * `obj`.  Returns false when `key` publishes another object or none. */
bool arts_route_table_set_destroyed_object(arts_guid_t key, const void *obj);

/* Type-aware safe lookups: return a caller-owned cb handle (strong ref held)
 * or NULL if the slot is absent / destroyed / a kind mismatch.  Use
 * arts_shared_get(h) for the object and arts_shared_release(&h) when done. */
arts_shared_ptr_t arts_route_table_lookup_event(arts_guid_t guid);
arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t guid);
arts_shared_ptr_t arts_route_table_lookup_edt(arts_guid_t guid);

/* Kind-agnostic handle lookup (no kind validation). */
arts_shared_ptr_t arts_route_table_lookup(arts_guid_t key);

arts_route_item_t *
arts_route_table_search_for_key(arts_route_table_t *route_table,
                                arts_guid_t key);

/* Linearly scan for an empty slot and CAS-claim it for `key` (or return the
 * already-claimed slot for `key`).  Operates on the GIVEN table, and is only
 * sound for a table no retire returns slots of (the GPU mirror tables): the
 * global table reserves through arts_route_table_reserve_or_lookup.
 * `mark_used` is ignored. */
arts_route_item_t *
arts_route_table_search_for_empty(arts_route_table_t *route_table,
                                  arts_guid_t key, bool mark_used);

/* Safe C-linkage acquire of a slot's published object; returns a caller-owned
 * handle (NULL if empty); release via arts_shared_release.  The C++/.cu bridge
 * for the otherwise C-only atomic-slot API. */
arts_shared_ptr_t arts_route_item_acquire(arts_route_item_t *item);

/* Publish `obj` into THIS slot's cb with an explicit deleter (CAS into an
 * empty value).  It installs into a caller-located slot of a non-global mirror
 * table (the GPU per-device route tables), whose slots stay bound to their key
 * for the table's life; never point it at the global table.
 * C-linkage bridge (slot CAS is otherwise C11-only) so C++/nvcc translation
 * units can publish a persistent mirror payload (pass NULL deleter for a cb
 * that never frees the object).  Returns true iff this caller won the install;
 * on loss the slot's existing cb is kept and `obj` is left untouched. */
bool arts_route_item_install_data(arts_route_item_t *item, void *obj,
                                  void (*deleter)(void *));

/* Slot reserve or lookup — returns the permanent route_item for `key`,
 * creating it (value == NULL) if absent.  The OoO engine (arts/ooo.h) uses
 * this to reach a slot's ooo_list. */
void arts_route_table_reserve_or_lookup(arts_guid_t key,
                                        arts_route_item_t **out);

void arts_reset_route_table_iterator(arts_route_table_iterator_t *iter,
                                     arts_route_table_t *table);
arts_route_item_t *arts_route_table_iterate(arts_route_table_iterator_t *iter);
void arts_print_item(arts_route_item_t *item);

uint64_t arts_clean_up_route_table(arts_route_table_t *route_table);
/* Payloads parked on the OoO lists of every slot of `route_table` (its whole
 * segment chain), read in place without consuming them.  Only meaningful once
 * no thread can push or drain. */
unsigned int arts_route_table_parked_count(arts_route_table_t *route_table);
void arts_delete_route_table(arts_route_table_t *route_table);
void arts_clean_up_dbs();

#ifdef __cplusplus
}
#endif

#endif
