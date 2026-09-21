/* A periodic heat ring with separate interior and boundary continuations. */
#include "hpx_mirror.h"
#include "extensions/ocr-runtime-itf.h"

enum { P_NX, P_NT, P_NP, P_ND, P_NL, P_RANK, P_COEF, P_RANGE,
       P_DB_RANGE, P_STEP_TPL, P_SIGNAL_TPL, P_SPAWN_TPL, P_GATHER_TPL,
       P_SUM_EDT, P_SUM_TPL, P_READ_TPL, P_GATHER_EDT, P_SEM, P_T, P_I, P_REQUEST_NP, P_RESULTS, P_PRINT, P_HEADER, P_SOLVE_START, P_ELAPSED,
       P_KERNEL_TPL, P_RETIRE_TPL, P_DRAIN_TPL, P_RETIRED, P_SHUTDOWN_EDT,
       P_SHUTDOWN_TPL, P_COLLECT_TPL, P_DRIVER_TPL, P_COUNT };

/* What a generation hands to the next one.  A point's ordinal names one of
 * these for one (generation, partition) and is never reused; the index names
 * the consumer, which decides the home only where a labeled range is homed by
 * index, and what follows from that is the hop count and nothing else.  The
 * control consumers receive identities without acquiring their payloads. */
enum { KIND_PARTITION, KIND_LEFT, KIND_RIGHT, KIND_USE_MIDDLE, KIND_USE_LEFT, KIND_USE_RIGHT,
       KIND_SLOT_RELEASE, KIND_SIGNAL_RELEASE, KIND_FINAL, KIND_STEPPER_RELEASE,
       KIND_RETIRED, KIND_COUNT };

/* The origin's operator, in the origin's order of operands. */
static inline double heat(double coef, double l, double m, double r) {
  return m + coef * (l - 2.0 * m + r);
}

static inline ocrGuid_t point(u64 *pv, u64 t, u64 i, u64 kind, u64 consumer) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]),
                     (t * pv[P_NP] + i) * KIND_COUNT + kind, consumer, pv[P_NL]);
}

static inline u64 owner_of(u64 *pv, u64 i) {
  return mirror_owner(i, pv[P_NP], pv[P_NL]);
}

/* A partition's blocks are a ring K deep.  The origin's free list holds one
 * array per generation its depth limit keeps in flight, which is the limit
 * plus the two the slot rotation itself holds; a ring is never deeper than
 * the run is long, and a depth limit past the end of the run recycles
 * nothing. */
static inline u64 ring_depth(u64 nt, u64 nd) {
  u64 gens = nt + 1;
  if (nd >= gens) return gens;
  u64 k = nd + 2;
  return k < gens ? k : gens;
}

static inline u64 ring_slots(u64 *pv) {
  return ring_depth(pv[P_NT], pv[P_ND]);
}

/* The block generation g of partition i writes: slot g mod K of that
 * partition's ring, derived by every task that needs it rather than passed
 * around.  A ring is named out of its own rank's reservation and by the
 * partition's index within that rank, because every task that creates,
 * writes or destroys a slot runs on the rank that owns the partition and no
 * other rank ever has to derive the name: a runtime that homes a reserved
 * range at the PD that reserved it then agrees with one that homes each name
 * by its index (the rank is encoded in the index either way), and the
 * object's name and its metadata are on the same rank on both. */
static inline ocrGuid_t slot_block(u64 *pv, u64 i, u64 g) {
  u64 k = ring_slots(pv), per = pv[P_NP] / pv[P_NL];
  return mirror_edge(mirror_u64_guid(pv[P_DB_RANGE]),
                     (i - pv[P_RANK] * per) * k + g % k, pv[P_RANK], pv[P_NL]);
}

/* How many names one rank's rings take. */
static inline u64 ring_names(u64 *pv) {
  return ring_slots(pv) * (pv[P_NP] / pv[P_NL]) * pv[P_NL];
}

/* The point reservation: one name per kind per partition per generation per
 * rank.  A range count and every index derived from it are narrowed to 32
 * bits, so the whole space has to fit there.  The factors are folded one at
 * a time against that ceiling, because a product that wrapped would sail
 * through the very test that exists to reject it. */
