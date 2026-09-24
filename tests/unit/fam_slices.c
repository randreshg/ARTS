/// @file fam_slices.c
/// @brief One pool, one address, disjoint slices, and nothing else inside it.
///
/// Fails on: a rank that mapped the pool somewhere else; a slice that is not
/// where the layout puts it; overlapping slices; a rank that cannot see a
/// peer's slice; a peer's flushed bytes read wrong; and any other mapping
/// inside the pool's range, which is what a rank that mapped after the fabric
/// and the registered pool would produce on some ranks and not others.

#include "arts.h"
#include "arts/fam/pool.h"
#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAM_SLICES_MAX_RANKS 8u
#define RECORD_BYTES 64u

struct slice_report_s {
  uint64_t pool_lo;
  uint64_t slice_lo;
  uint64_t slice_len;
  uint64_t block;
};

static struct slice_report_s g_reports[FAM_SLICES_MAX_RANKS];
static atomic_uint g_reported;

/* Whitebox, and deliberately: the header is at a compile-time address under
 * this backend, so a rank can state the layout it believes in without a new
 * entry point. */
static const struct arts_fam_header_s *fam_header(void) {
  return (const struct arts_fam_header_s *)(uintptr_t)ARTS_FAM_BASE;
}

/* Exactly one mapping must cover the whole pool: mappings never overlap, so a
 * single line containing the range is the same statement as "nothing else was
 * carved inside it". */
static int pool_is_one_mapping(uint64_t lo, uint64_t bytes) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    return -1;
  }
  char line[512];
  int found = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long a = 0, b = 0;
    if (sscanf(line, "%llx-%llx", &a, &b) != 2) {
      continue;
    }
    if (a <= lo && b >= lo + bytes) {
      found = 1;
      break;
    }
  }
  (void)fclose(f);
  return found;
}

static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned n = arts_get_total_ranks();
  const struct slice_report_s *r0 = &g_reports[0];
  int bad = 0;
  for (unsigned r = 0; r < n; r++) {
    const struct slice_report_s *r_ = &g_reports[r];
    if (r_->pool_lo != r0->pool_lo || r_->slice_len != r0->slice_len) {
      arts_printf("FAIL fam_slices: rank %u sees pool %llx len %llu, rank 0 "
                  "sees %llx len %llu\n",
                  r, (unsigned long long)r_->pool_lo,
                  (unsigned long long)r_->slice_len,
                  (unsigned long long)r0->pool_lo,
                  (unsigned long long)r0->slice_len);
      arts_test_fail();
      bad = 1;
    }
    uint64_t want =
        r0->pool_lo + ARTS_FAM_HEADER_BYTES + (uint64_t)r * r0->slice_len;
    if (r_->slice_lo != want) {
      arts_printf("FAIL fam_slices: rank %u slice at %llx, layout says %llx\n",
                  r, (unsigned long long)r_->slice_lo,
                  (unsigned long long)want);
      arts_test_fail();
      bad = 1;
    }
    if (!arts_fam_contains((const void *)(uintptr_t)r_->slice_lo) ||
        !arts_fam_contains((const void *)(uintptr_t)r_->block)) {
      arts_printf("FAIL fam_slices: rank 0 does not see rank %u's slice\n", r);
      arts_test_fail();
      bad = 1;
    }
    if (r_->block < r_->slice_lo ||
        r_->block + RECORD_BYTES > r_->slice_lo + r_->slice_len) {
      arts_printf("FAIL fam_slices: rank %u's block is outside its slice\n", r);
      arts_test_fail();
      bad = 1;
    }
    for (unsigned s = r + 1; s < n; s++) {
      uint64_t alo = r_->slice_lo, ahi = alo + r_->slice_len;
      uint64_t blo = g_reports[s].slice_lo, bhi = blo + g_reports[s].slice_len;
      if (alo < bhi && blo < ahi) {
        arts_printf("FAIL fam_slices: slices of ranks %u and %u overlap\n", r,
                    s);
        arts_test_fail();
        bad = 1;
      }
    }
    /* The owner of a peer's slot is its ADDRESS and nothing else: this is the
     * question every free-to-owner message must answer for its target slot,
     * asked here across ranks, where a wrong divisor or a wrong header
     * offset shows. */
    if (arts_fam_owner_of((const void *)(uintptr_t)r_->block) != r) {
      arts_printf("FAIL fam_slices: owner_of(rank %u's block) = %u\n", r,
                  arts_fam_owner_of((const void *)(uintptr_t)r_->block));
      arts_test_fail();
      bad = 1;
    }
    /* What the peer wrote and flushed must be what rank 0 reads. */
    const unsigned char *b = (const unsigned char *)(uintptr_t)r_->block;
    arts_fam_flush_consumer(b, RECORD_BYTES);
    for (unsigned k = 0; k < RECORD_BYTES; k++) {
      if (b[k] != (unsigned char)(r + 1u)) {
        arts_printf("FAIL fam_slices: rank %u's block byte %u is 0x%02x\n", r,
                    k, b[k]);
        arts_test_fail();
        bad = 1;
        break;
      }
    }
  }
  /* The verdict is printed only when there is one: a PASS line in a log that
   * also holds FAIL lines is how a later triage goes wrong. */
  if (!bad) {
    arts_printf(
        "PASS fam_slices: %u ranks, one pool at %llx, slice %llu bytes\n", n,
        (unsigned long long)r0->pool_lo, (unsigned long long)r0->slice_len);
  }
  arts_shutdown();
}

