/* SPDX-License-Identifier: Apache-2.0
 *
 * The state arts_db_downgrade leaves, read where it lives (whitebox).  One
 * shape at a time, each ordered after the previous one's EDTs:
 *
 *   creator reference (every arm): after a create's hold is downgraded, its
 *     created record is read-only and its buffer ref names the buffer the
 *     create's pointer names.  FLUSH, creator off the home: the write release
 *     wrote the copy back and emptied the slot, so that ref alone keeps the
 *     copy (where the runtime keeps its debug checks, its count is exactly
 *     1).
 *   EXCL: (a) the cache word goes from (GRANT, wc, rc) to (GRANT, wc - 1,
 *     rc + 1) and back to rest at the hold's release, for a dependence and a
 *     create's hold, on the home and off it; (b) an alias pair adds one
 *     reader; (c) with a second writer of the rank, the turn stays on rc
 *     after that writer's release.
 *   EXCL x PURGE: (d) a writer on another rank waits at the home, counted in
 *     w, while the read hold lives, and then reads D's bytes.
 *   EXCL x RETAIN: (e) a writer on another rank sets the migration target
 *     and the bytes stay resident while the read hold lives; (f) a reader on
 *     another rank is served by a demote of the write permission, and a
 *     writer on D's rank then takes the turn back by self-migration while D
 *     still reads.
 *   VAL, INV: (g) the grant word holds no write hold once the downgrade
 *     returns and the install's drain guard is gone (back to the sentinel
 *     under RETAIN; under PURGE handed back off the home, kept on it with no
 *     request waiting); (h) off the home,
 *     a writer on the home ordered after the hand-off writes B, a reader on
 *     D's rank ordered after that writer sees it in a buffer the read
 *     installed, and D's own buffer keeps its A.
 *
 * Shapes that need another rank run on two or more.
 */
#include "arts/utils/stdatomic_compat.h"

#include "arts.h"

#include "../test_failure_status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/types.h"
#include "arts/edt_context.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"
#include "arts/utils/vector.h"

#ifdef ARTS_VALIDITY_EXCL
#include "arts/coherence/excl/types.h"
#endif

#define NAME "db_downgrade_word"
#define WORDS 64u
#define WA 32u
#define SZ (WORDS * sizeof(uint64_t))
#define WAIT_MS 60000L

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void fail(const char *shape, const char *what) {
  printf("FAIL: " NAME " %s: %s\n", shape, what);
  arts_test_fail();
}

static void fill(uint64_t *p, uint64_t base, unsigned int from,
                 unsigned int to) {
  for (unsigned int j = from; j < to; j++) {
    p[j] = base + j;
  }
}

static bool holds(const uint64_t *p, uint64_t base, unsigned int from,
                  unsigned int to) {
  for (unsigned int j = from; j < to; j++) {
    if (p == NULL || p[j] != base + j) {
      return false;
    }
  }
  return true;
}

/* A block homed on `home`, its bytes zero, released. */
static arts_guid_t fresh_block(unsigned int home) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p != NULL) {
    for (unsigned int j = 0; j < WORDS; j++) {
      ((uint64_t *)p)[j] = 0;
    }
  }
  arts_db_release(g);
  return g;
}

/* This rank's descriptor of `g`, pinned by *h. */
static struct arts_db_s *local_db(arts_guid_t g, arts_shared_ptr_t *h) {
  *h = arts_route_table_lookup_db(g);
  return (struct arts_db_s *)arts_shared_get(*h);
}

/* The running EDT's created record of `g`. */
static struct arts_created_db_s *created_record(arts_guid_t g) {
  arts_vector_t *list = arts_get_created_db_list();
  for (uint64_t i = arts_vector_count(list); i > 0; i--) {
    struct arts_created_db_s *e =
        (struct arts_created_db_s *)arts_vector_at(list, i - 1);
    if (((struct arts_db_s *)arts_shared_get(e->db))->cache.db_guid == g) {
      return e;
    }
  }
  return NULL;
}

/* Same-rank hand-shakes of a shape's EDTs: process memory, not the block,
 * since D gives up writing the block at its downgrade. */
static _Atomic(unsigned int) g_peer_holds;
static _Atomic(unsigned int) g_peer_go;

