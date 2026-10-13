/* SPDX-License-Identifier: Apache-2.0
 *
 * What the flush arm owes a program whose write acquisitions of a block are
 * ordered: every remote acquire fetches the home's line and every remote write
 * release writes back into it, so a value written by one ordered writer is
 * what the next acquirer sees, wherever each of them runs.
 *
 * Cases, each on its own block, run side by side under one finish scope:
 *   - chain: a long strictly ordered chain of remote write acquisitions, each
 *     reading the counter and writing it back one higher; a write-back that is
 *     dropped, applied twice or applied out of turn shows in the total;
 *   - volume: many blocks created on one rank and homed on every rank, each
 *     driven by a short ordered chain of writers that visits every rank, all
 *     chains in flight together; each block's total is checked;
 *   - creator away from the home: the create's hold is a write acquisition
 *     whose release must reach the home, and the creating rank's next write
 *     acquisition must fetch the home's value, not reuse what the create left;
 *   - private copy: a read landing on the writer's rank while the writer still
 *     holds the block must not disturb the writer's bytes (reads are
 *     unconstrained by the model, so the overlap is a legal program);
 *   - racing readers: an ordered chain of writers on one rank that is not the
 *     home, and beside each link a reader on another rank that is not the
 *     home either (the writer's own rank when there are only two), unordered
 *     with that link, so its fetch races the link's write-back.  What a racing
 *     read returns the model leaves unspecified, so the readers check nothing;
 *     every one of them must be served, and the chain's total must come out
 *     exact;
 *   - zero size: a block with no bytes resolves both access modes to a NULL
 *     pointer and lets the EDTs run;
 *   - size discovery: a label carries no size, so the first fetch on a rank is
 *     answered with the size alone and asked again with a landing, in both
 *     modes, and the whole block arrives and returns;
 *   - spent credit: a write-back whose landing credit is gone asks the home for
 *     one and still delivers the bytes; the credit is spent by hand from inside
 *     the write acquisition, through the block's descriptor (whitebox).
 * The last four need a rank that is not the block's home.
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"
#include "arts.h"
#include "arts/coherence/types.h"
#include "arts/gas/route_table.h"
#include "arts/utils/shared.h"

static void fail_value(const char *what, uint64_t got, uint64_t want) {
  arts_test_fail();
  arts_printf("FAIL: wrf_flush %s got=%llu expected=%llu\n", what,
              (unsigned long long)got, (unsigned long long)want);
}

static arts_guid_t make_block(uint64_t bytes, unsigned int home,
                              uint64_t seed) {
  void *addr = NULL;
  arts_guid_t db = arts_db_create(&addr, bytes, ARTS_DB, ARTS_DB_PROP_NONE,
                                  &(arts_db_hint_t){.rank = home});
  if (bytes >= sizeof(uint64_t)) {
    *(uint64_t *)addr = seed;
  }
  arts_db_release(db, DB_MODE_RW);
  return db;
}

/* depv[0] = the block (RO); paramv = { expected value, case number }. */
static const char *const g_case[] = {
    "chain",        "creator away from the home",
    "private copy", "spent credit",
    "volume",       "racing readers",
};
static void expect_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *v = (const uint64_t *)depv[0].ptr;
  uint64_t got = v ? *v : (uint64_t)-1;
  if (got != paramv[0]) {
    fail_value(g_case[paramv[1]], got, paramv[0]);
  }
}

static arts_guid_t expect_after(arts_guid_t db, arts_guid_t gate, uint64_t want,
                                uint64_t which, arts_guid_t scope) {
  uint64_t pv[2] = {want, which};
  arts_guid_t e =
      arts_edt_create(expect_edt, 2, pv, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(db, e, 0, DB_MODE_RO);
  arts_add_dependence(gate, e, 1, DB_MODE_NULL);
  return e;
}

/* --- chain ------------------------------------------------------------- */

#define N_LINKS 200

static void link_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush chain link got NULL\n");
    return;
  }
  *v = *v + 1u;
}

static void case_chain(unsigned int away, arts_guid_t scope) {
  arts_guid_t db = make_block(sizeof(uint64_t), 0, 0);
  arts_guid_t start = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t done[N_LINKS];
  for (int i = 0; i < N_LINKS; i++) {
    done[i] = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  }
  (void)expect_after(db, done[N_LINKS - 1], N_LINKS, 0, scope);
  /* Back to front: the event a link waits on is created by the link made
   * after it, so every consumer exists before its producer can fire. */
  for (int i = N_LINKS - 1; i >= 0; i--) {
    arts_guid_t e = arts_edt_create(
        link_edt, 0, NULL, 2,
        &(arts_edt_hint_t){
            .rank = away, .finish_event = scope, .output_event = done[i]});
    arts_add_dependence(db, e, 0, DB_MODE_RW);
    arts_add_dependence((i == 0) ? start : done[i - 1], e, 1, DB_MODE_NULL);
  }
  arts_event_satisfy(start, NULL_GUID);
}

