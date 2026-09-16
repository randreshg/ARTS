#include "hpx_mirror.h"

enum {
  T_FACTORY, T_ROWS_GATHER, T_SOLVER_ALLOC, T_ITERATORS_GATHER,
  T_ROW_INIT, T_ITER_INIT, T_ITER_PUBLISH, T_BOUNDARY, T_GET_ROW, T_INSTALL,
  T_SPAWN, T_STEP, T_ROWJOIN, T_UPDATE, T_FINISH, T_SUM_START, T_SUM, T_TOTAL,
  T_COUNT
};
enum {
  P_NX, P_NY, P_ITERS, P_LB, P_NRANGES, P_NL,
  P_RANK, P_PHASE, P_IT, P_Y, P_GEN, P_LATCH, P_TOTAL, P_LEN, P_LLEN,
  P_STATE, P_TOP, P_BOTTOM, P_READY, P_READY_OTHER, P_START_NS,
  P_TPL, P_COUNT = P_TPL + T_COUNT
};

typedef struct {
  ocrGuid_t rows[2];
  ocrGuid_t blocks[];
} iterator_state_t;

static inline u64 span_begin(u64 lb, u64 r) { return 1 + r * lb; }
static inline u64 span_end(u64 nx, u64 lb, u64 r) {
  u64 e = 1 + (r + 1) * lb;
  return e < nx - 1 ? e : nx - 1;
}
static inline u64 row_slots(u64 nranges) { return nranges + 2; }
static inline u64 slot_len(u64 nx, u64 lb, u64 nranges, u64 k) {
  if (k == 0 || k == nranges + 1) return 1;
  return span_end(nx, lb, k - 1) - span_begin(lb, k - 1);
}
static inline u64 state_index(u64 nranges, u64 role, u64 gen, u64 k) {
  return (2 * role + gen) * row_slots(nranges) + k;
}
static inline u64 owner_begin(u64 ny, u64 nl, u64 rank) {
  u64 begin = rank * ((ny + nl - 1) / nl);
  return begin < ny ? begin : ny;
}
static inline u64 owner_end(u64 ny, u64 nl, u64 rank) {
  return owner_begin(ny, nl, rank + 1);
}
static ocrGuid_t task(u64 *pv, u32 which, u64 rank, u32 depc,
                      u16 properties, ocrGuid_t *out) {
  ocrHint_t h;
  mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  ocrGuid_t edt;
  ocrEdtCreate(&edt, mirror_u64_guid(pv[P_TPL + which]), P_COUNT, pv, depc,
               NULL, properties, &h, out);
  return edt;
}
static ocrGuid_t block(u64 rank, u64 size, u16 properties, void **ptr) {
  ocrHint_t h;
  mirror_rank_hint(&h, rank, OCR_HINT_DB_T);
  ocrGuid_t db;
  ocrDbCreate(&db, ptr, size, properties, &h, NO_ALLOC);
  return db;
}

static ocrGuid_t factory_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 ny = pv[P_NY], nl = pv[P_NL], rank = pv[P_RANK];
  u64 begin = owner_begin(ny, nl, rank), end = owner_end(ny, nl, rank);
  u64 stride = pv[P_PHASE] ? 3 : 1, count = stride * (end - begin);
  ocrGuid_t *names;
  ocrGuid_t result = block(rank, (count ? count : 1) * sizeof(ocrGuid_t),
                           DB_PROP_NONE, (void **)&names);
  for (u64 y = begin; y < end; ++y) {
    void *ptr;
    u64 size = pv[P_PHASE]
      ? sizeof(iterator_state_t) + 2 * row_slots(pv[P_NRANGES]) * sizeof(ocrGuid_t)
      : row_slots(pv[P_NRANGES]) * sizeof(ocrGuid_t);
    names[stride * (y - begin)] = block(rank, size, DB_PROP_NO_ACQUIRE, &ptr);
    if (pv[P_PHASE]) {
      for (u64 gen = 0; gen < 2; ++gen) {
        ocrGuid_t ready = NULL_GUID;
        if (y && y + 1 < ny)
          ocrEventCreate(&ready, OCR_EVENT_STICKY_T, EVT_PROP_TAKES_ARG);
        names[stride * (y - begin) + 1 + gen] = ready;
      }
    }
  }
  ocrDbRelease(result);
  return result;
}

