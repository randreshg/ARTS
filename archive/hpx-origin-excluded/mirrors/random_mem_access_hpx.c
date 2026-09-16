/* Component state is owner-affined; action synchronization stays inside it. */
#include "hpx_mirror.h"

enum { P_ARRAY, P_ELEM, P_ITER, P_SEED, P_NL, P_START, P_QUERY_TPL,
       P_INIT_TPL, P_UPDATE_TPL, P_SUMMER_TPL, P_UPDATES_TPL, P_REPORT_TPL, P_COUNT };

typedef struct { atomic_uint locked; u64 count, initial; uint32_t prefix; } element_t;

static u64 element_owner(u64 i, u64 n, u64 nl) {
  if (nl > 1 && n < nl) return n + 1 + i;
  return i / ((n + nl - 1) / nl);
}

static void element_lock(element_t *e) {
  while (atomic_exchange_explicit(&e->locked, 1, memory_order_acquire)) {}
}
static void element_unlock(element_t *e) {
  atomic_store_explicit(&e->locked, 0, memory_order_release);
}

static ocrGuid_t init_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  element_t *e = depv[0].ptr;
  element_lock(e); e->count = e->initial = pv[P_ELEM]; element_unlock(e);
  return NULL_GUID;
}

static ocrGuid_t update_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc;
  element_t *e = depv[0].ptr;
  element_lock(e); ++e->count; element_unlock(e);
  return NULL_GUID;
}

static ocrGuid_t query_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc;
  element_t *e = depv[0].ptr;
  element_lock(e); u64 value = e->count; element_unlock(e);
  ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
  ocrGuid_t result; u64 *p;
  ocrDbCreate(&result, (void **)&p, sizeof(*p), DB_PROP_NONE, &h, NO_ALLOC);
  *p = value;
  return result;
}

static ocrGuid_t summer_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 sum = 0;
  for (u32 i = 0; i < depc; ++i) sum += *(u64 *)depv[i].ptr - i;
  mirror_app_e2e(pv[P_START]);
  PRINTF("COUNT_SUM %lu\n", (unsigned long)sum);
  for (u32 i = 0; i < depc; ++i) ocrDbDestroy(depv[i].guid);
  ocrShutdown();
  return NULL_GUID;
}

static ocrGuid_t report_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrGuid_t *dbs = depv[1].ptr;
  u64 n = pv[P_ARRAY];
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t sum;
  ocrEdtCreate(&sum, mirror_u64_guid(pv[P_SUMMER_TPL]), P_COUNT, pv,
               (u32)n, NULL, EDT_PROP_NONE, &h, NULL);
  for (u64 i = 0; i < n; ++i) {
    mirror_rank_hint(&h, element_owner(i, n, pv[P_NL]), OCR_HINT_EDT_T);
    ocrGuid_t query, out;
    ocrEdtCreate(&query, mirror_u64_guid(pv[P_QUERY_TPL]), P_COUNT, pv,
                 1, NULL, EDT_PROP_NONE, &h, &out);
    ocrAddDependence(out, sum, (u32)i, DB_MODE_RO);
    ocrAddDependence(dbs[i], query, 0, DB_MODE_RW);
  }
  return NULL_GUID;
}

