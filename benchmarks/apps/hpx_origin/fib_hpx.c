/* OCR mirror of HPX's fibonacci_futures_distributed: the same recursion,
 * the same placement policy, the same argument list and result line.  A
 * value is an 8-byte block delivered to the sum that consumes it. */
#include "hpx_mirror.h"
#include <errno.h>
#include <limits.h>

enum { P_N, P_PARENT, P_SLOT, P_THRESHOLD, P_DIST_AT, P_LOC_REPEAT, P_NL,
       P_FIB_TPL, P_SUM_TPL, P_RUN_TPL, P_RUN, P_RUNS, P_RANK,
       P_TEST, P_QUERY_TPL, P_COUNT_TPL, P_QUERY_RANK, P_COUNT };

/* The origin's locality list is a vector every locality holds its own copy
 * of; here every task's parameters end with every rank's state block, so a
 * task names another rank's state without asking anyone for it. */
enum { MAX_RANKS = 256 };
#define PARAMC(pv) ((u32)(P_COUNT + (pv)[P_NL]))
#define STATE_OF(pv, rank) mirror_u64_guid((pv)[P_COUNT + (rank)])
#define COPY_PARAMS(dst, pv) memcpy((dst), (pv), PARAMC(pv) * sizeof(u64))

typedef struct { _Atomic u64 next_locality, serial_execution_count; } locality_t;

/* The origin's locality list is [here, every other locality, repeated
 * loc-repeat times]; index 0 is the caller itself. */
static u64 rank_of_index(u64 index, u64 nl, u64 repeat, u64 me) {
  if (nl == 1) return me;
  u64 size = 1 + (nl - 1) * repeat;
  u64 i = index % size;
  if (i == 0) return me;
  u64 j = (i - 1) % (nl - 1);
  return j < me ? j : j + 1;
}

/* The serial kernel is the row's one shared translation unit
 * (fib_serial_kernel.c), linked by every runtime's program. */
uint64_t fibonacci_serial_sub(uint64_t n);
static u64 fib_serial(u64 n, locality_t *state) { atomic_fetch_add(&state->serial_execution_count, 1); return fibonacci_serial_sub(n); }

/* Deliver a ready value to (parent, slot): the origin's make_ready_future. */
static void deliver(u64 value, ocrGuid_t parent, u32 slot) {
  ocrHint_t dh; mirror_here_hint(&dh, OCR_HINT_DB_T);
  ocrGuid_t db; u64 *p;
  ocrDbCreate(&db, (void **)&p, sizeof(u64), DB_PROP_NONE, &dh, NO_ALLOC);
  *p = value;
  ocrDbRelease(db);
  ocrAddDependence(db, parent, slot, DB_MODE_RO);
}

static void spawn_fib(const u64 *pv, u64 n, u64 rank, ocrGuid_t parent, u32 slot) {
  u64 params[P_COUNT + MAX_RANKS];
  COPY_PARAMS(params, pv);
  params[P_RANK] = rank;
  params[P_N] = n; params[P_PARENT] = mirror_guid_u64(parent); params[P_SLOT] = slot;
  ocrHint_t h; mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  ocrGuid_t e;
  ocrEdtCreate(&e, mirror_u64_guid(pv[P_FIB_TPL]), PARAMC(pv), params, 1, NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(STATE_OF(pv, rank), e, 0, DB_MODE_RW);
}

/* fibonacci_future(n) with its reply target: the recursion the origin runs
 * inline on the n-2 branch runs inline here too, inside one EDT. */
static void fib_future(const u64 *pv, locality_t *state, u64 n, ocrGuid_t parent, u32 slot) {
  u64 nl = pv[P_NL], me = pv[P_RANK];
  if (n < 2) { deliver(n, parent, slot); return; }
  if (n < pv[P_THRESHOLD]) { deliver(fib_serial(n, state), parent, slot); return; }

  u64 loc1 = 0, loc2 = 0;              /* indices into the origin's list; 0 = here */
  if (n == pv[P_DIST_AT]) {
    loc2 = atomic_fetch_add(&state->next_locality, 1) + 1;
  } else if (n - 1 == pv[P_DIST_AT]) {
    u64 next = atomic_fetch_add(&state->next_locality, 2) + 2;
    loc1 = next - 1; loc2 = next;
  }
  u64 rank1 = rank_of_index(loc1, nl, pv[P_LOC_REPEAT], me);
  u64 rank2 = rank_of_index(loc2, nl, pv[P_LOC_REPEAT], me);

  /* when_all(f, r).then(sum): the continuation runs where this task runs. */
  u64 params[P_COUNT + MAX_RANKS];
  COPY_PARAMS(params, pv);
  params[P_N] = n; params[P_PARENT] = mirror_guid_u64(parent); params[P_SLOT] = slot;
  ocrHint_t h; mirror_rank_hint(&h, me, OCR_HINT_EDT_T);
  ocrGuid_t sum;
  ocrEdtCreate(&sum, mirror_u64_guid(pv[P_SUM_TPL]), PARAMC(pv), params, 2, NULL, EDT_PROP_NONE, &h, NULL);

  spawn_fib(pv, n - 1, rank1, sum, 0);                  /* hpx::async(fib, loc1, n-1) */
  if (rank2 == me) fib_future(pv, state, n - 2, sum, 1); /* fib(loc2, n-2): inline when local */
  else spawn_fib(pv, n - 2, rank2, sum, 1);             /* remote: the origin waits, the sum waits here */
}

static ocrGuid_t fib_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  fib_future(pv, depv[0].ptr, pv[P_N], mirror_u64_guid(pv[P_PARENT]), (u32)pv[P_SLOT]);
  return NULL_GUID;
}

