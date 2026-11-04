/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_db_downgrade hands a block on and keeps reading it, for both kinds of
 * write hold: a block its EDT created and a block it acquired as an RW
 * dependence.
 *
 * D runs on the last rank.  It creates one block per home, takes one RW
 * dependence per home, writes every block, downgrades every hold, creates as
 * many blocks again and fills them with another pattern (storage a write
 * release let go could be drawn for one of them), then wakes one reader per
 * block with an RO dependence and re-reads every block through the pointer it
 * had: its own bytes, every slot now DB_MODE_RO with its guid and ptr
 * unchanged.  Each reader checks the bytes D wrote.
 *
 * Homes, by rank count: everything on rank 0 with one rank; with two, one
 * block of each kind on D's rank and one on rank 0, read on rank 0; with four,
 * also one of each on rank 1, read on rank 2 (a third rank's home).  A finale
 * ordered after every EDT destroys the blocks.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NAME "db_downgrade_handoff"
#define WORDS 512u
#define SZ (WORDS * sizeof(uint64_t))
#define MAXH 3u

static uint64_t pattern(unsigned int tag, unsigned int j) {
  return ((uint64_t)(0xD0u + tag) << 40) | ((uint64_t)tag << 32) | j;
}

static int check_block(const uint64_t *p, unsigned int tag, const char *who) {
  if (p == NULL) {
    printf("FAIL: " NAME " %s: block %u has no pointer\n", who, tag);
    arts_test_fail();
    return 0;
  }
  for (unsigned int j = 0; j < WORDS; j++) {
    if (p[j] != pattern(tag, j)) {
      printf("FAIL: " NAME " %s: block %u word %u is 0x%llx, wrote 0x%llx\n",
             who, tag, j, (unsigned long long)p[j],
             (unsigned long long)pattern(tag, j));
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

static void fill(uint64_t *p, unsigned int tag) {
  for (unsigned int j = 0; j < WORDS; j++) {
    p[j] = pattern(tag, j);
  }
}

/* paramv[0]: the block's tag. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)check_block((const uint64_t *)depv[0].ptr, (unsigned int)paramv[0],
                    "a reader");
}

/* paramv: nh, finale, E_dep[nh], E_cre[nh], home[nh].  depv[i]: the RW
 * dependence homed on home[i]. */
static void downgrader_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  unsigned int nh = (unsigned int)paramv[0];
  arts_guid_t finale = (arts_guid_t)paramv[1];
  const uint64_t *e_dep = &paramv[2];
  const uint64_t *e_cre = &paramv[2 + nh];
  const uint64_t *home = &paramv[2 + 2 * nh];
  if (depc != nh) {
    printf("FAIL: " NAME " D has %u dependences, wired %u\n", depc, nh);
    arts_test_fail();
    return;
  }

  arts_guid_t cre[MAXH], again[MAXH], dep_guid[MAXH];
  uint64_t *cp[MAXH], *dp[MAXH];
  for (unsigned int i = 0; i < nh; i++) {
    arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
    h.rank = (unsigned int)home[i];
    void *p = NULL;
    cre[i] = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
    cp[i] = (uint64_t *)p;
    dp[i] = (uint64_t *)depv[i].ptr;
    dep_guid[i] = depv[i].guid;
    if (cp[i] == NULL || dp[i] == NULL || depv[i].mode != DB_MODE_RW) {
      printf("FAIL: " NAME " D: a hold came without its pointer or mode\n");
      arts_test_fail();
      return;
    }
    fill(cp[i], 2 * i);
    fill(dp[i], 2 * i + 1);
  }

  for (unsigned int i = 0; i < nh; i++) {
    arts_db_downgrade(cre[i]);
    arts_db_downgrade(dep_guid[i]);
  }

  /* As many blocks again, of the same size, filled with another pattern. */
  for (unsigned int i = 0; i < nh; i++) {
    arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
    h.rank = (unsigned int)home[i];
    void *p = NULL;
    again[i] = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
    if (p != NULL) {
      memset(p, 0xA5, SZ);
    }
  }

  for (unsigned int i = 0; i < nh; i++) {
    arts_event_satisfy((arts_guid_t)e_cre[i], cre[i]);
    arts_event_satisfy((arts_guid_t)e_dep[i], dep_guid[i]);
    arts_edt_satisfy_slot(finale, 1 + i, cre[i], DB_MODE_NULL);
  }

  for (unsigned int i = 0; i < nh; i++) {
    (void)check_block(cp[i], 2 * i, "D after its downgrade");
    (void)check_block(dp[i], 2 * i + 1, "D after its downgrade");
    if (depv[i].mode != DB_MODE_RO || depv[i].guid != dep_guid[i] ||
        depv[i].ptr != (void *)dp[i]) {
      printf("FAIL: " NAME " D: slot %u reads mode %d guid %llu ptr %p after "
             "the downgrade\n",
             i, (int)depv[i].mode, (unsigned long long)depv[i].guid,
             depv[i].ptr);
      arts_test_fail();
    }
  }

  for (unsigned int i = 0; i < nh; i++) {
    arts_db_destroy(again[i]);
  }
}

/* paramv: nh, dep[nh].  depv[0]: the group; depv[1 + i]: created block i. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int nh = (unsigned int)paramv[0];
  for (unsigned int i = 0; i < nh; i++) {
    arts_db_destroy((arts_guid_t)paramv[1 + i]);
    arts_db_destroy(depv[1 + i].guid);
  }
  printf("PASS: " NAME " %u homes\n", nh);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  unsigned int dr = n - 1u;
  unsigned int home[MAXH], reader_rank[MAXH], nh = 0;
  home[nh] = dr;
  reader_rank[nh++] = 0u;
  if (n >= 2u) {
    home[nh] = 0u;
    reader_rank[nh++] = 0u;
  }
  if (n >= 4u) {
    home[nh] = 1u;
    reader_rank[nh++] = 2u;
  }

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  uint64_t fparam[1 + MAXH];
  fparam[0] = nh;
  arts_guid_t dep[MAXH];
  for (unsigned int i = 0; i < nh; i++) {
    arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
    h.rank = home[i];
    void *p = NULL;
    dep[i] = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
    if (p != NULL) {
      memset(p, 0, SZ);
    }
    arts_db_release(dep[i]);
    fparam[1 + i] = (uint64_t)dep[i];
  }
  arts_guid_t fin = arts_edt_create(finale_edt, 1 + nh, fparam, 1 + nh,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);

  uint64_t dparam[2 + 3 * MAXH];
  dparam[0] = nh;
  dparam[1] = (uint64_t)fin;
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  for (unsigned int i = 0; i < nh; i++) {
    for (unsigned int kind = 0; kind < 2u; kind++) {
      arts_guid_t e = arts_event_create(&eh);
      uint64_t tag = 2u * i + (kind == 0u ? 1u : 0u);
      arts_guid_t r = arts_edt_create(
          reader_edt, 1, &tag, 1,
          &(arts_edt_hint_t){.rank = reader_rank[i], .group = group});
      arts_add_dependence(e, r, 0, DB_MODE_RO);
      dparam[2 + kind * nh + i] = (uint64_t)e;
    }
    dparam[2 + 2 * nh + i] = home[i];
  }
  arts_guid_t d =
      arts_edt_create(downgrader_edt, 2 + 3 * nh, dparam, nh,
                      &(arts_edt_hint_t){.rank = dr, .group = group});
  for (unsigned int i = 0; i < nh; i++) {
    arts_add_dependence(dep[i], d, i, DB_MODE_RW);
  }
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