/* --- racing readers ------------------------------------------------------ */

#define N_RACE_LINKS 64

/* Its value is unspecified (the read races a write), so it is only read. */
static void race_reader_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const volatile uint64_t *v = (const volatile uint64_t *)depv[0].ptr;
  if (v != NULL) {
    (void)*v;
  }
}

static void case_racing_readers(unsigned int ranks, arts_guid_t scope) {
  unsigned int writer = 1u;
  unsigned int reader = (ranks > 2u) ? 2u : 1u;
  arts_guid_t db = make_block(sizeof(uint64_t), 0, 0);
  arts_guid_t start = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t done[N_RACE_LINKS];
  for (int i = 0; i < N_RACE_LINKS; i++) {
    done[i] = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  }
  (void)expect_after(db, done[N_RACE_LINKS - 1], N_RACE_LINKS, 5, scope);
  for (int i = N_RACE_LINKS - 1; i >= 0; i--) {
    arts_guid_t gate = (i == 0) ? start : done[i - 1];
    arts_guid_t e = arts_edt_create(
        link_edt, 0, NULL, 2,
        &(arts_edt_hint_t){
            .rank = writer, .finish_event = scope, .output_event = done[i]});
    arts_add_dependence(db, e, 0, DB_MODE_RW);
    arts_add_dependence(gate, e, 1, DB_MODE_NULL);
    arts_guid_t r = arts_edt_create(
        race_reader_edt, 0, NULL, 2,
        &(arts_edt_hint_t){.rank = reader, .finish_event = scope});
    arts_add_dependence(db, r, 0, DB_MODE_RO);
    arts_add_dependence(gate, r, 1, DB_MODE_NULL);
  }
  arts_event_satisfy(start, NULL_GUID);
}

/* --- volume ------------------------------------------------------------- */

#define N_VOLUME_BLOCKS 48
#define VOLUME_WRITERS 4

static void case_volume(unsigned int ranks, arts_guid_t scope) {
  for (unsigned int b = 0; b < N_VOLUME_BLOCKS; b++) {
    arts_guid_t db = make_block(sizeof(uint64_t), b % ranks, 0);
    arts_guid_t start = arts_event_create(&ARTS_EVENT_HINT_ONCE);
    arts_guid_t done[VOLUME_WRITERS];
    for (unsigned int j = 0; j < VOLUME_WRITERS; j++) {
      done[j] = arts_event_create(&ARTS_EVENT_HINT_ONCE);
    }
    (void)expect_after(db, done[VOLUME_WRITERS - 1], VOLUME_WRITERS, 4, scope);
    for (int j = VOLUME_WRITERS - 1; j >= 0; j--) {
      arts_guid_t e = arts_edt_create(
          link_edt, 0, NULL, 2,
          &(arts_edt_hint_t){.rank = (b + (unsigned int)j + 1u) % ranks,
                             .finish_event = scope,
                             .output_event = done[j]});
      arts_add_dependence(db, e, 0, DB_MODE_RW);
      arts_add_dependence((j == 0) ? start : done[j - 1], e, 1, DB_MODE_NULL);
    }
    arts_event_satisfy(start, NULL_GUID);
  }
}

/* --- creator away from the home ------------------------------------------ */

#define SEED 7u
#define AFTER_HOME_WRITE 8u
#define AFTER_REWRITE 9u

/* paramv = { value it must find, value it leaves }. */
static void step_writer_edt(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush step writer got NULL\n");
    return;
  }
  if (*v != paramv[0]) {
    fail_value("creator away from the home: a writer", *v, paramv[0]);
    return;
  }
  *v = paramv[1];
}

