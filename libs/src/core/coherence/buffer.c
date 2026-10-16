/* SPDX-License-Identifier: Apache-2.0
 *
 * Buffer lifecycle implementation.  See buffer.h for the contract.
 *
 * The buffer is managed as an arts_shared_ptr_t: cache.buffer is the atomic
 * slot, the slot holds the "cache-hold" ref, and every acquirer holds one more.
 * The cb deleter ends the buffer's life on the last drop.  Because the split-
 * reference-counting load pins the cb without dereferencing it, an in-flight
 * acquire is immune to a concurrent destroy: the bytes live until the final
 * holder releases.  That final holder can be anyone (a transfer's local
 * completion included) and can come after the block's descriptor was
 * destroyed, so the deleter touches no cache it does not keep alive itself:
 * where the arm keeps a free-list, a buffer holds its cache's storage until it
 * is parked; elsewhere the deleter frees the buffer and touches no cache.
 */

#include "arts/coherence/buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts/db.h"       /* arts_db_descriptor_free */
#include "arts/gas/guid.h" /* arts_db_szhint_bound */
#include "arts/memory/regpool.h"
#include "arts/system/print.h"  /* ARTS_ERROR (unadvertisable landing) */
#include "arts/transport/net.h" /* arts_net_rdzv_local / _txid_next */
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

#ifdef ARTS_DB_BUF_POOL
/* buf_holds: the number of buffers drawn from a cache and not parked back, in
 * the low bits, and HOLDS_DESTROYED, which the storage release sets once the
 * descriptor's destructor has run.
 * The storage goes when the word reaches exactly HOLDS_DESTROYED, which one
 * read-modify-write observes: no buffer can be drawn after the destructor,
 * since only a holder of the descriptor draws one. */
#define HOLDS_DESTROYED 0x80000000u
#define BUF_OUT 1u
#define BUF_PARKED 2u

/* Only a draw pushes here, and nothing draws from a destroyed descriptor, so
 * once the destructor has run the stack is drained whole without contention,
 * whether buffers are still out or not. */
static void buf_free_retired(struct arts_db_cache_s *cache) {
  arts_lf_link_t *n = arts_lf_stack_drain(&cache->buf_retired);
  while (n != NULL) {
    arts_lf_link_t *next = atomic_load_explicit(&n->next, memory_order_relaxed);
    arts_regpool_free(n);
    n = next;
  }
}

static void buf_storage_free(struct arts_db_cache_s *cache) {
  arts_lf_pool_destroy_with(&cache->buf_freelist, arts_regpool_free);
  arts_db_descriptor_free(arts_db_of_cache(cache));
}

/* Park a buffer drawn from a cache on that cache's list, then let go of the
 * storage it was keeping: the push lands while the storage is still held. */
static void buf_park(struct arts_db_buffer_s *b) {
  struct arts_db_cache_s *cache = b->owner_cache;
  if (cache == NULL) {
    arts_regpool_free(b);
    return;
  }
#ifndef NDEBUG
  /* Ended twice, a buffer would be parked twice and its cache's storage could
   * go while another buffer drawn from it is still out.  Only an end that
   * finds the buffer parked is caught: a second end after a re-draw is caught,
   * if at all, only at a later end, and not reliably once the buffer has been
   * freed. */
  if (atomic_exchange_explicit(&b->park_state, BUF_PARKED,
                               memory_order_relaxed) != BUF_OUT) {
    ARTS_ERROR("coherence: buffer %p ended twice", (void *)b);
  }
#endif
  b->cb = NULL;
  arts_lf_pool_release(&cache->buf_freelist, &b->pool_link);
  if (atomic_fetch_sub_explicit(&cache->buf_holds, 1u, memory_order_acq_rel) ==
      (HOLDS_DESTROYED | 1u)) {
    buf_storage_free(cache);
  }
}
#endif

/* cb deleter — runs once, on the last strong drop, which can come after the
 * block's descriptor was destroyed. */
