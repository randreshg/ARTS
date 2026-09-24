/* SPDX-License-Identifier: Apache-2.0
 *
 * Buffer lifecycle implementation.  See coherence_buffer.h for the contract.
 *
 * The buffer is managed as an arts_shared_ptr_t: cache.buffer is the atomic
 * slot, the slot holds the "cache-hold" ref, and every acquirer holds one more.
 * The cb deleter frees the buffer on the last drop.  Because the split-
 * reference-counting load pins the cb without dereferencing it (and the buffer
 * carries no back-pointer to its cache), an in-flight acquire is immune to a
 * concurrent destroy: the bytes live until the final holder releases, and the
 * release path touches only the (always-valid) cb handle, never a possibly-
 * freed buffer.
 */

#include "arts/coherence/buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts/gas/guid.h" /* arts_db_szhint_bound */
#include "arts/memory/regpool.h"
#include "arts/system/print.h"  /* ARTS_ERROR (unadvertisable landing) */
#include "arts/transport/net.h" /* arts_net_rdzv_local / _txid_next */
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

/* cb deleter — runs once, on the last strong drop.  When owner_cache is set,
 * recycles the buffer onto the per-DB free-list (unbounded) instead of freeing
 * so that subsequent installs can reuse it without hitting the allocator. */
static void buffer_deleter(void *obj) {
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)obj;
  b->cb = NULL;
  struct arts_db_cache_s *cache = b->owner_cache;
  if (cache != NULL) {
    arts_lf_pool_release(&cache->buf_freelist, &b->pool_link);
    return;
  }
  arts_regpool_free(b);
}

struct arts_db_buffer_s *arts_db_buf_alloc(struct arts_db_cache_s *cache,
                                           uint64_t db_size) {
  /* Pull a recycled buffer from the per-DB free-list when available. */
  arts_lf_link_t *node = arts_lf_pool_pop_or_null(&cache->buf_freelist);
#ifdef ARTS_FAM_DIRECT
  /* The payload is not the descriptor's to size, so every descriptor is one
   * size and the per-DB recycle pool stays uniform.  A descriptor leaves this
   * call naming no storage, recycled or fresh. */
  (void)db_size;
  struct arts_db_buffer_s *b =
      (node != NULL) ? (struct arts_db_buffer_s *)node
                     : (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
                           sizeof(struct arts_db_buffer_s), 64);
  if (b != NULL) {
    b->data = NULL;
  }
  return b;
#else
  if (node != NULL) {
    return (
        struct arts_db_buffer_s *)node; /* recycled; caller re-inits fields */
  }
  /* 64-byte aligned so buf->data (offset 64) lands on a cache-line / CXL
   * boundary.  Drawn from the registered pool so every DB payload buffer
   * falls inside a slab that is (or will be) pinned and pre-registered with
   * the fabric, resolvable by arts_regpool_lookup for one-sided RDMA.
   * Recycled by the cb deleter via the free-list; freed only when
   * owner_cache is NULL (shouldn't happen in normal operation). */
  return (struct arts_db_buffer_s *)arts_regpool_alloc_aligned(
      sizeof(struct arts_db_buffer_s) + db_size, 64);
#endif
}

#ifndef ARTS_FAM_DIRECT
/* As arts_db_buf_alloc, but buf->data reads as zero.  A recycled buffer is
 * cleared here (its previous contents are arbitrary); a fresh pool
 * allocation arrives zeroed without being touched. */
struct arts_db_buffer_s *arts_db_buf_alloc_zeroed(struct arts_db_cache_s *cache,
                                                  uint64_t db_size) {
  arts_lf_link_t *node = arts_lf_pool_pop_or_null(&cache->buf_freelist);
  if (node != NULL) {
    struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)node;
    memset(b->data, 0, (size_t)db_size);
    return b;
  }
  return (struct arts_db_buffer_s *)arts_regpool_zalloc_aligned(
      sizeof(struct arts_db_buffer_s) + db_size, 64);
}
#endif

arts_shared_ptr_t arts_db_buf_detached(uint64_t db_size,
                                       struct arts_db_buffer_s **out) {
#ifdef ARTS_FAM_DIRECT
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
  b->owner_cache = NULL;
  b->version = 0;
  b->cb = arts_shared_make(b, buffer_deleter);
  *out = b;
  return b->cb;
#endif
}

arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache) {
  /* Acquire-and-validate load: returns a caller-owned strong ref (keeps the
   * buffer alive) or NULL if no buffer is installed.  Caller releases via
   * arts_db_buf_release. */
  return arts_atomic_shared_load(&cache->buffer);
}

void arts_db_buf_release(arts_shared_ptr_t *h) { arts_shared_release(h); }

/* One place records that the payload slot is no longer empty, so the
 * per-acquire question "does this block still need materializing" is a byte
 * load instead of a refcounted look at the slot.  Called wherever a non-NULL
 * buffer ends up in the slot — including an install-if-absent that LOST,
 * since the winner's buffer is in there either way.  Release order pairs
 * with the relaxed load at the fast exit: the flag is a hint, and whoever
 * acts on it reads the pointer from the slot itself. */
static inline void buf_note_present(struct arts_db_cache_s *cache) {
  __atomic_store_n(&cache->payload_pending, (uint8_t)0, __ATOMIC_RELEASE);
}

#ifdef ARTS_FAM_DIRECT
bool arts_db_buf_adopt_external(struct arts_db_cache_s *cache, void *payload,
                                uint64_t version, uint64_t db_size) {
  if (payload == NULL) {
    /* A rank that has not learned the block's store, or a block with none,
     * has nothing to adopt. */
    return false;
  }
  if (__atomic_load_n(&cache->payload_pending, __ATOMIC_RELAXED) == 0u) {
    return false;
  }
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, 0);
  if (nb == NULL) {
    return false; /* OOM — caller decides how to surface. */
  }
  nb->owner_cache = cache;
  nb->version = version;
  nb->data = (char *)payload;
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_atomic_shared_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter recycles nb */
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
  if (cache->db_size == 0) {
    cache->db_size = db_size;
  }
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
#endif /* ARTS_FAM_DIRECT */

bool arts_db_buf_ensure(struct arts_db_cache_s *cache, uint64_t db_size) {
  /* A byte load answers the common case: once anything has installed a
   * buffer, no later use has to materialize one, and asking the slot itself
   * would cost every acquire a refcount round-trip on a shared line.
   * Relaxed, because nothing rests on the answer — a caller that stops here
   * goes on to read the pointer from the slot with its own acquire load, and
   * a caller that reads a stale 1 simply tries an install that then fails
   * and clears the flag. */
  if (__atomic_load_n(&cache->payload_pending, __ATOMIC_RELAXED) == 0u) {
    return false;
  }
  if (db_size == 0) {
    /* A zero-sized block has no storage to hold: NULL is its defined
     * value. */
    return false;
  }
#ifdef ARTS_FAM_DIRECT
  return arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_fam_slot_addr(cache), 1u, db_size);
#else
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  if (arts_shared_get(h) != NULL) {
    arts_db_buf_release(&h);
    return false;
  }
  struct arts_db_buffer_s *nb = arts_db_buf_alloc_zeroed(cache, db_size);
  if (nb == NULL) {
    return false; /* OOM — caller decides how to surface. */
  }
  nb->owner_cache = cache;
  /* Version 1, not 0: the block's first image is its VALUE, not a
   * placeholder awaiting a publication.  An arm whose "has anyone published
   * yet" predicate is version > 0 must read this as published, or a reader
   * would hold for a writer that may never come. */
  nb->version = 1;
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  /* Install-if-absent, never the version-conditional publish: this buffer
   * may only ever fill a hole.  Two first users therefore agree by
   * construction — one installs, the other adopts a buffer whose bytes are
   * identical to the one it built. */
  if (!arts_atomic_shared_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter recycles nb */
    buf_note_present(cache);
    return false;
  }
  if (cache->db_size == 0) {
    cache->db_size = db_size;
  }
  buf_note_present(cache);
  return true;
#endif
}

