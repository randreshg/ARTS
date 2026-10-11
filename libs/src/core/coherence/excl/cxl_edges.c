/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL x CXL: the two funnels through the block's store, shared by both
 * release policies.
 *
 * Not a translation unit of its own: purge.c and retain.c each #include it
 * (the arbiters.c pattern), and only one of them is linked into a library, so
 * every definition here exists once per library.  Compiled only under
 * ARTS_USE_CXL.
 *
 * The block's bytes live in its store, not on the wire.  A rank moves them
 * through the store at its two ownership edges and nowhere else:
 *
 *   fetch  where this rank is handed the block — before the CAS that admits
 *          anyone to it, so nothing reads or writes the working copy while it
 *          is being filled.
 *   purge  where this rank hands the block on — before the message that lets
 *          anyone else be admitted, so every onward reader finds the turn's
 *          writes in the store.
 *
 * WHERE those edges fall is the release policy's: PURGE fetches at every grant
 * and purges at every write turn's last release, because the right goes back
 * to the home at that release; RETAIN fetches where a DELIVER makes this rank
 * the owner or a reader, and purges only where the owner ships the block to
 * another rank, because the right stays put until someone asks for it.  Each
 * transfer is the flush and the copy, run synchronously on whatever thread
 * reaches the edge; nothing is deferred, and nothing is acknowledged beyond
 * the protocol's own messages.  Acquires and releases between the two edges
 * touch the working copy alone.  What "bring" and "write back" mean is the two
 * residency bodies at the end of this file. */

#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/cxl/store.h"
#include "arts/system/print.h"

/* The arm's own observables. */
uint64_t arts_cxl_fetches;
uint64_t arts_cxl_purges;

/* The residency seam: declared once here, defined exactly once per residency,
 * and called only from the two shared funnels below — which have already
 * validated the store, taken the buffer ref and read the size. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
#if !defined(ARTS_CXL_STAGED) && !defined(ARTS_CXL_DIRECT)
#error "a CXL build names a residency"
#endif

/* A SIZED block must know its store address.  A create records the
 * store before the block can be asked for and every grant or deliver names
 * it, so a zero here is a create that skipped its allocation or a message
 * that carried none — the same defect, with the same answer, on either
 * residency.  It lives in the shared part for that reason: the slot rule is
 * what it checks, not a funnel.  A block whose declared size is 0 is exempt: a
 * sentinel has no bytes, so it has no store and a message for it carries
 * address 0 legitimately. */
static void cxl_slot_required(const struct arts_db_cache_s *cache,
                              const char *edge) {
  if (arts_db_cxl_slot_addr(cache) == 0 && cache->db_size != 0) {
    ARTS_ERROR("cxl: guid %lu has no slot at its %s edge",
               (unsigned long)cache->db_guid, edge);
  }
}

/* Bring the block's canonical bytes under this rank's grant.  The caller runs
 * it before the CAS that admits the cohort, while every acquire of the axis is
 * still parked, so nothing reads or writes the working copy being filled.  The
 * buffer ref and the block's handle are held across it, and the slot outlives
 * every turn: nothing reclaims it within a run.
 * The block's handle keeps the descriptor's destructor away, so a working copy
 * is missing here only when the ensure could not allocate one — and a grant
 * whose cohort cannot be served is a hang, never a soft drop. */
static void cxl_fetch_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = cache->db_size;
  cxl_slot_required(cache, "grant");
  if (n == 0) {
    return; /* a sentinel-sized block has no bytes and no store */
  }
  (void)arts_db_buf_ensure(cache, n);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    ARTS_ERROR("cxl: guid %lu has no working copy of %llu bytes to fetch its "
               "grant into",
               (unsigned long)cache->db_guid, (unsigned long long)n);
  }
  cxl_fetch_body(cache, buf);
  arts_db_buf_release(&h);
  __atomic_fetch_add(&arts_cxl_fetches, 1u, __ATOMIC_RELAXED);
}

/* Return this rank's bytes to the block's slot.
 * Synchronous on the thread that reaches the hand-on edge: the caller has
 * already committed, in one CAS, the transition that stops any local holder
 * from being admitted to the working copy, so nothing can write the copy
 * while it is drained — a protection that holds only while the copy completes
 * before the message that hands the block on is sent.
 *
 * PRECONDITION, enforced rather than assumed: the caller holds the edge, and
 * the store is the one this block's creator allocated.  One generation has
 * one store because its creator mints it once, and a message naming a store
 * other than the one this rank's cache names is refused where it is
 * recorded. */
static void cxl_purge_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = cache->db_size;
  cxl_slot_required(cache, "release");
  if (n == 0) {
    return;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    /* No working copy: either the route slot is gone under a holder that
     * still owed its release (the documented boundary), or nobody has ever
     * written the block here.  Nothing is written. */
    arts_db_buf_release(&h);
    return;
  }
  cxl_purge_body(cache, buf);
  arts_db_buf_release(&h);
  __atomic_fetch_add(&arts_cxl_purges, 1u, __ATOMIC_RELAXED);
}

#ifdef ARTS_CXL_STAGED
/* Staged residency: the working copy is DRAM, so the bytes are reloaded out of
 * the block's store and copied in.  The consumer flush precedes the read: this
 * rank's private lines may be clean stale copies of the store. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = cache->db_size;
  const void *slot = (const void *)(uintptr_t)arts_db_cxl_slot_addr(cache);
  arts_cxl_flush_consumer(slot, (size_t)n);
  memcpy(buf->data, slot, (size_t)n);
}

/* Staged residency: the turn's bytes are copied out of the DRAM working copy
 * and the producer flush follows the write. */
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = cache->db_size;
  void *slot = (void *)(uintptr_t)arts_db_cxl_slot_addr(cache);
  memcpy(slot, buf->data, (size_t)n);
  arts_cxl_flush_producer(slot, (size_t)n);
}
#endif /* ARTS_CXL_STAGED */

#ifdef ARTS_CXL_DIRECT
/* The working bytes ARE the block's store here: the fetch's arts_db_buf_ensure
 * adopted the slot, so this descriptor names it and there is nothing to copy.
 * A fetch is the consumer flush that makes this rank's private lines the
 * store's. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_cxl_flush_consumer(buf->data, (size_t)cache->db_size);
}

/* A purge is the producer flush that publishes this rank's writes to the
 * store.  It runs before the hand-on message is sent, which is what makes it
 * precede every onward admission. */
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_cxl_flush_producer(buf->data, (size_t)cache->db_size);
}
#endif /* ARTS_CXL_DIRECT */
