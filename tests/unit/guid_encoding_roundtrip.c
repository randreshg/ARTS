/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_encoding_roundtrip — pure-unit coverage of guid.c encode/decode, the
 * range allocator and the key geometry, with NO ARTS runtime.
 *
 * guid.c and route_table.c are #include'd directly so the generator's
 * file-local state, the per-thread counter rows and the route-table
 * classifier can be driven straight from this TU.  node_init runs the
 * runtime's node init for one simulated rank: arts_guid_node_geometry_init,
 * then every thread's arts_guid_key_generator_init.  The runtime symbols the
 * included files reference (arts_node_info, arts_thread_info,
 * arts_global_rank_id/count, arts_abort, arts_malloc/calloc/free, the OoO
 * engine) are shimmed with libc-backed definitions.
 *
 * Properties asserted (all single-threaded — pure encoding contracts):
 *
 *   1. Field round-trip: ARTS_GUID_MAKE(type, rank, key) decodes back via
 *      get_kind / get_rank / get_key for the full field ranges (2-bit type,
 *      14-bit rank, large key) with no cross-field bleed.
 *   2. is_local matches arts_global_rank_id.
 *   3. A contiguous reserve_range mints `size` distinct GUIDs; from_index /
 *      index_from are exact inverses over [0,size); index_from returns -1 on a
 *      foreign-type GUID, on a foreign rank and on a key below the range
 *      start.  A round-robin range homes index i on rank i % nrank and
 *      inverts the same way, and index_from returns -1 below its base key and
 *      for a home past the last rank.
 *   4. Per-(rank,type) counters are disjoint: two reserves of the SAME rank
 *      advance, two reserves of DIFFERENT types never alias keys.
 *   5. Exhaustion of a thread's block or of a rank's DB seq slice is a HARD
 *      ERROR (no wraparound) — verified in a forked child so the abort doesn't
 *      take the test process down.
 *   6. szhint encode/decode.
 *   7. Node init alone fixes the geometry, from the rank, the rank count and
 *      the rank's thread count.  Over simulated ranks of 2-5 threads each,
 *      every thread block lies in its rank's slice [r*S, (r+1)*S),
 *      S = 2^48 / N, no two blocks overlap, each thread's mints stay in its
 *      block and each rank's DB seqs in its creator slice.  The classifier
 *      takes every key of a thread's block to that thread's table, and a
 *      slack key of the slice, a key below the rank's base and the key at
 *      base + span to a remote shard.
 *   8. Two ranks that run the same reserve sequence (arts_guid_reserve and
 *      an explicit-rank arts_guid_reserve_range, of every kind) get pairwise
 *      distinct GUIDs, each in its minter's slice.  The encoder's rank guards
 *      fail loudly (forked children): ARTS_HINT_ANY_RANK reaching it, and a
 *      home >= N of every kind.
 */

#include "arts/utils/stdatomic_compat.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── runtime symbol shims (must precede the #include of guid.c) ─────────── */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
void arts_abort(uint8_t code) { _exit(code ? code : 70); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_calloc_aligned(size_t n, size_t s, size_t a) {
  void *p = NULL;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n * s)) {
    return NULL;
  }
  memset(p, 0, n * s);
  return p;
}
void arts_free(void *p) { free(p); }
#ifdef ARTS_DEBUG_INJECTION
/* route_table.c's windows are scheduling-fuzz points in an injection tree. */
void arts_sched_fuzz_point(void) {}
#endif

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL guid_encoding_roundtrip: " __VA_ARGS__);       \
    return 1;                                                                  \
  } while (0)

#define MAX_THREADS 5u

/* The classifier only selects a table, so placeholders stand in for them. */
static arts_route_table_t g_local_tables[MAX_THREADS];
static arts_route_table_t *g_local_slots[MAX_THREADS];
static arts_route_table_t g_remote_tables[ARTS_REMOTE_ROUTE_SHARDS];

static void node_fini(void) {
  if (arts_node_info.keys != NULL) {
    for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
      free(arts_node_info.keys[t]);
    }
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
  arts_guid_generator_cleanup();
}

/* The runtime's node init for rank `rank` of `nrank` with `threads` threads:
 * the geometry, then each thread's key counters.  Leaves thread 0 current. */