static ocrGuid_t row_init_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nx = pv[P_NX], lb = pv[P_LB], nranges = pv[P_NRANGES];
  u64 owner = mirror_owner(pv[P_Y], pv[P_NY], pv[P_NL]);
  ocrGuid_t *names = depv[0].ptr;
  double value = pv[P_GEN] ? 0.0 : 1.0;
  for (u64 k = 0; k < row_slots(nranges); ++k) {
    u64 len = slot_len(nx, lb, nranges, k);
    double *values;
    names[k] = block(owner, len * sizeof(double), DB_PROP_NONE, (void **)&values);
    for (u64 j = 0; j < len; ++j) values[j] = value;
    ocrDbRelease(names[k]);
  }
  return NULL_GUID;
}

static ocrGuid_t rows_gather_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 ny = pv[P_NY], nl = pv[P_NL];
  ocrGuid_t *names;
  ocrGuid_t table = block(0, 4 * ny * sizeof(ocrGuid_t), DB_PROP_NONE, (void **)&names);
  for (u64 rank = 0; rank < nl; ++rank) {
    const ocrGuid_t *local = depv[rank].ptr;
    u64 begin = owner_begin(ny, nl, rank), end = owner_end(ny, nl, rank);
    for (u64 y = begin; y < end; ++y) names[y] = local[y - begin];
    ocrDbDestroy(depv[rank].guid);
  }
  ocrGuid_t join = mirror_latch(ny);
  ocrGuid_t next = task(pv, T_SOLVER_ALLOC, 0, 2, EDT_PROP_NONE, NULL);
  ocrAddDependence(join, next, 0, DB_MODE_NULL);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_GEN] = 0;
  for (u64 y = 0; y < ny; ++y) {
    params[P_Y] = y;
    ocrGuid_t out;
    ocrGuid_t init = task(params, T_ROW_INIT, mirror_owner(y, ny, nl), 1,
                          EDT_PROP_NONE, &out);
    ocrAddDependence(out, join, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(names[y], init, 0, DB_MODE_RW);
  }
  ocrDbRelease(table);
  ocrAddDependence(table, next, 1, DB_MODE_RW);
  return NULL_GUID;
}

static ocrGuid_t solver_alloc_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nl = pv[P_NL];
  ocrGuid_t next = task(pv, T_ITERATORS_GATHER, 0, (u32)(nl + 1), EDT_PROP_NONE, NULL);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_PHASE] = 1;
  for (u64 rank = 0; rank < nl; ++rank) {
    params[P_RANK] = rank;
    ocrGuid_t out;
    ocrGuid_t factory = task(params, T_FACTORY, rank, 1, EDT_PROP_NONE, &out);
    ocrAddDependence(out, next, (u32)(1 + rank), DB_MODE_RO);
    ocrAddDependence(NULL_GUID, factory, 0, DB_MODE_NULL);
  }
  ocrDbRelease(depv[1].guid);
  ocrAddDependence(depv[1].guid, next, 0, DB_MODE_RW);
  return NULL_GUID;
}

static ocrGuid_t iter_publish_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  iterator_state_t *state = depv[3].ptr;
  u64 slots = row_slots(pv[P_NRANGES]);
  for (u64 gen = 0; gen < 2; ++gen) {
    state->rows[gen] = depv[1 + gen].guid;
    memcpy(state->blocks + gen * slots, depv[1 + gen].ptr, slots * sizeof(ocrGuid_t));
  }
  return NULL_GUID;
}