static bool await_flag(_Atomic(unsigned int) *f, const char *shape,
                       const char *what) {
  long start = now_ms();
  while (atomic_load_explicit(f, memory_order_acquire) == 0u) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, what);
      return false;
    }
  }
  return true;
}

/* ---- the creator reference ---------------------------------------------- */

/* paramv[0]: the home. */
static void cref_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  const char *shape = "creator reference";
  unsigned int home = (unsigned int)paramv[0];
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p == NULL) {
    fail(shape, "an acquiring create was handed no pointer");
    return;
  }
  fill((uint64_t *)p, 0xCEF0000u, 0, WORDS);
  arts_db_downgrade(g);
  struct arts_created_db_s *e = created_record(g);
  if (e == NULL || !e->read_only || e->read == NULL) {
    fail(shape, "the created record is not a read hold with a buffer ref");
    return;
  }
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(e->read);
  if (buf == NULL || (void *)buf->data != p) {
    fail(shape, "the record's buffer is not the one the create's pointer "
                "names");
    return;
  }
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(e->db);
#ifdef ARTS_VALIDITY_FLUSH
  if (home != arts_get_current_rank()) {
    if (!arts_shared_slot_empty(&db->cache.buffer)) {
      fail(shape, "the write-back left the creator's slot full");
    }
#ifdef ARTS_TEST_RUNTIME_NDEBUG_UNSET
    if (arts_shared_debug_refs(e->read) != 1) {
      fail(shape, "the creator's copy is held by more than the record");
    }
#endif
  }
#else
  arts_shared_ptr_t cur = arts_db_buf_acquire(&db->cache);
  if (cur != e->read) {
    fail(shape, "the record's buffer is not the installed one");
  }
  arts_db_buf_release(&cur);
#endif
  if (!holds((const uint64_t *)p, 0xCEF0000u, 0, WORDS)) {
    fail(shape, "the creator's bytes changed under its read hold");
  }
}

/* ---- EXCL: the cache word ------------------------------------------------ */

#ifdef ARTS_VALIDITY_EXCL
#ifdef ARTS_RETENTION_PURGE
#define REST_RWS CACHE_ST_IDLE
#else
#define REST_RWS CACHE_ST_GRANT
#endif

static uint64_t cache_word_of(struct arts_db_s *db) {
  return atomic_load_explicit(&db->cache.cache_state, memory_order_acquire);
}

static void expect_word(struct arts_db_s *db, unsigned int rws, unsigned int wc,
                        unsigned int rc, const char *shape, const char *when) {
  uint64_t w = cache_word_of(db);
  bool ok =
      CACHE_RW_ST(w) == rws && CACHE_RW_CNT(w) == wc && CACHE_RO_CNT(w) == rc;
#ifdef ARTS_RETENTION_RETAIN
  ok = ok && CACHE_OWNER(w) == 1u;
#endif
  if (!ok) {
    printf("FAIL: " NAME " %s %s: word 0x%llx (rws %u wc %u rc %u), expected "
           "rws %u wc %u rc %u\n",
           shape, when, (unsigned long long)w, CACHE_RW_ST(w), CACHE_RW_CNT(w),
           CACHE_RO_CNT(w), rws, wc, rc);
    arts_test_fail();
  }
}

/* (a) and (b).  paramv: created (1) or dependence (0), the home for a
 * create.  depv: the dependence, RW, and with an alias a second slot, RO. */
static void excl_word_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  const char *shape = depc > 1u   ? "(b) alias"
                      : paramv[0] ? "(a) created"
                                  : "(a) dependence";
  arts_guid_t g;
  uint64_t *p;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db;
  if (paramv[0]) {
    arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
    dh.rank = (unsigned int)paramv[1];
    void *vp = NULL;
    g = arts_db_create(&vp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &dh);
    p = (uint64_t *)vp;
    struct arts_created_db_s *e = created_record(g);
    h = e ? arts_shared_copy(e->db) : NULL;
    db = (struct arts_db_s *)arts_shared_get(h);
  } else {
    g = depv[0].guid;
    p = (uint64_t *)depv[0].ptr;
    db = local_db(g, &h);
  }
  if (p == NULL || db == NULL) {
    fail(shape, "no pointer or no descriptor");
    arts_shared_release(&h);
    return;
  }
  fill(p, 0xA0A0000u, 0, WORDS);
  expect_word(db, CACHE_ST_GRANT, 1, 0, shape, "under the write hold");
  arts_db_downgrade(g);
  expect_word(db, CACHE_ST_GRANT, 0, 1, shape, "after the downgrade");
  if (!holds(p, 0xA0A0000u, 0, WORDS)) {
    fail(shape, "the bytes changed under the read hold");
  }
  arts_db_release(g);
  expect_word(db, REST_RWS, 0, 0, shape, "after the read hold's release");
  arts_shared_release(&h);
}

