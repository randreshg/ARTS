/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* edt_spread_depth — a creation with no placement preference takes the node's
 * rotation while its creator's depth is below nohint_edt_spread_depth and the
 * creating rank otherwise; every creation, whatever its hint, is one level.
 *
 * The argument is the bound the run was given ("unbounded" or a number).  A
 * judged creation is N EDTs (N = the rank count) that one creator makes with
 * no placement preference, even-numbered with a NULL hint and odd-numbered
 * with ARTS_HINT_ANY_RANK, gated on an event the creator satisfies after the
 * last create, so its N draws of the rotation are consecutive.  With the
 * creator at depth d, the N EDTs must run on N distinct ranks when d is below
 * the bound and all on the creator's rank otherwise.
 *
 * Every other EDT names its rank, so none moves a rotation.  Each judged EDT
 * creates a report EDT on rank 0 that marks its rank in the creation's set;
 * a judge on rank 0, gated on the latch that counts every report of the
 * phase, compares the set with the expectation.  A phase's latch fires
 * whether or not the phase passed, so a failure ends the run by status.
 *
 * The creation tree (depths follow from the construction):
 *   main (0): judged creation A (level 1); drivers B0, C0, G0 (level 1)
 *   A (1):  each makes judged creation A' (level 2)
 *   B0 (1): creates B1 with ARTS_HINT_CURRENT_RANK (level 2), which makes
 *           judged creation B (level 3)
 *   C0 (1): created with an explicit rank on another rank, makes judged
 *           creation C (level 2)
 *   G0 (1): created with a GUID reserved on another rank, makes judged
 *           creation G (level 2)
 *   L0 (1): starts a chain of ARTS_HINT_CURRENT_RANK EDTs whose last link, at
 *           depth 64, makes judged creation L (level 65): the default bound
 *           is above every depth
 *   P1, Q, P2: a local creation does not move the rotation.  P1 (1) makes
 *           judged creation R1 (level 2) and creates Q (2), whose one
 *           no-hint EDT is local when the bound is 2 and must draw nothing;
 *           P2 (1), after both, makes judged creation R2.  At bound 2 each
 *           index of R2 runs where the same index of R1 ran, since the
 *           rotation advanced by exactly N between them.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../test_failure_status.h"
#include "arts.h"

enum { PH_A, PH_A2, PH_B, PH_C, PH_G, PH_L, PH_R1, PH_R2, PHASES };
#define JUDGES (PHASES - 1u)
#define LEAF_DEPTH 64u

#define MAX_RANKS 16u
#define MAX_CIDS (8u + MAX_RANKS)

/* Rank 0 only: its report and judge EDTs run there. */
static arts_guid_t g_latch[PHASES];
static arts_guid_t g_done;
static _Atomic(uint64_t) g_set[MAX_CIDS];
static _Atomic(uint64_t) g_creators[MAX_CIDS];
/* The rank each index of R1 and of R2 ran on, plus the slot of Q's EDT. */
static _Atomic(unsigned int) g_rank_of[2][MAX_RANKS + 1u];

static void fail(const char *what, uint64_t a, uint64_t b) {
  arts_printf("FAIL: edt_spread_depth: %s (%llu, %llu)\n", what,
              (unsigned long long)a, (unsigned long long)b);
  arts_test_fail();
}

static bool spread(uint64_t creator_depth, uint64_t bound) {
  return creator_depth < bound;
}

/* A report: the judged EDT of creation cid ran on rank `rank`, its creator on
 * rank `creator`. */