static ocrGuid_t iter_init_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 owner = mirror_owner(pv[P_Y], pv[P_NY], pv[P_NL]);
  void *ptr;
  ocrGuid_t row = block(owner, row_slots(pv[P_NRANGES]) * sizeof(ocrGuid_t),
                        DB_PROP_NO_ACQUIRE, &ptr);
  ocrGuid_t publish = task(pv, T_ITER_PUBLISH, owner, 4, EDT_PROP_NONE, NULL);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_GEN] = 1;
  ocrGuid_t out;
  ocrGuid_t init = task(params, T_ROW_INIT, owner, 1, EDT_PROP_NONE, &out);
  ocrAddDependence(out, publish, 0, DB_MODE_NULL);
  ocrAddDependence(depv[0].guid, publish, 1, DB_MODE_RO);
  ocrAddDependence(row, publish, 2, DB_MODE_RO);
  ocrAddDependence(mirror_u64_guid(pv[P_STATE]), publish, 3, DB_MODE_RW);
  ocrAddDependence(row, init, 0, DB_MODE_RW);
  return NULL_GUID;
}

static ocrGuid_t get_row_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  const iterator_state_t *state = depv[0].ptr;
  return state->rows[pv[P_GEN]];
}

static ocrGuid_t install_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 slots = row_slots(pv[P_NRANGES]), gen = pv[P_GEN];
  const iterator_state_t *state = depv[0].ptr;
  ocrGuid_t *view;
  u64 owner = mirror_owner(pv[P_Y], pv[P_NY], pv[P_NL]);
  ocrGuid_t db = block(owner, 4 * slots * sizeof(ocrGuid_t),
                       DB_PROP_NONE, (void **)&view);
  memcpy(view, state->blocks + gen * slots, slots * sizeof(ocrGuid_t));
  memcpy(view + slots, state->blocks + (gen ^ 1) * slots, slots * sizeof(ocrGuid_t));
  memcpy(view + 2 * slots, depv[1].ptr, slots * sizeof(ocrGuid_t));
  memcpy(view + 3 * slots, depv[2].ptr, slots * sizeof(ocrGuid_t));
  ocrDbRelease(db);
  ocrEventSatisfy(mirror_u64_guid(pv[P_READY]), db);

  return NULL_GUID;
}

static ocrGuid_t boundary_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 y = pv[P_Y], ny = pv[P_NY], nl = pv[P_NL];
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  for (u64 gen = 0; gen < 2; ++gen) {
    params[P_GEN] = gen;
    params[P_READY] = pv[gen ? P_READY_OTHER : P_READY];
    ocrGuid_t install = task(params, T_INSTALL, mirror_owner(y, ny, nl), 3,
                             EDT_PROP_NONE, NULL);
    ocrAddDependence(depv[0].guid, install, 0, DB_MODE_RO);
    for (u64 side = 0; side < 2; ++side) {
      u64 neighbor = side ? y + 1 : y - 1;
      ocrGuid_t state = mirror_u64_guid(side ? pv[P_BOTTOM] : pv[P_TOP]);
      ocrGuid_t out;
      ocrGuid_t get = task(params, T_GET_ROW, mirror_owner(neighbor, ny, nl),
                           1, EDT_PROP_NONE, &out);
      ocrAddDependence(out, install, (u32)(1 + side), DB_MODE_RO);
      ocrAddDependence(state, get, 0, DB_MODE_RO);
    }
  }

  return NULL_GUID;
}

