/* SPDX-License-Identifier: Apache-2.0
 *
 * cover_gate_step — the destroy's gate and its two steps
 * (arts/coherence/cover_gate.h): each step is one RMW of the gate word, and
 * its pre-image decides whether the step is the one that leaves the gate
 * marked with the pre-claim write request settled (the teardown edge).
 *
 * Each step runs from every word where it is legal and is checked against
 * the transition the gate defines: the word it leaves and the edge.  A
 * duplicate MARK leaves the word as it found it and reports no edge, so a
 * second destroy can never tear the home down twice.  Every lifetime (the
 * gate seeded with the request or without it, the destroy and the arrival in
 * either order, a duplicate destroy after them) reaches the edge exactly
 * once.  An arrival the gate does not await is fatal.
 *
 * Whitebox unit test: header-only, no runtime.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/coherence/cover_gate.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "arts/system/shutdown.h"

unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(rc ? rc : 1);
}

#define TD ARTS_COVER_GATE_TD
#define RWO ARTS_COVER_GATE_RWO

static int g_fails;

static bool step(struct arts_db_s *db, uint32_t *word,
                 enum arts_cover_gate_op op) {
  atomic_store_explicit(&db->cover_gate, *word, memory_order_relaxed);
  bool edge = arts_cover_gate_step(db, op);
  *word = atomic_load_explicit(&db->cover_gate, memory_order_relaxed);
  return edge;
}

static void check(struct arts_db_s *db, uint32_t cur,
                  enum arts_cover_gate_op op, uint32_t want, bool want_edge) {
  uint32_t got = cur;
  bool edge = step(db, &got, op);
  if (got != want || edge != want_edge) {
    printf("FAIL cover_gate_step: op %d from 0x%x: word 0x%x (want 0x%x) "
           "edge %d (want %d)\n",
           (int)op, cur, got, want, (int)edge, (int)want_edge);
    g_fails++;
  }
}

/* seed, then ops[0..n): the number of steps that reported the edge. */
static unsigned int lifetime(struct arts_db_s *db, uint32_t seed,
                             const enum arts_cover_gate_op *ops,
                             unsigned int n) {
  uint32_t word = seed;
  unsigned int edges = 0;
  for (unsigned int i = 0; i < n; i++) {
    edges += step(db, &word, ops[i]) ? 1u : 0u;
  }
  return edges;
}

/* Whether an arrival on `word` ends the process with a failure status. */
static bool arrival_is_fatal(struct arts_db_s *db, uint32_t word) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    (void)step(db, &word, ARTS_COVER_GATE_RWO_ARRIVE);
    _exit(0);
  }
  int st = 0;
  if (pid < 0 || waitpid(pid, &st, 0) != pid) {
    return false;
  }
  return WIFEXITED(st) && WEXITSTATUS(st) != 0;
}

int main(void) {
  struct arts_db_s *db = (struct arts_db_s *)calloc(1, sizeof(*db));
  if (db == NULL) {
    printf("FAIL cover_gate_step: no descriptor\n");
    return 1;
  }
  check(db, 0u, ARTS_COVER_GATE_MARK, TD, true);
  check(db, RWO, ARTS_COVER_GATE_MARK, TD | RWO, false);
  check(db, TD, ARTS_COVER_GATE_MARK, TD, false);
  check(db, TD | RWO, ARTS_COVER_GATE_MARK, TD | RWO, false);
  check(db, RWO, ARTS_COVER_GATE_RWO_ARRIVE, 0u, false);
  check(db, TD | RWO, ARTS_COVER_GATE_RWO_ARRIVE, TD, true);

  const enum arts_cover_gate_op mark_only[] = {ARTS_COVER_GATE_MARK,
                                               ARTS_COVER_GATE_MARK};
  const enum arts_cover_gate_op mark_first[] = {
      ARTS_COVER_GATE_MARK, ARTS_COVER_GATE_RWO_ARRIVE, ARTS_COVER_GATE_MARK};
  const enum arts_cover_gate_op arrival_first[] = {
      ARTS_COVER_GATE_RWO_ARRIVE, ARTS_COVER_GATE_MARK, ARTS_COVER_GATE_MARK};
  if (lifetime(db, 0u, mark_only, 2u) != 1u ||
      lifetime(db, RWO, mark_first, 3u) != 1u ||
      lifetime(db, RWO, arrival_first, 3u) != 1u) {
    printf("FAIL cover_gate_step: a lifetime did not reach the edge exactly "
           "once\n");
    g_fails++;
  }

  if (!arrival_is_fatal(db, 0u) || !arrival_is_fatal(db, TD)) {
    printf("FAIL cover_gate_step: an arrival the gate does not await was not "
           "fatal\n");
    g_fails++;
  }
  free(db);
  if (g_fails != 0) {
    printf("FAIL cover_gate_step: %d mismatch(es)\n", g_fails);
    return 1;
  }
  printf("PASS cover_gate_step\n");
  return 0;
}