static void report(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t cid = paramv[0];
  atomic_fetch_or_explicit(&g_set[cid], 1ull << paramv[1],
                           memory_order_relaxed);
  atomic_fetch_or_explicit(&g_creators[cid], 1ull << paramv[2],
                           memory_order_relaxed);
  if (paramv[3] == PH_R1 || paramv[3] == PH_R2) {
    atomic_store_explicit(&g_rank_of[paramv[3] - PH_R1][paramv[4]],
                          (unsigned int)paramv[1], memory_order_relaxed);
  }
  arts_event_satisfy_slot(g_latch[paramv[3]], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

static void judged_create(uint64_t cid, uint64_t phase, uint64_t creator_depth,
                          uint64_t bound);

/* One of a judged creation's N EDTs: [cid, creator rank, expect local, phase,
 * index, bound]. */
static void judged(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int rank = arts_get_current_rank();
  if (paramv[2] && rank != paramv[1]) {
    fail("a creation that must stay local ran on another rank", paramv[0],
         rank);
  }
  uint64_t rp[5] = {paramv[0], rank, paramv[1], paramv[3], paramv[4]};
  (void)arts_edt_create(report, 5, rp, 0, &(arts_edt_hint_t){.rank = 0u});
  if (paramv[3] == PH_A) {
    judged_create(1u + paramv[4], PH_A2, 1u, paramv[5]);
  }
}

static void judged_create(uint64_t cid, uint64_t phase, uint64_t creator_depth,
                          uint64_t bound) {
  unsigned int n = arts_get_total_ranks();
  arts_guid_t go = arts_event_create(NULL);
  arts_guid_t e[MAX_RANKS];
  for (unsigned int k = 0; k < n; k++) {
    uint64_t p[6] = {cid,
                     arts_get_current_rank(),
                     !spread(creator_depth, bound),
                     phase,
                     k,
                     bound};
    if (k % 2u == 0u) {
      e[k] = arts_edt_create(judged, 6, p, 1, NULL);
    } else {
      arts_edt_hint_t h = {.rank = ARTS_HINT_ANY_RANK};
      e[k] = arts_edt_create(judged, 6, p, 1, &h);
    }
  }
  for (unsigned int k = 0; k < n; k++) {
    arts_add_dependence(go, e[k], 0, DB_MODE_NULL);
  }
  arts_event_satisfy(go, NULL_GUID);
}

/* Judge of one phase: [first cid, cid count, creator depth, bound, kind]; kind
 * 1 is the rotation judge, whose subject is the bound 2 alone. */
static void judge(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  if (paramv[4] == 1u) {
    if (paramv[3] == 2u) {
      for (unsigned int k = 0; k < n; k++) {
        unsigned int a =
            atomic_load_explicit(&g_rank_of[0][k], memory_order_relaxed);
        unsigned int b =
            atomic_load_explicit(&g_rank_of[1][k], memory_order_relaxed);
        if (a != b) {
          fail("a local creation moved the rotation", a, b);
        }
      }
    }
    arts_event_satisfy_slot(g_done, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
    return;
  }
  uint64_t full = n >= 64u ? ~0ull : (1ull << n) - 1ull;
  bool want_spread = spread(paramv[2], paramv[3]);
  for (uint64_t c = paramv[0]; c < paramv[0] + paramv[1]; c++) {
    uint64_t set = atomic_load_explicit(&g_set[c], memory_order_relaxed);
    uint64_t cr = atomic_load_explicit(&g_creators[c], memory_order_relaxed);
    if (cr == 0u || (cr & (cr - 1u)) != 0u) {
      fail("a creation has not exactly one creator rank", c, cr);
    } else if (want_spread && set != full) {
      fail("a creation below the bound did not spread over every rank", c,
           set);
    } else if (!want_spread && set != cr) {
      fail("a creation at the bound did not stay on its creator's rank", c,
           set);
    }
  }
  arts_event_satisfy_slot(g_done, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

/* B1: the level below B0, created with ARTS_HINT_CURRENT_RANK. */
static void b1(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  judged_create(1u + arts_get_total_ranks(), PH_B, 2u, paramv[0]);
}

static void b0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (arts_get_current_rank() != 0u) {
    fail("B0 ran off its creator's rank", arts_get_current_rank(), 0u);
  }
  (void)arts_edt_create(b1, 1, paramv, 0,
                        &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK});
}

/* C0 and G0: [bound, rank they must run on]. */
static void c0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (arts_get_current_rank() != paramv[1]) {
    fail("C0 ran off its explicit rank", arts_get_current_rank(), paramv[1]);
  }
  judged_create(2u + arts_get_total_ranks(), PH_C, 1u, paramv[0]);
}

static void g0(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (arts_get_current_rank() != paramv[1]) {
    fail("G0 ran off its reserved GUID's rank", arts_get_current_rank(),
         paramv[1]);
  }
  judged_create(3u + arts_get_total_ranks(), PH_G, 1u, paramv[0]);
}

/* L0 and its links: [bound, own depth]; the link at LEAF_DEPTH judges. */
static void chain(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (paramv[1] < LEAF_DEPTH) {
    uint64_t p[2] = {paramv[0], paramv[1] + 1u};
    (void)arts_edt_create(chain, 2, p, 0,
                          &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK});
  } else {
    judged_create(4u + arts_get_total_ranks(), PH_L, LEAF_DEPTH, paramv[0]);
  }
}

/* The no-hint EDT of Q: local at bound 2, so it draws nothing. */
static void z(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int rank = arts_get_current_rank();
  if (paramv[0] == 2u && rank != 0u) {
    fail("Q's local creation ran off its creator's rank", rank, 0u);
  }
  uint64_t rp[5] = {6u + arts_get_total_ranks(), rank, 0u, PH_R1,
                    arts_get_total_ranks()};
  (void)arts_edt_create(report, 5, rp, 0, &(arts_edt_hint_t){.rank = 0u});
}

static void q(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t go = arts_event_create(NULL);
  arts_guid_t e = arts_edt_create(z, 1, paramv, 1, NULL);
  arts_add_dependence(go, e, 0, DB_MODE_NULL);
  arts_event_satisfy(go, NULL_GUID);
}

static void p1(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  judged_create(5u + arts_get_total_ranks(), PH_R1, 1u, paramv[0]);
  (void)arts_edt_create(q, 1, paramv, 0,
                        &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK});
}

