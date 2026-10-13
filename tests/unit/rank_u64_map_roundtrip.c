/* SPDX-License-Identifier: Apache-2.0
 *
 * rank_u64_map serialize/load round-trip and the load's bounds.
 *
 * Wire layout: count(u32) + pad(u32) + count * {rank(u32), pad(u32), ver(u64)}.
 * len == 8 + count*16; zero slots are elided.
 *
 * Properties:
 *   1. Sparse round-trip: set a few non-zero (rank,version) pairs, serialize,
 *      load into a map of the SAME nranks -> every pair restored, every other
 *      slot cleared.
 *   2. count == number of NON-ZERO slots; serialized length == 8 + count*16.
 *   3. Empty map -> count==0, length==8, load -> all-zero map.
 *   4. Worst-case bound: ALL nranks slots non-zero -> length == 8 + nranks*16,
 *      and a caller buffer of exactly that size suffices.
 *   5. rank >= nranks pairs in the image are dropped by the load: they neither
 *      crash nor alias another slot.
 *   6. The load reads nothing past `size`: an image whose count names more
 *      pairs than `size` holds, or a `size` shorter than the count header, is
 *      refused (false) with the map untouched -- the short buffer is a heap
 *      allocation of exactly `size` bytes, so a read past it is an ASan report.
 *
 * Standalone: links rank_u64_map.c (no shims needed -- it uses libc malloc).
 */

#include "arts/coherence/directory.h"
#include "arts/transport/protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NRANKS 8

static int check_roundtrip(void) {
  struct arts_rank_to_u64_map_s *m = arts_rank_u64_map_create(NRANKS);
  /* 3 non-zero pairs. */
  arts_rank_u64_map_set(m, 1, 11);
  arts_rank_u64_map_set(m, 4, 44);
  arts_rank_u64_map_set(m, 7, 77);

  /* Worst-case buffer bound: 8 + nranks*16. */
  unsigned char buf[8 + NRANKS * 16];
  size_t len = arts_rank_u64_map_serialize(m, buf);
  size_t expect_len = 8 + (size_t)3 * 16;
  if (len != expect_len) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: len %zu != %zu\n", len,
                  expect_len);
    return 1;
  }
  uint32_t count = *(const uint32_t *)buf;
  if (count != 3) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: count %u != 3\n",
                  count);
    return 1;
  }

  struct arts_rank_to_u64_map_s *m2 = arts_rank_u64_map_create(NRANKS);
  /* Seed a slot the image does not name: a load must leave the sender's view
   * exactly, so this value has to be cleared rather than survive. */
  arts_rank_u64_map_set(m2, 5, 55);
  arts_rank_u64_map_load(m2, buf, len);
  int rc = 0;
  for (unsigned int r = 0; r < NRANKS; r++) {
    uint64_t want = (r == 1) ? 11 : (r == 4) ? 44 : (r == 7) ? 77 : 0;
    if (arts_rank_u64_map_get(m2, r) != want) {
      (void)fprintf(stderr,
                    "FAIL rank_u64_map_roundtrip: rank %u got %llu want %llu\n",
                    r, (unsigned long long)arts_rank_u64_map_get(m2, r),
                    (unsigned long long)want);
      rc = 1;
    }
  }
  arts_rank_u64_map_destroy(m);
  arts_rank_u64_map_destroy(m2);
  return rc;
}

static int check_empty(void) {
  struct arts_rank_to_u64_map_s *m = arts_rank_u64_map_create(NRANKS);
  unsigned char buf[8 + NRANKS * 16];
  size_t len = arts_rank_u64_map_serialize(m, buf);
  if (len != 8 || *(const uint32_t *)buf != 0) {
    (void)fprintf(stderr,
                  "FAIL rank_u64_map_roundtrip: empty len %zu count %u\n", len,
                  *(const uint32_t *)buf);
    return 1;
  }
  struct arts_rank_to_u64_map_s *m2 = arts_rank_u64_map_create(NRANKS);
  arts_rank_u64_map_load(m2, buf, len);
  int rc = 0;
  for (unsigned int r = 0; r < NRANKS; r++) {
    if (arts_rank_u64_map_get(m2, r) != 0) {
      rc = 1;
    }
  }
  arts_rank_u64_map_destroy(m);
  arts_rank_u64_map_destroy(m2);
  if (rc) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: empty not all-zero\n");
  }
  return rc;
}

