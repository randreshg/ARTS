/* SPDX-License-Identifier: Apache-2.0
 *
 * DB cache / db_s layout: the static invariants every protocol's descriptor
 * keeps, and C-vs-C++ parity of the cell's DB structs.
 *
 * A non-home / stub-installed / creator-remote DB is allocated as a
 * *cache-only stub* of arts_db_cache_stub_size() bytes instead of the full
 * sizeof(struct arts_db_s).  The cache destructor reads db->home_initialized
 * on EVERY free to decide whether to tear the home directory down; on a stub
 * it must read a (zeroed) false and skip teardown.  Therefore the following
 * MUST hold, by construction, for every protocol:
 *
 *   1. stub_size > offsetof(arts_db_s, db_type)         — db_type in bounds
 *      (every coherence lookup/free reads db_type through a stub).
 *   2. stub_size > offsetof(arts_db_s, home_initialized) — home_initialized in
 *      bounds (the destructor reads it on a stub).
 *   3. stub_size == offsetof(arts_db_s, <first home-arm field>) — the stub ends
 *      exactly at the first home-directory field (for VAL rw_holder, or
 *      pending_rw where the home names no holder; lock_state for EXCL), so it
 *      INCLUDES home_initialized but omits the bulky home directory.
 *   4. home_initialized is laid out AFTER db_type which is AFTER the cache —
 *      i.e. offsetof(cache)==0 < offsetof(db_type) <
 * offsetof(home_initialized).
 *   5. The buffer's payload member lands at offset 64 (cache-line
 *      boundary), whether it is the inline bytes or a pointer to storage
 *      the descriptor does not own.
 *   6. read_waiter.link is FIRST (offset 0) — required by arts_lf_stack_t.
 *   7. arts_db_total_size(db) spans the descriptor allocation: sizeof(arts_db_s)
 *      alone for a coherent DB (whose payload is its coherence buffer), and
 *      sizeof(arts_db_s) + db->cache.db_size for a subtype whose payload is
 *      inline after the struct.
 *
 * The descriptors have ONE definition per cell.  Their atomic members are
 * spelled _Atomic(T), which a C++ translation unit (the CUDA sources, and
 * the C sources a GPU build compiles as CUDA) sees as std::atomic<T>, and
 * such a unit shares the descriptors with the C runtime.  This source is
 * compiled both as C and as C++; each compilation prints one line of the
 * cell's DB struct sizes and member offsets, and a ctest of its own compares
 * the two lines.
 *
 * This is a pure static-layout test: it starts no runtime and links nothing.
 * Built per protocol; the first home-arm field and the member set are
 * protocol-dependent and selected by the same #if ladder the runtime header
 * (coherence/types.h) uses.
 */

#include "arts/coherence/types.h"

#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The protocol-dependent first home-arm field, mirroring
 * arts_db_cache_stub_size() in coherence/types.h. */
#if defined(ARTS_PROTOCOL_EXCL)
#define FIRST_HOME_ARM_OFF offsetof(struct arts_db_s, lock_state)
#elif defined(ARTS_PROTOCOL_INV)
#define FIRST_HOME_ARM_OFF offsetof(struct arts_db_s, dir_state)
#elif defined(ARTS_PROTOCOL_FLUSH)
#define FIRST_HOME_ARM_OFF offsetof(struct arts_db_s, sharers)
#elif defined(ARTS_PROTOCOL_VAL) && defined(ARTS_RELEASE_PURGE)
#define FIRST_HOME_ARM_OFF offsetof(struct arts_db_s, pending_rw)
#elif defined(ARTS_PROTOCOL_VAL)
#define FIRST_HOME_ARM_OFF offsetof(struct arts_db_s, rw_holder)
#endif

/* --- Compile-time invariants (the load-bearing ones) ------------------- */

/* (5) the buffer's payload member at offset 64. */
static_assert(offsetof(struct arts_db_buffer_s, data) == 64,
              "the buffer's payload member must land at offset 64 "
              "(cache-line boundary)");

/* (6) read waiter link first (Treiber/arts_lf_stack_t contract). */
static_assert(offsetof(struct arts_db_read_waiter_s, link) == 0,
              "read_waiter.link must be the FIRST member");

/* (4) cache first, then db_type, then home_initialized. */
static_assert(offsetof(struct arts_db_s, cache) == 0,
              "cache must be the FIRST member of arts_db_s");
