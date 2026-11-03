/* SPDX-License-Identifier: Apache-2.0
 *
 * A group opened by a remote member and chained into its current group, the
 * member's local proxy, reaches the outer group across ranks.
 *
 * Structure (needs node_count >= 2):
 *   - rank 0 main_edt opens an OUTER group G (homed on rank 0).
 *   - an ORCHESTRATOR EDT is placed on rank 1 and JOINED to G.  G is remote to
 *     rank 1, so the install joins the orchestrator to a local LATCH proxy of
 *     G instead; the proxy's fire carries one DECR to G (rank 1 -> 0).
 *   - the orchestrator, running on rank 1, opens an INNER group g (homed rank
 *     1) and chains it into its current group, the proxy: it INCRs the proxy
 *     (local) and registers g -> proxy DECR.
 *   - the orchestrator spawns leaves under g (rank 1), DECRs g and returns.
 *     Its epilogue DECRs the proxy for its own membership; once g's leaves
 *     also DECR, g fires and DECRs the proxy, which then fires and DECRs G on
 *     rank 0 (the cross-rank wire).
 *   - a CHECKER on rank 0 depends on G and verifies all leaves ran (via a
 *     counter DB), then shuts down.  A lost DECR -> G never fires -> ctest
 *     TIMEOUT; a DECR that reaches G before g fired -> the checker counts
 *     fewer leaves.
 *   - each leaf waits before it writes, so that a group G that does not wait
 *     for g fires with the leaves' slots still unset.
 */
#include "arts.h"

#include <errno.h>
#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"

#define INNER_LEAVES 4
#define N_SLOTS INNER_LEAVES

#define LEAF_DELAY_NS 100000000L

/* leaf on rank 1: RW dep on counter DB, paramv[0] = slot. */
void leaf(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int *c = (int *)depv[0].ptr;
  struct timespec left = {.tv_sec = 0, .tv_nsec = LEAF_DELAY_NS};
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
  }
  if (c) {
    c[(int)paramv[0]] = 1;
  }
}

/* orchestrator on rank 1: member of remote G through its local proxy; opens
 * g, chains it into the proxy, and g's fire reaches G on rank 0 through the
 * proxy.
 */
void orchestrator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  arts_guid_t cdb = depv[0].guid;

  arts_event_hint_t ig = ARTS_EVENT_HINT_LATCH(1);
  ig.auto_destroy = true;
  arts_guid_t g = arts_event_create(&ig);
  arts_guid_t proxy = arts_current_group();
  arts_event_satisfy_slot(proxy, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
  arts_add_dependence(g, proxy, ARTS_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);

  arts_edt_hint_t lh = ARTS_EDT_HINT_DEFAULTS;
  lh.group = g;
  lh.rank = 1; /* keep leaves local to rank 1 */
  for (int i = 0; i < INNER_LEAVES; i++) {
    uint64_t slot = (uint64_t)i;
    arts_guid_t l = arts_edt_create(leaf, 1, &slot, 1, &lh);
    arts_add_dependence(cdb, l, 0, DB_MODE_RW);
  }
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

/* checker on rank 0: depends on G + RO counter DB. */
void checker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  int *c = (int *)depv[1].ptr;
  if (c == NULL) {
    arts_printf("FAIL: group_nested_chain_writes: checker NULL counter DB\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  int sum = 0;
  for (int i = 0; i < INNER_LEAVES; i++) {
    sum += c[i];
  }
  if (sum != INNER_LEAVES) {
    arts_printf("FAIL: group_nested_chain_writes: only %d/%d inner leaves ran "
                "before G fired (cross-rank chain DECR lost?)\n",
                sum, INNER_LEAVES);
    arts_test_fail();
  } else {
    arts_printf("PASS: group_nested_chain_writes: %d inner leaves done via "
                "the chained cross-rank proxy DECR before G fired\n",
                INNER_LEAVES);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  /* Counter DB homed on rank 1 so the rank-1 leaves write locally. */
  int *c = NULL;
  arts_guid_t cdb =
      arts_db_create((void **)&c, sizeof(int) * N_SLOTS, ARTS_DB,
                     ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = 1});
  for (int i = 0; i < N_SLOTS; i++) {
    c[i] = 0;
  }
  arts_db_release(cdb);

  /* Outer group G homed on rank 0 (current rank). */
  arts_event_hint_t og = ARTS_EVENT_HINT_LATCH(1);
  og.auto_destroy = true;
  arts_guid_t G = arts_event_create(&og);

  /* Orchestrator on rank 1, joined to remote G. */
  arts_edt_hint_t oh = ARTS_EDT_HINT_DEFAULTS;
  oh.rank = 1;
  oh.group = G;
  arts_guid_t o = arts_edt_create(orchestrator, 0, NULL, 1, &oh);
  arts_add_dependence(cdb, o, 0, DB_MODE_RW);

  /* Checker on rank 0 depends on G firing. */
  arts_edt_hint_t ch = ARTS_EDT_HINT_DEFAULTS;
  ch.rank = 0;
  arts_guid_t chk = arts_edt_create(checker, 0, NULL, 2, &ch);
  arts_add_dependence(G, chk, 0, DB_MODE_NULL);
  arts_add_dependence(cdb, chk, 1, DB_MODE_RO);
  arts_event_satisfy_slot(G, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