#ifdef ARTS_FAM
bool arts_db_buf_withdraw(struct arts_db_cache_s *cache, const void *payload) {
  if (cache == NULL || payload == NULL) {
    return false;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  bool withdrawn = false;
  if (b != NULL && (const void *)b->data == payload) {
    /* The load ref pins the descriptor against recycle, so the slot's own
     * identity cannot have drifted and returned under the compare. */
    withdrawn = arts_atomic_shared_compare_exchange(&cache->buffer, h, NULL);
    if (withdrawn) {
      /* The slot is empty again, so the block needs materializing again.  The
       * flag is a hint with one safe direction: a 1 over a descriptor another
       * thread installs in between costs that descriptor's next materialize
       * one failed install and nothing else, whereas a 0 over an empty slot
       * is a claim no later call corrects -- so the restore is unconditional. */
      __atomic_store_n(&cache->payload_pending, (uint8_t)1, __ATOMIC_RELEASE);
    }
  }
  arts_db_buf_release(&h);
  return withdrawn;
}
#endif

/* Version-conditional publish of a fully-initialized private buffer (fields +
 * payload bytes already set; no cb yet).  Shared by the copy install
 * (arts_db_buf_install) and the rendezvous landed install
 * (arts_db_buf_install_landed).  On a stale loss the private buffer is
 * recycled and the newer installed buffer returned. */
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
    arts_shared_ptr_t old_h = arts_atomic_shared_load(&cache->buffer);
    struct arts_db_buffer_s *old =
        (struct arts_db_buffer_s *)arts_shared_get(old_h);
    /* The publish decision reads a version another rank's release may be
     * bumping in place; an acquire load pairs with that read-modify-write so
     * the comparison never observes a torn or reordered value. */
    if (old != NULL &&
        __atomic_load_n(&old->version, __ATOMIC_ACQUIRE) >= new_version) {
      /* Stale install: a newer (or equal) buffer is already published.
       * Drop our load ref, abandon the unpublished cb (keeps new_buf ours)
       * and free new_buf.  old stays alive via the slot's sentinel ref.
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
      buffer_deleter(new_buf); /* recycle the just-allocated buffer */
      buf_note_present(cache);
      return old;
    }
    /* Conditional publish: install new_cb only while the slot still holds
     * old_h.  old_h pins old's cb (strong >= 1) so the raw-pointer compare
     * cannot ABA.  On success the slot drops its ref on the old value. */
    if (arts_atomic_shared_compare_exchange(&cache->buffer, old_h, new_cb)) {
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
#ifdef ARTS_FAM_DIRECT
  if (data_payload != NULL) {
    ARTS_ERROR("coherence: a payload cannot be published into the block's "
               "store");
  }
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_fam_slot_addr(cache), new_version,
      db_size);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  arts_db_buf_release(&h);
  return b;
#else
  /* data_payload == NULL ⇒ initial install at create-time: the payload must
   * read as zero (deterministic state).  Take the zeroed allocation path so
   * only a recycled buffer is actually cleared — fresh pool memory is
   * kernel-zeroed already, and skipping the redundant full-payload memset
   * keeps the touch (and its page faults) off the creator's critical path. */
  struct arts_db_buffer_s *new_buf =
      (data_payload == NULL && db_size > 0)
          ? arts_db_buf_alloc_zeroed(cache, db_size)
          : arts_db_buf_alloc(cache, db_size);
  if (new_buf == NULL) {
    return NULL; /* OOM — caller decides how to surface. */
  }
  new_buf->owner_cache = cache;
  new_buf->version = new_version;
  /* Publish bytes into buf->data (FAM, canonical user-visible storage). */
  if (db_size > 0) {
    if (data_payload != NULL) {
      memcpy(new_buf->data, data_payload, (size_t)db_size);
    }
    /* Lazy-installed caches start with db_size==0; the first install learns
     * the real size from the wire payload. */
    if (cache->db_size == 0) {
      cache->db_size = db_size;
    }
  }
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
#ifdef ARTS_FAM_DIRECT
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
  uint64_t cur = __atomic_load_n(&buf->version, __ATOMIC_ACQUIRE);
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
    __atomic_store_n(&buf->version, version, __ATOMIC_RELEASE);
  }
  arts_db_buf_release(&h);
#endif
}

/* ===== Rendezvous landing lifecycle (see buffer.h) ======================= */