static ocrGuid_t iterators_gather_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 ny = pv[P_NY], nl = pv[P_NL];
  ocrGuid_t *names = depv[0].ptr, *states = names + ny, *ready = states + ny;
  for (u64 rank = 0; rank < nl; ++rank) {
    const ocrGuid_t *local = depv[1 + rank].ptr;
    u64 begin = owner_begin(ny, nl, rank), end = owner_end(ny, nl, rank);
    for (u64 y = begin; y < end; ++y) {
      states[y] = local[3 * (y - begin)];
      ready[2 * y] = local[3 * (y - begin) + 1];
      ready[2 * y + 1] = local[3 * (y - begin) + 2];
    }
    ocrDbDestroy(depv[1 + rank].guid);
  }
  ocrGuid_t *initializers = malloc(2 * ny * sizeof(ocrGuid_t));
  if (!initializers) {
    PRINTF("jacobi_hpx: initializer allocation failed\n");
    ocrShutdown();
    return NULL_GUID;
  }
  ocrGuid_t *initialized = initializers + ny;
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  for (u64 y = 0; y < ny; ++y) {
    params[P_Y] = y;
    params[P_STATE] = mirror_guid_u64(states[y]);
    initializers[y] = task(params, T_ITER_INIT, mirror_owner(y, ny, nl), 1,
                           EDT_PROP_FINISH, &initialized[y]);
  }
  ocrGuid_t join = mirror_latch(ny - 2);
  ocrGuid_t start = task(pv, pv[P_ITERS] ? T_SPAWN : T_FINISH, 0, 2, EDT_PROP_NONE, NULL);
  ocrAddDependence(join, start, 0, DB_MODE_NULL);
  for (u64 y = 1; y + 1 < ny; ++y) {
    params[P_Y] = y;
    params[P_TOP] = mirror_guid_u64(states[y - 1]);
    params[P_BOTTOM] = mirror_guid_u64(states[y + 1]);
    params[P_READY] = mirror_guid_u64(ready[2 * y]);
    params[P_READY_OTHER] = mirror_guid_u64(ready[2 * y + 1]);
    ocrGuid_t out;
    ocrGuid_t setup = task(params, T_BOUNDARY, mirror_owner(y, ny, nl), 4,
                           EDT_PROP_NONE, &out);
    ocrAddDependence(out, join, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(states[y], setup, 0, DB_MODE_RO);
    ocrAddDependence(initialized[y], setup, 1, DB_MODE_NULL);
    ocrAddDependence(initialized[y - 1], setup, 2, DB_MODE_NULL);
    ocrAddDependence(initialized[y + 1], setup, 3, DB_MODE_NULL);
  }
  /* All completion consumers exist before the first initializer can run. */
  for (u64 y = 0; y < ny; ++y)
    ocrAddDependence(names[y], initializers[y], 0, DB_MODE_RO);
  free(initializers);
  ocrDbRelease(depv[0].guid);
  ocrAddDependence(depv[0].guid, start, 1, DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t update_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 len = pv[P_LEN], llen = pv[P_LLEN];
  double *dst = depv[0].ptr;
  const double *left = depv[1].ptr, *mid = depv[2].ptr, *right = depv[3].ptr;
  const double *top = depv[4].ptr, *bottom = depv[5].ptr;
  for (u64 j = 0; j < len; ++j) {
    double l = j == 0 ? left[llen - 1] : mid[j - 1];
    double r = j + 1 == len ? right[0] : mid[j + 1];
    dst[j] = (l + r + top[j] + bottom[j]) * 0.25;
  }
  return NULL_GUID;
}

static ocrGuid_t rowjoin_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc; (void)depv;
  return NULL_GUID;
}