static void p2(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  judged_create(7u + arts_get_total_ranks(), PH_R2, 1u, paramv[0]);
}

static void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_spread_depth\n");
  }
  arts_shutdown();
}

static arts_guid_t latch_on_rank0(uint32_t count) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH((int32_t)count);
  h.rank = 0u;
  return arts_event_create(&h);
}

static arts_guid_t gated(arts_edt_t f, uint32_t paramc, const uint64_t *p,
                         const arts_edt_hint_t *hint, arts_guid_t gate) {
  arts_guid_t g = arts_edt_create(f, paramc, p, 1, hint);
  arts_add_dependence(gate, g, 0, DB_MODE_NULL);
  return g;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  const char *const *argv = (const char *const *)paramv[1];
  uint64_t bound = 0;
  if (argc != 2) {
    fail("the program takes one argument", (uint64_t)argc, 0u);
    arts_shutdown();
    return;
  }
  if (strcmp(argv[1], "unbounded") == 0) {
    bound = UINT32_MAX;
  } else {
    bound = strtoull(argv[1], NULL, 10);
  }

  unsigned int n = arts_get_total_ranks();
  if (n > MAX_RANKS) {
    fail("too many ranks", n, MAX_RANKS);
    arts_shutdown();
    return;
  }
  unsigned int r1 = n > 1u ? 1u : 0u;

  g_latch[PH_A] = latch_on_rank0(n);
  g_latch[PH_A2] = latch_on_rank0(n * n);
  g_latch[PH_B] = latch_on_rank0(n);
  g_latch[PH_C] = latch_on_rank0(n);
  g_latch[PH_G] = latch_on_rank0(n);
  g_latch[PH_L] = latch_on_rank0(n);
  g_latch[PH_R1] = latch_on_rank0(n + 1u);
  g_latch[PH_R2] = latch_on_rank0(n);
  g_done = latch_on_rank0(JUDGES);

  const arts_edt_hint_t on0 = {.rank = 0u};
  const uint64_t jp[PHASES][5] = {{0u, 1u, 0u, bound, 0u},
                                  {1u, n, 1u, bound, 0u},
                                  {1u + n, 1u, 2u, bound, 0u},
                                  {2u + n, 1u, 1u, bound, 0u},
                                  {3u + n, 1u, 1u, bound, 0u},
                                  {4u + n, 1u, LEAF_DEPTH, bound, 0u},
                                  {0u, 0u, 0u, 0u, 0u},
                                  {0u, 0u, 0u, bound, 1u}};
  for (unsigned int p = 0; p < PHASES; p++) {
    if (p != PH_R1) {
      (void)gated(judge, 5, jp[p], &on0, g_latch[p]);
    }
  }

  uint64_t bp[2] = {bound, 0u};
  (void)gated(b0, 2, bp, &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK},
              g_latch[PH_A2]);
  uint64_t cp[2] = {bound, r1};
  (void)gated(c0, 2, cp, &(arts_edt_hint_t){.rank = r1}, g_latch[PH_B]);
  arts_guid_t reserved = arts_guid_reserve(ARTS_GUID_EDT, r1);
  (void)gated(g0, 2, cp, &(arts_edt_hint_t){.guid = reserved}, g_latch[PH_C]);
  uint64_t lp[2] = {bound, 1u};
  (void)gated(chain, 2, lp, &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK},
              g_latch[PH_G]);
  uint64_t rp1[1] = {bound};
  (void)gated(p1, 1, rp1, &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK},
              g_latch[PH_L]);
  (void)gated(p2, 1, rp1, &(arts_edt_hint_t){.rank = ARTS_HINT_CURRENT_RANK},
              g_latch[PH_R1]);
  (void)gated(end_edt, 0, NULL, &on0, g_done);

  judged_create(0u, PH_A, 0u, bound);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