/* (c)'s second writer.  depv[0]: the block, RW. */
static void excl_cowriter_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_store_explicit(&g_peer_holds, 1u, memory_order_release);
  (void)await_flag(&g_peer_go, "(c) co-writer", "W2 was never let go");
}

/* (c).  depv[0]: the block, RW. */
static void excl_cowrite_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const char *shape = "(c) co-writer";
  arts_guid_t g = depv[0].guid;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db = local_db(g, &h);
  atomic_store_explicit(&g_peer_holds, 0u, memory_order_relaxed);
  atomic_store_explicit(&g_peer_go, 0u, memory_order_relaxed);
  arts_guid_t w2 =
      arts_edt_create(excl_cowriter_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(g, w2, 0, DB_MODE_RW);
  if (!await_flag(&g_peer_holds, shape, "W2 never ran")) {
    atomic_store_explicit(&g_peer_go, 1u, memory_order_release);
    arts_shared_release(&h);
    return;
  }
  expect_word(db, CACHE_ST_GRANT, 2, 0, shape, "with two writers");
  arts_db_downgrade(g);
  expect_word(db, CACHE_ST_GRANT, 1, 1, shape, "after D's downgrade");
  atomic_store_explicit(&g_peer_go, 1u, memory_order_release);
  long start = now_ms();
  while (CACHE_RW_CNT(cache_word_of(db)) != 0u) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "W2's release never came");
      break;
    }
  }
  expect_word(db, CACHE_ST_GRANT, 0, 1, shape, "after W2's release");
  arts_db_release(g);
  expect_word(db, REST_RWS, 0, 0, shape, "after D's release");
  arts_shared_release(&h);
}

/* (d), (e): the remote writer.  paramv[0]: the base D wrote.  depv[0]: the
 * block, RW. */
static void remote_writer_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  if (!holds(p, paramv[0], 0, WORDS)) {
    fail("remote writer", "D's bytes did not reach the next writer");
    return;
  }
  fill(p, 0xEEEE0000u, 0, WORDS);
}

/* (d) PURGE / (e) RETAIN: D on the home makes a writer on `other` ready and
 * watches it wait.  paramv[0]: the other rank.  depv[0]: the block, RW. */
static void excl_remote_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
#ifdef ARTS_RETENTION_PURGE
  const char *shape = "(d) remote writer";
#else
  const char *shape = "(e) migration held";
#endif
  arts_guid_t g = depv[0].guid;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db = local_db(g, &h);
  fill(p, 0xD0D0000u, 0, WORDS);
  arts_db_downgrade(g);
  uint64_t base = 0xD0D0000u;
  arts_guid_t w =
      arts_edt_create(remote_writer_edt, 1, &base, 1,
                      &(arts_edt_hint_t){.rank = (unsigned int)paramv[0]});
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  long start = now_ms();
#ifdef ARTS_RETENTION_PURGE
  while (EXCL_STATE_W(atomic_load_explicit(&db->lock_state,
                                           memory_order_acquire)) != 2u) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "the writer's request was never counted at the home");
      break;
    }
  }
  expect_word(db, CACHE_ST_GRANT, 0, 1, shape, "with the writer queued");
#else
  while (CACHE_MIGRATE_TARGET(cache_word_of(db)) != (uint32_t)paramv[0]) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "the writer's migration never reached the owner");
      break;
    }
  }
  expect_word(db, CACHE_ST_GRANT, 0, 1, shape, "with the migration pending");
#endif
  if (!holds(p, 0xD0D0000u, 0, WORDS)) {
    fail(shape, "D's bytes changed under its read hold");
  }
  arts_shared_release(&h);
}

#ifdef ARTS_RETENTION_RETAIN
/* (f)'s reader on the other rank.  depv[0]: the block, RO. */
static void demote_reader_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (!holds((const uint64_t *)depv[0].ptr, 0xF0F0000u, 0, WORDS)) {
    fail("(f) serve-demote", "the served reader did not read D's bytes");
  }
}

