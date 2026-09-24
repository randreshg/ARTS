/// @file fam_strict_flush.c
/// @brief Under the strict oracle, an omitted flush is a wrong value -- and a
/// consumer flush publishes nothing.
///
/// Fails on: a fresh block that is not poisoned; a write that crosses with no
/// producer flush (the half a hardware-coherent host cannot show, hence the
/// oracle); a write that does not cross after both flushes; a CONSUMER flush
/// that changes the backing; and an idle rank's random write-back reverting a
/// peer's flushed bytes in a page the two share.

#include "arts.h"
#include "arts/fam/pool.h"
#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BLK 64u
#define PAT_ONE 0x11
#define PAT_TWO 0x22
#define PAT_PEER 0x77
#define PINGPONGS 200

static int all_bytes_are(const void *p, unsigned char v) {
  const unsigned char *b = (const unsigned char *)p;
  for (unsigned i = 0; i < BLK; i++) {
    if (b[i] != v) {
      return 0;
    }
  }
  return 1;
}

/* The six steps of the chain below.  Each takes paramv[0] = a, paramv[1] = b
 * and creates the next on the rank named in the sequence. */
static void s2_peer_reads_unflushed_edt(uint32_t paramc, const uint64_t *paramv,
                                        uint32_t depc, arts_edt_dep_t depv[]);
static void s3_owner_flushes_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]);
static void s4_peer_reads_flushed_edt(uint32_t paramc, const uint64_t *paramv,
                                      uint32_t depc, arts_edt_dep_t depv[]);
static void s5_owner_churns_edt(uint32_t paramc, const uint64_t *paramv,
                                uint32_t depc, arts_edt_dep_t depv[]);
static void s6_peer_rechecks_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]);

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]);

static void s2_peer_reads_unflushed_edt(uint32_t paramc, const uint64_t *paramv,
                                        uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *a = (void *)(uintptr_t)paramv[0];

  arts_fam_flush_consumer(a, BLK);
  if (all_bytes_are(a, (unsigned char)PAT_ONE)) {
    arts_printf("FAIL fam_strict_flush: rank 1 saw the owner's write before "
                "any producer flush\n");
    arts_test_fail();
  }
  /* A second reload with nothing new flushed must land on the same bytes the
   * first one brought in: a consumer flush reloads, it never writes back. */
  unsigned char snap[BLK];
  memcpy(snap, a, BLK);
  arts_fam_flush_consumer(a, BLK);
  if (memcmp(snap, a, BLK) != 0) {
    arts_printf("FAIL fam_strict_flush: a repeated consumer flush changed "
                "bytes nobody flushed in\n");
    arts_test_fail();
  }

  uint64_t params[2] = {paramv[0], paramv[1]};
  (void)arts_edt_create(s3_owner_flushes_edt, 2, params, 0,
                        &(arts_edt_hint_t){.rank = 0});
}

static void s3_owner_flushes_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *a = (void *)(uintptr_t)paramv[0];

  arts_fam_flush_producer(a, BLK);
  /* The write turn is about to pass to the peer, which must reload the
   * range before it may hold it in turn -- so the owner drops its own hold
   * here rather than carry it across the handoff. */
  arts_fam_strict_unhold(a, BLK);

  uint64_t params[2] = {paramv[0], paramv[1]};
  (void)arts_edt_create(s4_peer_reads_flushed_edt, 2, params, 0,
                        &(arts_edt_hint_t){.rank = 1});
}