static void buffer_deleter(void *obj) {
#ifdef ARTS_DB_BUF_POOL
  buf_park((struct arts_db_buffer_s *)obj);
#else
  arts_regpool_free(obj);
#endif
}

void arts_db_buf_storage_release(struct arts_db_cache_s *cache) {
#ifdef ARTS_DB_BUF_POOL
  /* Nothing draws from a destroyed descriptor, so whatever is parked now is
   * reclaimable at once; only buffers still out park later, and the last of
   * them (or this release, if none is out) frees the rest with the storage.
   * The destroyed flag is not set yet, so a concurrent park cannot free. */
  for (arts_lf_link_t *n;
       (n = arts_lf_pool_pop_or_null(&cache->buf_freelist)) != NULL;) {
    arts_regpool_free(n);
  }
  buf_free_retired(cache);
  if (atomic_fetch_add_explicit(&cache->buf_holds, HOLDS_DESTROYED,
                                memory_order_acq_rel) == 0u) {
    buf_storage_free(cache);
  }
#else
  arts_db_descriptor_free(arts_db_of_cache(cache));
#endif
}

struct arts_db_buffer_s *arts_db_buf_alloc(struct arts_db_cache_s *cache,
                                           uint64_t db_size) {
#ifdef ARTS_DB_BUF_POOL
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)arts_lf_pool_pop_or_null(&cache->buf_freelist);
  if (b != NULL && b->capacity < db_size) {
    /* Parked for a smaller request (a size bound or an earlier size).  It is
     * not freed here: a concurrent popper may still be reading its link, so
     * it is retired and freed by the storage release that follows the
     * descriptor's destructor. */
    arts_lf_stack_push(&cache->buf_retired, &b->pool_link);
    b = NULL;
  }
  if (b == NULL) {
    /* 64-byte aligned so buf->data (offset 64) lands on a cache-line
     * boundary.  Drawn from the registered pool so every DB payload buffer
     * falls inside a slab that is (or will be) pinned and pre-registered with
     * the fabric, resolvable by arts_regpool_lookup for one-sided RDMA. */
    b = (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
        sizeof(struct arts_db_buffer_s) + db_size, 64);
    if (b == NULL) {
      return NULL;
    }
    b->capacity = db_size;
  }
  /* The caller holds the descriptor, so its storage is live here. */
  b->owner_cache = cache;
#ifndef NDEBUG
  atomic_store_explicit(&b->park_state, BUF_OUT, memory_order_relaxed);
#endif
  atomic_fetch_add_explicit(&cache->buf_holds, 1u, memory_order_relaxed);
  return b; /* caller re-inits fields */
#elif defined(ARTS_CXL_DIRECT)
  /* The payload is not the descriptor's to size: a descriptor leaves this call
   * naming no storage. */
  (void)cache;
  (void)db_size;
  struct arts_db_buffer_s *b =
      (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
          sizeof(struct arts_db_buffer_s), 64);
  if (b != NULL) {
    b->data = NULL;
  }
  return b;
#else
  (void)cache;
  return (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
      sizeof(struct arts_db_buffer_s) + db_size, 64);
#endif
}

arts_shared_ptr_t arts_db_buf_detached(uint64_t db_size,
                                       struct arts_db_buffer_s **out) {
#ifdef ARTS_CXL_DIRECT
  (void)db_size;
  (void)out;
  ARTS_ERROR("coherence: a detached payload has no meaning where the store "
             "is not this rank's");
  return NULL;
#else
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
      sizeof(struct arts_db_buffer_s) + db_size, 64);
  if (b == NULL) {
    ARTS_ERROR("coherence: detached buffer alloc failed (%llu bytes)",
               (unsigned long long)db_size);
  }
#ifdef ARTS_DB_BUF_POOL
  b->owner_cache = NULL;
#endif
  atomic_init(&b->version, 0);
  b->cb = arts_shared_make(b, buffer_deleter);
  *out = b;
  return b->cb;
