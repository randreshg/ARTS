/*
 * dbcreate_matrix.c
 *
 * DB-create capability matrix: exercises all 8 logical cells of the
 * (creator-acquisition A) x (home-determination B) x (locality C) grid
 * through the portable OCR API (ocr.h), so the SAME source compiles and
 * runs against every OCR-API-consuming backend (this repo builds it against
 * xsocr, ocr-vx, and ARTS-via-ocr-shim from one CMake wiring).  Prints a
 * scalar CELLS_OK=<k>/8.
 *
 * Grid:
 *   A: ACQUIRE (default hold) | NO_ACQUIRE (DB_PROP_NO_ACQUIRE)
 *   B: (i) affinity hint  (ii) no-hint (round robin)
 *   C: local (home == creator) | remote (home != creator)
 *
 *     1 ACQ/affinity/local     5 NOACQ/affinity/local
 *     2 ACQ/affinity/remote    6 NOACQ/affinity/remote
 *     3 ACQ/no-hint (a)        7 NOACQ/no-hint (a)
 *     4 ACQ/no-hint (b)        8 NOACQ/no-hint (b)
 *
 * Each cell is driven once from PD 0 and (when the affinity count allows)
 * once from PD 1, for coverage; a cell's pass bit requires every instance
 * that ran to pass.  No-hint cells have no caller-selectable target home,
 * so only data correctness is asserted for them; affinity cells assert both
 * data correctness AND, where ocrAffinityQuery is available, that the DB's
 * queried affinity equals the intended target.
 *
 * Ordering note: a NO_ACQUIRE cell's writer EDT (RW dependence) must itself
 * create and wire the consumer EDT (RO dependence) *after* writing the
 * payload -- wiring the consumer independently, from the same body that
 * wired the writer, would race the writer's own acquire (both dependences on
 * a freshly-created, still-idle DB can resolve at roughly the same time;
 * only program-order sequencing inside one EDT body guarantees the
 * hand-off). ACQUIRE cells avoid this entirely: the driver itself holds the
 * write from creation, writes, releases, and only then wires the consumer --
 * pure program order within one function.
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"

#include <stdbool.h>
#include <stdlib.h>

#define NUM_CELLS 8
#define PAYLOAD_COUNT 4
#define PAYLOAD_BYTES (PAYLOAD_COUNT * sizeof(u64))
#define HOME_SKIP ((u64)0xFFFFFFFFu)

typedef enum { AXIS_AFFINITY, AXIS_NOHINT } axis_t;

static inline u64 guid_u64(ocrGuid_t g) { return (u64)g.guid; }
static inline ocrGuid_t u64_guid(u64 v) {
  ocrGuid_t g;
  g.guid = (intptr_t)v;
  return g;
}

/* ---- consumer_edt: paramv={cellIdx, wantHome, createOk}; depv[0]=target DB
 * (RO), depv[1]=results DB (RW, u8[NUM_CELLS]).  createOk is 1 unless a
 * NO_ACQUIRE cell's create call violated the OCR NO_ACQUIRE contract (addr
 * must come back NULL) -- see run_cell. -----------------------------------
 */
ocrGuid_t consumer_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 cell_idx = paramv[0];
  u64 want_home = paramv[1];
  u64 create_ok = paramv[2];

  const u64 *data = (const u64 *)depv[0].ptr;
  bool data_ok = (data != NULL);
  for (u32 i = 0; data_ok && i < PAYLOAD_COUNT; i++) {
    data_ok = (data[i] == cell_idx * 1000 + i);
  }

  bool home_ok = true;
  if (want_home != HOME_SKIP) {
    ocrGuid_t got;
    u64 cnt = 1;
    ocrAffinityQuery(depv[0].guid, &cnt, &got);
    ocrGuid_t want;
    ocrAffinityGetAt(AFFINITY_PD, want_home, &want);
    home_ok = ocrGuidIsEq(got, want);
  }

  bool ok = data_ok && home_ok && (create_ok != 0);
  u8 *results = (u8 *)depv[1].ptr;
  results[cell_idx - 1] = results[cell_idx - 1] && (ok ? 1 : 0);

  ocrPrintf("  cell %2llu: %s (data=%s home=%s create=%s)\n",
            (unsigned long long)cell_idx, ok ? "PASS" : "FAIL",
            data_ok ? "ok" : "BAD",
            (want_home == HOME_SKIP) ? "n/a" : (home_ok ? "ok" : "BAD"),
            create_ok ? "ok" : "BAD");
  return NULL_GUID;
}