static_assert(offsetof(struct arts_db_s, db_type) >
                  offsetof(struct arts_db_s, cache),
              "db_type must follow the cache");
static_assert(offsetof(struct arts_db_s, home_initialized) >
                  offsetof(struct arts_db_s, db_type),
              "home_initialized must follow db_type");

#ifdef __cplusplus
#define LAYOUT_LANG "cpp"
#else
#define LAYOUT_LANG "c"
#endif

/* One struct's size and alignment, then one member's offset and size. */
#define TYPE(t) printf(" " #t "=%zu/%zu", sizeof(t), (size_t)alignof(t))
#define MEMBER(t, m)                                                           \
  printf(" " #t "." #m "=%zu/%zu", offsetof(t, m), sizeof(((t *)0)->m))
/* A member whose size is not the struct's to state (a flexible array). */
#define OFFSET(t, m) printf(" " #t "." #m "=%zu", offsetof(t, m))

typedef struct arts_db_cache_s cache_t;
typedef struct arts_db_s db_t;
typedef struct arts_db_buffer_s buffer_t;
typedef struct arts_db_read_waiter_s read_waiter_t;
typedef struct arts_db_rw_waiter_s rw_waiter_t;
#ifndef ARTS_PROTOCOL_FLUSH
typedef struct arts_home_grantreq_node_s grantreq_node_t;
typedef struct arts_home_grantreq_queue_s grantreq_queue_t;
#endif
#ifdef ARTS_PROTOCOL_INV
typedef struct arts_db_inv_waiter_pool_s inv_waiter_pool_t;
typedef struct arts_db_inv_waiter_s inv_waiter_t;
#endif
#ifdef ARTS_PROTOCOL_EXCL
typedef struct arts_lock_ro_node_s lock_ro_node_t;
#endif

/* Every member of the cell's DB structs, so the sibling compilation can be
 * compared byte for byte: size and alignment alone cannot detect two members
 * moving within unchanged padding. */
static void print_fingerprint(void) {
  printf("DB_LAYOUT %s", LAYOUT_LANG);

  TYPE(buffer_t);
#ifdef ARTS_DB_BUF_POOL
  MEMBER(buffer_t, pool_link);
#endif
  MEMBER(buffer_t, version);
  MEMBER(buffer_t, cb);
#ifdef ARTS_DB_BUF_POOL
  MEMBER(buffer_t, owner_cache);
  MEMBER(buffer_t, capacity);
  MEMBER(buffer_t, park_state);
#endif
  OFFSET(buffer_t, data);
  TYPE(read_waiter_t);
  MEMBER(read_waiter_t, link);
  MEMBER(read_waiter_t, edt_guid);
  MEMBER(read_waiter_t, slot);
  MEMBER(read_waiter_t, serve);
  MEMBER(read_waiter_t, requester);
  MEMBER(read_waiter_t, rdzv);
  TYPE(rw_waiter_t);
  MEMBER(rw_waiter_t, link);
  MEMBER(rw_waiter_t, edt_guid);
  MEMBER(rw_waiter_t, slot);
#ifndef ARTS_PROTOCOL_FLUSH
  TYPE(grantreq_node_t);
  MEMBER(grantreq_node_t, next);
  MEMBER(grantreq_node_t, rank);
  MEMBER(grantreq_node_t, have_version);
  MEMBER(grantreq_node_t, rdzv);
  TYPE(grantreq_queue_t);
  MEMBER(grantreq_queue_t, tail);
  MEMBER(grantreq_queue_t, head);
  MEMBER(grantreq_queue_t, stub);
#endif

  TYPE(cache_t);
#if defined(ARTS_PROTOCOL_INV) || defined(ARTS_PROTOCOL_EXCL)
  MEMBER(cache_t, cache_state);
#endif
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  MEMBER(cache_t, writer_count);
  MEMBER(cache_t, buf_holds);
#endif
  MEMBER(cache_t, buffer);
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
  MEMBER(cache_t, buf_freelist);
  MEMBER(cache_t, buf_retired);
  MEMBER(cache_t, pending_rw);
  MEMBER(cache_t, grant_req_in_flight);
#ifndef ARTS_RELEASE_PURGE
  MEMBER(cache_t, incoming_new_owner);
  MEMBER(cache_t, incoming_new_owner_rdzv);
  MEMBER(cache_t, incoming_new_owner_have);
#endif
  MEMBER(cache_t, cached_version);
#if defined(ARTS_WRITE_POLICY_WT)
  MEMBER(cache_t, publication_waiters);
#endif
#if defined(ARTS_PROTOCOL_INV) || defined(ARTS_WRITE_POLICY_WT)
  MEMBER(cache_t, pub_flight);
  MEMBER(cache_t, pub_waiters);
  MEMBER(cache_t, pub_parked);
  MEMBER(cache_t, home_pub_addr);
  MEMBER(cache_t, home_pub_rkey);
  MEMBER(cache_t, home_pub_txid);
#endif
#ifdef ARTS_RELEASE_PURGE
  MEMBER(cache_t, pending_grant_return);
  MEMBER(cache_t, grant_generation);
#endif
#endif
#ifdef ARTS_PROTOCOL_VAL
  MEMBER(cache_t, snapshot_req_in_flight);
  MEMBER(cache_t, ro_combine);
  MEMBER(cache_t, ro_combine_group);
#endif
#ifdef ARTS_PROTOCOL_INV
  MEMBER(cache_t, waiters);
  TYPE(inv_waiter_pool_t);
  MEMBER(inv_waiter_pool_t, chunks);
  MEMBER(inv_waiter_pool_t, next_fresh);
  MEMBER(inv_waiter_pool_t, free_head);
  TYPE(inv_waiter_t);
  MEMBER(inv_waiter_t, next);
  MEMBER(inv_waiter_t, slot);
  MEMBER(inv_waiter_t, edt_guid);
#endif
#ifdef ARTS_PROTOCOL_EXCL
  TYPE(lock_ro_node_t);
  MEMBER(lock_ro_node_t, link);
  MEMBER(lock_ro_node_t, rank);
  MEMBER(lock_ro_node_t, rdzv);
  MEMBER(cache_t, ro_pending);
  MEMBER(cache_t, rw_pending);
#if defined(ARTS_RELEASE_RETAIN)
  MEMBER(cache_t, ro_serve);
  MEMBER(cache_t, migrate_rdzv);
#elif !defined(ARTS_USE_CXL)
  MEMBER(cache_t, home_pub_rdzv);
#endif
#ifdef ARTS_USE_CXL
  MEMBER(cache_t, cxl_addr);
#endif
#endif
#ifdef ARTS_PROTOCOL_FLUSH
  MEMBER(cache_t, home_line_addr);
  MEMBER(cache_t, home_line_rkey);
  MEMBER(cache_t, flush_txid);
#endif
  MEMBER(cache_t, db_guid);
  MEMBER(cache_t, db_size);
  MEMBER(cache_t, payload_pending);
  MEMBER(cache_t, init_kind);

  TYPE(db_t);
  MEMBER(db_t, cache);
  MEMBER(db_t, db_type);
  MEMBER(db_t, home_initialized);
#if defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)
#ifndef ARTS_RELEASE_PURGE
  MEMBER(db_t, rw_holder);
  MEMBER(db_t, pending_install_owner);
#else
  MEMBER(db_t, pending_return_from);
  MEMBER(db_t, pending_return_cv);
  MEMBER(db_t, pending_return_version);
  MEMBER(db_t, pending_return_ack);
  MEMBER(db_t, pending_return_credit);
#endif
  MEMBER(db_t, pending_rw);
  MEMBER(db_t, invalidate_in_flight);
  MEMBER(db_t, cached_ranks);
#endif
#if defined(ARTS_PROTOCOL_VAL) && defined(ARTS_WRITE_POLICY_WT)
  MEMBER(db_t, cached_version);
#endif
#ifdef ARTS_PROTOCOL_INV
  MEMBER(db_t, dir_state);
  MEMBER(db_t, hver);
  MEMBER(db_t, pub_queue);
  MEMBER(db_t, round_entries);
  MEMBER(db_t, roster);
#endif
#ifdef ARTS_PROTOCOL_EXCL
  MEMBER(db_t, lock_state);
  MEMBER(db_t, rw_waiters);
  MEMBER(db_t, ro_waiters);
  MEMBER(db_t, cached_ranks);
#ifdef ARTS_RELEASE_RETAIN
  MEMBER(db_t, ro_retainers);
#endif
#endif
#ifdef ARTS_PROTOCOL_FLUSH
  MEMBER(db_t, sharers);
  MEMBER(db_t, sharer_words);
#endif
#ifdef ARTS_GPU_LAYOUT
  MEMBER(db_t, gpu_lock);
  MEMBER(db_t, version);
  MEMBER(db_t, time_stamp);
#endif

  printf("\n");
}

int main(void) {
  int rc = 0;
  uint64_t stub = arts_db_cache_stub_size();
  size_t off_db_type = offsetof(struct arts_db_s, db_type);
  size_t off_home_init = offsetof(struct arts_db_s, home_initialized);
  size_t off_first_home = FIRST_HOME_ARM_OFF;

  /* (1) db_type in bounds of the stub. */
  if (!(stub > off_db_type)) {
    (void)fprintf(stderr,
                  "FAIL db_cache_layout: stub_size(%llu) must be > "
                  "offsetof(db_type)=%zu\n",
                  (unsigned long long)stub, off_db_type);
    rc = 1;
  }
  /* (2) home_initialized in bounds. */
  if (!(stub > off_home_init)) {
    (void)fprintf(stderr,
                  "FAIL db_cache_layout: stub_size(%llu) must be > "
                  "offsetof(home_initialized)=%zu (the teardown reads it)\n",
                  (unsigned long long)stub, off_home_init);
    rc = 1;
  }
  /* The stub must still leave room to read the whole bool home_initialized. */
  if (!(stub >=
        off_home_init + sizeof(((struct arts_db_s *)0)->home_initialized))) {
    (void)fprintf(stderr,
                  "FAIL db_cache_layout: stub_size(%llu) truncates "
                  "home_initialized (end=%zu)\n",
                  (unsigned long long)stub,
                  off_home_init +
                      sizeof(((struct arts_db_s *)0)->home_initialized));
    rc = 1;
  }
  /* (3) stub ends exactly at the first home-arm field. */
  if (stub != off_first_home) {
    (void)fprintf(stderr,
                  "FAIL db_cache_layout: stub_size(%llu) != "
                  "offsetof(first home-arm field)=%zu\n",
                  (unsigned long long)stub, off_first_home);
    rc = 1;
  }
  /* first home-arm field must be at or after home_initialized. */
  if (!(off_first_home >= off_home_init)) {
    (void)fprintf(stderr,
                  "FAIL db_cache_layout: first home-arm off(%zu) precedes "
                  "home_initialized(%zu)\n",
                  off_first_home, off_home_init);
    rc = 1;
  }

  /* (7) total_size spans the descriptor allocation, which the subtype decides:
   * the struct alone when the payload lives in the coherence buffer, struct +
   * payload when it is inline. */
  struct arts_db_s db;
  memset((void *)&db, 0, sizeof(db));
  uint64_t sizes[] = {0, 1, 64, 4096};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    atomic_store_explicit(&db.cache.db_size, sizes[i], memory_order_relaxed);
    db.db_type = ARTS_DB;
    uint64_t got = arts_db_total_size(&db);
    if (got != (uint64_t)sizeof(struct arts_db_s)) {
      (void)fprintf(stderr,
                    "FAIL db_cache_layout: coherent total_size(db_size=%llu)"
                    "=%llu, expected %llu\n",
                    (unsigned long long)sizes[i], (unsigned long long)got,
                    (unsigned long long)sizeof(struct arts_db_s));
      rc = 1;
    }
    db.db_type = ARTS_DB_GPU_PIN;
    uint64_t want = (uint64_t)sizeof(struct arts_db_s) + sizes[i];
    got = arts_db_total_size(&db);
    if (got != want) {
      (void)fprintf(stderr,
                    "FAIL db_cache_layout: inline total_size(db_size=%llu)"
                    "=%llu, expected %llu\n",
                    (unsigned long long)sizes[i], (unsigned long long)got,
                    (unsigned long long)want);
      rc = 1;
    }
  }
  db.db_type = ARTS_DB;

  /* arts_db_of_cache round-trip (cache is first ⇒ same address; NULL-safe). */
  if (arts_db_of_cache(&db.cache) != &db) {
    (void)fprintf(stderr, "FAIL db_cache_layout: of_cache(&db.cache) != &db\n");
    rc = 1;
  }
  if (arts_db_of_cache(NULL) != NULL) {
    (void)fprintf(stderr, "FAIL db_cache_layout: of_cache(NULL) != NULL\n");
    rc = 1;
  }

  if (rc != 0) {
    return 1;
  }
  print_fingerprint();
  printf("PASS db_cache_layout [%s]: stub=%llu db_type=%zu home_init=%zu "
         "first_home_arm=%zu data=64 link=0\n",
         LAYOUT_LANG, (unsigned long long)stub, off_db_type, off_home_init,
         off_first_home);
  return 0;
}