#endif
}

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  /* Acquire-and-validate load: returns a caller-owned strong ref (keeps the
   * buffer alive) or NULL if no buffer is installed.  Caller releases via
   * arts_db_buf_release. */
  return arts_shared_slot_load(&cache->buffer);
}

void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }

/* One place records that the payload slot is no longer empty, so the
 * per-acquire question "does this block still need materializing" is a byte
 * load instead of a refcounted look at the slot.  Called wherever a non-NULL
 * buffer ends up in the slot — including an install-if-absent that LOST,
 * since the winner's buffer is in there either way.  Release: the install
 * that put that buffer in the slot happens before the store, and the fast
 * exit's acquire load hands it to a caller that reads the slot next. */
static inline void buf_note_present(struct arts_db_cache_s *cache) {
  atomic_store_explicit(&cache->payload_pending, (uint8_t)0,
                        memory_order_release);
}

#ifdef ARTS_CXL_DIRECT
/* Does this cache already name a descriptor?  Read from the slot itself, with
 * no ref taken.  Where the descriptor's payload is the block's own storage the
 * slot is the only sound answer to "does this block still need materializing":
 * a presence HINT is written after the transition it reports, so a thread
 * whose install was undone in between can leave the hint saying "present" over
 * an empty slot, and every later materialization would then stand down
 * forever.  The slot cannot say that: it is the thing being decided. */
static inline bool buf_slot_filled(struct arts_db_cache_s *cache) {
  return !arts_shared_slot_empty(&cache->buffer);
}

bool arts_db_buf_adopt_external(struct arts_db_cache_s *cache, void *payload,
                                uint64_t version, uint64_t db_size) {
  if (payload == NULL) {
    /* A rank that has not learned the block's store, or a block with none,
     * has nothing to adopt. */
    return false;
  }
  /* No pre-gate: the install CAS below is this call's first access to the
   * slot, and it is the only arbiter.  A cheaper look first could only report
   * the slot as it was, and the answer a caller needs is the slot as it is
   * when the descriptor goes in. */
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, 0);
  if (nb == NULL) {
    return false; /* OOM — caller decides how to surface. */
  }
  atomic_init(&nb->version, version);
  nb->data = (char *)payload;
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_shared_slot_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter ends nb */
    /* One descriptor serves a block for its whole life: the store does not
     * move, so a second descriptor would be two names for one range and
     * arts_db_buf_for_payload could not answer.  Losing this CAS to the SAME
     * store is an ordinary race between two first users; losing it to a
     * different pointer is two stores for one block. */
    arts_shared_ptr_t cur = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *inc =
        (struct arts_db_buffer_s *)arts_shared_get(cur);
    if (inc == NULL) {
      /* The winner's descriptor has since been withdrawn, so the block has no
       * payload again: this call installed none either, and the slot still
       * needs materializing. */
      arts_db_buf_release(&cur);
      return false;
    }
    if ((void *)inc->data != payload) {
      ARTS_ERROR("coherence: a block already has a descriptor naming other "
                 "storage");
    }
    arts_db_buf_release(&cur);
    buf_note_present(cache);
    return false;
  }
  arts_db_cache_learn_size(cache, db_size);
  buf_note_present(cache);
  return true;
}

struct arts_db_buffer_s *arts_db_buf_for_payload(struct arts_db_cache_s *cache,
                                                 void *payload) {
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (b != NULL && (void *)b->data != payload) {
    b = NULL;
  }
  arts_db_buf_release(&h);
  return b;
}
#endif /* ARTS_CXL_DIRECT */

