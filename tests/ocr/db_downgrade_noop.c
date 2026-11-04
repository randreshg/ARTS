/* SPDX-License-Identifier: Apache-2.0
 *
 * The calls of arts_db_downgrade that change nothing: on a hold already read
 * only (an RO dependence, or a second downgrade), on a hold released before
 * the call, and on GUIDs the EDT does not hold (NULL_GUID, an event's GUID, a
 * block it never held, a block it created with ARTS_DB_PROP_NO_ACQUIRE).  A
 * slot that carries an event's GUID in DB_MODE_RW holds nothing either: a
 * downgrade or a release of that GUID leaves the slot as it is.  One
 * downgrade is made from a function the body calls back through qsort, which
 * acts on the running EDT's hold like a call from the body itself.
 *
 * The oracle for the holds is the later writer: an RW acquisition on another
 * rank, ordered after D, of each block D held.  A write release run twice
 * leaves a read count behind under exclusion (the writer is never admitted),
 * and a missing one keeps the turn; it also checks D's bytes.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NAME "db_downgrade_noop"
#define WORDS 32u
#define SZ (WORDS * sizeof(uint64_t))

enum { B_RO, B_RW, B_CMP, B_CRE, NB };
/* D's fourth slot: an event's GUID in DB_MODE_RW. */
#define S_EVT 3u
static const uint64_t g_base[NB] = {0x1000u, 0x2000u, 0x3000u, 0x4000u};

static void fill(uint64_t *p, uint64_t base) {
  for (unsigned int j = 0; j < WORDS; j++) {
    p[j] = base + j;
  }
}

static void check(const uint64_t *p, uint64_t base, const char *who) {
  for (unsigned int j = 0; j < WORDS; j++) {
    if (p == NULL || p[j] != base + j) {
      printf("FAIL: " NAME " %s: word %u is 0x%llx, expected 0x%llx\n", who, j,
             p ? (unsigned long long)p[j] : 0ull,
             (unsigned long long)(base + j));
      arts_test_fail();
      return;
    }
  }
}

static void expect_slot(const arts_edt_dep_t *s, arts_guid_t g, void *ptr,
                        arts_db_access_mode_t mode, const char *when) {
  if (s->guid != g || s->ptr != ptr || s->mode != mode) {
    printf("FAIL: " NAME " %s: slot reads guid %llu ptr %p mode %d, expected "
           "guid %llu ptr %p mode %d\n",
           when, (unsigned long long)s->guid, s->ptr, (int)s->mode,
           (unsigned long long)g, ptr, (int)mode);
    arts_test_fail();
  }
}

static arts_guid_t g_cmp_guid;
static unsigned int g_cmp_calls;

/* Runs on the EDT's own thread, inside qsort: the call acts on the running
 * EDT's hold. */