static int point_space(u64 nt, u64 np, u64 nl, u64 *out) {
  u64 n = KIND_COUNT, f[3];
  f[0] = nt + 1;
  f[1] = np;
  f[2] = nl;
  for (int i = 0; i < 3; ++i) {
    if (f[i] == 0 || n > 0xffffffffu / f[i]) return 0;
    n *= f[i];
  }
  *out = n;
  return 1;
}

/* The block reservation: one name per ring slot per partition per rank,
 * folded against the same ceiling and for the same reason as the points. */
static int block_space(u64 slots, u64 np, u64 nl, u64 *out) {
  u64 n = 1;
  if (!mirror_fits(&n, slots, 0xffffffffu) || !mirror_fits(&n, np, 0xffffffffu)
      || !mirror_fits(&n, nl, 0xffffffffu))
    return 0;
  *out = n;
  return 1;
}

/* Published payloads are released before any consumer can acquire them. */
static void publish(u64 *pv, u64 t, u64 i, u64 kind, u64 consumer, ocrGuid_t db) {
  ocrGuid_t p = point(pv, t, i, kind, consumer);
  mirror_edge_open(p);
  ocrEventSatisfy(p, db);
}

/* Local views retain their backing buffer; remote views serialize one element. */
static void publish_edges(u64 *pv, u64 t, u64 i, ocrGuid_t buffer,
                          double left, double right, ocrHint_t *dh) {
  u64 np = pv[P_NP], own = owner_of(pv, i);
  u64 consumers[2] = {owner_of(pv, (i + np - 1) % np), owner_of(pv, (i + 1) % np)};
  double values[2] = {left, right};
  for (u64 side = 0; side < 2; ++side) {
    ocrGuid_t db = buffer;
    if (consumers[side] != own) {
      double *e;
      ocrDbCreate(&db, (void **)&e, sizeof(double), DB_PROP_NONE, dh, NO_ALLOC);
      *e = values[side]; ocrDbRelease(db);
    }
    publish(pv, t, i, side ? KIND_RIGHT : KIND_LEFT, consumers[side], db);
  }
}

static ocrGuid_t lifetime_point(u64 *pv, u64 g, u64 i, u64 kind) {
  return point(pv, g, i, kind, owner_of(pv, i));
}

static void lifetime_edge(u64 *pv, ocrGuid_t done, u64 g, u64 i, u64 kind) {
  ocrGuid_t event = lifetime_point(pv, g, i, kind);
  mirror_edge_open(event);
  ocrAddDependence(done, event, 0, DB_MODE_NULL);
}

/* A generation's block is freed once every reader has acknowledged it and the
 * slot that held its identity has been overwritten; what follows is that the
 * slot may be written again, so the generation waiting for it is told. */
static void retire_generation(u64 *pv, u64 g, u64 i) {
  u64 params[P_COUNT]; memcpy(params, pv, sizeof params);
  params[P_T] = g; params[P_I] = i;
  u64 per = pv[P_NP] / pv[P_NL];
  int checkpoint = g && i % per == 0 && (g - 1) % pv[P_ND] == 0;
  u32 controls = (g < pv[P_NT] ? 3 : 0) + 1 + checkpoint;
  ocrHint_t h; mirror_rank_hint(&h, owner_of(pv, i), OCR_HINT_EDT_T);
  ocrGuid_t retire, done;
  ocrEdtCreate(&retire, mirror_u64_guid(pv[P_RETIRE_TPL]), P_COUNT, params,
               controls, NULL, EDT_PROP_NONE, &h, &done);
  ocrAddDependence(done, mirror_u64_guid(pv[P_RETIRED]), OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  u32 slot = 0;
  if (g < pv[P_NT]) {
    for (u64 k = KIND_USE_MIDDLE; k <= KIND_USE_RIGHT; ++k) {
      ocrGuid_t e = lifetime_point(pv, g, i, k); mirror_edge_open(e);
      ocrAddDependence(e, retire, slot++, DB_MODE_NULL);
    }
  }
  u64 kind = g + 1 < pv[P_NT] ? KIND_SLOT_RELEASE : KIND_FINAL;
  if (kind == KIND_FINAL && g < pv[P_NT] && owner_of(pv, i) != 0)
    kind = KIND_STEPPER_RELEASE;
  ocrGuid_t e = kind == KIND_SLOT_RELEASE
      ? lifetime_point(pv, g, i, kind)
      : lifetime_point(pv, pv[P_NT], (i / per) * per, kind);
  mirror_edge_open(e); ocrAddDependence(e, retire, slot++, DB_MODE_NULL);
  if (checkpoint) {
    e = lifetime_point(pv, g, i, KIND_SIGNAL_RELEASE); mirror_edge_open(e);
    ocrAddDependence(e, retire, slot++, DB_MODE_NULL);
  }
}

static ocrGuid_t retire_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 g = pv[P_T], i = pv[P_I], per = pv[P_NP] / pv[P_NL];
  /* The slot is free.  The generation that writes it next is told, and told
   * the block's name with it, so that it asks for the write permission only
   * now; a slot no later generation reaches tells nobody and its block lives
   * until its rank's teardown. */
  if (g + ring_slots(pv) <= pv[P_NT])
    publish(pv, g, i, KIND_RETIRED, owner_of(pv, i), slot_block(pv, i, g));
  if (g < pv[P_NT])
    for (u64 k = KIND_USE_MIDDLE; k <= KIND_USE_RIGHT; ++k)
      ocrEventDestroy(lifetime_point(pv, g, i, k));
  if (g + 1 < pv[P_NT]) ocrEventDestroy(lifetime_point(pv, g, i, KIND_SLOT_RELEASE));
  if (g && i % per == 0 && (g - 1) % pv[P_ND] == 0)
    ocrEventDestroy(lifetime_point(pv, g, i, KIND_SIGNAL_RELEASE));
  return NULL_GUID;
}