static void case_creator_away(unsigned int home, arts_guid_t scope) {
  arts_guid_t db = make_block(sizeof(uint64_t), home, SEED);
  arts_guid_t after_home = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t after_read = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t after_rewrite = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  (void)expect_after(db, after_rewrite, AFTER_REWRITE, 1, scope);

  /* A second write acquisition on the creating rank: a copy kept from the
   * create would read the seed. */
  uint64_t rpv[2] = {AFTER_HOME_WRITE, AFTER_REWRITE};
  arts_guid_t rew = arts_edt_create(
      step_writer_edt, 2, rpv, 2,
      &(arts_edt_hint_t){
          .rank = 0, .finish_event = scope, .output_event = after_rewrite});
  arts_add_dependence(db, rew, 0, DB_MODE_RW);
  arts_add_dependence(after_read, rew, 1, DB_MODE_NULL);

  uint64_t epv[2] = {AFTER_HOME_WRITE, 1};
  arts_guid_t rdr = arts_edt_create(
      expect_edt, 2, epv, 2,
      &(arts_edt_hint_t){
          .rank = 0, .finish_event = scope, .output_event = after_read});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(after_home, rdr, 1, DB_MODE_NULL);

  uint64_t hpv[2] = {SEED, AFTER_HOME_WRITE};
  arts_guid_t hw = arts_edt_create(
      step_writer_edt, 2, hpv, 1,
      &(arts_edt_hint_t){
          .rank = home, .finish_event = scope, .output_event = after_home});
  arts_add_dependence(db, hw, 0, DB_MODE_RW);
}

/* --- private copy -------------------------------------------------------- */

#define PRIVATE_SEED 0x1111u
#define PRIVATE_WRITTEN 0xABCDu
#define OVERLAP_NS 50000000L /* 50 ms */

static void spin_ns(long ns) {
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  do {
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while ((now.tv_sec - t0.tv_sec) * 1000000000L + (now.tv_nsec - t0.tv_nsec) <
           ns);
}

/* paramv = { go event }.  It writes first, so a landing that arrives during
 * the spin can only destroy the written value, never race ahead of it; the
 * acquisition stays open across the spin, inside which the reader's payload
 * lands. */
static void overlap_writer_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t go = (arts_guid_t)paramv[0];
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush private copy writer got NULL\n");
    arts_event_satisfy(go, NULL_GUID);
    return;
  }
  *v = PRIVATE_WRITTEN;
  arts_event_satisfy(go, NULL_GUID);
  spin_ns(OVERLAP_NS);
}

/* Only its timing matters: its request leaves while the writer holds the
 * block, so a line shared with the writer would be written under its feet. */
static void overlap_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const volatile uint64_t *v = (const volatile uint64_t *)depv[0].ptr;
  if (v != NULL) {
    (void)*v;
  }
}

static void case_private_copy(unsigned int away, arts_guid_t scope) {
  arts_guid_t db = make_block(sizeof(uint64_t), 0, PRIVATE_SEED);
  arts_guid_t go = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  (void)expect_after(db, written, PRIVATE_WRITTEN, 2, scope);

  arts_guid_t rdr =
      arts_edt_create(overlap_reader_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = away, .finish_event = scope});
  arts_add_dependence(db, rdr, 0, DB_MODE_RO);
  arts_add_dependence(go, rdr, 1, DB_MODE_NULL);

  uint64_t pv[1] = {(uint64_t)go};
  arts_guid_t wtr = arts_edt_create(
      overlap_writer_edt, 1, pv, 1,
      &(arts_edt_hint_t){
          .rank = away, .finish_event = scope, .output_event = written});
  arts_add_dependence(db, wtr, 0, DB_MODE_RW);
}

/* --- zero size ----------------------------------------------------------- */

static void zero_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr != NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush zero-size acquire returned storage\n");
  }
}

static void case_zero_size(unsigned int away, arts_guid_t scope) {
  arts_guid_t db = make_block(0, 0, 0);
  arts_guid_t after_ro = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  /* Chained by an output event, so the write acquisition is a turn of its
   * own. */
  arts_guid_t w =
      arts_edt_create(zero_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = away, .finish_event = scope});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
  arts_add_dependence(after_ro, w, 1, DB_MODE_NULL);
  arts_guid_t r = arts_edt_create(zero_edt, 0, NULL, 1,
                                  &(arts_edt_hint_t){.rank = away,
                                                     .finish_event = scope,
                                                     .output_event = after_ro});
  arts_add_dependence(db, r, 0, DB_MODE_RO);
}

/* --- size discovery ------------------------------------------------------ */

/* Not a power of two, so a landing sized from anything but the answer shows
 * as a wrong byte index. */
#define DISCOVERY_BYTES 6000u

static uint8_t first_byte(unsigned int i) { return (uint8_t)(i * 7u + 1u); }
static uint8_t second_byte(unsigned int i) { return (uint8_t)(i * 13u + 5u); }

static bool holds(const uint8_t *buf, uint8_t (*want)(unsigned int),
                  const char *who) {
  if (buf == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush size discovery %s got NULL storage\n", who);
    return false;
  }
  for (unsigned int i = 0; i < DISCOVERY_BYTES; i++) {
    if (buf[i] != want(i)) {
      arts_test_fail();
      arts_printf("FAIL: wrf_flush size discovery %s byte %u got=%u "
                  "expected=%u\n",
                  who, i, (unsigned)buf[i], (unsigned)want(i));
      return false;
    }
  }
  return true;
}