bool arts_db_buf_ensure(struct arts_db_cache_s *cache, uint64_t db_size) {
#ifdef ARTS_CXL_DIRECT
  /* The slot, not the presence hint: see buf_slot_filled.  It costs a wide
   * atomic load instead of a byte load and takes no ref. */
  if (buf_slot_filled(cache)) {
    return false;
  }
#else
  /* A byte load answers the common case: once anything has installed a
   * buffer, no later use has to materialize one, and asking the slot itself
   * would cost every acquire a refcount round-trip on a shared line.
   * Acquire: a caller that stops here reads the pointer from the slot next,
   * and must find there the buffer whose install this 0 follows — a sized
   * block's EDT would otherwise be handed NULL.  A caller that reads a stale
   * 1 simply tries an install that then fails and clears the flag. */
  if (atomic_load_explicit(&cache->payload_pending, memory_order_acquire) ==
      0u) {
    return false;
  }
#endif
  if (db_size == 0) {
    /* A zero-sized block has no storage to hold: NULL is its defined
     * value. */
    return false;
  }
#ifdef ARTS_CXL_DIRECT
  return arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_cxl_slot_addr(cache), 1u, db_size);
#else
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  if (arts_shared_get(h) != NULL) {
    arts_db_buf_release(&h);
    return false;
  }
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, db_size);
  if (nb == NULL) {
    return false; /* OOM — caller decides how to surface. */
  }
  /* Version 1, not 0: the block's first image is its VALUE, not a
   * placeholder awaiting a publication.  An arm whose "has anyone published
   * yet" predicate is version > 0 must read this as published, or a reader
   * would hold for a writer that may never come. */
  atomic_init(&nb->version, 1);
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  /* Install-if-absent, never the version-conditional publish: this buffer
   * may only ever fill a hole.  Two first users therefore agree by
   * construction — one installs, the other adopts the installed image, and
   * neither image holds a value anyone wrote. */
  if (!arts_shared_slot_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter ends nb */
    buf_note_present(cache);
    return false;
  }
  arts_db_cache_learn_size(cache, db_size);
  buf_note_present(cache);
  return true;
#endif
}

