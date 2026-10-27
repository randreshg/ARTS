/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_db_seq_alloc_stress — global uniqueness of the DB seq allocator under
 * concurrency: minter threads lease chunks from the shared per-home counters
 * while reserver threads concurrently claim distributed (ROUND_ROBIN
 * CAS-claim) and oversize single-rank (direct fetch-add) ranges.  This is the
 * chunked-allocator successor of the per-thread key-partition coupling
 * regression: the historical failure mode is the same — two allocation paths
 * disagreeing about ownership of a key span and minting duplicate GUIDs.
 *
 * Checks, per home rank:
 *   - every minted seq is globally unique,
 *   - reserved intervals are pairwise disjoint,
 *   - no minted seq falls inside any reserved interval,
 * and every distributed-range member round-trips through index_from.
 *
 * No runtime: guid.c is #include'd with libc-backed shims; real atomics.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 4;
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

#include "../../libs/src/core/gas/guid.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#define NRANK 4u
#define MINTERS 6
#define MINTS_PER_THREAD 80000 /* ~20k per home per thread: several chunks */
#define RESERVERS 2
#define RESERVE_ITERS 200
#define RR_SIZE 40                                  /* stride 10 */
#define BIG_SIZE (3u * (unsigned)ARTS_GUID_DB_CHUNK + 7u) /* > any chunk */

struct interval_s {
  uint64_t lo, hi; /* [lo, hi) */
};

struct minter_out_s {
  unsigned int thread_id; /* the minter's runtime thread id */
  uint64_t *seqs[NRANK];
  unsigned count[NRANK];
};
struct reserver_out_s {
  struct interval_s *iv[NRANK];
  unsigned count[NRANK];
  int fail;
};

static void *minter(void *arg) {
  struct minter_out_s *out = (struct minter_out_s *)arg;
  arts_thread_info.thread_id = out->thread_id;
  for (unsigned r = 0; r < NRANK; r++) {
    out->seqs[r] =
        (uint64_t *)malloc(sizeof(uint64_t) * (MINTS_PER_THREAD / NRANK + 1));
    out->count[r] = 0;
  }
  for (unsigned i = 0; i < MINTS_PER_THREAD; i++) {
    unsigned r = i % NRANK;
    arts_guid_t g = arts_guid_create_for_rank(r, ARTS_GUID_DB);
    out->seqs[r][out->count[r]++] = ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(g));
  }
  return NULL;
}

static void *reserver(void *arg) {
  struct reserver_out_s *out = (struct reserver_out_s *)arg;
  unsigned cap = RESERVE_ITERS * 2;
  for (unsigned r = 0; r < NRANK; r++) {
    out->iv[r] = (struct interval_s *)malloc(sizeof(struct interval_s) * cap);
    out->count[r] = 0;
  }
  for (unsigned it = 0; it < RESERVE_ITERS; it++) {
    /* Distributed: same span claimed on EVERY home. */
    arts_guid_t dist =
        arts_guid_reserve_range(ARTS_GUID_DB, RR_SIZE, ARTS_HINT_ROUND_ROBIN);
    uint64_t base = ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(dist));
    uint64_t stride = (RR_SIZE + NRANK - 1) / NRANK;
    for (unsigned r = 0; r < NRANK; r++) {
      out->iv[r][out->count[r]++] =
          (struct interval_s){base, base + stride};
    }
    for (unsigned i = 0; i < RR_SIZE; i++) {
      arts_guid_t m = arts_guid_from_index(dist, i);
      if (arts_guid_index_from(dist, m) != (int64_t)i) {
        out->fail = 1;
      }
    }
    /* Oversize single-rank: forces the direct shared-counter claim. */
    unsigned home = it % NRANK;
    arts_guid_t big = arts_guid_reserve_range(ARTS_GUID_DB, BIG_SIZE, home);
    uint64_t b = ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(big));
    out->iv[home][out->count[home]++] = (struct interval_s){b, b + BIG_SIZE};
  }
  return NULL;
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
static int cmp_iv(const void *a, const void *b) {
  const struct interval_s *x = (const struct interval_s *)a;
  const struct interval_s *y = (const struct interval_s *)b;
  return (x->lo > y->lo) - (x->lo < y->lo);
}

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL guid_db_seq_alloc_stress: " __VA_ARGS__);      \
    return 1;                                                                  \
  } while (0)