/* Wires the RO-consumer(s) that validate a cell's DB.
 *
 *  - AXIS_AFFINITY: one policy-placed consumer that ALSO checks the DB's
 *    queried affinity (ocrAffinityQuery) == home_idx.  Non-tautological for
 *    affinity because the RUNTIME minted the GUID at the home it chose from
 *    the hint, so the query round-trips through actual placement.
 *  - AXIS_NOHINT: one policy-placed consumer, no home assert.
 */
static void wire_consumers(ocrGuid_t db, u64 cell_idx, axis_t axis,
                           u64 home_idx, u64 create_ok,
                           ocrGuid_t results_db, ocrGuid_t gate_event) {
  /* gate_event (NULL_GUID = none): for NO_ACQUIRE cells the writer's payload
   * is published only when the writer EDT finishes and its RW hold is
   * released (writeback).  A bare DB->consumer dep satisfies immediately, so
   * the consumer's RO acquire would race ahead of that writeback and read the
   * home's v1 zero placeholder -- an UNDEFINED RO/RW overlap per the OCR
   * model, not a coherence bug.  The gate event (the writer's OUTPUT EVENT,
   * satisfied strictly AFTER the writer's DBs are released) is added as an
   * extra consumer pre-slot so the consumer becomes runnable -- and thus does
   * its RO acquire -- only happens-after the writeback.  ACQUIRE cells pass
   * NULL_GUID: they are already release-before-wire in run_cell. */
  bool gated = !ocrGuidIsNull(gate_event);
  u32 depc = gated ? 3 : 2;
  ocrGuid_t ctmpl;
  ocrEdtTemplateCreate(&ctmpl, consumer_edt, 3, depc);
  u64 want_home = (axis == AXIS_AFFINITY) ? home_idx : HOME_SKIP;
  u64 cparam[3] = {cell_idx, want_home, create_ok};
  ocrGuid_t consumer;
  ocrEdtCreate(&consumer, ctmpl, EDT_PARAM_DEF, cparam, EDT_PARAM_DEF, NULL,
               EDT_PROP_NONE, NULL_HINT, NULL);
  ocrAddDependence(db, consumer, 0, DB_MODE_RO);
  ocrAddDependence(results_db, consumer, 1, DB_MODE_RW);
  if (gated) {
    ocrAddDependence(gate_event, consumer, 2, DB_MODE_NULL);
  }
}

/* ---- writer_edt (NO_ACQUIRE cells only): paramv={cellIdx}; depv[0]=target DB
 * (RW). Writes the payload and returns; OCR satisfies this EDT's output event
 * (its post-slot, see run_cell) after the EDT completes and its DBs are
 * released, so a consumer gated on that event is happens-after the writeback.
 */
ocrGuid_t writer_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 cell_idx = paramv[0];
  u64 *p = (u64 *)depv[0].ptr;
  for (u32 i = 0; i < PAYLOAD_COUNT; i++) {
    p[i] = cell_idx * 1000 + i;
  }
  return NULL_GUID;
}