static ocrGuid_t updates_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrGuid_t *dbs = depv[1].ptr;
  u64 n = pv[P_ARRAY], iters = pv[P_ITER];
  ocrGuid_t done = mirror_latch(iters);
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t report;
  ocrEdtCreate(&report, mirror_u64_guid(pv[P_REPORT_TPL]), P_COUNT, pv,
               2, NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(done, report, 0, DB_MODE_NULL);
  ocrAddDependence(depv[1].guid, report, 1, DB_MODE_RO);
  mirror_mt19937_t gen; mirror_mt_seed(&gen, (uint32_t)pv[P_SEED]);
  for (u64 i = 0; i < iters; ++i) {
    u64 e = mirror_mt_bounded(&gen, (uint32_t)n);
    mirror_rank_hint(&h, element_owner(e, n, pv[P_NL]), OCR_HINT_EDT_T);
    ocrGuid_t update, out;
    ocrEdtCreate(&update, mirror_u64_guid(pv[P_UPDATE_TPL]), P_COUNT, pv,
                 1, NULL, EDT_PROP_NONE, &h, &out);
    ocrAddDependence(out, done, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(dbs[e], update, 0, DB_MODE_RW);
  }
  if (!iters) ocrEventSatisfySlot(done, NULL_GUID, OCR_EVENT_LATCH_DECR_SLOT);
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb), n = 8, iters = 16, seed = 0;
  int seeded = mirror_option_u64(argdb, argc, "--seed", &seed);
  int bad = mirror_option_u64(argdb, argc, "--array-size", &n) < 0
         || mirror_option_u64(argdb, argc, "--iterations", &iters) < 0 || seeded < 0;
  if (bad || n > (u64)INT32_MAX + 1) {
    PRINTF("random_mem_access_hpx: usage --array-size=N --iterations=I --seed=S (N <= 2^31)\n");
    ocrShutdown();
    return NULL_GUID;
  }
  if (seeded == 0) seed = mirror_draw_seed();
  u64 nl; ocrAffinityCount(AFFINITY_PD, &nl);
  u64 start = mirror_now_ns();
  u64 created = nl > 1 && n < nl ? nl - n - 1 : n;
  if (n >= nl && (nl - 1) * ((n + nl - 1) / nl) > n) {
    PRINTF("random_mem_access_hpx: origin distribution underflows its last allocation count\n");
    ocrShutdown(); return NULL_GUID;
  }
  ocrGuid_t init_tpl, update_tpl, summer_tpl, updates_tpl, report_tpl, query_tpl;
  ocrEdtTemplateCreate(&init_tpl, init_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&update_tpl, update_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&query_tpl, query_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&summer_tpl, summer_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&updates_tpl, updates_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&report_tpl, report_edt, P_COUNT, 2);

  /* The element table: one block per element, homed by the origin's layout.
   * The table itself carries no hint because it never leaves the rank that
   * creates it: its only consumer is created on that same rank. */
  ocrGuid_t table; ocrGuid_t *dbs;
  ocrDbCreate(&table, (void **)&dbs, (created ? created : 1) * sizeof(ocrGuid_t), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  for (u64 i = 0; i < created; ++i) {
    ocrHint_t dh; mirror_rank_hint(&dh, element_owner(i, n, nl), OCR_HINT_DB_T);
    element_t *e;
    ocrDbCreate(&dbs[i], (void **)&e, sizeof(element_t), DB_PROP_NONE, &dh, NO_ALLOC);
    atomic_init(&e->locked, 0); e->count = e->initial = 0; e->prefix = (uint32_t)element_owner(i, n, nl);
    ocrDbRelease(dbs[i]);
  }
  if (created < n || (!n && iters)) {
    PRINTF("random_mem_access_hpx: origin indexes beyond its created client array\n");
    ocrShutdown(); return NULL_GUID;
  }
  /* Snapshot the table's guids into plain process memory before releasing
   * it: `updates` must be created and registered on both its dependences
   * before the first init EDT can run (an OCR once-type event's consumer
   * registers before the event can fire; a latch is destroyed the instant
   * it does), so the table's own acquire cannot be held open across that
   * whole window. */
  ocrGuid_t *local_dbs = (ocrGuid_t *)malloc((n ? n : 1) * sizeof(ocrGuid_t));
  if (local_dbs == NULL) {
    PRINTF("random_mem_access_hpx: out of memory\n");
    ocrShutdown();
    return NULL_GUID;
  }
  memcpy(local_dbs, dbs, n * sizeof(ocrGuid_t));
  ocrDbRelease(table);

  ocrGuid_t inited = mirror_latch(n);
  u64 pv[P_COUNT] = {0};
  pv[P_START] = start; pv[P_QUERY_TPL] = mirror_guid_u64(query_tpl);
  pv[P_ARRAY] = n; pv[P_ITER] = iters; pv[P_SEED] = seed; pv[P_NL] = nl;
  pv[P_INIT_TPL] = mirror_guid_u64(init_tpl); pv[P_UPDATE_TPL] = mirror_guid_u64(update_tpl);
  pv[P_SUMMER_TPL] = mirror_guid_u64(summer_tpl); pv[P_UPDATES_TPL] = mirror_guid_u64(updates_tpl);
  pv[P_REPORT_TPL] = mirror_guid_u64(report_tpl);

  ocrHint_t h0; mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t updates;
  ocrEdtCreate(&updates, updates_tpl, P_COUNT, pv, 2, NULL, EDT_PROP_NONE, &h0, NULL);
  ocrAddDependence(inited, updates, 0, DB_MODE_NULL);
  ocrAddDependence(table, updates, 1, DB_MODE_RO);

  for (u64 i = 0; i < n; ++i) {
    u64 params[P_COUNT]; memcpy(params, pv, sizeof(params));
    params[P_ELEM] = i;
    ocrHint_t h; mirror_rank_hint(&h, element_owner(i, n, nl), OCR_HINT_EDT_T);
    ocrGuid_t e, out;
    ocrEdtCreate(&e, init_tpl, P_COUNT, params, 1, NULL, EDT_PROP_NONE, &h, &out);
    ocrAddDependence(out, inited, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(local_dbs[i], e, 0, DB_MODE_RW);
  }
  free(local_dbs);
  if (!n) ocrEventSatisfySlot(inited, NULL_GUID, OCR_EVENT_LATCH_DECR_SLOT);
  return NULL_GUID;
}
