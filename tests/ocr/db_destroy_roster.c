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
 * it.  A writer's cache the destroy did not reach would outlive the block.
 * Where it kept a right to the block (the write right; under EXCL x RETAIN
 * also its resident bytes or a retained read grant), that rank's create waits
 * behind it forever, and the ctest timeout is the verdict.  Where it kept no
 * such right, the create takes it over as the next generation's first image,
 * since a cache is not told apart from one of an earlier generation, and this
 * test does not see it.  The value check covers the rest.
 *
 * The same is repeated with two zero-size labels, whose caches the home's
 * version bookkeeping never records: one read by those ranks, one written by
 * them (a write turn with no buffer publishes nothing).
 *
 * The runs are strictly sequential: each runs in a finish scope of its own,
 * and the next step, which issues any destroy due before its own run, is a
 * continuation gated on that scope.
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

/* {size, how the other ranks use it}: a written block, a zero-size block
 * read, a zero-size block written. */
static const struct {
  uint64_t size;
  arts_edt_t use;
} cases[3] = {{sizeof(uint32_t), writer}, {0u, reader}, {0u, writer}};

/* paramv of a step: the case, the step within it, the case's label. */
enum { P_CASE, P_STEP, P_LABEL, P_COUNT };

static void step_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]);

/* Runs fn on rank in a finish scope of its own, and the step after `pv`
 * gated on that scope. */
static void run_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                   const uint64_t *paramv, arts_guid_t dep,
                   const uint64_t *pv) {
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t e = arts_edt_create(
      fn, paramc, paramv, dep != NULL_GUID ? 1u : 0u,
      &(arts_edt_hint_t){.rank = rank, .finish_event = done});
  if (dep != NULL_GUID) {
    arts_add_dependence(dep, e, 0, fn == writer ? DB_MODE_RW : DB_MODE_RO);
  }
  uint64_t next[P_COUNT] = {pv[P_CASE], pv[P_STEP] + 1u, pv[P_LABEL]};
  arts_guid_t n = arts_edt_create(step_edt, P_COUNT, next, 1,
                                  &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(done, n, 0, DB_MODE_NULL);
}

/* A case is, with W writer ranks and R = 1 for the sized block (0 for the
 * others): steps 0 .. W-1 each writer rank's use, step W the home's read
 * (sized block only), then for each writer rank two steps, its create of the
 * next generation and the home's read of it.  The label is destroyed before
 * step W+R and before each create after it, and once more after the last
 * read; step 0 creates the case's first generation. */
static void step(const uint64_t *paramv) {
  unsigned int writers = arts_get_total_ranks() >= 3u ? 2u : 1u;
  uint64_t pv[P_COUNT] = {paramv[P_CASE], paramv[P_STEP], paramv[P_LABEL]};
  for (;;) {
    if (pv[P_CASE] == 3u) {
      if (arts_test_status() == 0) {
        arts_printf("PASS: db_destroy_roster %u writer ranks\n", writers);
      }
      arts_shutdown();
      return;
    }
    uint64_t first = writers + (cases[pv[P_CASE]].size != 0u ? 1u : 0u);
    uint64_t k = pv[P_STEP];
    if (k >= first && (k - first) % 2u == 0u) {
      arts_db_destroy((arts_guid_t)pv[P_LABEL]);
    }
    if (k < first + 2u * writers) {
      break;
    }
    pv[P_CASE] += 1u;
    pv[P_STEP] = 0u;
  }
  uint64_t size = cases[pv[P_CASE]].size;
  uint64_t k = pv[P_STEP];
  if (k == 0u) {
    arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, 0u);
    uint32_t *p = (uint32_t *)arts_db_create_with_guid(
        label, size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    if (size != 0u) {
      *p = VALUE(0);
    }
    arts_db_release(label, DB_MODE_RW);
    pv[P_LABEL] = (uint64_t)label;
  }
  arts_guid_t label = (arts_guid_t)pv[P_LABEL];
  uint64_t first = writers + (size != 0u ? 1u : 0u);
  if (k < writers) {
    uint64_t upv[2] = {k + 1u, size};
    run_on(cases[pv[P_CASE]].use, (unsigned int)(k + 1u), 2, upv, label, pv);
  } else if (k < first) {
    uint64_t rpv[2] = {writers, size};
    run_on(reader, 0u, 2, rpv, label, pv);
  } else {
    uint64_t w = (k - first) / 2u + 1u;
    uint64_t gen = 10u + w;
    if ((k - first) % 2u == 0u) {
      uint64_t cpv[3] = {(uint64_t)label, gen, size};
      run_on(creator, (unsigned int)w, 3, cpv, NULL_GUID, pv);
    } else {
      uint64_t gpv[2] = {gen, size};
      run_on(reader, 0u, 2, gpv, label, pv);
    }
  }
}

static void step_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  step(paramv);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u) {
    arts_printf("SKIP db_destroy_roster: needs >= 2 ranks\n");
    arts_shutdown();
    return;
  }
  step((const uint64_t[P_COUNT]){0u, 0u, NULL_GUID});
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