/* The interior of a generation, and the block that holds it.  The first
 * generation to reach a slot creates that slot's block here, on the thread
 * that computes it, so its pages are first-touched where they are written;
 * every later generation of the same slot is handed the same block by the
 * retirement that freed it.  The two boundary elements are left to the task
 * that has the neighbours' values. */
static ocrGuid_t kernel_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  double coef; memcpy(&coef, &pv[P_COEF], sizeof coef);
  const double *m = depv[0].ptr;
  u64 g = pv[P_T] + 1, i = pv[P_I];
  ocrGuid_t db; double *next;
  if (depc > 1) {
    db = depv[1].guid;
    next = depv[1].ptr;
    ocrEventDestroy(lifetime_point(pv, g - ring_slots(pv), i, KIND_RETIRED));
  } else {
    ocrHint_t h; mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_DB_T);
    db = slot_block(pv, i, g);
    ocrDbCreate(&db, (void **)&next, pv[P_NX] * sizeof(double),
                GUID_PROP_IS_LABELED | DB_PROP_NONE, &h, NO_ALLOC);
    if (next == NULL) {
      /* A slot has exactly one creator, on one rank: the first generation
       * that reaches it.  An installed label here means two. */
      PRINTF("stencil1d_hpx: ring slot already installed\n");
      ocrShutdown();
      return NULL_GUID;
    }
  }
  for (u64 j = 1; j + 1 < pv[P_NX]; ++j)
    next[j] = heat(coef, m[j - 1], m[j], m[j + 1]);
  retire_generation(pv, g, i);
  ocrDbRelease(db);
  return db;
}

typedef struct {
  atomic_uint locked;
  u64 lower, upper;
  ocrGuid_t waiter;
} semaphore_t;

static void semaphore_lock(semaphore_t *s) {
  while (atomic_exchange_explicit(&s->locked, 1, memory_order_acquire)) {}
}
static void semaphore_unlock(semaphore_t *s) {
  atomic_store_explicit(&s->locked, 0, memory_order_release);
}

static int semaphore_wait(semaphore_t *s, u64 upper, u64 depth, ocrGuid_t waiter) {
  semaphore_lock(s);
  int blocked = upper > depth && upper - depth > s->lower;
  if (blocked && !ocrGuidIsNull(waiter)) { s->upper = upper; s->waiter = waiter; }
  semaphore_unlock(s);
  return blocked;
}

static ocrGuid_t signal_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  semaphore_t *s = depv[1].ptr;
  ocrGuid_t wake = NULL_GUID;
  semaphore_lock(s);
  if (s->lower < pv[P_T]) s->lower = pv[P_T];
  if (!ocrGuidIsNull(s->waiter) && s->upper - pv[P_ND] <= s->lower) {
    wake = s->waiter; s->waiter = NULL_GUID;
  }
  semaphore_unlock(s);
  if (!ocrGuidIsNull(wake)) ocrEventSatisfy(wake, NULL_GUID);
  return NULL_GUID;
}