static int check_worst_case(void) {
  struct arts_rank_to_u64_map_s *m = arts_rank_u64_map_create(NRANKS);
  for (unsigned int r = 0; r < NRANKS; r++) {
    arts_rank_u64_map_set(m, r, (uint64_t)(r + 1) * 1000);
  }
  unsigned char buf[8 + NRANKS * 16];
  size_t len = arts_rank_u64_map_serialize(m, buf);
  if (len != 8 + (size_t)NRANKS * 16) {
    (void)fprintf(stderr,
                  "FAIL rank_u64_map_roundtrip: worst-case len %zu != %zu\n",
                  len, (size_t)(8 + NRANKS * 16));
    arts_rank_u64_map_destroy(m);
    return 1;
  }
  struct arts_rank_to_u64_map_s *m2 = arts_rank_u64_map_create(NRANKS);
  arts_rank_u64_map_load(m2, buf, len);
  int rc = 0;
  for (unsigned int r = 0; r < NRANKS; r++) {
    if (arts_rank_u64_map_get(m2, r) != (uint64_t)(r + 1) * 1000) {
      rc = 1;
    }
  }
  arts_rank_u64_map_destroy(m);
  arts_rank_u64_map_destroy(m2);
  if (rc) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: worst-case mismatch\n");
  }
  return rc;
}

/* rank >= nranks pairs in the wire are dropped (set() OOB guard). */
static int check_oob_rank_dropped(void) {
  /* Hand-craft a wire buffer: count=2, one valid (rank 3, v=33), one OOB
   * (rank NRANKS+5, v=99). */
  unsigned char buf[8 + 2 * 16];
  memset(buf, 0, sizeof(buf));
  uint32_t *cf = (uint32_t *)buf;
  cf[0] = 2;
  cf[1] = 0;
  struct arts_msg_rank_version_pair_s *e =
      (struct arts_msg_rank_version_pair_s *)(buf + 8);
  e[0].rank = 3;
  e[0].pad = 0;
  e[0].version = 33;
  e[1].rank = NRANKS + 5;
  e[1].pad = 0;
  e[1].version = 99;
  struct arts_rank_to_u64_map_s *m = arts_rank_u64_map_create(NRANKS);
  arts_rank_u64_map_load(m, buf, sizeof(buf));
  int rc = 0;
  if (arts_rank_u64_map_get(m, 3) != 33) {
    rc = 1;
  }
  for (unsigned int r = 0; r < NRANKS; r++) {
    if (r != 3 && arts_rank_u64_map_get(m, r) != 0) {
      rc = 1; /* OOB pair must not have aliased into a valid slot. */
    }
  }
  arts_rank_u64_map_destroy(m);
  if (rc) {
    (void)fprintf(
        stderr, "FAIL rank_u64_map_roundtrip: OOB rank not cleanly dropped\n");
  }
  return rc;
}

/* A load reads nothing past `size` and refuses an image that does not fit. */
static int check_load_bounds(void) {
  int rc = 0;
  /* Room for the header and ONE pair; the count claims 64. */
  size_t cap = 8 + 1 * 16;
  unsigned char *buf = (unsigned char *)malloc(cap);
  memset(buf, 0, cap);
  uint32_t *cf = (uint32_t *)buf;
  cf[0] = 64;
  struct arts_msg_rank_version_pair_s *e =
      (struct arts_msg_rank_version_pair_s *)(buf + 8);
  e[0].rank = 0;
  e[0].version = 1;
  struct arts_rank_to_u64_map_s *m = arts_rank_u64_map_create(NRANKS);
  arts_rank_u64_map_set(m, 2, 22);
  if (arts_rank_u64_map_load(m, buf, cap)) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: an inflated count "
                          "was loaded\n");
    rc = 1;
  }
  if (arts_rank_u64_map_load(m, buf, 4)) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: a buffer shorter "
                          "than the count header was loaded\n");
    rc = 1;
  }
  if (arts_rank_u64_map_get(m, 2) != 22 || arts_rank_u64_map_get(m, 0) != 0) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: a refused load "
                          "changed the map\n");
    rc = 1;
  }
  /* The same pair with its true count fits exactly and loads. */
  cf[0] = 1;
  if (!arts_rank_u64_map_load(m, buf, cap) ||
      arts_rank_u64_map_get(m, 0) != 1 || arts_rank_u64_map_get(m, 2) != 0) {
    (void)fprintf(stderr, "FAIL rank_u64_map_roundtrip: an exact-fit image "
                          "did not load\n");
    rc = 1;
  }
  arts_rank_u64_map_destroy(m);
  free(buf);
  return rc;
}

int main(void) {
  int rc = 0;
  rc |= check_roundtrip();
  rc |= check_empty();
  rc |= check_worst_case();
  rc |= check_oob_rank_dropped();
  rc |= check_load_bounds();
  if (rc != 0) {
    return 1;
  }
  printf("PASS rank_u64_map_roundtrip\n");
  return 0;
}

/* libc-backed allocator shims: rank_u64_map.c is compiled standalone, without
 * the runtime allocator. */
void *arts_malloc(size_t size) { return malloc(size); }
void *arts_calloc(size_t n, size_t size) { return calloc(n, size); }
void arts_free(void *ptr) { free(ptr); }