static void discovery_reader_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)holds((const uint8_t *)depv[0].ptr,
              paramv[0] ? second_byte : first_byte,
              paramv[0] ? "final reader" : "reader");
}

static void discovery_writer_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint8_t *buf = (uint8_t *)depv[0].ptr;
  if (!holds(buf, first_byte, "writer")) {
    return;
  }
  for (unsigned int i = 0; i < DISCOVERY_BYTES; i++) {
    buf[i] = second_byte(i);
  }
}

static void case_size_discovery(arts_guid_t scope) {
  unsigned int ranks = arts_get_total_ranks();
  unsigned int reader_rank = 1u;
  unsigned int writer_rank = (ranks > 2) ? 2u : reader_rank;

  /* Reserved at this rank, so the create is local and the block's home is
   * the rank that fills it. */
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());
  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  hint.guid = label;
  void *addr = NULL;
  arts_db_create(&addr, DISCOVERY_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &hint);
  if (addr == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush labeled create handed no storage\n");
    return;
  }
  for (unsigned int i = 0; i < DISCOVERY_BYTES; i++) {
    ((uint8_t *)addr)[i] = first_byte(i);
  }
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t read_done = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  arts_guid_t write_done = arts_event_create(&ARTS_EVENT_HINT_ONCE);

  uint64_t final_pv[1] = {1};
  arts_guid_t fin =
      arts_edt_create(discovery_reader_edt, 1, final_pv, 2,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(label, fin, 0, DB_MODE_RO);
  arts_add_dependence(write_done, fin, 1, DB_MODE_NULL);

  arts_guid_t wtr =
      arts_edt_create(discovery_writer_edt, 0, NULL, 2,
                      &(arts_edt_hint_t){.rank = writer_rank,
                                         .finish_event = scope,
                                         .output_event = write_done});
  arts_add_dependence(label, wtr, 0, DB_MODE_RW);
  arts_add_dependence(read_done, wtr, 1, DB_MODE_NULL);

  uint64_t first_pv[1] = {0};
  arts_guid_t rdr =
      arts_edt_create(discovery_reader_edt, 1, first_pv, 1,
                      &(arts_edt_hint_t){.rank = reader_rank,
                                         .finish_event = scope,
                                         .output_event = read_done});
  arts_add_dependence(label, rdr, 0, DB_MODE_RO);
}

/* --- spent credit -------------------------------------------------------- */

#define CREDIT_SEED 0x1234u
#define CREDIT_WRITTEN 0x5A5Au

/* paramv = { db guid } — the body runs on a rank whose process image never
 * saw the creating EDT's locals. */
static void credit_writer_edt(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t db = (arts_guid_t)paramv[0];
  uint64_t *v = (uint64_t *)depv[0].ptr;
  if (v == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush spent-credit writer got NULL\n");
    return;
  }
  *v = CREDIT_WRITTEN;

  arts_shared_ptr_t h = arts_route_table_lookup_db(db);
  struct arts_db_s *d = (struct arts_db_s *)arts_shared_get(h);
  if (d == NULL) {
    arts_test_fail();
    arts_printf("FAIL: wrf_flush spent-credit writer found no descriptor\n");
  } else {
    __atomic_store_n(&d->cache.flush_txid, (uint64_t)0, __ATOMIC_RELEASE);
  }
  arts_shared_release(&h);
}

static void case_spent_credit(unsigned int away, arts_guid_t scope) {
  arts_guid_t db = make_block(sizeof(uint64_t), 0, CREDIT_SEED);
  arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_ONCE);
  (void)expect_after(db, written, CREDIT_WRITTEN, 3, scope);
  uint64_t pv[1] = {(uint64_t)db};
  arts_guid_t wtr = arts_edt_create(
      credit_writer_edt, 1, pv, 1,
      &(arts_edt_hint_t){
          .rank = away, .finish_event = scope, .output_event = written});
  arts_add_dependence(db, wtr, 0, DB_MODE_RW);
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: wrf_flush\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  unsigned int away = (ranks > 1) ? 1u : 0u;

  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(scope, done, 0, DB_MODE_NULL);

  case_chain(away, scope);
  case_volume(ranks, scope);
  case_creator_away(away, scope);
  case_private_copy(away, scope);
  if (ranks > 1) {
    case_zero_size(away, scope);
    case_size_discovery(scope);
    case_spent_credit(away, scope);
    case_racing_readers(ranks, scope);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