static void s4_peer_reads_flushed_edt(uint32_t paramc, const uint64_t *paramv,
                                      uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *a = (void *)(uintptr_t)paramv[0];
  void *b = (void *)(uintptr_t)paramv[1];

  arts_fam_flush_consumer(a, BLK);
  if (!all_bytes_are(a, (unsigned char)PAT_ONE)) {
    arts_printf("FAIL fam_strict_flush: rank 1 did not see the owner's "
                "producer-flushed write\n");
    arts_test_fail();
  }
  /* Two more reloads with nothing new flushed must not move it either. */
  for (int i = 0; i < 2; i++) {
    arts_fam_flush_consumer(a, BLK);
    if (!all_bytes_are(a, (unsigned char)PAT_ONE)) {
      arts_printf("FAIL fam_strict_flush: a repeated consumer flush moved "
                  "already-flushed bytes\n");
      arts_test_fail();
    }
  }

  /* Reload b once before registering it held, same as the owner did for a. */
  arts_fam_flush_consumer(b, BLK);
  arts_fam_strict_hold(b, BLK);
  memset(b, PAT_PEER, BLK);
  arts_fam_flush_producer(b, BLK);
  /* The write turn on a is now this rank's: its last reload was the third
   * repeat check just above, so holding here follows the same reload-then-
   * hold order the owner used. */
  arts_fam_strict_hold(a, BLK);
  memset(a, PAT_TWO, BLK);
  arts_fam_flush_producer(a, BLK);
  arts_fam_strict_unhold(a, BLK);

  uint64_t params[2] = {paramv[0], paramv[1]};
  (void)arts_edt_create(s5_owner_churns_edt, 2, params, 0,
                        &(arts_edt_hint_t){.rank = 0});
}

static void s5_owner_churns_edt(uint32_t paramc, const uint64_t *paramv,
                                uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *a = (void *)(uintptr_t)paramv[0];

  arts_fam_flush_consumer(a, BLK);
  if (!all_bytes_are(a, (unsigned char)PAT_TWO)) {
    arts_printf("FAIL fam_strict_flush: rank 0 did not see rank 1's "
                "producer-flushed write\n");
    arts_test_fail();
  }
  /* The write turn returns to this rank; the reload just above is the one
   * the upcoming hold follows. */
  arts_fam_strict_hold(a, BLK);

  /* b is never held on this rank, so this rank's own sampled write-back must
   * never touch it, however many times its own range is cycled.  A held
   * line may only be producer-flushed, never consumer-flushed (that would
   * discard a write only this rank has); the producer flush alone already
   * runs the sampler, so PINGPONGS producer flushes fire it that many times. */
  for (int i = 0; i < PINGPONGS; i++) {
    memset(a, PAT_ONE, BLK);
    arts_fam_flush_producer(a, BLK);
  }
  arts_fam_strict_unhold(a, BLK);

  uint64_t params[2] = {paramv[0], paramv[1]};
  (void)arts_edt_create(s6_peer_rechecks_edt, 2, params, 0,
                        &(arts_edt_hint_t){.rank = 1});
}

static void s6_peer_rechecks_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  void *b = (void *)(uintptr_t)paramv[1];

  /* This rank's own hold on b must come off before b may be consumer-flushed
   * again; nothing else touches b between the drop and the reload below. */
  arts_fam_strict_unhold(b, BLK);
  arts_fam_flush_consumer(b, BLK);
  if (!all_bytes_are(b, (unsigned char)PAT_PEER)) {
    arts_printf("FAIL fam_strict_flush: rank 0's idle-block churn reverted "
                "rank 1's flushed bytes\n");
    arts_test_fail();
  } else {
    arts_printf("PASS fam_strict_flush\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  if (!arts_fam_strict()) {
    arts_printf("FAIL fam_strict_flush: this test needs the oracle; its cfg "
                "does not set fam_strict\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }

  void *a = arts_fam_alloc(BLK);
  void *b = arts_fam_alloc(BLK);
  if (((uintptr_t)a) / ARTS_FAM_PAGE != ((uintptr_t)b) / ARTS_FAM_PAGE) {
    arts_printf("FAIL fam_strict_flush: two fresh 64-byte blocks landed in "
                "different pages (%p, %p)\n",
                a, b);
    arts_test_fail();
  }

  /* A fresh block is reloaded once to establish this rank's baseline view
   * before it is registered as held -- the order pool.h documents. */
  arts_fam_flush_consumer(a, BLK);
  if (!all_bytes_are(a, (unsigned char)ARTS_FAM_POISON_BYTE)) {
    arts_printf("FAIL fam_strict_flush: a fresh block did not read as "
                "poison\n");
    arts_test_fail();
  }
  arts_fam_strict_hold(a, BLK);
  memset(a, PAT_ONE, BLK);

  uint64_t params[2] = {(uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b};
  (void)arts_edt_create(s2_peer_reads_unflushed_edt, 2, params, 0,
                        &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