static ocrGuid_t step_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 nx = pv[P_NX], t = pv[P_T], i = pv[P_I], np = pv[P_NP];
  u64 own = owner_of(pv, i);
  double coef; memcpy(&coef, &pv[P_COEF], sizeof coef);
  double *next = depv[0].ptr;
  const double *m = depv[1].ptr;
  u64 left_index = owner_of(pv, (i + np - 1) % np) == own ? nx - 1 : 0;
  next[0] = heat(coef, ((double *)depv[2].ptr)[left_index], m[0], m[1]);
  next[nx - 1] = heat(coef, m[nx - 2], m[nx - 1], *(double *)depv[3].ptr);
  double left = next[0], right = next[nx - 1];
  ocrDbRelease(depv[0].guid);
  publish(pv, t + 1, i, KIND_PARTITION, own, depv[0].guid);
  if (t + 1 < pv[P_NT]) {
    ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
    publish_edges(pv, t + 1, i, depv[0].guid, left, right, &h);
  }
  ocrEventDestroy(point(pv, t, i, KIND_PARTITION, own));
  ocrEventDestroy(point(pv, t, (i + np - 1) % np, KIND_RIGHT, own));
  ocrEventDestroy(point(pv, t, (i + 1) % np, KIND_LEFT, own));
  if (owner_of(pv, (i + np - 1) % np) != own) ocrDbDestroy(depv[2].guid);
  if (owner_of(pv, (i + 1) % np) != own) ocrDbDestroy(depv[3].guid);
  return NULL_GUID;
}

static void create_step(u64 *pv, u64 t, u64 i) {
  u64 params[P_COUNT]; memcpy(params, pv, sizeof params);
  params[P_T] = t; params[P_I] = i;
  u64 own = owner_of(pv, i), np = pv[P_NP];
  ocrHint_t h; mirror_rank_hint(&h, own, OCR_HINT_EDT_T);
  u64 slots = ring_slots(pv);
  ocrGuid_t kernel, boundary, done, computed;
  ocrEdtCreate(&boundary, mirror_u64_guid(pv[P_STEP_TPL]), P_COUNT, params,
               4, NULL, EDT_PROP_NONE, &h, &done);
  ocrEdtCreate(&kernel, mirror_u64_guid(pv[P_KERNEL_TPL]), P_COUNT, params,
               t + 1 >= slots ? 2 : 1, NULL, EDT_PROP_NONE, &h, &computed);
  ocrAddDependence(computed, boundary, 0, DB_MODE_RW);
  lifetime_edge(pv, done, t, i, KIND_USE_MIDDLE);
  lifetime_edge(pv, done, t, (i + np - 1) % np, KIND_USE_RIGHT);
  lifetime_edge(pv, done, t, (i + 1) % np, KIND_USE_LEFT);
  if (i == pv[P_RANK] * (np / pv[P_NL]) && t % pv[P_ND] == 0) {
    ocrGuid_t signal, signal_done;
    ocrEdtCreate(&signal, mirror_u64_guid(pv[P_SIGNAL_TPL]), P_COUNT, params,
                 2, NULL, EDT_PROP_NONE, &h, &signal_done);
    lifetime_edge(pv, signal_done, t + 1, i, KIND_SIGNAL_RELEASE);
    ocrAddDependence(mirror_u64_guid(pv[P_SEM]), signal, 1, DB_MODE_RW);
    ocrAddDependence(done, signal, 0, DB_MODE_NULL);
  }
  ocrGuid_t mine = point(pv, t, i, KIND_PARTITION, own);
  ocrGuid_t left = point(pv, t, (i + np - 1) % np, KIND_RIGHT, own);
  ocrGuid_t right = point(pv, t, (i + 1) % np, KIND_LEFT, own);
  mirror_edge_open(mine); mirror_edge_open(left); mirror_edge_open(right);
  ocrAddDependence(mine, boundary, 1, DB_MODE_RO);
  ocrAddDependence(left, boundary, 2, DB_MODE_RO);
  ocrAddDependence(right, boundary, 3, DB_MODE_RO);
  ocrAddDependence(mine, kernel, 0, DB_MODE_RO);
  if (t + 1 >= slots) {
    /* The slot this generation writes was last written K generations back, so
     * its retirement hands the block over.  The hand-over rides an event:
     * a dependence on the block itself is satisfied when it is added, which
     * would ask for the write permission while readers are still to come,
     * and a queue-fair arm would then order a reader that has not arrived
     * behind a writer waiting for that reader. */
    ocrGuid_t freed = lifetime_point(pv, t + 1 - slots, i, KIND_RETIRED);
    mirror_edge_open(freed);
    ocrAddDependence(freed, kernel, 1, DB_MODE_RW);
  }
  if (t) publish(pv, t - 1, i, KIND_SLOT_RELEASE, own, NULL_GUID);
}