static int downgrading_compare(const void *a, const void *b) {
  if (g_cmp_calls++ == 0u) {
    arts_db_downgrade(g_cmp_guid);
  }
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* paramv: the next stage, never-held block, event.  depv: B_RO (RO), B_RW
 * (RW), B_CMP (RW), S_EVT (the event, RW). */
static void downgrader_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t stage2 = (arts_guid_t)paramv[0];
  arts_guid_t never = (arts_guid_t)paramv[1];
  arts_guid_t event = (arts_guid_t)paramv[2];
  arts_edt_dep_t slot[3];
  for (unsigned int i = 0; i < 3u; i++) {
    slot[i] = depv[i];
  }
  if (slot[B_RW].ptr == NULL || slot[B_CMP].ptr == NULL ||
      slot[B_RO].ptr == NULL) {
    printf("FAIL: " NAME " a dependence came without a pointer\n");
    arts_test_fail();
    return;
  }
  fill((uint64_t *)slot[B_RW].ptr, g_base[B_RW]);
  fill((uint64_t *)slot[B_CMP].ptr, g_base[B_CMP]);

  /* An RO hold is already a read hold. */
  arts_db_downgrade(slot[B_RO].guid);
  expect_slot(&depv[B_RO], slot[B_RO].guid, slot[B_RO].ptr, DB_MODE_RO,
              "an RO slot after a downgrade");
  check((const uint64_t *)depv[B_RO].ptr, g_base[B_RO], "D's RO slot");

  /* An RW hold: twice, released, once more. */
  arts_db_downgrade(slot[B_RW].guid);
  expect_slot(&depv[B_RW], slot[B_RW].guid, slot[B_RW].ptr, DB_MODE_RO,
              "an RW slot after a downgrade");
  arts_db_downgrade(slot[B_RW].guid);
  expect_slot(&depv[B_RW], slot[B_RW].guid, slot[B_RW].ptr, DB_MODE_RO,
              "an RW slot after a second downgrade");
  check((const uint64_t *)slot[B_RW].ptr, g_base[B_RW], "D's downgraded slot");
  arts_db_release(slot[B_RW].guid);
  expect_slot(&depv[B_RW], NULL_GUID, NULL, DB_MODE_NULL,
              "a downgraded slot after its release");
  arts_db_downgrade(slot[B_RW].guid);
  expect_slot(&depv[B_RW], NULL_GUID, NULL, DB_MODE_NULL,
              "a released slot after a downgrade");

  /* A created hold: twice, released, once more. */
  void *cp = NULL;
  arts_guid_t cre = arts_db_create(&cp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (cp == NULL) {
    printf("FAIL: " NAME " an acquiring create was handed no pointer\n");
    arts_test_fail();
    return;
  }
  fill((uint64_t *)cp, g_base[B_CRE]);
  arts_db_downgrade(cre);
  arts_db_downgrade(cre);
  check((const uint64_t *)cp, g_base[B_CRE], "D's downgraded created block");
  arts_db_release(cre);
  arts_db_downgrade(cre);

  /* Nothing held. */
  arts_db_downgrade(NULL_GUID);
  expect_slot(&depv[S_EVT], event, NULL, DB_MODE_RW,
              "an event's GUID in an RW slot");
  arts_db_downgrade(event);
  expect_slot(&depv[S_EVT], event, NULL, DB_MODE_RW,
              "an event's GUID in an RW slot after a downgrade");
  arts_db_release(event);
  expect_slot(&depv[S_EVT], event, NULL, DB_MODE_RW,
              "an event's GUID in an RW slot after a release");
  arts_db_downgrade(never);
  void *none = NULL;
  arts_guid_t lazy =
      arts_db_create(&none, SZ, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, NULL);
  arts_db_downgrade(lazy);

  /* From a function the body calls back. */
  uint64_t keys[8] = {7, 3, 5, 1, 8, 2, 6, 4};
  g_cmp_guid = slot[B_CMP].guid;
  g_cmp_calls = 0u;
  qsort(keys, 8, sizeof(keys[0]), downgrading_compare);
  expect_slot(&depv[B_CMP], slot[B_CMP].guid, slot[B_CMP].ptr, DB_MODE_RO,
              "an RW slot after a downgrade from a callback");
  check((const uint64_t *)slot[B_CMP].ptr, g_base[B_CMP],
        "D's slot downgraded from a callback");

  arts_edt_satisfy_slot(stage2, 1, cre, DB_MODE_NULL);
  arts_edt_satisfy_slot(stage2, 2, lazy, DB_MODE_NULL);
}

/* paramv[0]: the base to expect; depv[0]: the block, RW. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *p = (uint64_t *)depv[0].ptr;
  check(p, paramv[0], "the later writer");
  if (p != NULL) {
    fill(p, 0xFFFF0000u);
  }
}

/* paramv: the four blocks, the no-acquire block, the never-held block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  for (unsigned int i = 0; i < NB + 2u; i++) {
    arts_db_destroy((arts_guid_t)paramv[i]);
  }
  printf("PASS: " NAME "\n");
  arts_shutdown();
}

/* Ordered after D's completion: one RW writer of each block D held, on rank
 * 0 (another rank than D's when there is one).  paramv: the three dependence
 * blocks, the never-held block.  depv[0]: D's completion; depv[1]: the
 * created block; depv[2]: the no-acquire block. */
static void stage2_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_guid_t blocks[NB] = {(arts_guid_t)paramv[0], (arts_guid_t)paramv[1],
                            (arts_guid_t)paramv[2], depv[1].guid};
  for (unsigned int b = 0; b < NB; b++) {
    uint64_t base = g_base[b];
    arts_guid_t w =
        arts_edt_create(writer_edt, 1, &base, 1,
                        &(arts_edt_hint_t){.rank = 0u, .group = group});
    arts_add_dependence(blocks[b], w, 0, DB_MODE_RW);
  }
  uint64_t fparam[NB + 2];
  for (unsigned int b = 0; b < NB; b++) {
    fparam[b] = (uint64_t)blocks[b];
  }
  fparam[NB] = (uint64_t)depv[2].guid;
  fparam[NB + 1] = paramv[3];
  arts_guid_t fin = arts_edt_create(finale_edt, NB + 2, fparam, 1,
                                    &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(group, fin, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

static arts_guid_t released_block(unsigned int home, uint64_t base) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.rank = home;
  void *p = NULL;
  arts_guid_t g = arts_db_create(&p, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
  if (p != NULL) {
    fill((uint64_t *)p, base);
  }
  arts_db_release(g);
  return g;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  unsigned int dr = n - 1u;
  arts_guid_t ro = released_block(0u, g_base[B_RO]);
  arts_guid_t rw = released_block(0u, 0);
  arts_guid_t cmp = released_block(dr, 0);
  arts_guid_t never = released_block(0u, 0);
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t event = arts_event_create(&eh);

  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t d_done = arts_event_create(&ch);
  uint64_t sparam[4] = {(uint64_t)ro, (uint64_t)rw, (uint64_t)cmp,
                        (uint64_t)never};
  arts_guid_t stage2 =
      arts_edt_create(stage2_edt, 4, sparam, 3, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(d_done, stage2, 0, DB_MODE_NULL);

  uint64_t dparam[3] = {(uint64_t)stage2, (uint64_t)never, (uint64_t)event};
  arts_guid_t d = arts_edt_create(
      downgrader_edt, 3, dparam, 4,
      &(arts_edt_hint_t){.rank = dr, .completion_event = d_done});
  arts_edt_satisfy_slot(d, S_EVT, event, DB_MODE_RW);
  arts_add_dependence(ro, d, B_RO, DB_MODE_RO);
  arts_add_dependence(rw, d, B_RW, DB_MODE_RW);
  arts_add_dependence(cmp, d, B_CMP, DB_MODE_RW);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
