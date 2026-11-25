/* SPDX-License-Identifier: Apache-2.0
 *
 * The calls of arts_db_publish that change nothing, and the sequences that
 * mix it with the other calls acting on a hold.
 *
 * Nothing to publish: on an RO dependence, on a hold already downgraded, on
 * a hold already released, and on GUIDs the EDT does not hold (NULL_GUID, an
 * event's GUID, a block it never held, a block it created with
 * ARTS_DB_PROP_NO_ACQUIRE).  A slot that carries an event's GUID in
 * DB_MODE_RW holds nothing either and keeps its guid, ptr and mode.
 *
 * On a write hold, each sequence on its own block: two publishes in a row
 * with no store between, on a block homed on D's rank and, when there is
 * another rank, on one homed there (off the home a publish sends the bytes
 * to the home under {VAL,INV}·HOME and CDAG, so the second is a second
 * payload flight, or a write-back on the credit the first one's answer
 * left); a publish from a function the body calls back through qsort, which
 * acts on the running EDT's hold like a call from the body; publish, then
 * downgrade, then read; publish, then release, then publish (the slot stays
 * empty); publish, then a destroy by the same EDT; an alias pair (an RW and
 * an RO slot naming one block), whose two slots both keep their guid, ptr
 * and mode; and a create's hold, homed on another rank than D's when there
 * is one, published, downgraded and released, with a publish after each.
 * Each published slot keeps its guid, ptr and mode,
 * and D stores again after the publish, so the bytes its hold ends with were
 * written after the publish.
 *
 * The oracle for the holds is the later writer: an RW acquisition on rank 0
 * (another rank than D's when there is one), ordered after D, of each block
 * D held and did not destroy.  It must be admitted and must read the bytes D
 * wrote last.  A publish that ran a release instead can show here only where
 * that early release is observable: where it gives the rank's write right
 * up, so that the bytes D stores after it stay unpublished (a block off its
 * home under PURGE on VAL and INV) or a downgrade after it finds no write
 * turn left (exclusion with hand-back); where it lets the creator's copy go
 * (CDAG, a create's hold off its home); or where the releases it adds use up
 * the rank's possession, which RETAIN counts as a hold on VAL and INV: there
 * a later writer is never admitted, so the run does not end and the test's
 * time limit fails it.  Where it changes nothing a later acquisition sees (a
 * block homed on D's rank under PURGE on VAL and INV, exclusion with
 * retained ownership, a dependence under CDAG, whose release is the
 * write-back) it goes unnoticed here; db_publish_word reads the state a
 * publish leaves.  A failure that leaves D unable to hand its blocks on ends
 * the run at once with a non-zero status.  Every write acquisition
 * of a block is ordered after the previous one's release, so the program is
 * valid under CDAG.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define NAME "db_publish_noop"
#define WORDS 32u
#define SZ (WORDS * sizeof(uint64_t))
#define SECOND 0x80000000u

/* The blocks D holds (B_CRE it creates) and its dependence slots.  B_ALIAS
 * fills two slots, S_ALIAS_RW and S_ALIAS_RO; S_EVT carries an event.
 * B_TWICE_OFF, homed on another rank than D's, and its slot S_TWICE_OFF, the
 * last, exist only when there is another rank (NULL_GUID otherwise). */