static void issue_generations(u64 *pv, u64 first_t, semaphore_t *sem) {
  u64 nd = pv[P_ND], first = pv[P_RANK] * (pv[P_NP] / pv[P_NL]);
  for (u64 t = first_t; t < pv[P_NT]; ++t) {
    for (u64 i = 0; i < pv[P_NP] / pv[P_NL]; ++i) create_step(pv, t, first + i);
    int blocked = semaphore_wait(sem, t, nd, NULL_GUID);
    if (blocked) {
      u64 params[P_COUNT]; memcpy(params, pv, sizeof params); params[P_T] = t + 1;
      ocrHint_t h; mirror_rank_hint(&h, pv[P_RANK], OCR_HINT_EDT_T);
      ocrGuid_t next, event;
      ocrEventCreate(&event, OCR_EVENT_ONCE_T, EVT_PROP_NONE);
      ocrEdtCreate(&next, mirror_u64_guid(pv[P_SPAWN_TPL]), P_COUNT, params,
                   2, NULL, EDT_PROP_NONE, &h, NULL);
      ocrAddDependence(event, next, 0, DB_MODE_NULL);
      ocrAddDependence(mirror_u64_guid(pv[P_SEM]), next, 1, DB_MODE_RW);
      blocked = semaphore_wait(sem, t, nd, event);
      if (!blocked) ocrEventSatisfy(event, NULL_GUID);
      return;
    }
  }
  ocrAddDependence(NULL_GUID, mirror_u64_guid(pv[P_GATHER_EDT]),
                   (u32)(pv[P_NP] / pv[P_NL]), DB_MODE_NULL);
}

static ocrGuid_t spawn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  issue_generations(pv, pv[P_T], depv[1].ptr);
  return NULL_GUID;
}

static ocrGuid_t gather_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
  ocrGuid_t db, *names;
  ocrDbCreate(&db, (void **)&names, (depc - 1) * sizeof(*names), DB_PROP_NONE, &h, NO_ALLOC);
  for (u32 i = 0; i + 1 < depc; ++i) {
    names[i] = depv[i].guid;
    ocrEventDestroy(point(pv, pv[P_NT], pv[P_RANK] * (pv[P_NP] / pv[P_NL]) + i,
                           KIND_PARTITION, pv[P_RANK]));
  }
  ocrDbRelease(db);
  ocrAddDependence(db, mirror_u64_guid(pv[P_SUM_EDT]), (u32)pv[P_RANK], DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t shutdown_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc; (void)depv;
  ocrShutdown(); return NULL_GUID;
}

static ocrGuid_t drain_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 per = pv[P_NP] / pv[P_NL], first = pv[P_RANK] * per;
  u64 slots = ring_slots(pv);
  /* Every slot of the ring was reached: a ring is never deeper than the run
   * is long, so generations 0..min(nt, K-1) created all K of them. */
  for (u64 i = 0; i < per; ++i)
    for (u64 s = 0; s < slots; ++s)
      ocrDbDestroy(slot_block(pv, first + i, s));
  ocrDbDestroy(mirror_u64_guid(pv[P_SEM]));
  ocrEventDestroy(lifetime_point(pv, pv[P_NT], first, KIND_FINAL));
  if (pv[P_RANK] && pv[P_NT])
    ocrEventDestroy(lifetime_point(pv, pv[P_NT], first, KIND_STEPPER_RELEASE));
  ocrAddDependence(NULL_GUID, mirror_u64_guid(pv[P_SHUTDOWN_EDT]), (u32)pv[P_RANK], DB_MODE_NULL);
  return NULL_GUID;
}

