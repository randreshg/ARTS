/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_reserve_range — pure-unit coverage of arts_guid_reserve_range and the
 * labeled-vs-auto collision-avoidance invariant.
 *
 * Properties:
 *
 *   1. A ROUND_ROBIN reserve_range claims the SAME span [base, base+stride)
 *      on every home's counter, so a subsequent AUTO mint (which leases a
 *      chunk from the same shared counter) can never produce a key inside
 *      the labeled span.  We reserve a distributed range, expand it via
 *      from_index, then auto-mint on every rank and assert NO auto GUID
 *      equals any labeled GUID.
 *
 *   2. A single-rank reserve_range claims `size` CONSECUTIVE seqs in one
 *      fetch-add on the shared counter — contiguity holds and later mints
 *      land strictly above the range.
 *
 *   3. DB range members inherit the range's exact szhint bits (the sentinel),
 *      and index_from round-trips every member.
 *
 *   4. The round-robin stride is ceil(size / nrank) for every size: at
 *      nrank = 3 a range of UINT_MAX GUIDs advances every home's counter by
 *      1,431,655,765, for a DB range (the per-home claim) and for a flat one
 *      (the thread's per-home counters) alike, and a later reserve on every
 *      home lies above the range's last member there.
 *
 * No runtime: guid.c is #include'd with libc-backed shims; node_init runs
 * the runtime's node init for one simulated rank.
 */

#include <limits.h>
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

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL guid_reserve_range: " __VA_ARGS__);            \
    return 1;                                                                  \
  } while (0)

static void node_fini(void) {
  if (arts_node_info.keys != NULL) {
    for (unsigned t = 0; t < arts_node_info.total_thread_count; t++) {
      free(arts_node_info.keys[t]);
    }
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
  arts_guid_generator_cleanup();
}

/* The runtime's node init for rank `rank` of `nrank` with one thread: the
 * geometry, then the thread's key counters. */
static void node_init(unsigned int rank, unsigned int nrank) {
  node_fini();
  arts_global_rank_id = rank;
  arts_global_rank_count = nrank;
  arts_node_info.total_thread_count = 1;
  arts_node_info.keys = (uint64_t **)calloc(1, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  arts_thread_info.thread_id = 0;
  arts_guid_key_generator_init();
}

/* Home h's counter of the arm that serves `kind`. */
static uint64_t home_counter(arts_guid_kind_t kind, unsigned int h) {
  return kind == ARTS_GUID_DB
             ? atomic_load_explicit(&db_seq_next[h], memory_order_relaxed)
             : *arts_guid_generator_get_key(h, (unsigned int)kind);
}

/* The key offset that orders two GUIDs of one home within its arm. */
static uint64_t arm_key(arts_guid_kind_t kind, arts_guid_t g) {
  uint64_t key = ARTS_GUID_GET_KEY(g);
  return kind == ARTS_GUID_DB ? ARTS_GUID_DB_GET_SEQ(key) : key;
}

/* Property 4 for `kind`; 0 when it holds. */
static int check_stride(arts_guid_kind_t kind) {
  const unsigned int nrank = 3;
  const uint64_t stride = 1431655765u; /* ceil(UINT_MAX / 3) */
  node_init(0, nrank);
  uint64_t before[3];
  for (unsigned int h = 0; h < nrank; h++) {
    before[h] = home_counter(kind, h);
  }
  arts_guid_t range =
      arts_guid_reserve_range(kind, UINT_MAX, ARTS_HINT_ROUND_ROBIN);
  if (range == NULL_GUID) {
    FAIL("kind %u: a round-robin range of UINT_MAX returned NULL_GUID\n",
         (unsigned)kind);
  }
  for (unsigned int h = 0; h < nrank; h++) {
    uint64_t advance = home_counter(kind, h) - before[h];
    if (advance != stride) {
      FAIL("kind %u: a range of UINT_MAX advanced home %u's counter by "
           "%lu, not %lu\n",
           (unsigned)kind, h, (unsigned long)advance, (unsigned long)stride);
    }
  }
  arts_guid_t next =
      arts_guid_reserve_range(kind, nrank, ARTS_HINT_ROUND_ROBIN);
  for (unsigned int h = 0; h < nrank; h++) {
    /* Home h's last member is the one of ordinal stride - 1. */
    arts_guid_t last =
        arts_guid_from_index(range, (unsigned int)((stride - 1) * nrank + h));
    arts_guid_t single = arts_guid_reserve(kind, h);
    arts_guid_t later = arts_guid_from_index(next, h);
    if (arts_guid_get_rank(last) != h ||
        arts_guid_index_from(range, last) !=
            (int64_t)((stride - 1) * nrank + h)) {
      FAIL("kind %u: home %u's last member does not round-trip\n",
           (unsigned)kind, h);
    }
    if (arm_key(kind, single) <= arm_key(kind, last) ||
        arm_key(kind, later) <= arm_key(kind, last)) {
      FAIL("kind %u: a later reserve on home %u lies inside the range\n",
           (unsigned)kind, h);
    }
  }
  return 0;
}

int main(void) {
  /* ---- 1. labeled-vs-auto collision avoidance. ---- */
  node_init(0, 4);
  const unsigned RSIZE = 40; /* spans several keys per rank (stride=10) */
  arts_guid_t dist =
      arts_guid_reserve_range(ARTS_GUID_DB, RSIZE, ARTS_HINT_ROUND_ROBIN);
  if (dist == NULL_GUID) {
    FAIL("distributed reserve_range returned NULL_GUID\n");
  }
  if (arts_guid_get_rank(dist) != ARTS_DISTRIBUTED_RANK) {
    FAIL("distributed range rank tag wrong: %u\n", arts_guid_get_rank(dist));
  }

  /* materialize all labeled GUIDs */
  arts_guid_t labeled[RSIZE];
  for (unsigned i = 0; i < RSIZE; i++) {
    labeled[i] = arts_guid_from_index(dist, i);
  }

  /* now auto-mint on EVERY rank; none may collide with a labeled GUID */
  for (unsigned r = 0; r < arts_global_rank_count; r++) {
    for (int k = 0; k < 8; k++) {
      arts_guid_t auto_g = arts_guid_create_for_rank(r, ARTS_GUID_DB);
      for (unsigned i = 0; i < RSIZE; i++) {
        if (auto_g == labeled[i]) {
          FAIL("auto GUID on rank %u collided with labeled[%u]\n", r, i);
        }
      }
    }
  }

  /* ---- 2. single-rank range: contiguous, exclusive. ---- */
  node_init(0, 4);
  const unsigned NEED = 8;
  arts_guid_t rstart = arts_guid_reserve_range(ARTS_GUID_DB, NEED, 2);
  if (rstart == NULL_GUID) {
    FAIL("single-rank reserve_range returned NULL_GUID\n");
  }
  for (unsigned i = 1; i < NEED; i++) {
    if (arts_guid_get_key(arts_guid_from_index(rstart, i)) !=
        arts_guid_get_key(rstart) + i) {
      FAIL("range not contiguous at %u\n", i);
    }
  }
  /* a later mint on the same home must land strictly above the range */
  arts_guid_t after = arts_guid_create_for_rank(2, ARTS_GUID_DB);
  if (ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(after)) <
      ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(rstart)) + NEED) {
    FAIL("auto mint landed inside the reserved single-rank range\n");
  }

  /* ---- 3. szhint inheritance + index_from round-trip. ---- */
  for (unsigned i = 0; i < NEED; i++) {
    arts_guid_t g = arts_guid_from_index(rstart, i);
    if (ARTS_GUID_DB_GET_SZHINT(ARTS_GUID_GET_KEY(g)) !=
        ARTS_GUID_DB_SZHINT_NONE) {
      FAIL("range member %u lost the sentinel szhint\n", i);
    }
    if (arts_guid_index_from(rstart, g) != (int64_t)i) {
      FAIL("index_from round-trip broke at %u\n", i);
    }
  }

  /* ---- 4. the round-robin stride, both arms. ---- */
  if (check_stride(ARTS_GUID_DB) != 0 || check_stride(ARTS_GUID_EVENT) != 0) {
    return 1;
  }

  node_fini();
  printf("PASS guid_reserve_range: labeled-vs-auto collision-free, "
         "contiguous single-rank claim, sentinel inheritance + round-trip, "
         "round-robin stride of a UINT_MAX range on both arms\n");
  return 0;
}