enum {
  B_RO,
  B_DG,
  B_REL,
  B_TWICE,
  B_CMP,
  B_ALIAS,
  B_TWICE_OFF,
  B_CRE,
  B_DESTROY,
  NB
};
enum {
  S_RO,
  S_DG,
  S_REL,
  S_TWICE,
  S_CMP,
  S_ALIAS_RW,
  S_ALIAS_RO,
  S_EVT,
  S_DESTROY,
  S_TWICE_OFF,
  NS
};
/* The blocks a later writer acquires: every one but B_DESTROY. */
#define NW B_DESTROY
static const uint64_t g_base[NB] = {0x1000u, 0x2000u, 0x3000u,
                                    0x4000u, 0x5000u, 0x6000u,
                                    0x9000u, 0x7000u, 0x8000u};

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
static int publishing_compare(const void *a, const void *b) {
  if (g_cmp_calls++ == 0u) {
    arts_db_publish(g_cmp_guid);
  }
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* A failure after which D cannot hand its blocks on ends the run at once,
 * with the failure in its status, instead of leaving the later stages
 * waiting. */
static _Noreturn void fail_and_end(const char *what) {
  printf("FAIL: " NAME " %s\n", what);
  arts_test_fail();
  arts_shutdown();
}

/* Two publishes in a row with no store between, then a store. */
static void publish_twice(arts_edt_dep_t *live, const arts_edt_dep_t *held,
                          uint64_t base, const char *when) {
  uint64_t *p = (uint64_t *)held->ptr;
  fill(p, base);
  arts_db_publish(held->guid);
  arts_db_publish(held->guid);
  expect_slot(live, held->guid, p, DB_MODE_RW, when);
  check(p, base, when);
  fill(p, base + SECOND);
}

/* paramv: the next stage, the never-held block, the event.  depv: see the
 * S_ enum (S_TWICE_OFF only when depc is NS); every block slot but S_RO and
 * S_ALIAS_RO is RW. */
static void publisher_edt(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  arts_guid_t stage2 = (arts_guid_t)paramv[0];
  arts_guid_t never = (arts_guid_t)paramv[1];
  arts_guid_t event = (arts_guid_t)paramv[2];
  arts_edt_dep_t slot[NS] = {{0}};
  for (unsigned int i = 0; i < depc; i++) {
    slot[i] = depv[i];
    if (i != S_EVT && slot[i].ptr == NULL) {
      fail_and_end("a dependence came without a pointer");
    }
  }
  if (slot[S_ALIAS_RO].guid != slot[S_ALIAS_RW].guid ||
      slot[S_ALIAS_RO].ptr != slot[S_ALIAS_RW].ptr) {
    fail_and_end("the alias pair does not name one acquisition");
  }

  /* An RO hold has nothing to publish. */
  arts_db_publish(slot[S_RO].guid);
  expect_slot(&depv[S_RO], slot[S_RO].guid, slot[S_RO].ptr, DB_MODE_RO,
              "an RO slot after a publish");
  check((const uint64_t *)depv[S_RO].ptr, g_base[B_RO], "D's RO slot");

  /* Publish, downgrade, read; then a publish of the downgraded hold. */
  uint64_t *dg = (uint64_t *)slot[S_DG].ptr;
  fill(dg, g_base[B_DG]);
  arts_db_publish(slot[S_DG].guid);
  expect_slot(&depv[S_DG], slot[S_DG].guid, dg, DB_MODE_RW,
              "an RW slot after a publish");
  fill(dg, g_base[B_DG] + SECOND);
  arts_db_downgrade_release(slot[S_DG].guid);
  expect_slot(&depv[S_DG], slot[S_DG].guid, dg, DB_MODE_RO,
              "a published slot after a downgrade");
  check(dg, g_base[B_DG] + SECOND, "D's downgraded slot");
  arts_db_publish(slot[S_DG].guid);
  expect_slot(&depv[S_DG], slot[S_DG].guid, dg, DB_MODE_RO,
              "a downgraded slot after a publish");
  check(dg, g_base[B_DG] + SECOND, "D's downgraded slot after a publish");

  /* Publish, release, publish. */
  uint64_t *rel = (uint64_t *)slot[S_REL].ptr;
  fill(rel, g_base[B_REL]);
  arts_db_publish(slot[S_REL].guid);
  expect_slot(&depv[S_REL], slot[S_REL].guid, rel, DB_MODE_RW,
              "an RW slot after a publish");
  fill(rel, g_base[B_REL] + SECOND);
  arts_db_release(slot[S_REL].guid);
  expect_slot(&depv[S_REL], NULL_GUID, NULL, DB_MODE_NULL,
              "a published slot after its release");
  arts_db_publish(slot[S_REL].guid);
  expect_slot(&depv[S_REL], NULL_GUID, NULL, DB_MODE_NULL,
              "a released slot after a publish");

  /* Twice in a row, no store between, on D's rank and off it. */
  publish_twice(&depv[S_TWICE], &slot[S_TWICE], g_base[B_TWICE],
                "an RW slot homed on D's rank after two publishes");
  if (depc == NS) {
    publish_twice(&depv[S_TWICE_OFF], &slot[S_TWICE_OFF], g_base[B_TWICE_OFF],
                  "an RW slot homed off D's rank after two publishes");
  }

  /* From a function the body calls back. */
  uint64_t *cmp = (uint64_t *)slot[S_CMP].ptr;
  fill(cmp, g_base[B_CMP]);
  uint64_t keys[8] = {7, 3, 5, 1, 8, 2, 6, 4};
  g_cmp_guid = slot[S_CMP].guid;
  g_cmp_calls = 0u;
  qsort(keys, 8, sizeof(keys[0]), publishing_compare);
  expect_slot(&depv[S_CMP], slot[S_CMP].guid, cmp, DB_MODE_RW,
              "an RW slot after a publish from a callback");
  check(cmp, g_base[B_CMP], "D's slot published from a callback");
  fill(cmp, g_base[B_CMP] + SECOND);

  /* The alias pair. */
  uint64_t *al = (uint64_t *)slot[S_ALIAS_RW].ptr;
  fill(al, g_base[B_ALIAS]);
  arts_db_publish(slot[S_ALIAS_RW].guid);
  expect_slot(&depv[S_ALIAS_RW], slot[S_ALIAS_RW].guid, al, DB_MODE_RW,
              "the alias pair's RW slot after a publish");
  expect_slot(&depv[S_ALIAS_RO], slot[S_ALIAS_RO].guid, al, DB_MODE_RO,
              "the alias pair's RO slot after a publish");
  check(al, g_base[B_ALIAS], "D's alias pair after a publish");
  fill(al, g_base[B_ALIAS] + SECOND);

  /* Publish, then destroy. */
  uint64_t *ds = (uint64_t *)slot[S_DESTROY].ptr;
  fill(ds, g_base[B_DESTROY]);
  arts_db_publish(slot[S_DESTROY].guid);
  expect_slot(&depv[S_DESTROY], slot[S_DESTROY].guid, ds, DB_MODE_RW,
              "an RW slot after a publish");
  check(ds, g_base[B_DESTROY], "D's slot before its destroy");
  arts_db_destroy(slot[S_DESTROY].guid);

  /* A create's hold: publish, downgrade, publish, release, publish.  Homed
   * on rank 0, another rank than D's when there is one, so the hold's
   * publish, downgrade and release take the paths of a creator off the
   * block's home. */
  arts_db_hint_t ch = ARTS_DB_HINT_DEFAULTS;
  ch.rank = 0u;
  void *vp = NULL;
  arts_guid_t cre = arts_db_create(&vp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &ch);
  uint64_t *cp = (uint64_t *)vp;
  if (cp == NULL) {
    fail_and_end("an acquiring create was handed no pointer");
  }
  fill(cp, g_base[B_CRE]);
  arts_db_publish(cre);
  check(cp, g_base[B_CRE], "D's created block after a publish");
  fill(cp, g_base[B_CRE] + SECOND);
  arts_db_downgrade_release(cre);
  arts_db_publish(cre);
  check(cp, g_base[B_CRE] + SECOND, "D's downgraded created block");
  arts_db_release(cre);
  arts_db_publish(cre);

  /* Nothing held. */
  arts_db_publish(NULL_GUID);
  expect_slot(&depv[S_EVT], event, NULL, DB_MODE_RW,
              "an event's GUID in an RW slot");
  arts_db_publish(event);
  expect_slot(&depv[S_EVT], event, NULL, DB_MODE_RW,
              "an event's GUID in an RW slot after a publish");
  arts_db_publish(never);
  void *none = NULL;
  arts_guid_t lazy =
      arts_db_create(&none, SZ, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, NULL);
  arts_db_publish(lazy);

  arts_edt_satisfy_slot(stage2, 1, cre, DB_MODE_NULL);
  arts_edt_satisfy_slot(stage2, 2, lazy, DB_MODE_NULL);
}

/* paramv[0]: the bytes to expect; depv[0]: the block, RW. */
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

/* paramv: the blocks the writers held, the no-acquire block, the never-held
 * block. */
static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  for (unsigned int i = 0; i < NW + 2u; i++) {
    if ((arts_guid_t)paramv[i] != NULL_GUID) {
      arts_db_destroy((arts_guid_t)paramv[i]);
    }
  }
  printf("PASS: " NAME "\n");
  arts_shutdown();
}