static void release_final(u64 *pv) {
  u64 per = pv[P_NP] / pv[P_NL];
  for (u64 r = 0; r < pv[P_NL]; ++r)
    publish(pv, pv[P_NT], r * per, KIND_FINAL, r, NULL_GUID);
}

static void print_timing(u64 *pv) {
  if (pv[P_HEADER]) PRINTF("Localities,OS_Threads,Execution_Time_sec,Points_per_Partition,Partitions,Time_Steps\n");
  PRINTF("%llu, %llu, %.14e, %llu, %llu, %llu\n", (unsigned long long)pv[P_NL],
         (unsigned long long)ocrNbWorkers(), (double)pv[P_ELAPSED] / 1e9,
         (unsigned long long)pv[P_NX], (unsigned long long)pv[P_REQUEST_NP], (unsigned long long)pv[P_NT]);
}

static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  double sum = 0.0;
  for (u64 i = 0; i < pv[P_NP]; ++i) {
    const double *v = depv[i].ptr;
    for (u64 j = 0; j < pv[P_NX]; ++j) sum += v[j];
  }
  PRINTF("CHECKSUM %.14e\n", sum);
  if (pv[P_RESULTS]) {
    for (u32 i = 0; i + 1 < depc; ++i) ocrDbRelease(depv[i].guid);
    pv[P_PRINT] = 1; pv[P_I] = 0;
    ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
    ocrGuid_t read;
    ocrEdtCreate(&read, mirror_u64_guid(pv[P_READ_TPL]), P_COUNT, pv, 2, NULL, EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(depv[depc - 1].guid, read, 1, DB_MODE_RO);
    ocrAddDependence(depv[0].guid, read, 0, DB_MODE_RO);
  } else {
    print_timing(pv);
    for (u32 i = 0; i + 1 < depc; ++i) ocrDbRelease(depv[i].guid);
    ocrDbDestroy(depv[depc - 1].guid);
    release_final(pv);
  }
  return NULL_GUID;
}

static ocrGuid_t read_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrGuid_t *names = depv[1].ptr;
  if (pv[P_PRINT]) {
    PRINTF("U[%llu] = {", (unsigned long long)pv[P_I]);
    const double *v = depv[0].ptr;
    for (u64 j = 0; j < pv[P_NX]; ++j) PRINTF("%s%.5e", j ? ", " : "", v[j]);
    PRINTF("}\n");
    ocrDbRelease(depv[0].guid);
    if (pv[P_I] + 1 == pv[P_NP]) {
      print_timing(pv); ocrDbDestroy(depv[1].guid); release_final(pv); return NULL_GUID;
    }
  }
  if (!pv[P_PRINT]) ocrDbRelease(depv[0].guid);
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t next;
  if (pv[P_I] + 1 < pv[P_NP]) {
    ++pv[P_I];
    ocrEdtCreate(&next, mirror_u64_guid(pv[P_READ_TPL]), P_COUNT, pv,
                 2, NULL, EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(depv[1].guid, next, 1, DB_MODE_RO);
    ocrAddDependence(names[pv[P_I]], next, 0, DB_MODE_RO);
  } else {
    pv[P_ELAPSED] = mirror_now_ns() - pv[P_SOLVE_START];
    ocrEdtCreate(&next, mirror_u64_guid(pv[P_SUM_TPL]), P_COUNT, pv,
                 (u32)pv[P_NP] + 1, NULL, EDT_PROP_NONE, &h, NULL);
    for (u64 i = 0; i < pv[P_NP]; ++i)
      ocrAddDependence(names[i], next, (u32)i, DB_MODE_RO);
    ocrAddDependence(depv[1].guid, next, (u32)pv[P_NP], DB_MODE_RO);
  }
  return NULL_GUID;
}

