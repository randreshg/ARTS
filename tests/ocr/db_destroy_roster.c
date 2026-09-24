/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_roster — a block's destroy reaches every rank that holds a cache
 * of it, so each of those ranks can create the block's label again.
 *
 * A label homed on rank 0 is created there and written in turn by ranks 1 and
 * 2 (rank 1 alone with two ranks), each through an RW dependence and nothing
 * else, then destroyed at the home.  Each writer rank then creates the label's
 * next generation itself, writes it, and the home reads it back and destroys
 * it.  A writer's cache the destroy did not reach would still hold the rank's
 * slot for the GUID, and that rank's create would wait behind it forever: the
 * ctest timeout is the verdict for that, the value check for the rest.
 *
 * The same is repeated with two zero-size labels, whose caches the home's
 * version bookkeeping never records: one read by those ranks, one written by
 * them (a write turn with no buffer publishes nothing).
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define VALUE(k) (0xB0000000u + (uint32_t)(k))

static void writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (paramv[1] == 0u) {
    return; /* a zero-size block: a turn with nothing to write */
  }
  uint32_t *p = (uint32_t *)depv[0].ptr;
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_roster writer got no storage\n");
    arts_test_fail();
    return;
  }
  *p = VALUE(paramv[0]);
}

static void creator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[0];
  uint64_t size = paramv[2];
  uint32_t *p = (uint32_t *)arts_db_create_with_guid(label, size, ARTS_DB,
                                                     ARTS_DB_PROP_NONE, NULL);
  if (size == 0u) {
    return;
  }
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_roster create handed back no pointer\n");
    arts_test_fail();
    return;
  }
  *p = VALUE(paramv[1]);
}

static void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (paramv[1] == 0u) {
    return; /* a zero-size block has no value to check */
  }
  const uint32_t *v = (const uint32_t *)depv[0].ptr;
  if (v == NULL || *v != VALUE(paramv[0])) {
    arts_printf("FAIL: db_destroy_roster read 0x%x, want 0x%x\n",
                v ? *v : 0u, VALUE(paramv[0]));
    arts_test_fail();
  }
}

static void run_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                   const uint64_t *paramv, arts_guid_t dep) {
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t e = arts_edt_create(
      fn, paramc, paramv, dep != NULL_GUID ? 1u : 0u,
      &(arts_edt_hint_t){.rank = rank, .finish_event = done});
  if (dep != NULL_GUID) {
    arts_add_dependence(dep, e, 0, fn == writer ? DB_MODE_RW : DB_MODE_RO);
  }
  (void)arts_event_wait(done);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2u) {
    arts_printf("SKIP db_destroy_roster: needs >= 2 ranks\n");
    arts_shutdown();
    return;
  }
  unsigned int writers = nranks >= 3u ? 2u : 1u;
  /* {size, how the other ranks use it}: a written block, a zero-size block
   * read, a zero-size block written. */
  const struct {
    uint64_t size;
    arts_edt_t use;
  } cases[3] = {{sizeof(uint32_t), writer}, {0u, reader}, {0u, writer}};
  for (unsigned int c = 0; c < 3u; c++) {
    uint64_t size = cases[c].size;
    arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, 0u);
    uint32_t *p = (uint32_t *)arts_db_create_with_guid(
        label, size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    if (size != 0u) {
      *p = VALUE(0);
    }
    arts_db_release(label, DB_MODE_RW);
    for (unsigned int w = 1; w <= writers; w++) {
      uint64_t upv[2] = {w, size};
      run_on(cases[c].use, w, 2, upv, label);
    }
    if (size != 0u) {
      uint64_t rpv[2] = {writers, size};
      run_on(reader, 0u, 2, rpv, label);
    }
    arts_db_destroy(label);

    for (unsigned int w = 1; w <= writers; w++) {
      uint64_t gen = 10u + w;
      uint64_t cpv[3] = {(uint64_t)label, gen, size};
      run_on(creator, w, 3, cpv, NULL_GUID);
      uint64_t gpv[2] = {gen, size};
      run_on(reader, 0u, 2, gpv, label);
      arts_db_destroy(label);
    }
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_roster %u writer ranks\n", writers);
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