static ocrGuid_t sum_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 v = *(u64 *)depv[0].ptr + *(u64 *)depv[1].ptr;
  /* A value block has exactly one consumer, so it is dead the moment that
   * consumer has read it. */
  ocrDbDestroy(depv[0].guid);
  ocrDbDestroy(depv[1].guid);
  deliver(v, mirror_u64_guid(pv[P_PARENT]), (u32)pv[P_SLOT]);
  return NULL_GUID;
}

static void start_run(const u64 *pv, locality_t *state, u64 run);

static ocrGuid_t query_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv; (void)depc;
  locality_t *state = depv[0].ptr;
  ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
  ocrGuid_t db; u64 *p;
  ocrDbCreate(&db, (void **)&p, sizeof(*p), DB_PROP_NONE, &h, NO_ALLOC);
  *p = atomic_load(&state->serial_execution_count);
  return db;
}

static void query_count(u64 *pv) {
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t next, query, out;
  ocrEdtCreate(&next, mirror_u64_guid(pv[P_COUNT_TPL]), PARAMC(pv), pv,
               1, NULL, EDT_PROP_NONE, &h, NULL);
  mirror_rank_hint(&h, pv[P_QUERY_RANK], OCR_HINT_EDT_T);
  out = mirror_counted(1);    /* the count task's one slot */
  ocrEdtCreate(&query, mirror_u64_guid(pv[P_QUERY_TPL]), PARAMC(pv), pv,
               1, NULL, EDT_PROP_OEVT_VALID, &h, &out);
  ocrAddDependence(out, next, 0, DB_MODE_RO);
  ocrAddDependence(STATE_OF(pv, pv[P_QUERY_RANK]), query, 0, DB_MODE_RO);
}

static ocrGuid_t count_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  PRINTF("  serial-count,%lu,%lu\n", (unsigned long)pv[P_QUERY_RANK],
         (unsigned long)(*(u64 *)depv[0].ptr / pv[P_RUNS]));
  ocrDbDestroy(depv[0].guid);
  if (++pv[P_QUERY_RANK] < pv[P_NL]) query_count(pv);
  else ocrShutdown();
  return NULL_GUID;
}

static ocrGuid_t run_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 value = *(u64 *)depv[0].ptr;
  ocrDbDestroy(depv[0].guid);
  if (pv[P_RUN] + 1 < pv[P_RUNS]) {
    start_run(pv, depv[1].ptr, pv[P_RUN] + 1);
    return NULL_GUID;
  }
  locality_t *root = depv[1].ptr;
  PRINTF("fibonacci_future(%lu) == %lu,next_locality,%lu\n", (unsigned long)pv[P_N],
         (unsigned long)value, (unsigned long)atomic_load(&root->next_locality));
  query_count(pv);
  return NULL_GUID;
}