#ifdef ARTS_USE_CXL
bool arts_db_buf_withdraw(struct arts_db_cache_s *cache, const void *payload) {
  if (cache == NULL || payload == NULL) {
    return false;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  bool withdrawn = false;
  if (b != NULL && (const void *)b->data == payload) {
    /* The load ref pins the descriptor against reuse, so the slot's own
     * identity cannot have drifted and returned under the compare. */
    withdrawn = arts_shared_slot_compare_exchange(&cache->buffer, h, NULL);
    if (withdrawn) {
      /* The slot is empty again, so the block needs materializing again.  The
       * flag is a hint with one safe direction: a 1 over a descriptor another
       * thread installs in between costs that descriptor's next materialize
       * one failed install and nothing else, whereas a 0 over an empty slot
       * is a claim no later call corrects -- so the restore is unconditional. */
      atomic_store_explicit(&cache->payload_pending, (uint8_t)1,
                            memory_order_release);
    }
  }
  arts_db_buf_release(&h);
  return withdrawn;
}
#endif

/* Version-conditional publish of a fully-initialized private buffer (fields +
 * payload bytes already set; no cb yet).  Shared by the copy install
 * (arts_db_buf_install) and the rendezvous landed install
 * (arts_db_buf_install_landed).  On a stale loss the private buffer's life
 * ends and the newer installed buffer is returned. */
static struct arts_db_buffer_s *buf_publish(struct arts_db_cache_s *cache,
                                            struct arts_db_buffer_s *new_buf,
                                            uint64_t new_version) {
  /* Wrap the buffer in a fresh control block (strong = 1).  The slot will
   * take this ref as the cache-hold on a successful publish.  Stash the cb in
   * the buffer so a holder can recover it (buf->cb) to release without
   * threading the handle through the acquire call chain. */
  arts_shared_ptr_t new_cb = arts_shared_make(new_buf, buffer_deleter);
  new_buf->cb = new_cb;

  for (;;) {
    arts_shared_ptr_t old_h = arts_shared_slot_load(&cache->buffer);
    struct arts_db_buffer_s *old =
        (struct arts_db_buffer_s *)arts_shared_get(old_h);
    /* The publish decision reads a version another rank's release may be
     * bumping in place; an acquire load pairs with that read-modify-write so
     * the comparison never observes a torn or reordered value. */
    if (old != NULL &&
        atomic_load_explicit(&old->version, memory_order_acquire) >=
            new_version) {
      /* Stale install: a newer (or equal) buffer is already published.
       * Drop our load ref, abandon the unpublished cb (keeps new_buf ours)
       * and end new_buf.  old stays alive via the slot's sentinel ref.
       *
       * The comparison is what keeps an INVENTED first image from displacing
       * real bytes, so the two live at different stamps by construction: an
       * invented image — the one a first user materializes for a block whose
       * create left it no storage — is version 1, and the first real bytes
       * of such a block arrive at 2 or above, because whoever writes them
       * bumps past the image it was handed.  An install that arrives at 1
       * against a 1 already there is therefore the other first user, and
       * keeping the published one is right. */
      arts_shared_release(&old_h);
      arts_shared_abandon(&new_cb);
      buffer_deleter(new_buf); /* never shared: end it here */
      buf_note_present(cache);
      return old;
    }
    /* Conditional publish: install new_cb only while the slot still holds
     * old_h.  old_h pins old's cb (strong >= 1) so the raw-pointer compare
     * cannot ABA.  On success the slot drops its ref on the old value. */
    if (arts_shared_slot_compare_exchange(&cache->buffer, old_h, new_cb)) {
      if (old_h != NULL) {
        arts_shared_release(&old_h); /* our load ref on old */
      }
      buf_note_present(cache);
      return new_buf;
    }
    /* CAS lost — cache.buffer changed concurrently; re-evaluate. */
    if (old_h != NULL) {
      arts_shared_release(&old_h);
    }
  }
}

struct arts_db_buffer_s *arts_db_buf_install(struct arts_db_cache_s *cache,
                                             uint64_t new_version,
                                             const void *data_payload,
                                             uint64_t db_size) {
#ifdef ARTS_CXL_DIRECT
  if (data_payload != NULL) {
    ARTS_ERROR("coherence: a payload cannot be published into the block's "
               "store");
  }
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_cxl_slot_addr(cache), new_version,
      db_size);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  arts_db_buf_release(&h);
  return b;
#else
  struct arts_db_buffer_s *new_buf = arts_db_buf_alloc(cache, db_size);
  if (new_buf == NULL) {
    return NULL; /* OOM — caller decides how to surface. */
  }
  atomic_init(&new_buf->version, new_version);
  /* Publish bytes into the buffer's payload (canonical user-visible storage). */
  if (db_size > 0 && data_payload != NULL) {
    memcpy(new_buf->data, data_payload, (size_t)db_size);
  }
  /* Lazy-installed caches start with db_size==0; the first install learns
   * the real size from the wire payload. */
  arts_db_cache_learn_size(cache, db_size);
  return buf_publish(cache, new_buf, new_version);
#endif
}

/* In-place publish commit: stamp the stable buffer with the published
 * version.  The bytes are already in place ("imm seen => landing valid"
 * precedes this call); the release-store pairs with the serve side's
 * acquire-loads.  The single-flight publish discipline makes a
 * non-increasing version unreachable — hard-error rather than retreat
 * (this is NOT buf_publish: the shared install path keeps its documented
 * stale-retreat contract for the grant plane). */
void arts_db_buf_bump_inplace(struct arts_db_cache_s *cache,
                              uint64_t version) {
#ifdef ARTS_CXL_DIRECT
  (void)cache;
  (void)version;
  ARTS_ERROR("coherence: a version bump publishes nothing where the store is "
             "not this rank's");
#else
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    ARTS_ERROR("coherence: publish commit with no stable buffer installed");
  }
  uint64_t cur = atomic_load_explicit(&buf->version, memory_order_acquire);
  if (version < cur) {
    /* Publishes to one home are serialized (one flight per rank, and the
     * write right migrates only between flights), so a version below the
     * buffer's is not reordering — it is corruption. */
    ARTS_ERROR("coherence: publish commit version regressed (%llu < %llu)",
               (unsigned long long)version, (unsigned long long)cur);
  }
  if (version > cur) {
    /* An equal version is an idempotent republish: a releaser whose waiter
     * was covered while it raced for the flight claim ships the same bytes
     * under the same stamp — the ACK matters, the stamp is a no-op. */
    atomic_store_explicit(&buf->version, version, memory_order_release);
  }
  arts_db_buf_release(&h);
