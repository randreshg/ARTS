/// @file fam_fetch_off_progress.c
/// @brief The copy a grant pays for runs on a worker, never on the rank's
/// progress thread.
///
/// Whitebox, because the tallies are the arm's own observables and no public
/// call reports them.  A progress thread is a rank's single inbound coherence
/// processor: every message it spends time copying is a message every other
/// block's round waits behind, so "the copy happens somewhere else" is a
/// property of the arm, not an optimization.
///
/// The shape drives grants onto every rank twice over -- a write chain that
/// walks the ranks several times, then one reader per rank -- and each rank's
/// own checker then asserts both halves: its progress threads copied nothing,
/// and the rank did fetch, so a zero is not the answer of a rank that was
/// never granted anything.  Only the master rank's exit status reaches the
/// harness, so every rank prints its verdict as well.

#include "arts.h"

#include "arts/coherence/excl/types.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>

#define MAX_RANKS 16u
#define TURNS_PER_RANK 4u

/* depv[0] = the counter block (RW).  depv[1], when present, = the previous
 * writer's output event: the chain edge that makes a plain increment
 * correct. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *counter = (uint64_t *)depv[0].ptr;
  if (counter == NULL) {
    (void)fprintf(stderr, "FAIL: fam_fetch_off_progress writer got no "
                          "storage (rank %u)\n",
                  arts_get_current_rank());
    arts_test_fail();
    return;
  }
  *counter += 1u;
}

/* depv[0] = the counter block (RO).  One per rank, so every rank takes a read
 * turn of its own; the whole phase is already behind the write chain. */
static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t want = paramv[0];
  const uint64_t *counter = (const uint64_t *)depv[0].ptr;
  if (counter == NULL || *counter != want) {
    (void)fprintf(stderr,
                  "FAIL: fam_fetch_off_progress rank %u read %llu (want %llu)\n",
                  arts_get_current_rank(),
                  (unsigned long long)((counter != NULL) ? *counter : 0u),
                  (unsigned long long)want);
    arts_test_fail();
  }
}

/* One per rank, behind the scope that closes every turn this rank took. */
static void check_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int me = arts_get_current_rank();
  uint64_t all = __atomic_load_n(&arts_fam_fetches, __ATOMIC_RELAXED);
  uint64_t on_progress =
      __atomic_load_n(&arts_fam_progress_fetches, __ATOMIC_RELAXED);
  if (on_progress != 0u) {
    (void)fprintf(stderr,
                  "FAIL: fam_fetch_off_progress rank %u ran %llu of its %llu "
                  "fetches on a progress thread\n",
                  me, (unsigned long long)on_progress,
                  (unsigned long long)all);
    arts_test_fail();
    return;
  }
  if (all == 0u) {
    (void)fprintf(stderr,
                  "FAIL: fam_fetch_off_progress rank %u took no turn at all, "
                  "so its zero says nothing\n",
                  me);
    arts_test_fail();
    return;
  }
  arts_printf("fam_fetch_off_progress: rank %u fetched %llu times, none on a "
              "progress thread\n",
              me, (unsigned long long)all);
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("fam_fetch_off_progress: every rank's fetches ran on a worker "
              "— PASS\n");
  arts_shutdown();
}

/* paramv = {the block, the rank count, the writer count}.  Gated on the write
 * chain's last output event, so the readers start on a settled block. */
static void read_phase_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  unsigned int nranks = (unsigned int)paramv[1];
  uint64_t writers = paramv[2];

  arts_guid_t read_done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t rpv[1] = {writers};
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t rd = arts_edt_create(
        reader_edt, 1, rpv, 1,
        &(arts_edt_hint_t){.rank = r, .finish_event = read_done});
    arts_add_dependence(g, rd, 0, DB_MODE_RO);
  }

  /* The tallies are read behind the scope that closes every one of this
   * rank's turns, so a rank's count is final when its checker runs. */
  arts_guid_t checked = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  for (unsigned int r = 0; r < nranks; r++) {
    arts_guid_t c = arts_edt_create(
        check_edt, 0, NULL, 1,
        &(arts_edt_hint_t){.rank = r, .finish_event = checked});
    arts_add_dependence(read_done, c, 0, DB_MODE_NULL);
  }

  arts_guid_t d =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(checked, d, 0, DB_MODE_NULL);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int nranks = arts_get_total_ranks();
  if (nranks > MAX_RANKS) {
    (void)fprintf(stderr, "FAIL: fam_fetch_off_progress is built for at most "
                          "%u ranks (have %u)\n",
                  MAX_RANKS, nranks);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int writers = nranks * TURNS_PER_RANK;

  void *cp = NULL;
  arts_guid_t g =
      arts_db_create(&cp, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0u});
  if (g == NULL_GUID || cp == NULL) {
    (void)fprintf(stderr, "FAIL: fam_fetch_off_progress create\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  *(uint64_t *)cp = 0u;
  arts_db_release(g, DB_MODE_RW);

  /* Writer i on rank i % nranks, each behind its predecessor's output event:
   * the chain is strictly ordered, so it migrates the write right around the
   * ranks TURNS_PER_RANK times and every rank is granted several times. */
  arts_guid_t oe[MAX_RANKS * TURNS_PER_RANK];
  for (unsigned int i = 0; i < writers; i++) {
    oe[i] = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  }
  for (unsigned int i = 0; i < writers; i++) {
    uint32_t edepc = (i == 0u) ? 1u : 2u;
    arts_guid_t w = arts_edt_create(
        writer_edt, 0, NULL, edepc,
        &(arts_edt_hint_t){.rank = i % nranks, .output_event = oe[i]});
    arts_add_dependence(g, w, 0, DB_MODE_RW);
    if (i > 0u) {
      arts_add_dependence(oe[i - 1u], w, 1, DB_MODE_NULL);
    }
  }

  uint64_t rpv[3] = {(uint64_t)g, (uint64_t)nranks, (uint64_t)writers};
  arts_guid_t rp = arts_edt_create(read_phase_edt, 3, rpv, 1,
                                   &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(oe[writers - 1u], rp, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
