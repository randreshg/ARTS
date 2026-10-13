/* SPDX-License-Identifier: Apache-2.0
 *
 * coherence_rwro_phase — RW -> RO -> RW phase alternation with ordered reads,
 * beside an ordered chain of writers that migrates the write right around the
 * ranks.
 *
 * Phases, on one counter block: in phase k ONE writer RW-acquires the block
 * and increments it to k+1; then M readers RO-acquire it and ASSERT they
 * observe exactly k+1; then the next phase's writer RW-acquires it again.  The
 * phases are wired with strict happens-before so the assertions are
 * deterministic (no data race):
 *
 *   writer[k] --output_event ew[k]--> readers[k] (RO, xM) --finish lk[k]-->
 *   writer[k+1]
 *
 * - writer[k] -> readers[k] via ew[k]: an EDT's output event fires only after
 *   it released its blocks, so every reader sees the committed k+1.
 * - readers[k] -> writer[k+1] via the finish event lk[k]: each reader leaves
 *   the scope at completion, after its RO release, so writer[k+1] RW-acquires
 *   only once every reader has given its read turn back.
 *
 * Chain, on a second counter block: writer i runs on rank i % nranks and is
 * gated on writer i-1's output event, so each writer acquires the block only
 * after the previous one released it and a plain increment is ordered.  With
 * more writers than ranks the write right leaves every rank and comes back to
 * it, so each rank in turn holds a copy that a later owner's release must
 * retire; the final reader on rank 0, gated on the last writer's output event,
 * is such an ex-holder and must read the full count.
 *
 * Portable (arts.h + arts_rt): it asserts coherence alone, so every protocol
 * owes the same answers, and its write acquisitions are all ordered.
 */

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#define N_PHASES 6
#define N_READERS 4
#define N_CHAIN_WRITERS 12 /* > nranks, so the block migrates several times */

/* Writer: DB (RW) is the LAST dep slot (slot 0 for phase 0, slot 1 otherwise —
 * the earlier slot is the prior phase's finish gate, NULL/control).  Increment
 * is HB-ordered after the prior phase, so a plain ++ is correct. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  uint64_t *counter = (uint64_t *)depv[depc - 1u].ptr;
  if (counter != NULL) {
    *counter += 1u;
  }
}

/* Chain writer: depv[0] = the chain's counter (RW); depv[1], when present, =
 * the previous writer's output event. */
static void chain_writer_edt(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *counter = (uint64_t *)depv[0].ptr;
  if (counter != NULL) {
    *counter += 1u;
  }
}

/* Reader: depv[0] = writer's output_event (NULL/control), depv[1] = counter DB
 * (RO).  Must observe exactly the post-write value paramv[0] (= phase+1). */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t want = paramv[0];
  const uint64_t *counter = (const uint64_t *)depv[1].ptr;
  uint64_t got = (counter != NULL) ? *counter : ~want;
  if (got != want) {
    (void)fprintf(stderr,
                  "FAIL: coherence_rwro_phase reader saw %llu (want %llu)\n",
                  (unsigned long long)got, (unsigned long long)want);
    arts_abort(1);
  }
}

/* Final, on rank 0: depv[0] = the last phase's finish event, depv[1] = the
 * phase counter (RO), depv[2] = the last chain writer's output event,
 * depv[3] = the chain counter (RO).  Every writer ran exactly once, in order.
 */
