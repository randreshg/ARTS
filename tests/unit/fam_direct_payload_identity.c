/* SPDX-License-Identifier: Apache-2.0
 *
 * Under direct residency an EDT's working bytes ARE the block's store:
 *
 *   1. every holder's pointer lies inside the pool (arts_fam_contains), and
 *      the creator's pointer and the next holder's are the same address --
 *      a staged arm hands each turn its own rank-local copy, so any copy in
 *      the path changes the address;
 *   3. what a holder writes through that pointer is what the next holder
 *      reads, so the identity is not identity of an unused pointer.
 *
 * (Property 2 -- the address is the same on every RANK that holds it -- needs
 * more than one rank and is added with the multinode registration.)
 *
 * The reader names the block TWICE, so one of its slots is an alias of the
 * other's acquisition: a second slot filled from the first's resolved pointer
 * rather than by an acquisition of its own.  Both slots must carry the same
 * address as the creator's, which is what puts the alias fill and its release
 * on this program's path as well as the ordinary acquire.
 */
#include "arts.h"
#include "arts/fam/pool.h"

#include "../test_failure_status.h"

#include <stdio.h>

#define WORDS 64u
#define SENTINEL 0x5A5AA5A5u

static arts_guid_t g_db;
static uint64_t g_first_ptr;

static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || !arts_fam_contains(d)) {
    (void)fprintf(stderr, "FAIL fam_direct_payload_identity: a holder's "
                          "pointer is not in the pool\n");
    arts_test_fail();
    return;
  }
  if ((uint64_t)(uintptr_t)d != g_first_ptr) {
    (void)fprintf(stderr,
                  "FAIL fam_direct_payload_identity: the holder's pointer "
                  "%llx is not the creator's %llx\n",
                  (unsigned long long)(uintptr_t)d,
                  (unsigned long long)g_first_ptr);
    arts_test_fail();
    return;
  }
  for (unsigned int i = 0; i < WORDS; i++) {
    d[i] = SENTINEL ^ i;
  }
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const unsigned int *d = (const unsigned int *)depv[0].ptr;
  if (d == NULL || !arts_fam_contains(d) ||
      (uint64_t)(uintptr_t)d != g_first_ptr) {
    (void)fprintf(stderr, "FAIL fam_direct_payload_identity: the reader's "
                          "pointer is not the block's slot\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (depv[2].ptr != depv[0].ptr || !arts_fam_contains(depv[2].ptr)) {
    (void)fprintf(stderr, "FAIL fam_direct_payload_identity: a second slot "
                          "naming the block was handed %p, not the block's "
                          "slot %p\n",
                  depv[2].ptr, depv[0].ptr);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < WORDS; i++) {
    if (d[i] != (SENTINEL ^ i)) {
      (void)fprintf(stderr,
                    "FAIL fam_direct_payload_identity: word %u reads 0x%x\n", i,
                    d[i]);
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  printf("PASS fam_direct_payload_identity\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *p = NULL;
  g_db = arts_db_create(&p, WORDS * sizeof(unsigned int), ARTS_DB,
                        ARTS_DB_PROP_NONE, NULL);
  if (p == NULL || !arts_fam_contains(p)) {
    (void)fprintf(stderr, "FAIL fam_direct_payload_identity: the creator's "
                          "pointer is not in the pool\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  g_first_ptr = (uint64_t)(uintptr_t)p;
  arts_db_release(g_db, DB_MODE_RW);

  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_edt_hint_t wh = {.finish_event = written};
  arts_guid_t w = arts_edt_create(writer_edt, 0, NULL, 1, &wh);
  arts_guid_t r = arts_edt_create(reader_edt, 0, NULL, 3, NULL);
  arts_add_dependence(g_db, w, 0, DB_MODE_RW);
  arts_add_dependence(g_db, r, 0, DB_MODE_RO);
  arts_add_dependence(written, r, 1, DB_MODE_NULL);
  arts_add_dependence(g_db, r, 2, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