static ocrGuid_t step_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nx = pv[P_NX], ny = pv[P_NY], lb = pv[P_LB], nranges = pv[P_NRANGES];
  u64 nl = pv[P_NL], it = pv[P_IT], y = pv[P_Y], owner = mirror_owner(y, ny, nl);
  const ocrGuid_t *tab = depv[0].ptr;
  u64 slots = row_slots(nranges);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  ocrGuid_t join = mirror_latch(nranges), tail_out;
  ocrGuid_t tail = task(params, T_ROWJOIN, owner, 1, EDT_PROP_NONE, &tail_out);
  ocrAddDependence(tail_out, mirror_u64_guid(pv[P_LATCH]), OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  ocrAddDependence(join, tail, 0, DB_MODE_NULL);
  for (u64 r = 0; r < nranges; ++r) {
    params[P_LEN] = slot_len(nx, lb, nranges, r + 1);
    params[P_LLEN] = slot_len(nx, lb, nranges, r);
    ocrGuid_t out;
    ocrGuid_t u = task(params, T_UPDATE, owner, 6, EDT_PROP_NONE, &out);
    ocrAddDependence(out, join, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
    ocrAddDependence(tab[r], u, 1, DB_MODE_RO);
    ocrAddDependence(tab[r + 1], u, 2, DB_MODE_RO);
    ocrAddDependence(tab[r + 2], u, 3, DB_MODE_RO);
    ocrAddDependence(tab[2 * slots + r + 1], u, 4, DB_MODE_RO);
    ocrAddDependence(tab[3 * slots + r + 1], u, 5, DB_MODE_RO);
    ocrAddDependence(tab[slots + r + 1], u, 0, DB_MODE_RW);
  }
  return NULL_GUID;
}

static ocrGuid_t spawn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 ny = pv[P_NY], nl = pv[P_NL], it = pv[P_IT];
  const ocrGuid_t *ready = (const ocrGuid_t *)depv[1].ptr + 2 * ny;
  ocrGuid_t barrier = mirror_latch(ny - 2);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_IT] = it + 1;
  ocrGuid_t next = task(params, it + 1 == pv[P_ITERS] ? T_FINISH : T_SPAWN,
                        0, 2, EDT_PROP_NONE, NULL);
  ocrAddDependence(barrier, next, 0, DB_MODE_NULL);
  ocrAddDependence(depv[1].guid, next, 1, DB_MODE_RO);
  params[P_IT] = it;
  params[P_LATCH] = mirror_guid_u64(barrier);
  for (u64 y = 1; y + 1 < ny; ++y) {
    params[P_Y] = y;
    ocrGuid_t row = task(params, T_STEP, mirror_owner(y, ny, nl), 1, EDT_PROP_NONE, NULL);
    ocrAddDependence(ready[2 * y + (it & 1)], row, 0, DB_MODE_RO);
  }
  return NULL_GUID;
}

static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 nx = pv[P_NX], ny = pv[P_NY], lb = pv[P_LB], nranges = pv[P_NRANGES];
  double sum = 0.0;
  for (u32 k = 0; k < depc; ++k) {
    const double *values = depv[k].ptr;
    u64 len = slot_len(nx, lb, nranges, k);
    for (u64 j = 0; j < len; ++j) sum += values[j];
  }
  double *out;
  ocrGuid_t db = block(mirror_owner(pv[P_Y], ny, pv[P_NL]), sizeof(double),
                       DB_PROP_NONE, (void **)&out);
  *out = sum;
  ocrDbRelease(db);
  ocrAddDependence(db, mirror_u64_guid(pv[P_TOTAL]), (u32)pv[P_Y], DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t sum_start_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 y = pv[P_Y], ny = pv[P_NY], nranges = pv[P_NRANGES];
  u64 gen = (y == 0 || y + 1 == ny) ? 0 : (pv[P_ITERS] & 1);
  const iterator_state_t *state = depv[0].ptr;
  ocrGuid_t sum = task(pv, T_SUM, mirror_owner(y, ny, pv[P_NL]),
                       (u32)row_slots(nranges), EDT_PROP_NONE, NULL);
  for (u64 k = 0; k < row_slots(nranges); ++k)
    ocrAddDependence(state->blocks[state_index(nranges, 0, gen, k)], sum, (u32)k, DB_MODE_RO);
  if (y && y + 1 < ny) {
    ocrEventDestroy(mirror_u64_guid(pv[P_READY]));
    ocrEventDestroy(mirror_u64_guid(pv[P_READY_OTHER]));
    ocrDbDestroy(depv[1].guid);
    ocrDbDestroy(depv[2].guid);
  }
  return NULL_GUID;
}

static ocrGuid_t total_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv;
  double total = 0.0;
  for (u32 y = 0; y < depc; ++y) total += *(const double *)depv[y].ptr;
  PRINTF("CHECKSUM %.14e\n", total);
  for (u32 y = 0; y < depc; ++y) ocrDbDestroy(depv[y].guid);
  ocrShutdown();
  return NULL_GUID;
}