/* (f)'s writer on D's rank.  depv[0]: the block, RW. */
static void selfmig_writer_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)await_flag(&g_peer_go, "(f) serve-demote", "W2 was never let go");
}

/* (f).  paramv[0]: the other rank.  depv[0]: the block, RW. */
static void excl_demote_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const char *shape = "(f) serve-demote";
  arts_guid_t g = depv[0].guid;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db = local_db(g, &h);
  fill(p, 0xF0F0000u, 0, WORDS);
  arts_db_downgrade(g);
  arts_guid_t r =
      arts_edt_create(demote_reader_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = (unsigned int)paramv[0]});
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  long start = now_ms();
  uint64_t w;
  while (w = cache_word_of(db),
         CACHE_RW_ST(w) != CACHE_ST_IDLE || CACHE_OWNER(w) != 1u) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "the reader's serve never demoted the write permission");
      arts_shared_release(&h);
      return;
    }
  }
  atomic_store_explicit(&g_peer_go, 0u, memory_order_relaxed);
  arts_guid_t w2 =
      arts_edt_create(selfmig_writer_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(g, w2, 0, DB_MODE_RW);
  start = now_ms();
  while (w = cache_word_of(db),
         CACHE_RW_ST(w) != CACHE_ST_GRANT || CACHE_RW_CNT(w) != 1u) {
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "the writer on D's rank never took the turn back");
      break;
    }
  }
  expect_word(db, CACHE_ST_GRANT, 1, 1, shape,
              "with the writer self-migrated under the read hold");
  atomic_store_explicit(&g_peer_go, 1u, memory_order_release);
  if (!holds(p, 0xF0F0000u, 0, WORDS)) {
    fail(shape, "D's bytes changed under its read hold");
  }
  arts_shared_release(&h);
}
#endif /* ARTS_RETENTION_RETAIN */
#endif /* ARTS_VALIDITY_EXCL */

/* ---- VAL / INV: the grant word ------------------------------------------- */

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
static void grant_of(struct arts_db_s *db, bool *own, unsigned int *count) {
  unsigned int w =
      atomic_load_explicit(&db->cache.writer_count, memory_order_acquire);
  *own = ARTS_GRANT_OWN_OF(w);
  *count = ARTS_GRANT_COUNT_OF(w);
}

/* (g).  paramv: created (1) or dependence (0), the home for a create.
 * depv[0]: the dependence, RW. */
static void valinv_word_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const char *shape = paramv[0] ? "(g) created" : "(g) dependence";
  arts_guid_t g;
  uint64_t *p;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db;
  unsigned int home;
  if (paramv[0]) {
    arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
    dh.rank = (unsigned int)paramv[1];
    void *vp = NULL;
    g = arts_db_create(&vp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &dh);
    p = (uint64_t *)vp;
    struct arts_created_db_s *e = created_record(g);
    h = e ? arts_shared_copy(e->db) : NULL;
    db = (struct arts_db_s *)arts_shared_get(h);
  } else {
    g = depv[0].guid;
    p = (uint64_t *)depv[0].ptr;
    db = local_db(g, &h);
  }
  home = arts_guid_get_rank(g);
  if (p == NULL || db == NULL) {
    fail(shape, "no pointer or no descriptor");
    arts_shared_release(&h);
    return;
  }
  fill(p, 0x6060000u, 0, WORDS);
  /* The grant's install keeps a drain guard of its own in the count until its
   * drain returns, which can be after the drained writer runs: the hold is
   * one count among those, and the downgrade must leave none but the guard,
   * which goes by itself. */
  bool own;
  unsigned int count;
  grant_of(db, &own, &count);
  if (!own || count < 1u) {
    printf("FAIL: " NAME " %s: under the write hold own %d count %u\n", shape,
           (int)own, count);
    arts_test_fail();
  }
  arts_db_downgrade(g);
  bool want_own = true;
#ifdef ARTS_RETENTION_PURGE
  /* Off the home the right went back; the home keeps it, nobody waiting. */
  want_own = home == arts_get_current_rank();
