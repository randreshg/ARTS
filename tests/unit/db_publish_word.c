/* SPDX-License-Identifier: Apache-2.0
 *
 * The state arts_db_publish leaves, read where it lives (whitebox).  One
 * shape at a time, each ordered after the previous one's EDTs: a dependence's
 * write hold and a create's hold, each on the block's home and off it, and a
 * dependence's read hold.  The EDT stores range A, publishes, and checks:
 *
 *   VAL, INV: once the grant word holds possession and exactly the EDT's own
 *     hold (an install's drain guard leaves by itself), the publish bumps the
 *     installed buffer's version by one and leaves the word as it was; the
 *     installed buffer is the one the EDT's pointer names, and under PURGE
 *     no hand-back is armed.  On a read hold the version of the EDT's buffer
 *     and the word do not change.
 *   EXCL (the CXL stores included): the cache word does not change.
 *   FLUSH, off the home: the write-back consumes the rank's PUT credit and
 *     its ACK leaves another, so the credit after the publish is non-zero
 *     and not the one before (a fetch's answer and a create's return leave a
 *     credit before any publish, so its presence alone proves nothing); a
 *     create's hold keeps its copy in the slot with no payload pending.  On
 *     the home, and on a read hold, nothing changes.
 *
 * Its own bytes do not change.  A write hold then stores range B and ends;
 * a reader ordered after the EDT, on another rank when there is one, checks
 * both ranges, so the hold survived the publish to publish B at its release.
 *
 * A wait that runs out fails the run and stops the later shapes' waits: the
 * publisher raises its rank's flag and hands a non-null result to its
 * reader, which raises the flag on its rank, the rank of the other
 * publishers.  So a broken runtime ends by status after one deadline.
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

#define NAME "db_publish_word"
#define WORDS 64u
#define WA 32u
#define SZ (WORDS * sizeof(uint64_t))
#define WAIT_MS 60000L
#define BASE_FRESH 0x5150000u
#define BASE_A 0xA0A0000u
#define BASE_B 0xB0B0000u

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

/* A block homed on `home`, filled with BASE_FRESH, released. */
static arts_guid_t fresh_block(unsigned int home) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p != NULL) {
    fill((uint64_t *)p, BASE_FRESH, 0, WORDS);
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

/* Raised on this rank once a shape's wait ran out here, or when a publisher
 * on another rank handed this rank's reader a non-null result; read by every
 * later publisher of the rank before it waits. */
static _Atomic(unsigned int) g_wait_ran_out;

/* A shape's wait may start: no earlier shape's wait ran out. */
static bool may_wait(const char *shape) {
  if (atomic_load_explicit(&g_wait_ran_out, memory_order_acquire) != 0u) {
    printf(NAME " %s: not judged, an earlier shape's wait ran out\n", shape);
    return false;
  }
  return true;
}

/* A wait ran out: the shape fails, and the later shapes do not wait. */
static void ran_out(const char *shape, const char *what) {
  fail(shape, what);
  atomic_store_explicit(&g_wait_ran_out, 1u, memory_order_release);
}

#if !defined(ARTS_VALIDITY_EXCL)
/* Whether the slot's installed buffer is the one `p` names, and its version
 * when it is. */
static bool installed_is(struct arts_db_s *db, const void *p,
                         uint64_t *version) {
  arts_shared_ptr_t cur = arts_db_buf_acquire(&db->cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(cur);
  bool is = b != NULL && (const void *)b->data == p;
  if (is) {
    *version = atomic_load_explicit(&b->version, memory_order_acquire);
  }
  arts_db_buf_release(&cur);
  return is;
}
#endif

/* ---- what a publish leaves, per validity ------------------------------- */

#if defined(ARTS_VALIDITY_VAL) || defined(ARTS_VALIDITY_INV)
struct snap_s {
  unsigned int word;
  uint64_t version;
};

static unsigned int grant_word(struct arts_db_s *db) {
  return atomic_load_explicit(&db->cache.writer_count, memory_order_acquire);
}

static bool before_publish(struct arts_db_s *db, arts_guid_t g, const void *p,
                           bool write, const char *shape, struct snap_s *s) {
  (void)g;
  if (write) {
    if (!may_wait(shape)) {
      return false;
    }
    long start = now_ms();
    for (;;) {
      unsigned int w = grant_word(db);
      if (ARTS_GRANT_OWN_OF(w) && ARTS_GRANT_COUNT_OF(w) == 1u) {
        break;
      }
      if (now_ms() - start > WAIT_MS) {
        printf(NAME " %s: own %d count %u\n", shape,
               (int)ARTS_GRANT_OWN_OF(w), ARTS_GRANT_COUNT_OF(w));
        ran_out(shape, "the grant word never held the EDT's hold alone");
        return false;
      }
    }
    s->word = grant_word(db);
    if (!installed_is(db, p, &s->version)) {
      fail(shape, "the installed buffer is not the one the hold's pointer "
                  "names");
      return false;
    }
  } else {
    s->word = grant_word(db);
    s->version = atomic_load_explicit(
        &arts_db_buf_from_data((void *)p)->version, memory_order_acquire);
  }
  return true;
}

static void after_publish(struct arts_db_s *db, const void *p, bool write,
                          const char *shape, const struct snap_s *s) {
  unsigned int w = grant_word(db);
  if (w != s->word) {
    printf("FAIL: " NAME " %s: the grant word went from 0x%x to 0x%x\n", shape,
           s->word, w);
    arts_test_fail();
  }
  if (write) {
    uint64_t v = 0;
    if (!installed_is(db, p, &v)) {
      fail(shape, "after the publish the installed buffer is not the hold's");
    } else if (v != s->version + 1u) {
      printf("FAIL: " NAME " %s: the version went from %llu to %llu\n", shape,
             (unsigned long long)s->version, (unsigned long long)v);
      arts_test_fail();
    }
#ifdef ARTS_RETENTION_PURGE
    if (atomic_load_explicit(&db->cache.pending_grant_return,
                             memory_order_acquire) != 0u) {
      fail(shape, "the publish armed a hand-back");
    }
#endif
  } else {
    uint64_t v = atomic_load_explicit(
        &arts_db_buf_from_data((void *)p)->version, memory_order_acquire);
    if (v != s->version) {
      printf("FAIL: " NAME " %s: a read hold's version went from %llu to "
             "%llu\n",
             shape, (unsigned long long)s->version, (unsigned long long)v);
      arts_test_fail();
    }
  }
}
#endif /* VAL / INV */

#ifdef ARTS_VALIDITY_EXCL
struct snap_s {
  uint64_t word;
};

static uint64_t cache_word(struct arts_db_s *db) {
  return atomic_load_explicit(&db->cache.cache_state, memory_order_acquire);
}

static bool before_publish(struct arts_db_s *db, arts_guid_t g, const void *p,
                           bool write, const char *shape, struct snap_s *s) {
  (void)g;
  (void)p;
  (void)write;
  (void)shape;
  s->word = cache_word(db);
  return true;
}

static void after_publish(struct arts_db_s *db, const void *p, bool write,
                          const char *shape, const struct snap_s *s) {
  (void)p;
  (void)write;
  uint64_t w = cache_word(db);
  if (w != s->word) {
    printf("FAIL: " NAME " %s: the cache word went from 0x%llx to 0x%llx\n",
           shape, (unsigned long long)s->word, (unsigned long long)w);
    arts_test_fail();
  }
}
#endif /* EXCL */

#ifdef ARTS_VALIDITY_FLUSH
struct snap_s {
  uint64_t txid;
  uint8_t pending;
  bool writes_back; /* a write hold off the home */
  bool in_slot;     /* the hold's bytes are the slot's buffer */
};

static uint64_t credit(struct arts_db_s *db) {
  return atomic_load_explicit(&db->cache.flush_txid, memory_order_acquire);
}

static uint8_t pending(struct arts_db_s *db) {
  return atomic_load_explicit(&db->cache.payload_pending,
                              memory_order_acquire);
}

/* On the home a hold's bytes are the line; off it only a create's hold
 * keeps its bytes in the slot. */
static bool before_publish(struct arts_db_s *db, arts_guid_t g, const void *p,
                           bool write, const char *shape, struct snap_s *s) {
  bool home = arts_guid_get_rank(g) == arts_get_current_rank();
  struct arts_created_db_s *e = created_record(g);
  s->writes_back = write && !home;
  s->in_slot = home || (write && e != NULL);
  if (s->writes_back) {
    if (!may_wait(shape)) {
      return false;
    }
    long start = now_ms();
    while (credit(db) == 0u) {
      if (now_ms() - start > WAIT_MS) {
        ran_out(shape, "the rank never held a PUT credit");
        return false;
      }
    }
  }
  s->txid = credit(db);
  s->pending = pending(db);
  uint64_t v;
  if (s->in_slot && !installed_is(db, p, &v)) {
    fail(shape, "the slot does not hold the hold's bytes");
    return false;
  }
  return true;
}

static void after_publish(struct arts_db_s *db, const void *p, bool write,
                          const char *shape, const struct snap_s *s) {
  (void)write;
  uint64_t t = credit(db);
  if (s->writes_back) {
    if (t == 0u || t == s->txid) {
      printf("FAIL: " NAME " %s: the credit went from %llu to %llu, so no "
             "write-back ran\n",
             shape, (unsigned long long)s->txid, (unsigned long long)t);
      arts_test_fail();
    }
  } else if (t != s->txid) {
    printf("FAIL: " NAME " %s: the credit went from %llu to %llu\n", shape,
           (unsigned long long)s->txid, (unsigned long long)t);
    arts_test_fail();
  }
  if (pending(db) != s->pending) {
    fail(shape, "the publish changed whether a payload is pending");
  }
  uint64_t v;
  if (s->in_slot && !installed_is(db, p, &v)) {
    fail(shape, "after the publish the slot no longer holds the hold's bytes");
  }
}
#endif /* FLUSH */

/* ---- the shapes ---------------------------------------------------------- */

enum { S_DEP_HOME, S_DEP_OFF, S_CRE_HOME, S_CRE_OFF, S_READ, S_COUNT };

static const char *const shape_name[S_COUNT] = {
    "write dependence on the home", "write dependence off the home",
    "create's hold on the home", "create's hold off the home",
    "read dependence"};

static bool shape_applies(unsigned int s, unsigned int n) {
  return !(s == S_DEP_OFF || s == S_CRE_OFF) || n > 1u;
}

/* paramv: the shape, the reader.  depv[0]: the block (dependence shapes). */
static void publisher_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int s = (unsigned int)paramv[0];
  arts_guid_t reader = (arts_guid_t)paramv[1];
  const char *shape = shape_name[s];
  bool created = s == S_CRE_HOME || s == S_CRE_OFF;
  bool write = s != S_READ;
  arts_guid_t g;
  uint64_t *p;
  arts_shared_ptr_t h = NULL;
  struct arts_db_s *db;
  if (created) {
    arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
    dh.rank = 0u;
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
  } else {
    if (write) {
      fill(p, BASE_A, 0, WA);
    }
    struct snap_s before;
    if (before_publish(db, g, p, write, shape, &before)) {
      arts_db_publish(g);
      after_publish(db, p, write, shape, &before);
    }
    if (write ? !holds(p, BASE_A, 0, WA) : !holds(p, BASE_FRESH, 0, WORDS)) {
      fail(shape, "the hold's bytes changed under the publish");
    }
    if (write) {
      fill(p, BASE_B, WA, WORDS);
    }
  }
  arts_add_dependence(g, reader, 0, DB_MODE_RO);
  arts_shared_release(&h);
  /* The result is the reader's own GUID, which names no block, so the slot
   * it fills is no second slot of the block. */
  if (atomic_load_explicit(&g_wait_ran_out, memory_order_acquire) != 0u) {
    arts_edt_set_result(reader);
  }
}

/* paramv[0]: whether the publisher held the block for writing.  depv[0]: the
 * block, RO; depv[1]: the publisher's completion, whose result is non-null
 * when one of its waits ran out. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[1].guid != NULL_GUID) {
    atomic_store_explicit(&g_wait_ran_out, 1u, memory_order_release);
  }
  const uint64_t *p = (const uint64_t *)depv[0].ptr;
  bool ok = paramv[0] ? holds(p, BASE_A, 0, WA) && holds(p, BASE_B, WA, WORDS)
                      : holds(p, BASE_FRESH, 0, WORDS);
  if (!ok) {
    fail("the reader", "a reader ordered after the publisher read other bytes "
                       "than it held last");
  }
  if (depv[0].guid != NULL_GUID) {
    arts_db_destroy(depv[0].guid);
  }
}

/* Launches shape s inside `group`: the publisher, on rank 0 or the last
 * rank, and its reader on the other of the two (the same rank on a run of
 * one). */
static void launch(unsigned int s, unsigned int n, arts_guid_t group) {
  unsigned int at = (s == S_DEP_HOME || s == S_CRE_HOME) ? 0u : n - 1u;
  uint64_t write = s != S_READ;
  arts_guid_t reader = arts_edt_create(
      reader_edt, 1, &write, 2,
      &(arts_edt_hint_t){.rank = at == 0u ? n - 1u : 0u, .group = group});
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t done = arts_event_create(&eh);
  arts_add_dependence(done, reader, 1, DB_MODE_NULL);
  uint64_t pv[2] = {s, (uint64_t)reader};
  bool created = s == S_CRE_HOME || s == S_CRE_OFF;
  arts_guid_t d = arts_edt_create(
      publisher_edt, 2, pv, created ? 0u : 1u,
      &(arts_edt_hint_t){.rank = at, .group = group, .completion_event = done});
  if (!created) {
    arts_add_dependence(fresh_block(0u), d, 0,
                        s == S_READ ? DB_MODE_RO : DB_MODE_RW);
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