static void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *counter = (const uint64_t *)depv[1].ptr;
  uint64_t got = (counter != NULL) ? *counter : 0u;
  if (got != (uint64_t)N_PHASES) {
    (void)fprintf(stderr,
                  "FAIL: coherence_rwro_phase final counter=%llu "
                  "(want %d)\n",
                  (unsigned long long)got, N_PHASES);
    arts_abort(1);
  }
  const uint64_t *chain = (const uint64_t *)depv[3].ptr;
  uint64_t chained = (chain != NULL) ? *chain : 0u;
  if (chained != (uint64_t)N_CHAIN_WRITERS) {
    (void)fprintf(stderr,
                  "FAIL: coherence_rwro_phase chain counter=%llu (want %d)\n",
                  (unsigned long long)chained, N_CHAIN_WRITERS);
    arts_abort(1);
  }
  arts_printf("coherence_rwro_phase: %d phases × %d readers, counter=%llu, "
              "%d-writer chain=%llu — PASS\n",
              N_PHASES, N_READERS, (unsigned long long)got, N_CHAIN_WRITERS,
              (unsigned long long)chained);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== coherence_rwro_phase (%d phases, %d readers) ===\n",
              N_PHASES, N_READERS);

  unsigned int nranks = arts_get_total_ranks();

  void *cp = NULL;
  arts_guid_t counter =
      arts_db_create(&cp, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0u});
  if (counter == NULL_GUID || cp == NULL) {
    (void)fprintf(stderr, "FAIL: counter DB create\n");
    arts_abort(1);
  }
  *(uint64_t *)cp = 0u;
  arts_db_release(counter, DB_MODE_RW);

  /* Per-phase writer output_event (ew) + reader-batch finish event (lk). */
  arts_guid_t ew[N_PHASES];
  arts_guid_t lk[N_PHASES];
  for (unsigned int k = 0; k < N_PHASES; k++) {
    ew[k] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    lk[k] = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  }

  for (unsigned int k = 0; k < N_PHASES; k++) {
    /* writer[k] on rank k%nranks; output_event=ew[k]; gated on lk[k-1] (k>0).
     */
    unsigned int wr = (nranks > 1u) ? (k % nranks) : 0u;
    uint32_t wdepc = (k == 0u) ? 1u : 2u;
    arts_guid_t w =
        arts_edt_create(writer_edt, 0, NULL, wdepc,
                        &(arts_edt_hint_t){.rank = wr, .output_event = ew[k]});
    if (k > 0u) {
      arts_add_dependence(lk[k - 1u], w, 0, DB_MODE_NULL);
      arts_add_dependence(counter, w, 1, DB_MODE_RW);
    } else {
      arts_add_dependence(counter, w, 0, DB_MODE_RW);
    }

    /* M readers, gated on ew[k], joined to finish event lk[k]; assert == k+1.
     */
    uint64_t rp[1] = {(uint64_t)(k + 1u)};
    for (unsigned int m = 0; m < N_READERS; m++) {
      unsigned int rr = (nranks > 1u) ? ((k + m + 1u) % nranks) : 0u;
      arts_guid_t rd = arts_edt_create(
          reader_edt, 1, rp, 2,
          &(arts_edt_hint_t){.rank = rr, .finish_event = lk[k]});
      arts_add_dependence(ew[k], rd, 0, DB_MODE_NULL);
      arts_add_dependence(counter, rd, 1, DB_MODE_RO);
    }
  }

  void *chp = NULL;
  arts_guid_t chain =
      arts_db_create(&chp, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0u});
  if (chain == NULL_GUID || chp == NULL) {
    (void)fprintf(stderr, "FAIL: chain counter DB create\n");
    arts_abort(1);
  }
  *(uint64_t *)chp = 0u;
  arts_db_release(chain, DB_MODE_RW);

  /* One output event per chain writer: it fires after that writer's release
   * has published its increment. */
  arts_guid_t oe[N_CHAIN_WRITERS];
  for (unsigned int i = 0; i < N_CHAIN_WRITERS; i++) {
    oe[i] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  }
  for (unsigned int i = 0; i < N_CHAIN_WRITERS; i++) {
    unsigned int r = (nranks > 1u) ? (i % nranks) : 0u;
    arts_guid_t w =
        arts_edt_create(chain_writer_edt, 0, NULL, (i == 0u) ? 1u : 2u,
                        &(arts_edt_hint_t){.rank = r, .output_event = oe[i]});
    arts_add_dependence(chain, w, 0, DB_MODE_RW);
    if (i > 0u) {
      arts_add_dependence(oe[i - 1u], w, 1, DB_MODE_NULL);
    }
  }

  arts_guid_t v =
      arts_edt_create(verify_edt, 0, NULL, 4, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(lk[N_PHASES - 1], v, 0, DB_MODE_NULL);
  arts_add_dependence(counter, v, 1, DB_MODE_RO);
  arts_add_dependence(oe[N_CHAIN_WRITERS - 1], v, 2, DB_MODE_NULL);
  arts_add_dependence(chain, v, 3, DB_MODE_RO);
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