static ocrGuid_t finish_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  mirror_app_e2e(pv[P_START_NS]);
  u64 ny = pv[P_NY], nl = pv[P_NL];
  const ocrGuid_t *states = (const ocrGuid_t *)depv[1].ptr + ny, *ready = states + ny;
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  ocrGuid_t total = task(params, T_TOTAL, 0, (u32)ny, EDT_PROP_NONE, NULL);
  params[P_TOTAL] = mirror_guid_u64(total);
  for (u64 y = 0; y < ny; ++y) {
    params[P_Y] = y;
    params[P_READY] = mirror_guid_u64(ready[2 * y]);
    params[P_READY_OTHER] = mirror_guid_u64(ready[2 * y + 1]);
    ocrGuid_t sum = task(params, T_SUM_START, mirror_owner(y, ny, nl), 3, EDT_PROP_NONE, NULL);
    ocrAddDependence(states[y], sum, 0, DB_MODE_RO);
    for (u64 gen = 0; gen < 2; ++gen)
      ocrAddDependence(ready[2 * y + gen], sum, (u32)(1 + gen), DB_MODE_RO);
  }
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb), nx = 10, ny = 10, iters = 10, lb = 10;
  int bad = mirror_option_u64(argdb, argc, "--nx", &nx) < 0
         || mirror_option_u64(argdb, argc, "--ny", &ny) < 0
         || mirror_option_u64(argdb, argc, "--max_iterations", &iters) < 0
         || mirror_option_u64(argdb, argc, "--line_block", &lb) < 0;
  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  int sane = !bad && nl != 0 && nx >= 3 && ny >= 3 && lb != 0;
  u64 nranges = 1, blocks = 2, cells = 1;
  if (sane) {
    if (lb > nx - 2) lb = nx - 2;
    nranges = (nx - 2) / lb + ((nx - 2) % lb ? 1 : 0);
    cells = nx;
    sane = mirror_fits(&cells, ny, (u64)1 << 30)
        && mirror_fits(&blocks, ny, (u64)1 << 24)
        && mirror_fits(&blocks, row_slots(nranges), (u64)1 << 24);
  }
  if (!sane) {
    PRINTF("jacobi_hpx: usage --nx=X --ny=Y --max_iterations=I --line_block=L"
           " (X >= 3, Y >= 3, L >= 1, X*Y < 2^30, 2*Y*(ranges+2) < 2^24)\n");
    ocrShutdown();
    return NULL_GUID;
  }
  ocrEdt_t functions[T_COUNT] = {
    factory_edt, rows_gather_edt, solver_alloc_edt, iterators_gather_edt,
    row_init_edt, iter_init_edt, iter_publish_edt, boundary_edt, get_row_edt, install_edt,
    spawn_edt, step_edt, rowjoin_edt, update_edt, finish_edt, sum_start_edt, sum_edt, total_edt
  };
  u32 dependencies[T_COUNT] = {
    1, (u32)nl, 2, (u32)(nl + 1), 1, 1, 4, 4, 1, 3,
    2, 1, 1, 6, 2, 3, (u32)row_slots(nranges), (u32)ny
  };
  u64 pv[P_COUNT] = {0};
  pv[P_START_NS] = mirror_now_ns();
  pv[P_NX] = nx; pv[P_NY] = ny; pv[P_ITERS] = iters; pv[P_LB] = lb;
  pv[P_NRANGES] = nranges; pv[P_NL] = nl;
  for (u32 i = 0; i < T_COUNT; ++i) {
    ocrGuid_t tpl;
    ocrEdtTemplateCreate(&tpl, functions[i], P_COUNT, dependencies[i]);
    pv[P_TPL + i] = mirror_guid_u64(tpl);
  }
  ocrGuid_t gather = task(pv, T_ROWS_GATHER, 0, (u32)nl, EDT_PROP_NONE, NULL);
  for (u64 rank = 0; rank < nl; ++rank) {
    pv[P_RANK] = rank;
    ocrGuid_t out;
    ocrGuid_t factory = task(pv, T_FACTORY, rank, 1, EDT_PROP_NONE, &out);
    ocrAddDependence(out, gather, (u32)rank, DB_MODE_RO);
    ocrAddDependence(NULL_GUID, factory, 0, DB_MODE_NULL);
  }
  return NULL_GUID;
}
