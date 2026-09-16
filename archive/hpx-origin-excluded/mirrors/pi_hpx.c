/* OCR mirror of HPX's distributed_pi: one block per rank, one reduction. */
#include "hpx_mirror.h"

enum { P_N, P_RANK, P_NL, P_REDUCE, P_COUNT };

static inline double sqr(double v) { return v * v; }

static ocrGuid_t block_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 N = pv[P_N], nl = pv[P_NL], rank = pv[P_RANK];
  u64 start = rank == 0 ? mirror_now_ns() : 0;
  u64 blocksize = N / nl, begin = blocksize * rank, end = blocksize * (rank + 1);
  double h = 1.0 / (double)N, pi = 0.0;
  for (u64 i = begin; i != end; ++i) pi += h * 4.0 / (1 + sqr((double)i * h));
  ocrHint_t dh; mirror_here_hint(&dh, OCR_HINT_DB_T);
  ocrGuid_t db; double *p;
  ocrDbCreate(&db, (void **)&p, sizeof(double), DB_PROP_NONE, &dh, NO_ALLOC);
  *p = pi;
  ocrDbRelease(db);
  if (rank == 0) {
    ocrGuid_t timing;
    u64 *stamp;
    ocrDbCreate(&timing, (void **)&stamp, sizeof(*stamp), DB_PROP_NONE, &dh, NO_ALLOC);
    *stamp = start;
    ocrDbRelease(timing);
    ocrAddDependence(timing, mirror_u64_guid(pv[P_REDUCE]), (u32)nl, DB_MODE_RO);
  }
  ocrAddDependence(db, mirror_u64_guid(pv[P_REDUCE]), (u32)rank, DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t reduce_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  double pi = 0.0;
  for (u32 i = 0; i < pv[P_NL]; ++i) pi += *(double *)depv[i].ptr;
  mirror_app_e2e(*(const u64 *)depv[pv[P_NL]].ptr);
  PRINTF("pi: %.14f\n", pi);
  /* A partial has exactly one consumer, so it is dead once summed. */
  for (u32 i = 0; i < depc; ++i) ocrDbDestroy(depv[i].guid);
  ocrShutdown();
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 N = 1000000;
  if (getArgc(depv[0].ptr) > 1) {
    char *end;
    unsigned long long x = strtoull(getArgv(depv[0].ptr, 1), &end, 10);
    if (*end != '\0' || x == 0) { PRINTF("pi_hpx: usage [N]\n"); ocrShutdown(); return NULL_GUID; }
    N = x;
  }
  u64 nl; ocrAffinityCount(AFFINITY_PD, &nl);
  ocrGuid_t block_tpl, reduce_tpl;
  ocrEdtTemplateCreate(&block_tpl, block_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&reduce_tpl, reduce_edt, P_COUNT, (u32)(nl + 1));
  ocrHint_t h0; mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t reduce;
  u64 pv[P_COUNT] = {N, 0, nl, 0};
  ocrEdtCreate(&reduce, reduce_tpl, P_COUNT, pv, (u32)(nl + 1), NULL, EDT_PROP_NONE, &h0, NULL);
  pv[P_REDUCE] = mirror_guid_u64(reduce);
  for (u64 r = 0; r < nl; ++r) {
    pv[P_RANK] = r;
    ocrHint_t h; mirror_rank_hint(&h, r, OCR_HINT_EDT_T);
    ocrGuid_t e;
    ocrEdtCreate(&e, block_tpl, P_COUNT, pv, 0, NULL, EDT_PROP_NONE, &h, NULL);
  }
  return NULL_GUID;
}