/* Ordered after D's completion: one RW writer of each block D held and did
 * not destroy, on rank 0 (another rank than D's when there is one).  paramv:
 * the dependence blocks in B_ order up to B_TWICE_OFF (NULL_GUID on one
 * rank), the never-held block.  depv[0]: D's completion; depv[1]: the
 * created block; depv[2]: the no-acquire block. */
static void stage2_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_guid_t blocks[NW];
  for (unsigned int b = 0; b < B_CRE; b++) {
    blocks[b] = (arts_guid_t)paramv[b];
  }
  blocks[B_CRE] = depv[1].guid;
  for (unsigned int b = 0; b < NW; b++) {
    if (blocks[b] == NULL_GUID) {
      continue;
    }
    uint64_t want = b == B_RO ? g_base[b] : g_base[b] + SECOND;
    arts_guid_t w =
        arts_edt_create(writer_edt, 1, &want, 1,
                        &(arts_edt_hint_t){.rank = 0u, .group = group});
    arts_add_dependence(blocks[b], w, 0, DB_MODE_RW);
  }
  uint64_t fparam[NW + 2];
  for (unsigned int b = 0; b < NW; b++) {
    fparam[b] = (uint64_t)blocks[b];
  }
  fparam[NW] = (uint64_t)depv[2].guid;
  fparam[NW + 1] = paramv[B_CRE];
  arts_guid_t fin = arts_edt_create(finale_edt, NW + 2, fparam, 1,
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
  /* Homes: B_TWICE and B_CMP on D's rank, the others on rank 0; B_TWICE_OFF
   * only where rank 0 is another rank than D's. */
  static const unsigned int on_d[B_CRE] = {0, 0, 0, 1, 1, 0, 0};
  arts_guid_t dep[B_CRE];
  for (unsigned int b = 0; b < B_CRE; b++) {
    dep[b] = b == B_TWICE_OFF && n < 2u
                 ? NULL_GUID
                 : released_block(on_d[b] ? dr : 0u,
                                  b == B_RO ? g_base[b] : 0u);
  }
  arts_guid_t destroyed = released_block(0u, 0u);
  arts_guid_t never = released_block(0u, 0u);
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t event = arts_event_create(&eh);

  arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t d_done = arts_event_create(&ch);
  uint64_t sparam[B_CRE + 1];
  for (unsigned int b = 0; b < B_CRE; b++) {
    sparam[b] = (uint64_t)dep[b];
  }
  sparam[B_CRE] = (uint64_t)never;
  arts_guid_t stage2 = arts_edt_create(stage2_edt, B_CRE + 1, sparam, 3,
                                       &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(d_done, stage2, 0, DB_MODE_NULL);

  uint64_t dparam[3] = {(uint64_t)stage2, (uint64_t)never, (uint64_t)event};
  arts_guid_t d = arts_edt_create(
      publisher_edt, 3, dparam, n > 1u ? NS : NS - 1u,
      &(arts_edt_hint_t){.rank = dr, .completion_event = d_done});
  arts_edt_satisfy_slot(d, S_EVT, event, DB_MODE_RW);
  arts_add_dependence(dep[B_RO], d, S_RO, DB_MODE_RO);
  arts_add_dependence(dep[B_DG], d, S_DG, DB_MODE_RW);
  arts_add_dependence(dep[B_REL], d, S_REL, DB_MODE_RW);
  arts_add_dependence(dep[B_TWICE], d, S_TWICE, DB_MODE_RW);
  arts_add_dependence(dep[B_CMP], d, S_CMP, DB_MODE_RW);
  arts_add_dependence(dep[B_ALIAS], d, S_ALIAS_RW, DB_MODE_RW);
  arts_add_dependence(dep[B_ALIAS], d, S_ALIAS_RO, DB_MODE_RO);
  arts_add_dependence(destroyed, d, S_DESTROY, DB_MODE_RW);
  if (n > 1u) {
    arts_add_dependence(dep[B_TWICE_OFF], d, S_TWICE_OFF, DB_MODE_RW);
  }
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