#endif
}

/* ===== Rendezvous landing lifecycle (see buffer.h) ======================= */

struct arts_db_buffer_s *
arts_db_buf_landing_alloc(struct arts_db_cache_s *cache, uint64_t db_size,
                          struct arts_rdzv_landing_s *out) {
#ifdef ARTS_CXL_DIRECT
  (void)cache;
  (void)db_size;
  (void)out;
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
  return NULL;
#else
  struct arts_db_buffer_s *b = arts_db_buf_alloc(cache, db_size);
  if (b == NULL) {
    ARTS_ERROR("coherence: rendezvous landing alloc failed (%llu bytes)",
               (unsigned long long)db_size);
  }
  if (!arts_net_rdzv_local(b->data, db_size, &out->addr, &out->key)) {
    /* The buffer lies in no fabric-registered slab, so no peer can PUT into
     * it.  One-sided bulk transfer requires the registered arena pool. */
    ARTS_ERROR("coherence: landing buffer is not fabric-registered — "
               "one-sided payloads require the registered pool "
               "(ARTS_MALLOC=mimalloc)");
  }
  out->txid = arts_net_rdzv_txid_next();
  out->cookie = (uint64_t)(uintptr_t)b;
  return b;
#endif
}

void arts_db_buf_landing_return(struct arts_db_buffer_s *b) {
  if (b == NULL) {
    return;
  }
#ifdef ARTS_CXL_DIRECT
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
#else
  buffer_deleter(b);
#endif
}

bool arts_db_buf_adopt_landing(struct arts_db_cache_s *cache, uint64_t version,
                               struct arts_db_buffer_s *landing,
                               uint64_t db_size) {
#ifdef ARTS_CXL_DIRECT
  (void)cache;
  (void)version;
  (void)landing;
  (void)db_size;
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
  return false;
#else
  if (landing == NULL) {
    return false;
  }
  if (db_size == 0) {
    /* A zero-sized block has no storage to hold: NULL is its defined value. */
    arts_db_buf_landing_return(landing);
    return false;
  }
  atomic_init(&landing->version, version);
  arts_shared_ptr_t cb = arts_shared_make(landing, buffer_deleter);
  landing->cb = cb;
  /* Install-if-absent, NOT the version-conditional publish: this landing
   * carries no data, so it may only ever fill a hole.  A buffer already in the
   * slot holds real bytes at some version — replacing it, at any version,
   * would destroy them. */
  if (!arts_shared_slot_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter ends the landing */
    buf_note_present(cache);
    return false;
  }
  arts_db_cache_learn_size(cache, db_size);
  buf_note_present(cache);
  return true;
#endif
}

struct arts_db_buffer_s *
arts_db_buf_install_landed(struct arts_db_cache_s *cache, uint64_t new_version,
                           struct arts_db_buffer_s *landing,
                           uint64_t db_size) {
#ifdef ARTS_CXL_DIRECT
  (void)cache;
  (void)new_version;
  (void)landing;
  (void)db_size;
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
  return NULL;
#else
  atomic_init(&landing->version, new_version);
  arts_db_cache_learn_size(cache, db_size);
  return buf_publish(cache, landing, new_version);
#endif
}

void arts_db_buf_ref_release_cb(void *arg) {
  arts_shared_ptr_t h = (arts_shared_ptr_t)arg;
  arts_shared_release(&h);
}

uint64_t arts_db_first_fetch_size(const struct arts_db_cache_s *cache) {
  /* The exact size once this rank has learned it (from any wire-carried
   * declaration), else the GUID's encoded upper bound — good enough to size
   * a first-fetch landing, so the size-discovery round becomes the sentinel
   * fallback rather than every cold acquire's first leg.  0 = advertise no
   * landing (sentinel GUID or genuinely empty DB). */
  uint64_t size = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  return size ? size : arts_db_szhint_bound(cache->db_guid);
}