static ocrGuid_t collect_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  ocrGuid_t table, *names;
  ocrDbCreate(&table, (void **)&names, pv[P_NP] * sizeof(*names), DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  u64 per = pv[P_NP] / pv[P_NL];
  for (u32 r = 0; r < depc; ++r) {
    memcpy(names + r * per, depv[r].ptr, per * sizeof(*names));
    ocrDbDestroy(depv[r].guid);
  }
  ocrGuid_t first = names[0];
  ocrDbRelease(table);
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t read;
  ocrEdtCreate(&read, mirror_u64_guid(pv[P_READ_TPL]), P_COUNT, pv,
               2, NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(table, read, 1, DB_MODE_RO);
  ocrAddDependence(first, read, 0, DB_MODE_RO);
  return NULL_GUID;
}

static ocrGuid_t driver_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 rank = pv[P_RANK], per = pv[P_NP] / pv[P_NL], nx = pv[P_NX];
  ocrGuid_t db_range;
  if (ocrGuidRangeCreate(&db_range, ring_names(pv), GUID_USER_DB) != 0) {
    PRINTF("stencil1d_hpx: the rank's ring names could not be reserved\n");
    ocrShutdown();
    return NULL_GUID;
  }
  pv[P_DB_RANGE] = mirror_guid_u64(db_range);
  ocrHint_t h; mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  ocrGuid_t gather, gathered;
  ocrEdtCreate(&gather, mirror_u64_guid(pv[P_GATHER_TPL]), P_COUNT, pv,
               (u32)per + 1, NULL, EDT_PROP_NONE, &h, rank && pv[P_NT] ? &gathered : NULL);
  if (rank && pv[P_NT])
    lifetime_edge(pv, gathered, pv[P_NT], rank * per, KIND_STEPPER_RELEASE);
  pv[P_GATHER_EDT] = mirror_guid_u64(gather);
  for (u64 i = 0; i < per; ++i) {
    ocrGuid_t event = point(pv, pv[P_NT], rank * per + i, KIND_PARTITION, rank);
    mirror_edge_open(event); ocrAddDependence(event, gather, (u32)i, DB_MODE_RO);
  }
  mirror_rank_hint(&h, rank, OCR_HINT_DB_T);
  pv[P_RETIRED] = mirror_guid_u64(mirror_latch((pv[P_NT] + 1) * per));
  ocrGuid_t sem_db; semaphore_t *sem;
  ocrDbCreate(&sem_db, (void **)&sem, sizeof(*sem), DB_PROP_NONE, &h, NO_ALLOC);
  atomic_init(&sem->locked, 0); sem->lower = 0; sem->upper = 0; sem->waiter = NULL_GUID;
  pv[P_SEM] = mirror_guid_u64(sem_db);
  ocrHint_t eh; mirror_rank_hint(&eh, rank, OCR_HINT_EDT_T);
  ocrGuid_t drain;
  ocrEdtCreate(&drain, mirror_u64_guid(pv[P_DRAIN_TPL]), P_COUNT, pv, 1, NULL, EDT_PROP_NONE, &eh, NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_RETIRED]), drain, 0, DB_MODE_NULL);
  for (u64 i = 0; i < per; ++i) {
    double *p;
    ocrGuid_t db = slot_block(pv, rank * per + i, 0);
    ocrDbCreate(&db, (void **)&p, nx * sizeof(double),
                GUID_PROP_IS_LABELED | DB_PROP_NONE, &h, NO_ALLOC);
    retire_generation(pv, 0, rank * per + i);
    for (u64 j = 0; j < nx; ++j) p[j] = (double)i * (double)nx + (double)j;
    double left = 0.0, right = 0.0;
    if (pv[P_NT]) { left = p[0]; right = p[nx - 1]; }
    ocrDbRelease(db);
    publish(pv, 0, rank * per + i, KIND_PARTITION, rank, db);
    if (pv[P_NT]) publish_edges(pv, 0, rank * per + i, db, left, right, &h);
  }
  issue_generations(pv, 0, sem);
  ocrDbRelease(sem_db);
  return NULL_GUID;
}

/* The solve is timed on rank 0, where its end is read; a program's main task
 * may run on any rank, so the clock starts here and not there. */