/* Runs one grid cell from the calling (driver) EDT's body. */
static void run_cell(u64 cell_idx, axis_t axis, u64 home_idx, bool no_acquire,
                     ocrGuid_t results_db) {
  ocrHint_t db_hint;
  ocrHint_t *hintp = NULL_HINT;
  ocrGuid_t db = UNINITIALIZED_GUID;
  u16 flags = no_acquire ? DB_PROP_NO_ACQUIRE : DB_PROP_NONE;

  if (axis == AXIS_AFFINITY) {
    ocrHintInit(&db_hint, OCR_HINT_DB_T);
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, home_idx, &aff);
    ocrSetHintValue(&db_hint, OCR_HINT_DB_AFFINITY, ocrAffinityToHintValue(aff));
    hintp = &db_hint;
  } /* AXIS_NOHINT: hintp stays NULL_HINT -> backend's no-hint (RR) policy. */

  void *addr = NULL;
  ocrDbCreate(&db, &addr, PAYLOAD_BYTES, flags, hintp, NO_ALLOC);

  if (!no_acquire) {
    u64 *p = (u64 *)addr;
    for (u32 i = 0; i < PAYLOAD_COUNT; i++) {
      p[i] = cell_idx * 1000 + i;
    }
    ocrDbRelease(db);
    wire_consumers(db, cell_idx, axis, home_idx, 1, results_db, NULL_GUID);
  } else {
    /* OCR contract: DB_PROP_NO_ACQUIRE must come back with addr==NULL (the
     * creator does not hold). A backend that silently drops the flag hands
     * back a non-NULL pointer instead -- catch that here rather than
     * relying on it to also corrupt the payload
     * (a mis-acquired creator hold still gets released, and released,
     * before the real writer's RW acquire proceeds, so the data can end up
     * correct despite the contract violation). */
    u64 create_ok = (addr == NULL) ? 1 : 0;
    ocrGuid_t wtmpl;
    ocrEdtTemplateCreate(&wtmpl, writer_edt, 1, 1);
    u64 wparam[1] = {cell_idx};
    /* Writer's output event (post-slot): OCR satisfies it after the writer
     * completes and its RW hold is released (writeback published).  The
     * consumer(s) gate on it so their RO read is happens-after the write.
     * The post-slot event is a single-fire (auto-destroy) event, so the
     * consumer(s) MUST be wired to it BEFORE the writer can become runnable
     * and fire+destroy it -- hence wire_consumers precedes the writer's
     * DB-dep satisfy below (which is what makes the writer runnable). */
    ocrGuid_t writer, writer_done;
    ocrEdtCreate(&writer, wtmpl, EDT_PARAM_DEF, wparam, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, NULL_HINT, &writer_done);
    wire_consumers(db, cell_idx, axis, home_idx, create_ok, results_db,
                   writer_done);
    ocrAddDependence(db, writer, 0, DB_MODE_RW);
  }
}

/* ---- driver_edt: paramv={driverPd, resultsDB}.
 * Runs all 8 cells for one creator PD. -------------------------------------
 */
ocrGuid_t driver_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  u64 d = paramv[0];
  ocrGuid_t results_db = u64_guid(paramv[1]);
  u64 n = 0;
  ocrAffinityCount(AFFINITY_PD, &n);
  u64 remote = (d + 1) % n;

  run_cell(1, AXIS_AFFINITY, d, false, results_db);
  run_cell(2, AXIS_AFFINITY, remote, false, results_db);
  run_cell(3, AXIS_NOHINT, HOME_SKIP, false, results_db);
  run_cell(4, AXIS_NOHINT, HOME_SKIP, false, results_db);
  run_cell(5, AXIS_AFFINITY, d, true, results_db);
  run_cell(6, AXIS_AFFINITY, remote, true, results_db);
  run_cell(7, AXIS_NOHINT, HOME_SKIP, true, results_db);
  run_cell(8, AXIS_NOHINT, HOME_SKIP, true, results_db);
  return NULL_GUID;
}

/* ---- cellrun_edt (FINISH): paramv={resultsDB, n}; depv[0]=control slot,
 * satisfied last, once everything that must exist before the drivers run is
 * wired.  Spawns driver PD0 (and PD1, for coverage, when n>1); its
 * outputEvent fires once every driver and all of its transitive descendant
 * EDTs (writers/consumers) have completed. ---------------------------------
 */