static void node_init(unsigned int rank, unsigned int nrank,
                      unsigned int threads) {
  node_fini();
  arts_global_rank_id = rank;
  arts_global_rank_count = nrank;
  arts_node_info.total_thread_count = threads;
  arts_node_info.keys = (uint64_t **)calloc(threads, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  for (unsigned int t = 0; t < threads; t++) {
    arts_thread_info.thread_id = t;
    arts_guid_key_generator_init();
  }
  arts_thread_info.thread_id = 0;
}

/* The geometry node init must set for rank r of n ranks with t threads, or
 * NULL when it does. */
static const char *geometry_mismatch(unsigned int r, unsigned int n,
                                     unsigned int t) {
  uint64_t slice = ((uint64_t)1 << ARTS_GUID_KEY_BITS) / n;
  uint64_t budget = (ARTS_GUID_DB_SEQ_MASK + 1) / n;
  if (arts_guid_table_count != t) {
    return "arts_guid_table_count";
  }
  if (arts_guid_keys_per_thread != slice / t) {
    return "arts_guid_keys_per_thread";
  }
  if (arts_guid_flat_base != slice * r) {
    return "flat base";
  }
  if (arts_guid_flat_span != arts_guid_keys_per_thread * t) {
    return "flat span";
  }
  if (arts_db_seq_budget != budget || db_seq_creator_base != budget * r) {
    return "DB creator slice";
  }
  for (unsigned int h = 0; h < n; h++) {
    if (atomic_load_explicit(&db_seq_next[h], memory_order_relaxed) !=
        budget * r + 1) {
      return "DB counters";
    }
  }
  return NULL;
}

/* The local table the classifier picks for flat key `key`, -1 for the remote
 * shard its low bits name, -2 for anything else. */
static int flat_table_of(uint64_t key) {
  arts_route_table_t *t =
      arts_get_route_table(ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0, key));
  for (unsigned int i = 0; i < arts_guid_table_count; i++) {
    if (t == arts_node_info.route_table[i]) {
      return (int)i;
    }
  }
  return t == arts_node_info
                     .remote_route_table[key & (ARTS_REMOTE_ROUTE_SHARDS - 1)]
             ? -1
             : -2;
}

/* Runs `fn` in a forked child; true when the child ended through
 * arts_abort(1), the exit every ARTS_ERROR takes. */