void arts_db_buf_prepare_inplace(struct arts_db_cache_s *cache,
                                 uint64_t capacity) {
#ifdef ARTS_CXL_DIRECT
  /* A first touch adopts the block's own store, so a capacity bound sizes
   * nothing and declares nothing. */
  (void)capacity;
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_cxl_slot_addr(cache), 0u,
      atomic_load_explicit(&cache->db_size, memory_order_relaxed));
#else
  /* First-touch variant that sizes the ALLOCATION without declaring the
   * DB's size: `cache->db_size` only ever records a wire-derived exact
   * value, so a requester materializing its stable buffer from a GUID size
   * BOUND must not let the bound become the size — a bound recorded there
   * would later travel as an exact wire length (an over-long PUT into a
   * peer's exactly-sized landing).  The buffer may be larger than the size
   * the wire later declares; nothing reads a buffer's length (size lives
   * solely in the descriptor).  Same install-if-absent discipline as
   * write_inplace's first touch; an established buffer is left as is. */
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  if (arts_shared_get(h) != NULL) {
    arts_shared_release(&h);
    return;
  }
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, capacity);
  if (nb == NULL) {
    return; /* OOM — caller decides how to surface. */
  }
  atomic_init(&nb->version, 0);
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_shared_slot_compare_exchange(&cache->buffer, NULL, cb)) {
    /* Lost the install race: adopt the winner (the fetch overwrites
     * either). */
    arts_shared_release(&cb);
  }
  buf_note_present(cache);
#endif
}

void arts_db_buf_write_inplace(struct arts_db_cache_s *cache, const void *data,
                               uint64_t db_size) {
#ifdef ARTS_CXL_DIRECT
  if (data != NULL) {
    ARTS_ERROR("coherence: a payload cannot be published into the block's "
               "store");
  }
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_cxl_slot_addr(cache), 0u, db_size);
#else
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf != NULL) {
    /* Established stable buffer: overwrite in place.  The address is fixed, so
     * a DB holding internal self-pointers stays valid.  Safe because the
     * caller's protocol guarantees no concurrent reader during the write. */
    if (db_size > 0 && data != NULL) {
      memcpy(buf->data, data, (size_t)db_size);
    }
    arts_db_cache_learn_size(cache, db_size);
    arts_shared_release(&h);
    return;
  }
  /* First touch: allocate the one stable buffer (no further realloc).
   *
   * First touches CAN race: independent acquisition paths materialize the
   * same cache's buffer concurrently (e.g. two request rounds whose replies
   * arrive on different progress threads).  Publish with an
   * install-if-absent CAS, never an unconditional store — a losing store
   * would REPLACE the winner's established buffer, detaching any
   * fixed-address landing already advertised on it (silently discarding
   * data already landed there) while later readers re-derive their pointers
   * from the fresh, never-written instance. */
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, db_size);
  if (nb == NULL) {
    return; /* OOM — caller decides how to surface. */
  }
  atomic_init(&nb->version, 0);
  if (db_size > 0 && data != NULL) {
    memcpy(nb->data, data, (size_t)db_size);
  }
  arts_db_cache_learn_size(cache, db_size);
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_shared_slot_compare_exchange(&cache->buffer, NULL, cb)) {
    /* Lost the install race.  A first touch without data holds no value
     * anyone wrote, so adopting the winner loses nothing; a data-carrying
     * caller (whose write the coherence protocol serializes
     * against every reader) writes its bytes through the established buffer
     * instead. */
    arts_shared_release(&cb);
    if (db_size > 0 && data != NULL) {
      arts_shared_ptr_t wh = arts_db_buf_acquire(cache);
      struct arts_db_buffer_s *wbuf =
          (struct arts_db_buffer_s *)arts_shared_get(wh);
      if (wbuf != NULL) {
        memcpy(wbuf->data, data, (size_t)db_size);
      }
      arts_shared_release(&wh);
    }
  }
  buf_note_present(cache);
#endif
}