ocrGuid_t cellrun_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  ocrGuid_t results_db = u64_guid(paramv[0]);
  u64 n = paramv[1];

  ocrGuid_t dtmpl;
  ocrEdtTemplateCreate(&dtmpl, driver_edt, 2, 0);

  u64 num_drivers = (n > 1) ? 2u : 1u;
  for (u64 d = 0; d < num_drivers; d++) {
    ocrHint_t h;
    ocrHintInit(&h, OCR_HINT_EDT_T);
    ocrGuid_t aff;
    ocrAffinityGetAt(AFFINITY_PD, d, &aff);
    ocrSetHintValue(&h, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(aff));

    u64 dp[2] = {d, guid_u64(results_db)};
    ocrGuid_t driver;
    ocrEdtCreate(&driver, dtmpl, EDT_PARAM_DEF, dp, EDT_PARAM_DEF, NULL,
                 EDT_PROP_NONE, &h, NULL);
  }
  return NULL_GUID;
}

/* ---- reduce_edt: depv[0]=cellrun's finish control, depv[1]=results DB
 * (RO). --------------------------------------------------------------- */
static void launch_sweep(u64 rounds_left);

/* paramv={rounds_left}. Sequential sweep repetitions (argv[1], default 1):
 * each round re-runs the full 8-cell matrix with fresh DBs; only the last
 * round reports and shuts down (turns the probe into a create-path stress).
 * The round count chains through paramv -- EDTs may run on any PD. */
ocrGuid_t reduce_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (paramv[0] > 1) {
    launch_sweep(paramv[0] - 1);
    return NULL_GUID;
  }
  const u8 *results = (const u8 *)depv[1].ptr;
  u32 k = 0;
  for (u32 i = 0; i < NUM_CELLS; i++) {
    k += results[i] ? 1 : 0;
  }
  ocrPrintf("CELLS_OK=%u/8\n", k);
  ocrShutdown();
  return NULL_GUID;
}

static void launch_sweep(u64 rounds_left) {
  u64 n = 0;
  ocrAffinityCount(AFFINITY_PD, &n);
  ocrPrintf("=== dbcreate_matrix (%llu PDs) ===\n", (unsigned long long)n);

  ocrGuid_t results_db;
  void *raddr = NULL;
  ocrDbCreate(&results_db, &raddr, NUM_CELLS, DB_PROP_NONE, NULL_HINT,
              NO_ALLOC);
  u8 *results = (u8 *)raddr;
  for (u32 i = 0; i < NUM_CELLS; i++) {
    results[i] = 1;
  }
  ocrDbRelease(results_db);

  ocrGuid_t crtmpl;
  ocrEdtTemplateCreate(&crtmpl, cellrun_edt, 2, 1);
  u64 crparam[2] = {guid_u64(results_db), n};
  ocrGuid_t cellrun, cells_done;
  ocrEdtCreate(&cellrun, crtmpl, EDT_PARAM_DEF, crparam, EDT_PARAM_DEF, NULL,
               EDT_PROP_FINISH, NULL_HINT, &cells_done);

  ocrGuid_t rdtmpl;
  ocrEdtTemplateCreate(&rdtmpl, reduce_edt, 1, 2);
  ocrGuid_t reduce;
  ocrEdtCreate(&reduce, rdtmpl, EDT_PARAM_DEF, &rounds_left, EDT_PARAM_DEF, NULL,
               EDT_PROP_NONE, NULL_HINT, NULL);
  ocrAddDependence(cells_done, reduce, 0, DB_MODE_NULL);
  ocrAddDependence(results_db, reduce, 1, DB_MODE_RO);

  /* A FINISH EDT's output event must have its waiter wired before the EDT can
   * complete, so the control slot is satisfied only after reduce is wired. */
  ocrAddDependence(NULL_GUID, cellrun, 0, DB_MODE_NULL);
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;

  u64 rounds = 1;
  if (ocrGetArgc(depv[0].ptr) > 1) {
    u64 r = strtoull(ocrGetArgv(depv[0].ptr, 1), NULL, 10);
    if (r >= 1) {
      rounds = r;
    }
  }
  launch_sweep(rounds);
  return NULL_GUID;
}