struct arts_db_buffer_s *
arts_db_buf_landing_alloc(struct arts_db_cache_s *cache, uint64_t db_size,
                          struct arts_rdzv_landing_s *out) {
#ifdef ARTS_FAM_DIRECT
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
  b->owner_cache = cache;
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

void arts_db_buf_landing_recycle(struct arts_db_cache_s *cache,
                                 struct arts_db_buffer_s *b) {
#ifdef ARTS_FAM_DIRECT
  (void)cache;
  (void)b;
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
#else
  if (b == NULL) {
    return;
  }
  b->cb = NULL;
  b->owner_cache = cache;
  arts_lf_pool_release(&cache->buf_freelist, &b->pool_link);
#endif
}

bool arts_db_buf_adopt_landing(struct arts_db_cache_s *cache, uint64_t version,
                               struct arts_db_buffer_s *landing,
                               uint64_t db_size) {
#ifdef ARTS_FAM_DIRECT
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
    arts_db_buf_landing_recycle(cache, landing);
    return false;
  }
  landing->owner_cache = cache;
  landing->version = version;
  /* The landing may be recycled memory, so its bytes are arbitrary.  Every
   * other path that materializes a never-written block hands out zeroes; match
   * it, so "contents undefined" cannot mean "another block's contents". */
  memset(landing->data, 0, (size_t)db_size);
  arts_shared_ptr_t cb = arts_shared_make(landing, buffer_deleter);
  landing->cb = cb;
  /* Install-if-absent, NOT the version-conditional publish: this landing
   * carries no data, so it may only ever fill a hole.  A buffer already in the
   * slot holds real bytes at some version — replacing it, at any version,
   * would destroy them. */
  if (!arts_atomic_shared_compare_exchange(&cache->buffer, NULL, cb)) {
    arts_shared_release(&cb); /* last ref: the deleter recycles the landing */
    buf_note_present(cache);
    return false;
  }
  if (cache->db_size == 0) {
    cache->db_size = db_size;
  }
  buf_note_present(cache);
  return true;
#endif
}

struct arts_db_buffer_s *
arts_db_buf_install_landed(struct arts_db_cache_s *cache, uint64_t new_version,
                           struct arts_db_buffer_s *landing,
                           uint64_t db_size) {
#ifdef ARTS_FAM_DIRECT
  (void)cache;
  (void)new_version;
  (void)landing;
  (void)db_size;
  ARTS_ERROR("coherence: a landing has no meaning where the payload "
             "is not this rank's");
  return NULL;
#else
  landing->owner_cache = cache;
  landing->version = new_version;
  if (db_size > 0 && cache->db_size == 0) {
    cache->db_size = db_size;
  }
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
  return cache->db_size ? cache->db_size
                        : arts_db_szhint_bound(cache->db_guid);
}

void arts_db_buf_prepare_inplace(struct arts_db_cache_s *cache,
                                 uint64_t capacity) {
#ifdef ARTS_FAM_DIRECT
  /* A first touch adopts the block's own store, so a capacity bound sizes
   * nothing and declares nothing. */
  (void)capacity;
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_fam_slot_addr(cache), 0u,
      cache->db_size);
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
  nb->owner_cache = cache;
  nb->version = 0;
  if (capacity > 0) {
    memset(nb->data, 0, (size_t)capacity);
  }
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_atomic_shared_compare_exchange(&cache->buffer, NULL, cb)) {
    /* Lost the install race: adopt the winner (both first images are
     * zero-identical). */
    arts_shared_release(&cb);
  }
  buf_note_present(cache);
#endif
}

void arts_db_buf_write_inplace(struct arts_db_cache_s *cache, const void *data,
                               uint64_t db_size) {
#ifdef ARTS_FAM_DIRECT
  if (data != NULL) {
    ARTS_ERROR("coherence: a payload cannot be published into the block's "
               "store");
  }
  (void)arts_db_buf_adopt_external(
      cache, (void *)(uintptr_t)arts_db_fam_slot_addr(cache), 0u, db_size);
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
    if (cache->db_size == 0) {
      cache->db_size = db_size;
    }
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
   * from the fresh, still-zero instance. */
  struct arts_db_buffer_s *nb = arts_db_buf_alloc(cache, db_size);
  if (nb == NULL) {
    return; /* OOM — caller decides how to surface. */
  }
  nb->owner_cache = cache;
  nb->version = 0;
  if (db_size > 0) {
    if (data != NULL) {
      memcpy(nb->data, data, (size_t)db_size);
    } else {
      memset(nb->data, 0, (size_t)db_size);
    }
    if (cache->db_size == 0) {
      cache->db_size = db_size;
    }
  }
  arts_shared_ptr_t cb = arts_shared_make(nb, buffer_deleter);
  nb->cb = cb;
  if (!arts_atomic_shared_compare_exchange(&cache->buffer, NULL, cb)) {
    /* Lost the install race.  Racing first touches publish the same zero
     * first image, so adopting the winner is value-identical; a
     * data-carrying caller (whose write the coherence protocol serializes
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