int main(void) {
  /* Node init as the runtime runs it, with one runtime thread per minter:
   * each minter's chunk cursor is then the registry's to free.  The
   * reservers claim straight from the shared counters and hold no cursor. */
  arts_global_rank_id = 0;
  arts_global_rank_count = NRANK;
  arts_node_info.total_thread_count = MINTERS;
  arts_guid_node_geometry_init();

  pthread_t mt[MINTERS], rt[RESERVERS];
  static struct minter_out_s mo[MINTERS];
  static struct reserver_out_s ro[RESERVERS];
  for (int i = 0; i < MINTERS; i++) {
    mo[i].thread_id = (unsigned int)i;
    pthread_create(&mt[i], NULL, minter, &mo[i]);
  }
  for (int i = 0; i < RESERVERS; i++) {
    pthread_create(&rt[i], NULL, reserver, &ro[i]);
  }
  for (int i = 0; i < MINTERS; i++) {
    pthread_join(mt[i], NULL);
  }
  for (int i = 0; i < RESERVERS; i++) {
    pthread_join(rt[i], NULL);
    if (ro[i].fail) {
      FAIL("distributed member failed index_from round-trip\n");
    }
  }

  for (unsigned r = 0; r < NRANK; r++) {
    /* gather all minted seqs for this home */
    unsigned total = 0;
    for (int i = 0; i < MINTERS; i++) {
      total += mo[i].count[r];
    }
    uint64_t *all = (uint64_t *)malloc(sizeof(uint64_t) * total);
    unsigned n = 0;
    for (int i = 0; i < MINTERS; i++) {
      memcpy(all + n, mo[i].seqs[r], sizeof(uint64_t) * mo[i].count[r]);
      n += mo[i].count[r];
    }
    qsort(all, total, sizeof(uint64_t), cmp_u64);
    for (unsigned i = 1; i < total; i++) {
      if (all[i] == all[i - 1]) {
        FAIL("duplicate minted seq %lu on home %u\n", (unsigned long)all[i],
             r);
      }
    }
    /* gather intervals for this home */
    unsigned ivn = 0;
    for (int i = 0; i < RESERVERS; i++) {
      ivn += ro[i].count[r];
    }
    struct interval_s *iv =
        (struct interval_s *)malloc(sizeof(struct interval_s) * ivn);
    unsigned m = 0;
    for (int i = 0; i < RESERVERS; i++) {
      memcpy(iv + m, ro[i].iv[r], sizeof(struct interval_s) * ro[i].count[r]);
      m += ro[i].count[r];
    }
    qsort(iv, ivn, sizeof(struct interval_s), cmp_iv);
    for (unsigned i = 1; i < ivn; i++) {
      if (iv[i].lo < iv[i - 1].hi) {
        FAIL("overlapping reserved intervals on home %u: [%lu,%lu) vs "
             "[%lu,%lu)\n",
             r, (unsigned long)iv[i - 1].lo, (unsigned long)iv[i - 1].hi,
             (unsigned long)iv[i].lo, (unsigned long)iv[i].hi);
      }
    }
    /* no minted seq inside any reserved interval (both sorted: sweep) */
    unsigned k = 0;
    for (unsigned i = 0; i < total; i++) {
      while (k < ivn && iv[k].hi <= all[i]) {
        k++;
      }
      if (k < ivn && iv[k].lo <= all[i]) {
        FAIL("minted seq %lu on home %u landed inside reserved [%lu,%lu)\n",
             (unsigned long)all[i], r, (unsigned long)iv[k].lo,
             (unsigned long)iv[k].hi);
      }
    }
    free(all);
    free(iv);
  }

  for (int i = 0; i < MINTERS; i++) {
    for (unsigned r = 0; r < NRANK; r++) {
      free(mo[i].seqs[r]);
    }
  }
  for (int i = 0; i < RESERVERS; i++) {
    for (unsigned r = 0; r < NRANK; r++) {
      free(ro[i].iv[r]);
    }
  }
  arts_guid_generator_cleanup();

  printf("PASS guid_db_seq_alloc_stress: %u mints x %u threads + %u "
         "concurrent reserves per thread — unique, disjoint, non-intersecting\n",
         (unsigned)MINTS_PER_THREAD, (unsigned)MINTERS,
         (unsigned)RESERVE_ITERS);
  return 0;
}
