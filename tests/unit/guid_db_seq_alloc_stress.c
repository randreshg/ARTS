/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_db_seq_alloc_stress — global uniqueness of the DB seq allocator under
 * concurrency: minter threads lease chunks from the shared per-home counters
 * at the same time, and the failure mode this pins is two threads leasing
 * overlapping spans and minting duplicate GUIDs.
 *
 * Check, per home rank: every minted seq is globally unique.
 *
 * No runtime: guid.c is #include'd with libc-backed shims; real atomics.
 */

#include "arts/system/shutdown.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 4;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(rc ? rc : 70);
}
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
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#define NRANK 4u
#define MINTERS 6
#define MINTS_PER_THREAD 80000 /* ~20k per home per thread: several chunks */

struct minter_out_s {
  unsigned int thread_id; /* the minter's runtime thread id */
  uint64_t *seqs[NRANK];
  unsigned count[NRANK];
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

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL guid_db_seq_alloc_stress: " __VA_ARGS__);      \
    return 1;                                                                  \
  } while (0)

int main(void) {
  /* Node init as the runtime runs it, with one runtime thread per minter:
   * each minter's chunk cursor is then the registry's to free. */
  arts_global_rank_id = 0;
  arts_global_rank_count = NRANK;
  arts_node_info.total_thread_count = MINTERS;
  arts_guid_node_geometry_init();

  pthread_t mt[MINTERS];
  static struct minter_out_s mo[MINTERS];
  for (int i = 0; i < MINTERS; i++) {
    mo[i].thread_id = (unsigned int)i;
    pthread_create(&mt[i], NULL, minter, &mo[i]);
  }
  for (int i = 0; i < MINTERS; i++) {
    pthread_join(mt[i], NULL);
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
    free(all);
  }

  for (int i = 0; i < MINTERS; i++) {
    for (unsigned r = 0; r < NRANK; r++) {
      free(mo[i].seqs[r]);
    }
  }
  arts_guid_generator_cleanup();

  printf("PASS guid_db_seq_alloc_stress: %u mints x %u threads - every seq "
         "unique per home\n",
         (unsigned)MINTS_PER_THREAD, (unsigned)MINTERS);
  return 0;
}