#endif
  long start = now_ms();
  for (;;) {
    grant_of(db, &own, &count);
    if (count == 0u && own == want_own) {
      break;
    }
    if (now_ms() - start > WAIT_MS) {
      printf("FAIL: " NAME " %s: after the downgrade own %d count %u, "
             "expected own %d count 0\n",
             shape, (int)own, count, (int)want_own);
      arts_test_fail();
      break;
    }
  }
  if (!holds(p, 0x6060000u, 0, WORDS)) {
    fail(shape, "the bytes changed under the read hold");
  }
  arts_shared_release(&h);
}

/* (h)'s writer on the home.  depv[0]: the block, RW. */
static void refresh_writer_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  if (p == NULL) {
    fail("(h) refresh", "the writer has no pointer");
    return;
  }
  fill(p, 0xBBBB0000u, WA, WORDS);
}

/* (h)'s reader on D's rank.  depv[0]: the block, RO; depv[1]: the writer's
 * completion. */
static void refresh_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (!holds((const uint64_t *)depv[0].ptr, 0xBBBB0000u, WA, WORDS)) {
    fail("(h) refresh", "a reader ordered after the writer read a stale B");
  }
}

/* (h).  depv[0]: the block, RW, homed on rank 0; this EDT runs on another. */
static void refresh_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const char *shape = "(h) refresh";
  arts_guid_t g = depv[0].guid;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db = local_db(g, &h);
  if (p == NULL || db == NULL) {
    fail(shape, "no pointer or no descriptor");
    arts_shared_release(&h);
    return;
  }
  fill(p, 0xAAAA0000u, 0, WORDS);
  arts_db_downgrade(g);
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t written = arts_event_create(&eh);
  arts_guid_t w =
      arts_edt_create(refresh_writer_edt, 0, NULL, 1,
                      &(arts_edt_hint_t){.rank = arts_guid_get_rank(g),
                                         .completion_event = written});
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  arts_guid_t r =
      arts_edt_create(refresh_reader_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  arts_add_dependence(written, r, 1, DB_MODE_NULL);
  long start = now_ms();
  for (;;) {
    arts_shared_ptr_t cur = arts_db_buf_acquire(&db->cache);
    struct arts_db_buffer_s *b =
        (struct arts_db_buffer_s *)arts_shared_get(cur);
    bool replaced = b != NULL && (void *)b->data != (void *)p;
    arts_db_buf_release(&cur);
    if (replaced) {
      break;
    }
    if (now_ms() - start > WAIT_MS) {
      fail(shape, "the ordered reader never installed a fresh buffer");
      break;
    }
  }
  if (!holds(p, 0xAAAA0000u, 0, WA)) {
    fail(shape, "D's A changed under its read hold");
  }
  arts_shared_release(&h);
}
#endif /* VAL / INV */

/* ---- the stages ---------------------------------------------------------- */

enum {
  S_CREF,
  S_EXCL_DEP_HOME,
  S_EXCL_DEP_OFF,
  S_EXCL_CRE_HOME,
  S_EXCL_CRE_OFF,
  S_EXCL_ALIAS,
  S_EXCL_COWRITE,
  S_EXCL_REMOTE,
  S_EXCL_DEMOTE,
  S_VALINV_DEP,
  S_VALINV_CRE,
  S_VALINV_REFRESH,
  S_COUNT
};

static const char *const shape_name[S_COUNT] = {
    "creator reference",
    "(a) EXCL dependence on the home",
    "(a) EXCL dependence off the home",
    "(a) EXCL create on the home",
    "(a) EXCL create off the home",
    "(b) EXCL alias",
    "(c) EXCL co-writer",
    "(d)/(e) EXCL remote writer",
    "(f) EXCL serve-demote and self-migration",
    "(g) grant word, dependence",
    "(g) grant word, create",
    "(h) refresh under the read hold"};

static bool shape_applies(unsigned int s, unsigned int n) {
  bool excl = false, purge = false, valinv = false;
#ifdef ARTS_VALIDITY_EXCL
  excl = true;
#endif
#ifdef ARTS_RETENTION_PURGE
  purge = true;
#endif
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
  valinv = true;
#endif
  (void)purge;
  switch (s) {
  case S_CREF:
    return true;
  case S_EXCL_DEP_HOME:
  case S_EXCL_CRE_HOME:
  case S_EXCL_ALIAS:
  case S_EXCL_COWRITE:
    return excl;
  case S_EXCL_DEP_OFF:
  case S_EXCL_CRE_OFF:
  case S_EXCL_REMOTE:
    return excl && n > 1u;
  case S_EXCL_DEMOTE:
    return excl && !purge && n > 1u;
  case S_VALINV_DEP:
  case S_VALINV_CRE:
    return valinv;
  case S_VALINV_REFRESH:
    return valinv && n > 1u;
  default:
    return false;
  }
}