static int dies(void (*fn)(void)) {
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int st = 0;
  if (pid < 0 || waitpid(pid, &st, 0) != pid) {
    return 0;
  }
  return WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

static void exhaust_db_slice(void) {
  /* The home's counter at the slice's last seqs: the next chunk lease runs
   * past the slice. */
  node_init(0, 1, 1);
  atomic_store_explicit(&db_seq_next[0],
                        db_seq_creator_base + arts_db_seq_budget - 4,
                        memory_order_relaxed);
  (void)arts_guid_create_for_rank(0, ARTS_GUID_DB);
}

static void exhaust_thread_block(void) {
  node_init(0, 1, 1);
  *arts_guid_generator_get_key(0, ARTS_GUID_EDT) =
      arts_guid_keys_per_thread - 4;
  for (int i = 0; i < 4; i++) {
    (void)arts_guid_create_for_rank(0, ARTS_GUID_EDT);
  }
}

static void encode_any_rank_flat(void) {
  (void)arts_guid_reserve_range(ARTS_GUID_EVENT, 2, ARTS_HINT_ANY_RANK);
}

static void encode_any_rank_db(void) {
  (void)arts_guid_create_for_rank(ARTS_HINT_ANY_RANK, ARTS_GUID_DB);
}

static void encode_db_home_past_n(void) {
  (void)arts_guid_create_for_rank(arts_global_rank_count, ARTS_GUID_DB);
}

static void reserve_db_home_past_n(void) {
  (void)arts_guid_reserve_range(ARTS_GUID_DB, 2, arts_global_rank_count);
}

static void encode_event_home_past_n(void) {
  (void)arts_guid_create_for_rank(arts_global_rank_count, ARTS_GUID_EVENT);
}

static void reserve_edt_home_past_n(void) {
  (void)arts_guid_reserve_range(ARTS_GUID_EDT, 2, arts_global_rank_count);
}

typedef struct {
  uint64_t lo, hi; /* [lo, hi) */
  unsigned int rank;
} block_t;

static int cmp_block(const void *a, const void *b) {
  const block_t *x = (const block_t *)a;
  const block_t *y = (const block_t *)b;
  return (x->lo > y->lo) - (x->lo < y->lo);
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

int main(void) {
  for (unsigned int i = 0; i < MAX_THREADS; i++) {
    g_local_slots[i] = &g_local_tables[i];
  }
  arts_node_info.route_table = g_local_slots;
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = &g_remote_tables[s];
  }

  /* ---- Test 1: field round-trip, no cross-field bleed. ---- */
  for (unsigned t = 0; t <= ARTS_GUID_TYPE_MASK; t++) {
    unsigned ranks[] = {0u, 1u, 4242u, (unsigned)ARTS_GUID_RANK_MASK};
    uint64_t keys[] = {1u, 0xABCDEFu, ((uint64_t)1 << 47), ARTS_GUID_KEY_MASK};
    for (unsigned ri = 0; ri < 4; ri++) {
      for (unsigned ki = 0; ki < 4; ki++) {
        arts_guid_t g = ARTS_GUID_MAKE(t, ranks[ri], keys[ki]);
        if ((unsigned)arts_guid_get_kind(g) != t) {
          FAIL("kind decode %u != %u\n", (unsigned)arts_guid_get_kind(g), t);
        }
        if (arts_guid_get_rank(g) != ranks[ri]) {
          FAIL("rank decode %u != %u\n", arts_guid_get_rank(g), ranks[ri]);
        }
        if (arts_guid_get_key(g) != keys[ki]) {
          FAIL("key decode %lu != %lu\n", (unsigned long)arts_guid_get_key(g),
               (unsigned long)keys[ki]);
        }
      }
    }
  }

  /* ---- Test 2: is_local. ---- */
  node_init(3, 4, 1);
  if (arts_guid_is_local(ARTS_GUID_MAKE(ARTS_GUID_DB, 3, 5))) {
    /* ok */
  } else {
    FAIL("is_local(rank==self) returned false\n");
  }
  if (arts_guid_is_local(ARTS_GUID_MAKE(ARTS_GUID_DB, 4, 5))) {
    FAIL("is_local(rank!=self) returned true\n");
  }

  /* ---- Test 3: contiguous range round-trip. ---- */
  node_init(0, 1, 1);
  const unsigned SIZE = 64;
  arts_guid_t start = arts_guid_reserve_range(ARTS_GUID_DB, SIZE, 0);
  if (start == NULL_GUID) {
    FAIL("reserve_range returned NULL_GUID\n");
  }
  arts_guid_t prev = NULL_GUID;
  for (unsigned i = 0; i < SIZE; i++) {
    arts_guid_t g = arts_guid_from_index(start, i);
    if (arts_guid_get_kind(g) != ARTS_GUID_DB) {
      FAIL("range[%u] kind mismatch\n", i);
    }
    if (arts_guid_get_rank(g) != 0) {
      FAIL("range[%u] rank mismatch\n", i);
    }
    if (i > 0 && g == prev) {
      FAIL("range[%u] not distinct\n", i);
    }
    int64_t idx = arts_guid_index_from(start, g);
    if (idx != (int64_t)i) {
      FAIL("index_from(range[%u]) = %" PRId64 "\n", i, idx);
    }
    prev = g;
  }
  /* foreign-type GUID → -1 */
  arts_guid_t edt0 = arts_guid_reserve_range(ARTS_GUID_EDT, 1, 0);
  if (arts_guid_index_from(start, arts_guid_from_index(edt0, 0)) != -1) {
    FAIL("index_from did not reject foreign type\n");
  }
  /* key below range start → -1 (same type, smaller key) */
  arts_guid_t below =
      ARTS_GUID_MAKE(ARTS_GUID_DB, 0, ARTS_GUID_GET_KEY(start) - 1);
  if (arts_guid_index_from(start, below) != -1) {
    FAIL("index_from did not reject key below start\n");
  }
  /* same type and key, another rank → -1 */
  if (arts_guid_index_from(
          start, ARTS_GUID_MAKE(ARTS_GUID_DB, 1, ARTS_GUID_GET_KEY(start))) !=
      -1) {
    FAIL("index_from did not reject a foreign rank\n");
  }
  /* A round-robin range, over three ranks so a wrong home shows. */
  node_init(0, 3, 1);
  arts_guid_t dist =
      arts_guid_reserve_range(ARTS_GUID_DB, 8, ARTS_HINT_ROUND_ROBIN);
  if (dist == NULL_GUID) {
    FAIL("round-robin reserve_range returned NULL_GUID\n");
  }
  for (unsigned i = 0; i < 8; i++) {
    arts_guid_t g = arts_guid_from_index(dist, i);
    if (arts_guid_get_rank(g) != i % arts_global_rank_count ||
        arts_guid_index_from(dist, g) != (int64_t)i) {
      FAIL("round-robin range[%u] home or inverse\n", i);
    }
  }
  uint64_t base_key = ARTS_GUID_GET_KEY(dist);
  if (base_key == 0) {
    FAIL("round-robin range has base key 0\n");
  }
  if (arts_guid_index_from(
          dist, ARTS_GUID_MAKE(ARTS_GUID_DB, 0, base_key - 1)) != -1) {
    FAIL("index_from did not reject a key below the round-robin base\n");
  }
  if (arts_guid_index_from(dist,
                           ARTS_GUID_MAKE(ARTS_GUID_DB, arts_global_rank_count,
                                          base_key)) != -1) {
    FAIL("index_from did not reject a home past the last rank\n");
  }

  /* ---- Test 4: counter disjointness across type. ---- */
  node_init(0, 1, 1);
  arts_guid_t a = arts_guid_create_for_rank(0, ARTS_GUID_DB);
  arts_guid_t b = arts_guid_create_for_rank(0, ARTS_GUID_DB);
  if (a == b) {
    FAIL("two same-(rank,type) reserves aliased\n");
  }
  arts_guid_t e = arts_guid_create_for_rank(0, ARTS_GUID_EVENT);
  /* different type → different kind field; keys may coincide but the encoded
   * GUIDs must differ because the type bits differ. */
  if (a == e) {
    FAIL("DB and EVENT GUID aliased\n");
  }

  /* ---- Test 5: exhaustion is a HARD ERROR (forked children). ---- */
  if (!dies(exhaust_db_slice)) {
    FAIL("DB seq slice exhaustion did not hard-error\n");
  }
  if (!dies(exhaust_thread_block)) {
    FAIL("flat-kind block exhaustion did not hard-error\n");
  }

  /* ---- Test 6: szhint encode/decode — bound covers, sentinel unreachable. */
  {
    uint64_t probes[] = {0,    1,    63,   64,   65,   2047, 2048,
                         2049, 4095, 4096, 4097, 65536};
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
      uint64_t sz = probes[i];
      uint64_t hint = arts_db_szhint_encode(sz);
      if (hint == ARTS_GUID_DB_SZHINT_NONE) {
        FAIL("szhint encode hit the sentinel for size %lu\n",
             (unsigned long)sz);
      }
      arts_guid_t g = arts_db_guid_stamp_szhint(
          ARTS_GUID_MAKE(ARTS_GUID_DB, 1, 12345), sz);
      uint64_t bound = arts_db_szhint_bound(g);
      if (bound < sz) {
        FAIL("szhint bound %lu < size %lu\n", (unsigned long)bound,
             (unsigned long)sz);
      }
      if (sz >= 4096 && bound > sz + sz / 32 + 64) {
        FAIL("szhint overshoot too large: size %lu bound %lu\n",
             (unsigned long)sz, (unsigned long)bound);
      }
      if (ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(g)) != 12345) {
        FAIL("stamp disturbed the seq field\n");
      }
    }
    /* power sweep to the 4TB edge: bound covers up to the encodable max,
     * beyond it the sentinel (bound 0) takes over. */
    for (uint64_t sz = 64; sz <= ((uint64_t)4 << 40); sz <<= 1) {
      uint64_t hint = arts_db_szhint_encode(sz);
      arts_guid_t g = arts_db_guid_stamp_szhint(
          ARTS_GUID_MAKE(ARTS_GUID_DB, 1, 7), sz);
      uint64_t bound = arts_db_szhint_bound(g);
      if (hint != ARTS_GUID_DB_SZHINT_NONE && bound < sz) {
        FAIL("power sweep: bound %lu < size %lu\n", (unsigned long)bound,
             (unsigned long)sz);
      }
      if (hint == ARTS_GUID_DB_SZHINT_NONE && bound != 0) {
        FAIL("sentinel decoded a nonzero bound\n");
      }
      arts_guid_t gm1 = arts_db_guid_stamp_szhint(
          ARTS_GUID_MAKE(ARTS_GUID_DB, 1, 7), sz - 1);
      if (arts_db_szhint_bound(gm1) != 0 && arts_db_szhint_bound(gm1) < sz - 1) {
        FAIL("power-1 sweep: bound < size at %lu\n", (unsigned long)(sz - 1));
      }
    }
    /* the all-ones sentinel value itself decodes to 0 */
    arts_guid_t s_g = ARTS_GUID_MAKE(
        ARTS_GUID_DB, 1, ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, 7));
    if (arts_db_szhint_bound(s_g) != 0) {
      FAIL("explicit sentinel decoded nonzero\n");
    }
  }

  /* ---- Test 7: node init fixes the geometry; the thread blocks of ranks of
   * 2-5 threads are disjoint and inside their rank's slice. ---- */
  {
    static const struct {
      unsigned int nrank;
      unsigned int count; /* simulated ranks */
      unsigned int rank[5];
      unsigned int threads[5];
    } shapes[] = {
        {5, 5, {0, 1, 2, 3, 4}, {2, 3, 4, 5, 2}},
        {3, 3, {0, 1, 2}, {4, 3, 2}},
        {2, 2, {0, 1}, {3, 5}},
        {16382, 4, {0, 1, 8191, 16381}, {5, 4, 3, 2}},
    };
    const uint64_t space = (uint64_t)1 << ARTS_GUID_KEY_BITS;
    for (unsigned si = 0; si < sizeof(shapes) / sizeof(shapes[0]); si++) {
      unsigned int n = shapes[si].nrank;
      uint64_t slice = space / n;
      block_t blocks[5 * MAX_THREADS];
      unsigned int nblocks = 0;
      for (unsigned ri = 0; ri < shapes[si].count; ri++) {
        unsigned int r = shapes[si].rank[ri];
        unsigned int nt = shapes[si].threads[ri];
        node_init(r, n, nt);
        const char *bad = geometry_mismatch(r, n, nt);
        if (bad != NULL) {
          FAIL("rank %u of %u, %u threads: node init set the wrong %s\n", r, n,
               nt, bad);
        }
        if (slice * (r + 1) > space) {
          FAIL("rank %u of %u: the slice runs past the key field\n", r, n);
        }
        uint64_t base = arts_guid_flat_base;
        for (unsigned int t = 0; t < nt; t++) {
          uint64_t lo = base + arts_guid_keys_per_thread * t;
          uint64_t hi = lo + arts_guid_keys_per_thread;
          blocks[nblocks++] = (block_t){lo, hi, r};
          arts_thread_info.thread_id = t;
          unsigned int home = (r + t) % n;
          uint64_t first = ARTS_GUID_GET_KEY(
              arts_guid_create_for_rank(home, ARTS_GUID_EVENT));
          *arts_guid_generator_get_key(home, ARTS_GUID_EDT) =
              arts_guid_keys_per_thread - 2;
          uint64_t last =
              ARTS_GUID_GET_KEY(arts_guid_create_for_rank(home, ARTS_GUID_EDT));
          if (first != lo + 1 || last != hi - 2) {
            FAIL("rank %u of %u, thread %u: mints left the block [%" PRIu64
                 ", %" PRIu64 ")\n",
                 r, n, t, lo, hi);
          }
          if (flat_table_of(lo) != (int)t || flat_table_of(first) != (int)t ||
              flat_table_of(last) != (int)t ||
              flat_table_of(hi - 1) != (int)t) {
            FAIL("rank %u of %u: a key of thread %u's block left its table\n",
                 r, n, t);
          }
          uint64_t seq = ARTS_GUID_DB_GET_SEQ(
              ARTS_GUID_GET_KEY(arts_guid_create_for_rank(home, ARTS_GUID_DB)));
          if (seq / arts_db_seq_budget != r) {
            FAIL("rank %u of %u: a DB seq left the creator slice\n", r, n);
          }
        }
        arts_thread_info.thread_id = 0;
        for (uint64_t k = base + arts_guid_flat_span; k < base + slice; k++) {
          if (flat_table_of(k) != -1) {
            FAIL("rank %u of %u: slack key %" PRIu64 " took a local table\n", r,
                 n, k);
          }
        }
        if (flat_table_of(base + arts_guid_flat_span) != -1) {
          FAIL("rank %u of %u: the key at base + span took a local table\n", r,
               n);
        }
        if (r > 0 && flat_table_of(base - 1) != -1) {
          FAIL("rank %u of %u: the key below the base took a local table\n", r,
               n);
        }
        node_init(r, n, nt);
        bad = geometry_mismatch(r, n, nt);
        if (bad != NULL) {
          FAIL("rank %u of %u: node init after mints set the wrong %s\n", r, n,
               bad);
        }
      }
      qsort(blocks, nblocks, sizeof(blocks[0]), cmp_block);
      for (unsigned int i = 0; i < nblocks; i++) {
        if (blocks[i].lo < slice * blocks[i].rank ||
            blocks[i].hi > slice * (blocks[i].rank + 1)) {
          FAIL("%u ranks: a block of rank %u leaves its slice\n", n,
               blocks[i].rank);
        }
        if (i > 0 && blocks[i].lo < blocks[i - 1].hi) {
          FAIL("%u ranks: blocks of ranks %u and %u overlap\n", n,
               blocks[i - 1].rank, blocks[i].rank);
        }
      }
    }
  }

  /* ---- Test 8: two ranks running the same reserve sequence get distinct
   * GUIDs, each in its minter's slice; the encoder's rank guards. ---- */
  {
    enum { NRANK = 2, NKINDS = 3, PER_HOME = 2 + 3 };
    enum { PER_RANK = NKINDS * NRANK * PER_HOME };
    static const arts_guid_kind_t kinds[NKINDS] = {
        ARTS_GUID_DB, ARTS_GUID_EVENT, ARTS_GUID_EDT};
    static const unsigned int threads[NRANK] = {2, 3};
    uint64_t got[NRANK * PER_RANK];
    unsigned int n = 0;
    for (unsigned int r = 0; r < NRANK; r++) {
      node_init(r, NRANK, threads[r]);
      for (unsigned int ki = 0; ki < NKINDS; ki++) {
        for (unsigned int h = 0; h < NRANK; h++) {
          arts_guid_t seq[PER_HOME];
          seq[0] = arts_guid_reserve(kinds[ki], h);
          seq[1] = arts_guid_reserve(kinds[ki], h);
          arts_guid_t range = arts_guid_reserve_range(kinds[ki], 3, h);
          for (unsigned int i = 0; i < 3; i++) {
            seq[2 + i] = arts_guid_from_index(range, i);
          }
          for (unsigned int i = 0; i < PER_HOME; i++) {
            uint64_t key = ARTS_GUID_GET_KEY(seq[i]);
            int in_slice =
                kinds[ki] == ARTS_GUID_DB
                    ? ARTS_GUID_DB_GET_SEQ(key) / arts_db_seq_budget == r
                    : key - arts_guid_flat_base < arts_guid_flat_span;
            if (arts_guid_get_kind(seq[i]) != kinds[ki] ||
                arts_guid_get_rank(seq[i]) != h || !in_slice) {
              FAIL("rank %u: GUID %u of kind %u on home %u is not its own\n", r,
                   i, (unsigned)kinds[ki], h);
            }
            got[n++] = (uint64_t)seq[i];
          }
        }
      }
    }
    qsort(got, n, sizeof(got[0]), cmp_u64);
    for (unsigned int i = 1; i < n; i++) {
      if (got[i] == got[i - 1]) {
        FAIL("two ranks running one reserve sequence minted GUID %" PRIx64
             " twice\n",
             got[i]);
      }
    }
    if (!dies(encode_any_rank_flat) || !dies(encode_any_rank_db)) {
      FAIL("ARTS_HINT_ANY_RANK reached the encoder without a hard error\n");
    }
    if (!dies(encode_db_home_past_n) || !dies(reserve_db_home_past_n)) {
      FAIL("a DB home >= N did not hard-error\n");
    }
    if (!dies(encode_event_home_past_n) || !dies(reserve_edt_home_past_n)) {
      FAIL("an EVENT or EDT home >= N did not hard-error\n");
    }
  }

  node_fini();
  printf("PASS guid_encoding_roundtrip: field round-trip, range inverse, "
         "counter disjoint, exhaustion hard-errors, szhint bound-covers, "
         "rank-sliced thread blocks, same reserve sequence distinct per "
         "rank, rank guards\n");
  return 0;
}