static ocrGuid_t root_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 nl = pv[P_NL];
  pv[P_SOLVE_START] = mirror_now_ns();

  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t shutdown;
  ocrEdtCreate(&shutdown, mirror_u64_guid(pv[P_SHUTDOWN_TPL]), P_COUNT, pv, (u32)nl, NULL,
               EDT_PROP_NONE, &h0, NULL);
  pv[P_SHUTDOWN_EDT] = mirror_guid_u64(shutdown);
  ocrGuid_t sum;
  ocrEdtCreate(&sum, mirror_u64_guid(pv[P_COLLECT_TPL]), P_COUNT, pv, (u32)nl, NULL,
               EDT_PROP_NONE, &h0, NULL);
  pv[P_SUM_EDT] = mirror_guid_u64(sum);

  mirror_spmd_fork(mirror_u64_guid(pv[P_DRIVER_TPL]), pv, P_COUNT, P_RANK, nl, 0, NULL);
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 nx = 10, nt = 45, np = 10, nd = 10;
  double k = 0.5, dt = 1.0, dx = 1.0;
  int bad = mirror_option_u64(argdb, argc, "--nx", &nx) < 0
         || mirror_option_u64(argdb, argc, "--nt", &nt) < 0
         || mirror_option_u64(argdb, argc, "--np", &np) < 0
         || mirror_option_u64(argdb, argc, "--nd", &nd) < 0
         || mirror_option_f64(argdb, argc, "--k", &k) < 0
         || mirror_option_f64(argdb, argc, "--dt", &dt) < 0
         || mirror_option_f64(argdb, argc, "--dx", &dx) < 0;
  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  u64 requested_np = np;
  np = (np / nl) * nl;
  u64 points = 0, blocks = 0;
  int reject = bad || np < nl || (nt && (nx < 2 || nd == 0))
            || !point_space(nt, np, nl, &points)
            || !block_space(ring_depth(nt, nd), np / nl, nl, &blocks);
  ocrGuid_t range = NULL_GUID;
  if (!reject && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0)
    reject = 1;
  if (reject) {
    PRINTF("stencil1d_hpx: usage --nx=N --nt=T --np=P --nd=D [--k=K --dt=S --dx=X]"
           " (nx >= 2 and nd >= 1 when nt > 0, P >= ranks,"
           " 11*(T+1)*P*ranks < 2^32)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t step_tpl, signal_tpl, spawn_tpl, gather_tpl, sum_tpl, collect_tpl, read_tpl, driver_tpl, root_tpl;
  ocrEdtTemplateCreate(&step_tpl, step_edt, P_COUNT, 4);
  ocrEdtTemplateCreate(&signal_tpl, signal_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&collect_tpl, collect_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&read_tpl, read_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&spawn_tpl, spawn_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&gather_tpl, gather_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&sum_tpl, sum_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&driver_tpl, driver_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&root_tpl, root_edt, P_COUNT, 0);

  ocrGuid_t kernel_tpl, retire_tpl, drain_tpl, shutdown_tpl;
  ocrEdtTemplateCreate(&kernel_tpl, kernel_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&retire_tpl, retire_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&drain_tpl, drain_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&shutdown_tpl, shutdown_edt, P_COUNT, EDT_PARAM_UNK);
  u64 pv[P_COUNT] = {0};
  pv[P_KERNEL_TPL] = mirror_guid_u64(kernel_tpl);
  pv[P_RETIRE_TPL] = mirror_guid_u64(retire_tpl); pv[P_DRAIN_TPL] = mirror_guid_u64(drain_tpl);
  pv[P_REQUEST_NP] = requested_np; pv[P_HEADER] = 1;
  for (u64 i = 1; i < argc; ++i) {
    if (!strcmp(getArgv(argdb, i), "--results")) pv[P_RESULTS] = 1;
    if (!strcmp(getArgv(argdb, i), "--no-header")) pv[P_HEADER] = 0;
  }
  pv[P_SIGNAL_TPL] = mirror_guid_u64(signal_tpl);
  pv[P_READ_TPL] = mirror_guid_u64(read_tpl);
  pv[P_SUM_TPL] = mirror_guid_u64(sum_tpl);
  pv[P_NX] = nx; pv[P_NT] = nt; pv[P_NP] = np; pv[P_ND] = nd; pv[P_NL] = nl;
  double coef = k * dt / (dx * dx);
  memcpy(&pv[P_COEF], &coef, sizeof coef);
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_STEP_TPL] = mirror_guid_u64(step_tpl);
  pv[P_SPAWN_TPL] = mirror_guid_u64(spawn_tpl);
  pv[P_GATHER_TPL] = mirror_guid_u64(gather_tpl);

  pv[P_SHUTDOWN_TPL] = mirror_guid_u64(shutdown_tpl);
  pv[P_COLLECT_TPL] = mirror_guid_u64(collect_tpl);
  pv[P_DRIVER_TPL] = mirror_guid_u64(driver_tpl);

  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t root;
  ocrEdtCreate(&root, root_tpl, P_COUNT, pv, 0, NULL, EDT_PROP_NONE, &h0, NULL);
  return NULL_GUID;
}