/* Launches shape s inside `group`. */
static void launch(unsigned int s, unsigned int n, arts_guid_t group) {
  unsigned int other = n > 1u ? 1u : 0u;
  arts_edt_hint_t hint = {.group = group};
  (void)other;
  switch (s) {
  case S_CREF: {
    uint64_t home = 0u;
    hint.rank = other;
    arts_edt_create(cref_edt, 1, &home, 0, &hint);
    return;
  }
#ifdef ARTS_VALIDITY_EXCL
  case S_EXCL_DEP_HOME:
  case S_EXCL_DEP_OFF: {
    uint64_t pv[2] = {0u, 0u};
    hint.rank = s == S_EXCL_DEP_HOME ? 0u : other;
    arts_guid_t d = arts_edt_create(excl_word_edt, 2, pv, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
  case S_EXCL_CRE_HOME:
  case S_EXCL_CRE_OFF: {
    uint64_t pv[2] = {1u, 0u};
    hint.rank = s == S_EXCL_CRE_HOME ? 0u : other;
    arts_edt_create(excl_word_edt, 2, pv, 0, &hint);
    return;
  }
  case S_EXCL_ALIAS: {
    uint64_t pv[2] = {0u, 0u};
    hint.rank = n - 1u;
    arts_guid_t g = fresh_block(0u);
    arts_guid_t d = arts_edt_create(excl_word_edt, 2, pv, 2, &hint);
    arts_add_dependence(g, d, 0, DB_MODE_RW);
    arts_add_dependence(g, d, 1, DB_MODE_RO);
    return;
  }
  case S_EXCL_COWRITE: {
    hint.rank = n - 1u;
    arts_guid_t d = arts_edt_create(excl_cowrite_edt, 0, NULL, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
  case S_EXCL_REMOTE: {
    uint64_t pv = other;
    hint.rank = 0u;
    arts_guid_t d = arts_edt_create(excl_remote_edt, 1, &pv, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
#ifdef ARTS_RETENTION_RETAIN
  case S_EXCL_DEMOTE: {
    uint64_t pv = other;
    hint.rank = 0u;
    arts_guid_t d = arts_edt_create(excl_demote_edt, 1, &pv, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
#endif
#endif
#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
  case S_VALINV_DEP: {
    uint64_t pv[2] = {0u, 0u};
    hint.rank = n - 1u;
    arts_guid_t d = arts_edt_create(valinv_word_edt, 2, pv, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
  case S_VALINV_CRE: {
    uint64_t pv[2] = {1u, 0u};
    hint.rank = n - 1u;
    arts_edt_create(valinv_word_edt, 2, pv, 0, &hint);
    return;
  }
  case S_VALINV_REFRESH: {
    hint.rank = other;
    arts_guid_t d = arts_edt_create(refresh_edt, 0, NULL, 1, &hint);
    arts_add_dependence(fresh_block(0u), d, 0, DB_MODE_RW);
    return;
  }
#endif
  default:
    return;
  }
}

/* paramv[0]: the first shape not yet run, paramv[1]: the shapes run so far.
 * depv[0]: the previous shape's group. */
static void stage_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  unsigned int s = (unsigned int)paramv[0];
  while (s < S_COUNT && !shape_applies(s, n)) {
    s++;
  }
  if (s == S_COUNT) {
    printf("PASS: " NAME " %llu shapes\n", (unsigned long long)paramv[1]);
    arts_shutdown();
    return;
  }
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  printf(NAME ": shape %s\n", shape_name[s]);
  launch(s, n, group);
  uint64_t next[2] = {s + 1u, paramv[1] + 1u};
  arts_guid_t st =
      arts_edt_create(stage_edt, 2, next, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, st, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t first[2] = {0u, 0u};
  arts_guid_t st =
      arts_edt_create(stage_edt, 2, first, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(NULL_GUID, st, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
