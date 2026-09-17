/*
 * serial_rounds.c -- round-structured reader/writer probe over one data
 * block, forced by an event chain into rounds of [1 writer -> (n-1)
 * readers].  The writer rank rotates 0..n-1 across rounds = n *
 * rounds_per_rank, and each round's readers cover every non-writer rank
 * exactly once.  A round cannot open until the previous round's readers
 * have all joined its gate, so every write finds n-1 standing copies to
 * invalidate and no protocol is free to reorder or overlap acquires across
 * rounds -- which is what makes a chain of rounds a convergence control:
 * every coherence arm pays the same forced sequence.
 *
 * args: rounds_per_rank e_us bytes   (defaults 160 50 65536)
 * output: SERIAL_ROUNDS OK PDS=.. ROUNDS=.. E_US=.. BYTES=.. ELAPSED_US=..
 */

#include "ocr.h"
#include "extensions/ocr-affinity.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static inline u64 guid_u64(ocrGuid_t g) { return (u64)g.guid; }
static inline ocrGuid_t u64_guid(u64 v) {
  ocrGuid_t g;
  g.guid = (intptr_t)v;
  return g;
}

static inline u64 now_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (u64)tv.tv_sec * 1000000ull + (u64)tv.tv_usec;
}

static void spin_us(u64 us) {
  if (us == 0) return;
  u64 t0 = now_us();
  while (now_us() - t0 < us)
    ;
}

/* paramv = {role, e_us, bytes, idx}; depv[0]=data block, depv[1]=the event
 * that opens this op (satisfied once the predecessor op the round forces
 * has run) */
ocrGuid_t task_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (paramv[0]) {
    u64 *p = (u64 *)depv[0].ptr;
    u64 words = paramv[2] / sizeof(u64);
    p[0] = paramv[3];
    if (words > 1) p[words - 1] = paramv[3];
  } else {
    const volatile u64 *p = (const volatile u64 *)depv[0].ptr;
    u64 words = paramv[2] / sizeof(u64);
    u64 sum = 0;
    for (u64 i = 0; i < words; i++) sum += p[i];
    if (sum == 0xDEADBEEFDEADBEEFull) PRINTF("");
  }
  spin_us(paramv[1]);
  return NULL_GUID;
}

/* joins a round's readers; pure control */
ocrGuid_t gate_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  return NULL_GUID;
}

/* paramv = {db, start_evt, rounds, e_us, bytes, done_evt}; depv[0]=timing
 * block (RW) */
ocrGuid_t spawner_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  ocrGuid_t db = u64_guid(paramv[0]);
  ocrGuid_t start_evt = u64_guid(paramv[1]);
  u64 rounds = paramv[2];
  u64 e_us = paramv[3];
  u64 bytes = paramv[4];

  u64 n = 0;
  ocrAffinityCount(AFFINITY_PD, &n);
  ocrGuid_t task_tpl, gate_tpl;
  ocrEdtTemplateCreate(&task_tpl, task_edt, 4, 2);
  ocrEdtTemplateCreate(&gate_tpl, gate_edt, 0, EDT_PARAM_UNK);

  ocrGuid_t prev = start_evt; /* fires to begin round 0 */
  for (u64 r = 0; r < rounds; r++) {
    u64 wnode = r % n;
    ocrGuid_t round_head = prev; /* event that opens this round */

    ocrHint_t wh;
    ocrHintInit(&wh, OCR_HINT_EDT_T);
    ocrGuid_t waff;
    ocrAffinityGetAt(AFFINITY_PD, wnode, &waff);
    ocrSetHintValue(&wh, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(waff));
    u64 wparams[4] = {1, e_us, bytes, r};
    ocrGuid_t w, writer_out;
    ocrEdtCreate(&w, task_tpl, 4, wparams, 2, NULL, EDT_PROP_NONE, &wh,
                 &writer_out);
    ocrAddDependence(db, w, 0, DB_MODE_RW);
    ocrAddDependence(round_head, w, 1, DB_MODE_NULL);

    /* readers open on the writer's release; the round is not closed for the
     * next writer until every reader has joined this gate */
    u32 nreaders = (u32)(n - 1);
    ocrGuid_t gate;
    ocrEdtCreate(&gate, gate_tpl, 0, NULL, nreaders ? nreaders : 1, NULL,
                 EDT_PROP_NONE, NULL_HINT, &prev);
    if (!nreaders) {
      /* one rank: no reader exists to join the gate, so it follows the
       * writer directly and the round still closes */
      ocrAddDependence(writer_out, gate, 0, DB_MODE_NULL);
    }
    u32 slot = 0;
    for (u64 p = 0; p < n; p++) {
      if (p == wnode) continue; /* the writer's own rank has no reader this round */
      if (slot == nreaders) break;
      ocrHint_t rh;
      ocrHintInit(&rh, OCR_HINT_EDT_T);
      ocrGuid_t raff;
      ocrAffinityGetAt(AFFINITY_PD, p, &raff);
      ocrSetHintValue(&rh, OCR_HINT_EDT_AFFINITY, ocrAffinityToHintValue(raff));
      u64 rparams[4] = {0, e_us, bytes, r};
      ocrGuid_t rd, rd_out;
      ocrEdtCreate(&rd, task_tpl, 4, rparams, 2, NULL, EDT_PROP_NONE, &rh,
                   &rd_out);
      ocrAddDependence(db, rd, 0, DB_MODE_RO);
      ocrAddDependence(writer_out, rd, 1, DB_MODE_NULL);
      ocrAddDependence(rd_out, gate, slot, DB_MODE_NULL);
      slot++;
    }
  }

  /* stamp the span's start on the spawning rank's own clock before handing
   * the chain's completion to the sink */
  u64 *tp = (u64 *)depv[0].ptr;
  *tp = now_us();
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(prev, u64_guid(paramv[5]), 0, DB_MODE_NULL);
  ocrEventSatisfy(start_evt, NULL_GUID);
  return NULL_GUID;
}

