/* SPDX-License-Identifier: Apache-2.0
 *
 * ctx_owned_finish_cleanup — a finish event's creator token is dropped exactly
 * once: by arts_event_wait when the creator waits on the event (consumed), or
 * by the creator EDT's epilogue when it returns without waiting (unconsumed).
 *
 * The token list is private to the runtime, so the contract is checked through
 * what it controls: every finish scope fires exactly once, and only after its
 * member finished.  A missing drop leaves a scope that never fires (the run
 * times out); an extra drop fires a scope early (its successor sees the member
 * unfinished) or twice (a successor count above one).
 *
 * Two shapes, one checker:
 *  (1) One orchestrator creates K scopes; it waits on the even ones and
 *      returns with the odd ones unconsumed, so a wait must clear only the
 *      token it names and leave the rest to the epilogue.
 *  (2) WAVES producer EDTs each create SCOPES scopes and return without
 *      waiting, so every token of every wave is dropped by an epilogue; the
 *      producers run back to back on the workers' reused per-thread lists,
 *      each of which must be left empty.
 *
 * Each scope has one member, a leaf that marks its slot, and one successor,
 * gated on the scope, that checks the leaf's mark and counts itself.  Every
 * successor joins an outer scope F that gates the checker.
 */
#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define K 6 /* orchestrator scopes: even ones waited on, odd ones not */
#define WAVES 4
#define SCOPES 5
#define TOTAL (K + WAVES * SCOPES)

/* Counter block: TOTAL leaf slots, then TOTAL successor-count slots. */
#define LEAF_SLOT(g) (g)
#define SUCC_SLOT(g) (TOTAL + (g))
#define N_SLOTS (2 * TOTAL)

void leaf(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int *c = (int *)depv[0].ptr;
  if (c) {
    c[LEAF_SLOT((int)paramv[0])] = 1;
  }
}

void succ(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int g = (int)paramv[0];
  int *c = (int *)depv[0].ptr;
  if (c == NULL) {
    arts_printf("FAIL ctx_owned_finish_cleanup: successor %d got a NULL "
                "counter block\n",
                g);
    arts_test_fail();
    return;
  }
  if (c[LEAF_SLOT(g)] != 1) {
    arts_printf("FAIL ctx_owned_finish_cleanup: scope %d fired before its "
                "leaf finished\n",
                g);
    arts_test_fail();
  }
  c[SUCC_SLOT(g)] += 1;
}

/* One scope with index g: a leaf member and a successor gated on the scope. */
static arts_guid_t make_scope(int g, arts_guid_t cdb, arts_guid_t outer) {
  arts_event_hint_t fh = ARTS_EVENT_HINT_FINISH;
  arts_guid_t fe = arts_event_create(&fh);

  arts_edt_hint_t lh = ARTS_EDT_HINT_DEFAULTS;
  lh.finish_event = fe;
  uint64_t pg = (uint64_t)g;
  arts_guid_t l = arts_edt_create(leaf, 1, &pg, 1, &lh);
  arts_add_dependence(cdb, l, 0, DB_MODE_RW);

  arts_edt_hint_t sh = ARTS_EDT_HINT_DEFAULTS;
  sh.finish_event = outer;
  arts_guid_t s = arts_edt_create(succ, 1, &pg, 2, &sh);
  arts_add_dependence(cdb, s, 0, DB_MODE_RW);
  arts_add_dependence(fe, s, 1, DB_MODE_NULL);
  return fe;
}

void checker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const int *c = (const int *)depv[1].ptr;
  if (c == NULL) {
    arts_printf("FAIL ctx_owned_finish_cleanup: checker got a NULL counter "
                "block\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (int g = 0; g < TOTAL; g++) {
    if (c[LEAF_SLOT(g)] != 1) {
      arts_printf("FAIL ctx_owned_finish_cleanup: scope %d leaf never ran\n",
                  g);
      arts_test_fail();
    }
    if (c[SUCC_SLOT(g)] != 1) {
      arts_printf("FAIL ctx_owned_finish_cleanup: scope %d successor fired %d "
                  "times, want 1\n",
                  g, c[SUCC_SLOT(g)]);
      arts_test_fail();
    }
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS ctx_owned_finish_cleanup: %d scopes (%d waited on, %d "
                "dropped by an epilogue) each fired exactly once\n",
                TOTAL, (K + 1) / 2, TOTAL - (K + 1) / 2);
  }
  arts_shutdown();
}

/* The orchestrator holds no grant on the counter block: it waits on scopes
 * whose leaves need the block for writing. */
void orchestrator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t cdb = (arts_guid_t)paramv[0];
  arts_guid_t outer = (arts_guid_t)paramv[1];
  for (int i = 0; i < K; i++) {
    arts_guid_t fe = make_scope(i, cdb, outer);
    if ((i % 2) == 0) {
      arts_event_wait(fe);
    }
  }
}

void producer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int w = (int)paramv[0];
  arts_guid_t cdb = (arts_guid_t)paramv[1];
  arts_guid_t outer = (arts_guid_t)paramv[2];
  for (int j = 0; j < SCOPES; j++) {
    (void)make_scope(K + w * SCOPES + j, cdb, outer);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  int *c = NULL;
  arts_guid_t cdb = arts_db_create((void **)&c, sizeof(int) * N_SLOTS, ARTS_DB,
                                   ARTS_DB_PROP_NONE, NULL);
  for (int i = 0; i < N_SLOTS; i++) {
    c[i] = 0;
  }
  arts_db_release(cdb, DB_MODE_RW);

  arts_event_hint_t Fh = ARTS_EVENT_HINT_FINISH;
  arts_guid_t F = arts_event_create(&Fh);

  arts_edt_hint_t oh = ARTS_EDT_HINT_DEFAULTS;
  oh.finish_event = F;
  uint64_t op[2] = {(uint64_t)cdb, (uint64_t)F};
  (void)arts_edt_create(orchestrator, 2, op, 0, &oh);

  for (int w = 0; w < WAVES; w++) {
    arts_edt_hint_t ph = ARTS_EDT_HINT_DEFAULTS;
    ph.finish_event = F;
    uint64_t pv[3] = {(uint64_t)w, (uint64_t)cdb, (uint64_t)F};
    (void)arts_edt_create(producer, 3, pv, 0, &ph);
  }

  arts_edt_hint_t ch = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t chk = arts_edt_create(checker, 0, NULL, 2, &ch);
  arts_add_dependence(F, chk, 0, DB_MODE_NULL);
  arts_add_dependence(cdb, chk, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
