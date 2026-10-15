/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_shared_ptr_t — atomic_shared_ptr pattern (ad-hoc lock-free).
 *
 * Semantic equivalent of std::atomic<std::shared_ptr<T>>: a control block
 * (cb) carries a reference count, the managed object pointer, and a per-object
 * deleter.  Slots that publish a cb are declared by the caller as
 * _Atomic(arts_shared_slot_t) (a 16-byte DWCAS slot).  Concurrent readers use
 * the split-reference-counting load below (Williams "C++ Concurrency in
 * Action" §7.2.4): it DWCAS-bumps a per-slot in-flight counter WITHOUT
 * dereferencing the cb (so it never touches a cb a concurrent store may be
 * freeing), which pins the cb, then folds the claim into the cb's count and
 * returns a plain owned cb pointer.  Control blocks are allocated/freed per-op
 * by the allocator (no type-stable pool); a per-slot generation tag defeats
 * ABA on a recycled cb address, and a two-field count (refs + in_slot
 * presence bit) makes the free decision immune to the reconcile/load
 * transient-zero race.  Full protocol and the invariants it rests on are
 * documented in shared.c.
 *
 * Local API (single-owner):
 *   make    — allocate a cb (ref = 1) wrapping object + deleter.
 *   copy    — ref++ on a cb the caller already holds (never fails).
 *   release — ref--; last drop runs deleter(object) + frees the cb.
 *   get     — raw object pointer (valid while the caller holds a ref).
 *
 * Atomic slot API (multi-thread shared, arts_shared_slot_*):
 *   load     — split-count acquire (claim + fold); returns a caller-owned ref.
 *   store    — slot takes ownership of new_val; old slot value is released.
 *   exchange — atomic swap; returns the old value (caller releases it).
 */

#ifndef ARTS_UTILS_SHARED_H
#define ARTS_UTILS_SHARED_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/utils/stdatomic_compat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque control-block pointer.  The struct definition lives in shared.c;
 * callers only ever hold the pointer. */
typedef struct arts_shared_s *arts_shared_ptr_t;

/* Slot word — SPLIT REFERENCE COUNTING.  The slot packs the cb pointer with a
 * 64-bit external counter `ext` into a 16-byte DWCAS word: a load
 * DWCAS-increments `ext` WITHOUT dereferencing the cb (so it never touches a
 * cb a concurrent store may be freeing), which pins the cb; only then does it
 * dereference to fold the claim into the cb's internal count.  16-byte
 * aligned, as a 16-byte atomic requires (cmpxchg16b on x86-64; casp, or an
 * ldaxp/stlxp loop, on AArch64).  A slot is an _Atomic(arts_shared_slot_t),
 * accessed only through the functions below.
 *
 * Orders on a slot: every installing CAS (store, exchange, compare_exchange,
 * install_empty) is acq_rel.  Its release publishes the new cb with the
 * object, deleter and tag set before it; its acquire reads the release of
 * every loader's claim it displaces, which puts each loader's fold into the
 * displaced cb's count before the installer's reconcile, so the reconcile
 * never takes the count to zero under a loader.  A load takes the
 * publisher's release with its acquire load and its claim CAS; the probes
 * (empty, holds, peek_empty) load with acquire. */
typedef struct {
  arts_shared_ptr_t cb;
  uint64_t ext;
} __attribute__((aligned(16))) arts_shared_slot_t;

/* ── Local API (single-owner) ──────────────────────────────────────────── */

/* Allocate a cb (from the allocator) with ref = 1, wrapping `object` and
 * `deleter`.  `deleter` may be NULL (object is unmanaged). */
arts_shared_ptr_t arts_shared_make(void *object, void (*deleter)(void *));

/* Ref++ on a cb the caller already holds.  Returns b (or NULL if b is NULL).
 * Always succeeds: the caller's ref keeps the cb alive. */
arts_shared_ptr_t arts_shared_copy(arts_shared_ptr_t b);

/* Ref--; on the last drop runs deleter(object) and frees the cb.  Sets
 * *p = NULL.  No-op when *p is NULL. */
void arts_shared_release(arts_shared_ptr_t *p);

/* Cancel an UNPUBLISHED cb (just returned by arts_shared_make, never stored
 * into a slot and never copied): free the control block WITHOUT running the
 * deleter, so the wrapped object stays owned by the caller.  Use on the losing
 * side of an install race ("insert-or-fail": the object is still mine").
 * Precondition: ref == 1 (no other holder).  Sets *p = NULL; no-op when *p is
 * NULL. */
void arts_shared_abandon(arts_shared_ptr_t *p);

/* Raw managed-object pointer; valid while the caller holds a ref. */
void *arts_shared_get(arts_shared_ptr_t p);

/* The deleter the cb was made with (NULL for an unmanaged object, or when p
 * is NULL): what tells an object the runtime owns as a given kind from one a
 * caller published under that kind's key without handing it over. */
void (*arts_shared_deleter(arts_shared_ptr_t p))(void *);

/* Identity tag: the key a cb is published under, stamped by the publisher
 * BEFORE the cb first enters any slot (re-stamped only while the cb is
 * detached from every slot).  A reader that pinned a cb out of a keyed slot
 * can verify the value it HOLDS against the key it ASKED for by comparing
 * this field: the pinned cb cannot be recycled beneath the reader, so the
 * comparison cannot suffer the ABA that re-reading the table's own key word
 * can — a freed and re-claimed slot may return to a previously observed key
 * while holding a different object.  Atomic because stale handles may still
 * be read across a re-stamp; a racing reader observes one of the two
 * identities, each of which it must treat correctly.  0 = untagged. */
void arts_shared_set_tag(arts_shared_ptr_t p, uint64_t tag);
uint64_t arts_shared_tag(arts_shared_ptr_t p);

/* ── Atomic slot API (multi-thread shared) ─────────────────────────────── */

/* Acquire-and-validate load: returns a caller-owned ref (strong already
 * incremented) or NULL if the slot is empty / the cb is dying.  The caller
 * must arts_shared_release the result.
 *
 * Both of its CASes, the claim and the claim's release, are acq_rel, and
 * never weaker than release: besides the fold-before-reconcile order above,
 * a caller that stores a word and then loads the slot is ordered, by the
 * claim, before any remover whose CAS takes the slot afterwards, so that
 * remover sees the store. */
arts_shared_ptr_t arts_shared_slot_load(_Atomic(arts_shared_slot_t) *slot);

/* Non-owning peek: whether the slot holds anything.  Takes no ref, so the
 * answer is only as fresh as the acquire load behind it -- sound for "is
 * there something to install", never for reaching the object. */
static inline bool arts_shared_slot_empty(_Atomic(arts_shared_slot_t) *slot) {
  arts_shared_slot_t cur = atomic_load_explicit(slot, memory_order_acquire);
  return cur.cb == NULL;
}

/* Non-owning peek: whether the slot holds exactly `cb`.  Sound only while
 * the caller holds its own ref on `cb`, which keeps the cb from being
 * recycled, so pointer equality is identity. */
static inline bool arts_shared_slot_holds(_Atomic(arts_shared_slot_t) *slot,
                                          arts_shared_ptr_t cb) {
  arts_shared_slot_t cur = atomic_load_explicit(slot, memory_order_acquire);
  return cur.cb == cb;
}

/* Publish new_val into the slot (slot takes ownership of new_val's ref);
 * the previous slot value, if any, is released. */
void arts_shared_slot_store(_Atomic(arts_shared_slot_t) *slot,
                            arts_shared_ptr_t new_val);

/* Atomically swap new_val into the slot and return the previous value.  The
 * caller owns the returned ref and must release it. */
arts_shared_ptr_t arts_shared_slot_exchange(_Atomic(arts_shared_slot_t) *slot,
                                            arts_shared_ptr_t new_val);

/* Conditional publish (std::atomic<shared_ptr>::compare_exchange_strong).  If
 * the slot still holds `expected`, replace it with `new_val` (slot takes
 * new_val's ref, drops the ref it held on `expected`) and return true.  On
 * mismatch return false and leave the slot + both refs untouched.  Caller must
 * keep its own ref on `expected` alive across the call (pinning it against cb
 * recycle, so the raw-pointer compare cannot ABA). */
bool arts_shared_slot_compare_exchange(_Atomic(arts_shared_slot_t) *slot,
                                       arts_shared_ptr_t expected,
                                       arts_shared_ptr_t new_val);

/* Snapshot of an empty slot.  Returns true, with the slot's whole generation
 * word in *ext, when the slot holds nothing; takes no ref.  Every store or
 * exchange into the slot moves that word, even one that leaves it empty. */
bool arts_shared_slot_peek_empty(_Atomic(arts_shared_slot_t) *slot,
                                 uint64_t *ext);

/* Install new_val only if the slot still holds exactly the empty word a
 * peek_empty returned (slot takes new_val's ref).  Fails, leaving the slot
 * and new_val's ref untouched, once anything was stored or exchanged into the
 * slot after the peek — so a decision made from state read after the peek is
 * committed together with the install, or not at all. */
bool arts_shared_slot_install_empty(_Atomic(arts_shared_slot_t) *slot,
                                    uint64_t ext, arts_shared_ptr_t new_val);

#ifdef __cplusplus
}
#endif
#endif /* ARTS_UTILS_SHARED_H */