/* paramv = {rounds, e_us, bytes}; depv[0]=chain done, depv[1]=timing */
ocrGuid_t sink_edt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 t1 = now_us();
  const u64 *tp = (const u64 *)depv[1].ptr;
  u64 n = 0;
  ocrAffinityCount(AFFINITY_PD, &n);
  PRINTF(
      "SERIAL_ROUNDS OK PDS=%lu ROUNDS=%lu E_US=%lu BYTES=%lu ELAPSED_US=%lu\n",
      (unsigned long)n, (unsigned long)paramv[0], (unsigned long)paramv[1],
      (unsigned long)paramv[2], (unsigned long)(t1 - tp[0]));
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 rounds_per_rank = 160, e_us = 50, bytes = 65536;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) rounds_per_rank = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) e_us = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) bytes = (u64)atol(getArgv(depv[0].ptr, 3));
  if (bytes < 2 * sizeof(u64)) bytes = 2 * sizeof(u64);

  u64 n = 0;
  ocrAffinityCount(AFFINITY_PD, &n);
  u64 rounds = n * rounds_per_rank;

  ocrGuid_t pd0_aff;
  ocrAffinityGetAt(AFFINITY_PD, 0, &pd0_aff);
  ocrHint_t pd0_edt_hint;
  ocrHintInit(&pd0_edt_hint, OCR_HINT_EDT_T);
  ocrSetHintValue(&pd0_edt_hint, OCR_HINT_EDT_AFFINITY,
                  ocrAffinityToHintValue(pd0_aff));
  ocrHint_t pd0_db_hint;
  ocrHintInit(&pd0_db_hint, OCR_HINT_DB_T);
  ocrSetHintValue(&pd0_db_hint, OCR_HINT_DB_AFFINITY,
                  ocrAffinityToHintValue(pd0_aff));

  ocrGuid_t db;
  u64 *dbp;
  ocrDbCreate(&db, (void **)&dbp, bytes, DB_PROP_NONE, &pd0_db_hint, NO_ALLOC);
  for (u64 i = 0; i < bytes / sizeof(u64); i++) dbp[i] = i;
  ocrDbRelease(db);

  ocrGuid_t tdb;
  u64 *tdbp;
  ocrDbCreate(&tdb, (void **)&tdbp, sizeof(u64), DB_PROP_NONE, &pd0_db_hint,
              NO_ALLOC);
  *tdbp = 0;
  ocrDbRelease(tdb);

  /* ONCE events: the OCR spec destroys each once its (single) consumer has
   * fired, so neither is destroyed here */
  ocrGuid_t start_evt, done_evt;
  ocrEventCreate(&start_evt, OCR_EVENT_ONCE_T, EVT_PROP_NONE);
  ocrEventCreate(&done_evt, OCR_EVENT_ONCE_T, EVT_PROP_NONE);

  ocrGuid_t sink_tpl, sink;
  ocrEdtTemplateCreate(&sink_tpl, sink_edt, 3, 2);
  u64 sink_params[3] = {rounds, e_us, bytes};
  ocrEdtCreate(&sink, sink_tpl, 3, sink_params, 2, NULL, EDT_PROP_NONE,
               &pd0_edt_hint, NULL);
  ocrAddDependence(done_evt, sink, 0, DB_MODE_NULL);
  ocrAddDependence(tdb, sink, 1, DB_MODE_RO);

  ocrGuid_t sp_tpl, spawner;
  ocrEdtTemplateCreate(&sp_tpl, spawner_edt, 6, 1);
  u64 sp_params[6] = {guid_u64(db),  guid_u64(start_evt), rounds,
                      e_us,          bytes,               guid_u64(done_evt)};
  ocrEdtCreate(&spawner, sp_tpl, 6, sp_params, 1, NULL, EDT_PROP_NONE,
               &pd0_edt_hint, NULL);
  ocrAddDependence(tdb, spawner, 0, DB_MODE_RW);
  return NULL_GUID;
}