static void start_run(const u64 *pv, locality_t *state, u64 run) {
  atomic_store(&state->next_locality, 0);
  u64 params[P_COUNT + MAX_RANKS]; COPY_PARAMS(params, pv); params[P_RUN] = run;
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t root;
  ocrEdtCreate(&root, mirror_u64_guid(pv[P_RUN_TPL]), PARAMC(pv), params,
               2, NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(STATE_OF(pv, 0), root, 1, DB_MODE_RW);
  fib_future(pv, state, pv[P_N], root, 0);
}

static ocrGuid_t start_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  if (pv[P_TEST] != 1) {
    u64 value = fib_serial(pv[P_N], depv[0].ptr);
    PRINTF("fibonacci_serial(%lu) == %lu\n", (unsigned long)pv[P_N], (unsigned long)value);
  }
  if (pv[P_TEST] == 0) { ocrShutdown(); return NULL_GUID; }
  start_run(pv, depv[0].ptr, 0);
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 n = 10, runs = 1, threshold = 2, dist_at = 2, repeat = 1;
  int repeat_bad = 0;
  const char *repeat_text = mirror_option_raw(argdb, argc, "--loc-repeat", &repeat_bad);
  if (repeat_text) {
    char *end; errno = 0;
    long long value = strtoll(repeat_text, &end, 10);
    if (errno || !*repeat_text || *end || value < INT_MIN || value > INT_MAX) repeat_bad = 1;
    else repeat = value > 0 ? (u64)value : 0;
  }
  char test[8] = "all";
  int bad = mirror_option_u64(argdb, argc, "--n-value", &n) < 0
         || mirror_option_u64(argdb, argc, "--n-runs", &runs) < 0
         || mirror_option_u64(argdb, argc, "--threshold", &threshold) < 0
         || mirror_option_u64(argdb, argc, "--distribute-at", &dist_at) < 0
         || repeat_bad
         || mirror_option_str(argdb, argc, "--test", test, sizeof test) < 0;
  if (bad || runs == 0 || threshold > UINT32_MAX || dist_at > UINT32_MAX || threshold < 2 || threshold > n || dist_at < 2 || dist_at > n
      || (strcmp(test, "0") != 0 && strcmp(test, "1") != 0 && strcmp(test, "all") != 0)) {
    PRINTF("fib_hpx: usage --n-value=N --threshold=T --distribute-at=D --n-runs=R [--loc-repeat=K] --test=1\n");
    ocrShutdown();
    return NULL_GUID;
  }
  u64 nl; ocrAffinityCount(AFFINITY_PD, &nl);
  if (nl > MAX_RANKS) {
    PRINTF("fib_hpx: at most %d ranks\n", (int)MAX_RANKS);
    ocrShutdown();
    return NULL_GUID;
  }
  u32 width = (u32)(P_COUNT + nl);
  ocrGuid_t fib_tpl, sum_tpl, run_tpl, start_tpl, query_tpl, count_tpl;
  ocrEdtTemplateCreate(&fib_tpl, fib_edt, width, 1);
  ocrEdtTemplateCreate(&sum_tpl, sum_edt, width, 2);
  ocrEdtTemplateCreate(&run_tpl, run_edt, width, 2);
  ocrEdtTemplateCreate(&start_tpl, start_edt, width, 1);
  ocrEdtTemplateCreate(&query_tpl, query_edt, width, 1);
  ocrEdtTemplateCreate(&count_tpl, count_edt, width, 1);
  u64 pv[P_COUNT + MAX_RANKS] = {0};
  pv[P_N] = n; pv[P_THRESHOLD] = threshold; pv[P_DIST_AT] = dist_at;
  pv[P_LOC_REPEAT] = repeat; pv[P_NL] = nl; pv[P_RUNS] = runs;
  pv[P_FIB_TPL] = mirror_guid_u64(fib_tpl); pv[P_SUM_TPL] = mirror_guid_u64(sum_tpl);
  pv[P_RUN_TPL] = mirror_guid_u64(run_tpl);
  pv[P_TEST] = strcmp(test, "all") == 0 ? 2 : (u64)(test[0] - '0');
  pv[P_QUERY_TPL] = mirror_guid_u64(query_tpl); pv[P_COUNT_TPL] = mirror_guid_u64(count_tpl);
  for (u64 rank = 0; rank < nl; ++rank) {
    ocrHint_t h; mirror_rank_hint(&h, rank, OCR_HINT_DB_T);
    ocrGuid_t db; locality_t *state;
    ocrDbCreate(&db, (void **)&state, sizeof(*state), DB_PROP_NONE, &h, NO_ALLOC);
    atomic_init(&state->next_locality, 0); atomic_init(&state->serial_execution_count, 0);
    ocrDbRelease(db);
    pv[P_COUNT + rank] = mirror_guid_u64(db);
  }
  ocrHint_t h; mirror_rank_hint(&h, 0, OCR_HINT_EDT_T);
  ocrGuid_t start;
  ocrEdtCreate(&start, start_tpl, width, pv, 1, NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(STATE_OF(pv, 0), start, 0, DB_MODE_RW);
  return NULL_GUID;
}