static void report_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  if (paramc < 5) {
    arts_printf("FAIL fam_slices: short report\n");
    arts_test_fail();
    return;
  }
  unsigned r = (unsigned)paramv[0];
  if (r >= FAM_SLICES_MAX_RANKS) {
    arts_printf("FAIL fam_slices: rank %u beyond this test's %u\n", r,
                FAM_SLICES_MAX_RANKS);
    arts_test_fail();
    return;
  }
  g_reports[r].pool_lo = paramv[1];
  g_reports[r].slice_lo = paramv[2];
  g_reports[r].slice_len = paramv[3];
  g_reports[r].block = paramv[4];
  if (atomic_fetch_add_explicit(&g_reported, 1u, memory_order_acq_rel) + 1u ==
      arts_get_total_ranks()) {
    arts_guid_t chk =
        arts_edt_create(check_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
    (void)chk;
  }
}

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned me = arts_get_current_rank();
  const struct arts_fam_header_s *h = fam_header();
  uint64_t pool_lo = (uint64_t)(uintptr_t)ARTS_FAM_BASE;
  uint64_t slice_lo =
      pool_lo + ARTS_FAM_HEADER_BYTES + (uint64_t)me * h->slice_len;

  if (pool_is_one_mapping(pool_lo, h->bytes) != 1) {
    arts_printf("FAIL fam_slices: rank %u has something else mapped inside "
                "the pool\n",
                me);
    arts_test_fail();
  }

  void *p = arts_fam_alloc(RECORD_BYTES);
  if (((uintptr_t)p % ARTS_FAM_GRANULE) != 0 || !arts_fam_contains(p) ||
      (uint64_t)(uintptr_t)p < slice_lo ||
      (uint64_t)(uintptr_t)p >= slice_lo + h->slice_len) {
    arts_printf("FAIL fam_slices: rank %u got %p, outside its own slice\n", me,
                p);
    arts_test_fail();
    return;
  }
  memset(p, (int)(unsigned char)(me + 1u), RECORD_BYTES);
  arts_fam_flush_producer(p, RECORD_BYTES);

  uint64_t params[5] = {(uint64_t)me, pool_lo, slice_lo, h->slice_len,
                        (uint64_t)(uintptr_t)p};
  (void)arts_edt_create(report_edt, 5, params, 0,
                        &(arts_edt_hint_t){.rank = 0});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned n = arts_get_total_ranks();
  if (n > FAM_SLICES_MAX_RANKS) {
    arts_printf("FAIL fam_slices: %u ranks exceeds this test's %u\n", n,
                FAM_SLICES_MAX_RANKS);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned r = 0; r < n; r++) {
    (void)arts_edt_create(writer_edt, 0, NULL, 0,
                          &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
