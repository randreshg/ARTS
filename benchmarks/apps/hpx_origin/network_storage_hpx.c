/* Owner-side storage actions and requester-side completion continuations. */
#include "hpx_mirror.h"
#include "extensions/ocr-runtime-itf.h"
#include <stdio.h>

enum { P_SLOTS, P_BYTES, P_ITERS, P_NL, P_RANK, P_ALL2ALL, P_NOLOCAL, P_RANDOM,
       P_TEST, P_TURN, P_BARRIER, P_RANGE, P_FINAL, P_TURN_TPL, P_RELAY_TPL,
       P_TALLY_TPL, P_PUT_TPL, P_GET_TPL, P_LAND_TPL, P_COMPLETE_TPL,
       P_COUNTERS, P_TO, P_OFFSET, P_BLOCKS, P_STORAGE, P_OPTIONS, P_RESULTS, P_INDEX,
       P_SRC_ALIAS, P_COUNT };

/* The origin sizes a per-locality array at compile time and refuses to run
 * on more localities than it holds. */
enum { MAX_RANKS = 16384 };

/* The tests in the order they run.  Each is opened and closed by a barrier of
 * its own, so barrier 2t opens test t and barrier 2t+1 closes it. */
enum { TEST_WARMUP, TEST_WRITE, TEST_READ, TEST_COUNT };
enum { BARRIER_COUNT = 2 * TEST_COUNT };

/* Three kinds of rendezvous point.  A table carries an owner's slot names to
 * a rank that has no other way to learn them; an entry carries one rank's
 * arrival at a barrier to the rank that holds the barrier, and a release
 * carries the barrier's end back to each rank.  Every entry of a barrier has
 * the same consumer, so an entry's unit names its sender; the kind stride
 * keeps the kinds from aliasing one another. */
enum { KIND_TABLE, KIND_ENTRY, KIND_RELEASE, KIND_COUNT };

typedef struct {
  _Atomic u64 puts, gets;
  _Atomic int waiting[MAX_RANKS];
} counters_t;

/* What a rank carries from one of its turns to the next: its random stream,
 * which runs on from one test into the next, and every rank's slot names. */
enum { PROF_WRITE, PROF_ITERATION, PROF_SETUP, PROF_PUT, PROF_MOVE, PROF_WAIT, PROF_BARRIER, PROF_COUNT };
typedef struct { double seconds; u64 count, level; } profile_t;
typedef struct {
  mirror_mt19937_t gen;
  u64 phase_start, iteration_start, wait_start;
  profile_t phase[PROF_COUNT], iteration[PROF_COUNT];
  ocrGuid_t names[];
} state_t;

static void profile_add(profile_t *dest, profile_t value) {
  if (!dest->count) { *dest = value; ++dest->level; }
  else { dest->seconds += value.seconds; ++dest->count; }
}
static void profile_span(profile_t *dest, u64 start) {
  profile_t value = {(double)(mirror_now_ns() - start) / 1e9, 1, 0};
  profile_add(dest, value);
}

static void report_test(u64 *pv, state_t *st, const char *network) {
  double seconds = (double)(mirror_now_ns() - st->phase_start) / 1e9;
  u64 passes = pv[P_TEST] == TEST_WARMUP ? 1 : pv[P_ITERS];
  double mb = (double)((pv[P_ALL2ALL] ? pv[P_NL] : 1) * (pv[P_STORAGE] / (1024 * 1024)) * passes);
  double iops = (double)(passes * pv[P_SLOTS]);
  if (pv[P_RANK] == 0) {
    PRINTF("Total time           : %.9g\nMemory Transferred   : %.9g MB\n"
           "Number of local IOPs : %.9g\nIOPs/s (local)       : %.9g\n"
           "Aggregate BW %s   : %.9g MB/s\n", seconds, mb, iops, iops / seconds,
           pv[P_TEST] == TEST_READ ? "Read" : "Write", mb / seconds);
    if (pv[P_TEST] != TEST_WARMUP)
      PRINTF("CSVData, %s, network, %s, ranks, %llu, threads, %llu, Memory, %.9g, "
             "IOPsize, %llu, IOPS/s, %.9g, BW(MB/s), %.9g, \n",
             pv[P_TEST] == TEST_READ ? "read" : "write", network,
             (unsigned long long)pv[P_NL], (unsigned long long)ocrNbWorkers(), mb, (unsigned long long)pv[P_BYTES], iops / seconds, mb / seconds);
  }
  if (pv[P_TEST] != TEST_READ) {
    profile_span(&st->phase[PROF_BARRIER], st->wait_start);
    st->phase[PROF_WRITE] = (profile_t){(double)(mirror_now_ns() - st->phase_start) / 1e9, 1, 0};
    if (pv[P_RANK] == 0 && pv[P_TEST] == TEST_WRITE) {
      static const char *const names[] = {"Write function", "Iteration", "Setup slots", "Put", "Moving futures", "Future wait", "Final Barrier"};
      for (u32 i = 0; i < PROF_COUNT; ++i)
        if (st->phase[i].count)
          PRINTF("Profile %-20s : %llu %llu %.9g %.9g\n", names[i],
                 (unsigned long long)st->phase[i].level, (unsigned long long)st->phase[i].count,
                 st->phase[i].seconds, 100.0 * st->phase[i].seconds / st->phase[PROF_WRITE].seconds);
    }
  }
}

static inline ocrGuid_t point(u64 *pv, u64 unit, u64 kind, u64 consumer) {
  return mirror_edge(mirror_u64_guid(pv[P_RANGE]), unit * KIND_COUNT + kind,
                     consumer, pv[P_NL]);
}
static inline ocrGuid_t table_point(u64 *pv, u64 owner, u64 consumer) {
  return point(pv, owner, KIND_TABLE, consumer);
}
static inline ocrGuid_t entry_point(u64 *pv, u64 barrier, u64 rank) {
  return point(pv, barrier * pv[P_NL] + rank, KIND_ENTRY, 0);
}
static inline ocrGuid_t release_point(u64 *pv, u64 barrier, u64 rank) {
  return point(pv, barrier, KIND_RELEASE, rank);
}

/* A signal carries no block, so there is no release for it to follow and it
 * may be raised from a body. */
static void raise_point(ocrGuid_t p) {
  mirror_edge_open(p);
  ocrEventSatisfy(p, NULL_GUID);
}

/* The point reservation: every kind of every unit for every consumer, the
 * largest unit being an entry's (barriers times ranks).  A range count and
 * every index derived from it are narrowed to 32 bits, so the factors are
 * folded one at a time against that ceiling. */
static int point_space(u64 nl, u64 *out) {
  u64 n = (u64)KIND_COUNT * BARRIER_COUNT;
  u64 f[2] = {nl, nl};
  for (int i = 0; i < 2; ++i) {
    if (f[i] == 0 || n > 0xffffffffu / f[i]) return 0;
    n *= f[i];
  }
  *out = n;
  return 1;
}

static ocrGuid_t success_result(void) {
  ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
  ocrGuid_t db; int *p;
  ocrDbCreate(&db, (void **)&p, sizeof(*p), DB_PROP_NONE, &h, NO_ALLOC);
  *p = 1;
  return db;
}

static void copy_into(u64 *pv, ocrEdtDep_t *dst, const char *src) {
  u64 offset = pv[P_OFFSET], first = pv[P_BYTES] - offset;
  if (offset && src == dst[0].ptr) {
    memmove(dst[1].ptr, src + first, offset);
    memmove((char *)dst[0].ptr + offset, src, first);
  } else {
    memmove((char *)dst[0].ptr + offset, src, first);
    if (offset) memmove(dst[1].ptr, src + first, offset);
  }
}

/* A source slot that names a destination block is the origin's copy of a
 * storage range onto itself: one array, one std::copy.  A block may not be
 * taken twice in two modes, so the source then rides the destination's
 * acquisition instead of a slot of its own. */
static ocrGuid_t put_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  u64 alias = pv[P_SRC_ALIAS];
  const char *src = alias ? depv[alias - 1].ptr : depv[2].ptr;
  copy_into(pv, depv, src);
  return success_result();
}

static ocrGuid_t get_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  ocrHint_t h; mirror_here_hint(&h, OCR_HINT_DB_T);
  ocrGuid_t reply; char *p;
  ocrDbCreate(&reply, (void **)&p, pv[P_BYTES], DB_PROP_NONE, &h, NO_ALLOC);
  u64 offset = pv[P_OFFSET], first = pv[P_BYTES] - offset;
  memmove(p, (char *)depv[0].ptr + offset, first);
  if (offset) memmove(p + first, depv[1].ptr, offset);
  return reply;
}

static ocrGuid_t land_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  copy_into(pv, depv, depv[2].ptr);
  ocrDbDestroy(depv[2].guid);
  return NULL_GUID;
}

static ocrGuid_t complete_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc;
  counters_t *c = depv[1].ptr;
  int result = pv[P_TEST] == TEST_READ ? 1 : *(int *)depv[0].ptr;
  atomic_fetch_sub(&c->waiting[pv[P_TO]], 1);
  if (pv[P_TEST] == TEST_READ) atomic_fetch_add(&c->gets, 1);
  else {
    ocrDbDestroy(depv[0].guid);
    atomic_fetch_add(&c->puts, 1);
  }
  ((int *)depv[2].ptr)[pv[P_INDEX]] = result;
  return NULL_GUID;
}

static void issue(u64 *pv, const state_t *st, ocrGuid_t join, u64 i, u64 to, u64 slot) {
  u64 blocks = pv[P_BLOCKS], rank = pv[P_RANK], bytes = pv[P_BYTES];
  uint32_t address = (uint32_t)(slot * bytes);
  if ((u64)address > pv[P_STORAGE] || bytes > pv[P_STORAGE] - address) {
    PRINTF("network_storage_hpx: transfer exceeds the origin's allocated storage\n");
    ocrShutdown(); return;
  }
  u64 block = address / bytes;
  u64 params[P_COUNT]; memcpy(params, pv, sizeof params);
  params[P_OFFSET] = address % bytes; params[P_TO] = to; params[P_INDEX] = i;
  params[P_SRC_ALIAS] = 0;
  if (to == rank) {
    if (i == block) params[P_SRC_ALIAS] = 1;
    else if (params[P_OFFSET] && i == block + 1) params[P_SRC_ALIAS] = 2;
  }
  int get = pv[P_TEST] == TEST_READ;
  ocrHint_t h; mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  ocrGuid_t complete, counted;
  ocrEdtCreate(&complete, mirror_u64_guid(pv[P_COMPLETE_TPL]), P_COUNT, params,
               3, NULL, EDT_PROP_NONE, &h, &counted);
  ocrAddDependence(counted, join, OCR_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  ocrAddDependence(mirror_u64_guid(pv[P_COUNTERS]), complete, 1, DB_MODE_RW);
  ocrAddDependence(mirror_u64_guid(pv[P_RESULTS]), complete, 2, DB_MODE_RW);
  ocrGuid_t action, result;
  if (get) {
    ocrGuid_t landing, landed;
    ocrEdtCreate(&landing, mirror_u64_guid(pv[P_LAND_TPL]), P_COUNT, params,
                 3, NULL, EDT_PROP_NONE, &h, &landed);
    ocrAddDependence(landed, complete, 0, DB_MODE_NULL);
    ocrAddDependence(st->names[rank * blocks + block], landing, 0, DB_MODE_RW);
    ocrAddDependence(params[P_OFFSET] ? st->names[rank * blocks + block + 1] : NULL_GUID,
                     landing, 1, params[P_OFFSET] ? DB_MODE_RW : DB_MODE_NULL);
    mirror_rank_hint(&h, to, OCR_HINT_EDT_T);
    ocrEdtCreate(&action, mirror_u64_guid(pv[P_GET_TPL]), P_COUNT, params,
                 2, NULL, EDT_PROP_NONE, &h, &result);
    ocrAddDependence(result, landing, 2, DB_MODE_RO);
    ocrAddDependence(st->names[to * blocks + block], action, 0, DB_MODE_RO);
    ocrAddDependence(params[P_OFFSET] ? st->names[to * blocks + block + 1] : NULL_GUID,
                     action, 1, params[P_OFFSET] ? DB_MODE_RO : DB_MODE_NULL);
  } else {
    mirror_rank_hint(&h, to, OCR_HINT_EDT_T);
    ocrEdtCreate(&action, mirror_u64_guid(pv[P_PUT_TPL]), P_COUNT, params,
                 3, NULL, EDT_PROP_NONE, &h, &result);
    ocrAddDependence(result, complete, 0, DB_MODE_RO);
    ocrAddDependence(st->names[to * blocks + block], action, 0, DB_MODE_RW);
    ocrAddDependence(params[P_OFFSET] ? st->names[to * blocks + block + 1] : NULL_GUID,
                     action, 1, params[P_OFFSET] ? DB_MODE_RW : DB_MODE_NULL);
    if (params[P_SRC_ALIAS])
      ocrAddDependence(NULL_GUID, action, 2, DB_MODE_NULL);
    else
      ocrAddDependence(st->names[rank * blocks + i], action, 2, DB_MODE_RO);
  }
}

/* One turn of one rank in one test.  A turn inside the test issues a pass --
 * a transfer for every slot, its destination and offset drawn from the
 * rank's stream in the origin's order -- and hands the stream to the next
 * turn behind the pass's join.  The turn after the last pass reports the rank
 * at the barrier that closes the test and wires what follows it.  A rank the
 * test leaves inactive has no pass.
 *
 * A join is once-type, so the next turn is registered on it before the first
 * transfer exists.  The state is handed on released, and last: a plain block
 * dependence is an edge only when the block is complete and published by the
 * time the consumer's slot is filled. */
static ocrGuid_t turn_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc;
  u64 slots = pv[P_SLOTS], blocks = pv[P_BLOCKS], nl = pv[P_NL], rank = pv[P_RANK];
  u64 test = pv[P_TEST], turn = pv[P_TURN];
  ocrGuid_t state = depv[depc - 1].guid;
  state_t *st = depv[depc - 1].ptr;

  /* A test's first turn runs behind the barrier that opens it; the very first
   * also takes delivery of every rank's slot names. */
  if (turn == 0) {
    st->phase_start = mirror_now_ns();
    memset(st->phase, 0, sizeof(st->phase));
    if (rank == 0) PRINTF("Iteration ");
    ocrEventDestroy(release_point(pv, 2 * test, rank));
    if (test == TEST_WARMUP) {
      for (u64 q = 0; q < nl; ++q) {
        memcpy(st->names + q * blocks, depv[1 + q].ptr, blocks * sizeof(ocrGuid_t));
        ocrEventDestroy(table_point(pv, q, rank));
      }
    }
  }

  if (turn) {
    const int *results = depv[1].ptr;
    const u64 *order = depv[2].ptr;
    int result = 1;
    for (u64 i = 0; i < slots; ++i)
      if (results[order[i]] == 0) { result = 0; break; }
    (void)result;
    ocrDbDestroy(depv[1].guid); ocrDbDestroy(depv[2].guid);
  }
  if (turn && test != TEST_READ) {
    profile_span(&st->iteration[PROF_WAIT], st->wait_start);
    profile_span(&st->phase[PROF_ITERATION], st->iteration_start);
    for (u32 i = 0; i < PROF_COUNT; ++i)
      if (st->iteration[i].count) profile_add(&st->phase[i], st->iteration[i]);
  }
  ocrHint_t h;
  mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);
  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  u64 passes = test == TEST_WARMUP ? 1 : pv[P_ITERS];
  int active = rank == 0 || pv[P_ALL2ALL];

  if (active && turn < passes) {
    st->iteration_start = mirror_now_ns();
    memset(st->iteration, 0, sizeof(st->iteration));
    if (rank == 0) { PRINTF("%s", turn % 10 == 0 ? "x" : "."); fflush(stdout); }
    ocrGuid_t results; void *unused;
    ocrHint_t dh; mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
    ocrDbCreate(&results, &unused, (slots ? slots : 1) * sizeof(int), DB_PROP_NO_ACQUIRE, &dh, NO_ALLOC);
    pv[P_RESULTS] = mirror_guid_u64(results);
    params[P_RESULTS] = pv[P_RESULTS];
    u64 *heads = malloc(nl * sizeof(u64)), *tails = malloc(nl * sizeof(u64));
    u64 *links = malloc((slots ? slots : 1) * sizeof(u64));
    if (!heads || !tails || !links) { PRINTF("network_storage_hpx: out of memory\n"); ocrShutdown(); return NULL_GUID; }
    for (u64 q = 0; q < nl; ++q) heads[q] = tails[q] = UINT64_MAX;
    ocrGuid_t join = mirror_latch(slots);
    params[P_TURN] = turn + 1;
    ocrGuid_t next;
    ocrEdtCreate(&next, mirror_u64_guid(pv[P_TURN_TPL]), P_COUNT, params, 4, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(join, next, 0, DB_MODE_NULL);
    ocrAddDependence(results, next, 1, DB_MODE_RO);
    /* A storage smaller than one transfer has no slots, and its passes join
     * nothing, at once. */
    if (slots == 0) ocrEventSatisfySlot(join, NULL_GUID, OCR_EVENT_LATCH_DECR_SLOT);
    for (u64 i = 0; i < slots; ++i) {
      u64 setup_start = mirror_now_ns();
      u64 to = pv[P_RANDOM] ? mirror_mt_bounded(&st->gen, (uint32_t)nl) : (rank + i) % nl;
      while (pv[P_NOLOCAL] && to == rank) to = mirror_mt_bounded(&st->gen, (uint32_t)nl);
      u64 slot = mirror_mt_bounded(&st->gen, (uint32_t)slots);
      if (test != TEST_READ) profile_span(&st->iteration[PROF_SETUP], setup_start);
      u64 put_start = test != TEST_READ ? mirror_now_ns() : 0;
      issue(pv, st, join, i, to, slot);
      links[i] = UINT64_MAX;
      if (heads[to] == UINT64_MAX) heads[to] = i;
      else links[tails[to]] = i;
      tails[to] = i;
      if (test != TEST_READ) profile_span(&st->iteration[PROF_PUT], put_start);
    }
    u64 move_start = mirror_now_ns();
    ocrGuid_t order; u64 *flat;
    ocrDbCreate(&order, (void **)&flat, (slots ? slots : 1) * sizeof(u64), DB_PROP_NONE, &dh, NO_ALLOC);
    u64 at = 0;
    for (u64 q = 0; q < nl; ++q)
      for (u64 j = heads[q]; j != UINT64_MAX; j = links[j]) flat[at++] = j;
    free(heads); free(tails); free(links);
    ocrDbRelease(order);
    ocrAddDependence(order, next, 2, DB_MODE_RO);
    if (test != TEST_READ) profile_span(&st->iteration[PROF_MOVE], move_start);
    st->wait_start = mirror_now_ns();
    ocrDbRelease(state);
    ocrAddDependence(state, next, 3, DB_MODE_RW);
    return NULL_GUID;
  }

  if (rank == 0 && test != TEST_READ) PRINTF("\n");
  st->wait_start = mirror_now_ns();
  u64 closing = 2 * test + 1;
  if (test + 1 < TEST_COUNT) {
    /* Between two tests the rank enters the closing barrier and, released
     * from it, the opening one; the next test's first turn waits on that. */
    params[P_BARRIER] = closing;
    ocrGuid_t relay;
    ocrEdtCreate(&relay, mirror_u64_guid(pv[P_RELAY_TPL]), P_COUNT, params, 3, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(state, relay, 1, DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(pv[P_OPTIONS]), relay, 2, DB_MODE_RO);
    ocrGuid_t released = release_point(pv, closing, rank);
    mirror_edge_open(released);
    ocrAddDependence(released, relay, 0, DB_MODE_NULL);

    params[P_TEST] = test + 1;
    params[P_TURN] = 0;
    ocrGuid_t first;
    ocrEdtCreate(&first, mirror_u64_guid(pv[P_TURN_TPL]), P_COUNT, params, 2, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrGuid_t opened = release_point(pv, closing + 1, rank);
    mirror_edge_open(opened);
    ocrAddDependence(opened, first, 0, DB_MODE_NULL);
    ocrDbRelease(state);
    ocrAddDependence(state, first, 1, DB_MODE_RW);
  } else {
    ocrGuid_t tally;
    ocrEdtCreate(&tally, mirror_u64_guid(pv[P_TALLY_TPL]), P_COUNT, params, 4, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrGuid_t released = release_point(pv, closing, rank);
    mirror_edge_open(released);
    ocrAddDependence(released, tally, 0, DB_MODE_NULL);
    ocrAddDependence(mirror_u64_guid(pv[P_COUNTERS]), tally, 1, DB_MODE_RW);
    ocrAddDependence(mirror_u64_guid(pv[P_OPTIONS]), tally, 3, DB_MODE_RO);
    ocrDbRelease(state);
    ocrAddDependence(state, tally, 2, DB_MODE_RW);
  }
  raise_point(entry_point(pv, closing, rank));
  return NULL_GUID;
}

/* A rank between two barriers with nothing to do between them: released from
 * the first, it enters the second. */
static ocrGuid_t relay_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 barrier = pv[P_BARRIER], rank = pv[P_RANK];
  report_test(pv, depv[1].ptr, depv[2].ptr);
  ocrEventDestroy(release_point(pv, barrier, rank));
  raise_point(entry_point(pv, barrier + 1, rank));
  return NULL_GUID;
}

/* One barrier, held by the root: once every rank has entered it, every rank
 * is released. */
static ocrGuid_t barrier_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 barrier = pv[P_BARRIER], nl = pv[P_NL];
  for (u64 q = 0; q < nl; ++q) ocrEventDestroy(entry_point(pv, barrier, q));
  for (u64 q = 0; q < nl; ++q) raise_point(release_point(pv, barrier, q));
  return NULL_GUID;
}

/* A rank's count, once the barrier that closes the last test has released
 * it.  The block is complete and released before it is handed to the task
 * that adds the counts up. */
static ocrGuid_t tally_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 rank = pv[P_RANK];
  if (rank == 0) PRINTF("\n");
  report_test(pv, depv[2].ptr, depv[3].ptr);
  ocrEventDestroy(release_point(pv, BARRIER_COUNT - 1, rank));
  ocrHint_t dh;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  ocrGuid_t db;
  u64 *count;
  ocrDbCreate(&db, (void **)&count, 2 * sizeof(u64), DB_PROP_NONE, &dh, NO_ALLOC);
  counters_t *c = depv[1].ptr;
  count[0] = atomic_load(&c->puts);
  count[1] = atomic_load(&c->gets);
  ocrDbRelease(db);
  ocrAddDependence(db, mirror_u64_guid(pv[P_FINAL]), (u32)rank, DB_MODE_RO);
  return NULL_GUID;
}

/* The sum over ranks, and the program's end. */
static ocrGuid_t final_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)pv;
  u64 puts = 0, gets = 0;
  for (u32 r = 0; r < depc; ++r) {
    const u64 *count = depv[r].ptr;
    puts += count[0];
    gets += count[1];
  }
  PRINTF("TRANSFERS_OK %llu %llu\n", (unsigned long long)puts, (unsigned long long)gets);
  for (u32 r = 0; r < depc; ++r) ocrDbDestroy(depv[r].guid);
  ocrShutdown();
  return NULL_GUID;
}

/* One rank's storage and the names of its slots, published to every rank;
 * its stream at the origin's default seed; its first turn, waiting on every
 * rank's names and on the barrier that opens the warm-up; and its entry into
 * that barrier. */
static ocrGuid_t driver_edt(u32 paramc, u64 *pv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)depc; (void)depv;
  u64 slots = pv[P_SLOTS], blocks = pv[P_BLOCKS], nl = pv[P_NL], rank = pv[P_RANK];
  ocrHint_t dh, h;
  mirror_rank_hint(&dh, rank, OCR_HINT_DB_T);
  mirror_rank_hint(&h, rank, OCR_HINT_EDT_T);

  ocrGuid_t counters; counters_t *c;
  ocrDbCreate(&counters, (void **)&c, sizeof(*c), DB_PROP_NONE, &dh, NO_ALLOC);
  atomic_init(&c->puts, 0); atomic_init(&c->gets, 0);
  for (u64 q = 0; q < MAX_RANKS; ++q) atomic_init(&c->waiting[q], 0);
  ocrGuid_t *storage = malloc((blocks ? blocks : 1) * sizeof(*storage));
  int failed = storage == NULL;
  for (u64 i = 0; !failed && i < blocks; ++i) {
    void *unused;
    u64 remaining = pv[P_STORAGE] - i * pv[P_BYTES];
    failed = ocrDbCreate(&storage[i], &unused, remaining < pv[P_BYTES] ? remaining : pv[P_BYTES],
                         DB_PROP_NO_ACQUIRE, &dh, NO_ALLOC) != 0;
  }
  if (failed) { free(storage); PRINTF("network_storage_hpx: out of memory\n"); ocrShutdown(); return NULL_GUID; }
  ocrGuid_t table, state; ocrGuid_t *names; state_t *st;
  ocrDbCreate(&table, (void **)&names, (blocks ? blocks : 1) * sizeof(*names), DB_PROP_NONE, &dh, NO_ALLOC);
  memcpy(names, storage, blocks * sizeof(*names)); free(storage);
  ocrDbCreate(&state, (void **)&st, sizeof(*st) + nl * blocks * sizeof(ocrGuid_t), DB_PROP_NONE, &dh, NO_ALLOC);
  for (u64 q = 0; q < nl; ++q) atomic_store(&c->waiting[q], 0);
  ocrDbRelease(counters); pv[P_COUNTERS] = mirror_guid_u64(counters);
  mirror_mt_seed(&st->gen, 5489u);
  ocrDbRelease(table);
  ocrDbRelease(state);
  for (u64 c = 0; c < nl; ++c) {
    ocrGuid_t pt = table_point(pv, rank, c);
    mirror_edge_open(pt);
    ocrEventSatisfy(pt, table);
  }

  u64 params[P_COUNT];
  memcpy(params, pv, sizeof params);
  params[P_TEST] = TEST_WARMUP;
  params[P_TURN] = 0;
  ocrGuid_t first;
  ocrEdtCreate(&first, mirror_u64_guid(pv[P_TURN_TPL]), P_COUNT, params, (u32)(nl + 2),
               NULL, EDT_PROP_NONE, &h, NULL);
  ocrGuid_t opened = release_point(pv, 0, rank);
  mirror_edge_open(opened);
  ocrAddDependence(opened, first, 0, DB_MODE_NULL);
  for (u64 q = 0; q < nl; ++q) {
    ocrGuid_t pt = table_point(pv, q, rank);
    mirror_edge_open(pt);
    ocrAddDependence(pt, first, (u32)(1 + q), DB_MODE_RO);
  }
  ocrAddDependence(state, first, (u32)(nl + 1), DB_MODE_RW);
  raise_point(entry_point(pv, 0, rank));
  return NULL_GUID;
}

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  void *argdb = depv[0].ptr;
  u64 argc = getArgc(argdb);
  u64 local_mb = 256, global_mb = 0, transfer_kb = 64, semaphore = 16, iters = 5;
  u64 distribution = 1;
  int all2all = 1, nolocal = 0, label_bad = 0;
  int bad = mirror_option_u64(argdb, argc, "--localMB", &local_mb) < 0
         || mirror_option_u64(argdb, argc, "--globalMB", &global_mb) < 0
         || mirror_option_u64(argdb, argc, "--transferKB", &transfer_kb) < 0
         || mirror_option_u64(argdb, argc, "--semaphore", &semaphore) < 0
         || mirror_option_u64(argdb, argc, "--iterations", &iters) < 0
         || mirror_option_bool(argdb, argc, "--all-to-all", &all2all) < 0
         || mirror_option_bool(argdb, argc, "--no-local", &nolocal) < 0
         || mirror_option_u64(argdb, argc, "--distribution", &distribution) < 0;
  /* Two options change nothing the program does: the semaphore is read only
   * by code the test does not compile, and the parcel type only labels its
   * report.  Both are still arguments, so a malformed one is still usage. */
  (void)semaphore;
  const char *network = mirror_option_raw(argdb, argc, "--parceltype", &label_bad);
  if (!network) network = "unknown";
  bad = bad || label_bad;

  u64 nl;
  ocrAffinityCount(AFFINITY_PD, &nl);
  if (global_mb > 0 && nl != 0) local_mb = global_mb / nl;

  /* The slot count is a quotient of two products of the arguments, so each
   * factor is bounded before its product is formed.  A zero transfer size is
   * a division by zero, and more slots than a signed 32-bit index holds
   * overflow the origin's slot index. */
  u64 bytes = 0, slots = 0, points = 0;
  int sane = !bad && nl != 0 && nl <= MAX_RANKS && !(nolocal && nl == 1)
          && transfer_kb != 0 && transfer_kb <= UINT64_MAX / 1024
          && local_mb <= UINT64_MAX / (1024 * 1024);
  if (sane) {
    bytes = transfer_kb * 1024;
    slots = local_mb * 1024 * 1024 / bytes;
    sane = slots <= (u64)INT32_MAX && point_space(nl, &points);
  }
  ocrGuid_t range = NULL_GUID;
  if (sane && ocrGuidRangeCreate(&range, points, GUID_USER_EVENT_STICKY) != 0) sane = 0;
  if (!sane) {
    PRINTF("network_storage_hpx: usage --localMB=M --globalMB=G --transferKB=K"
           " --semaphore=S --iterations=I --all-to-all=B --no-local=B"
           " --distribution=D [--parceltype=T] (K >= 1, fewer than 2^31 slots"
           " per rank, --no-local with at least two ranks, at most 16384 ranks,"
           " 18*ranks^2 < 2^32)\n");
    ocrShutdown();
    return NULL_GUID;
  }

  ocrGuid_t driver_tpl, turn_tpl, relay_tpl, barrier_tpl, tally_tpl, final_tpl;
  ocrGuid_t put_tpl, get_tpl, land_tpl, complete_tpl;
  ocrEdtTemplateCreate(&driver_tpl, driver_edt, P_COUNT, 0);
  ocrEdtTemplateCreate(&turn_tpl, turn_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&relay_tpl, relay_edt, P_COUNT, 3);
  ocrEdtTemplateCreate(&barrier_tpl, barrier_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&tally_tpl, tally_edt, P_COUNT, 4);
  ocrEdtTemplateCreate(&final_tpl, final_edt, P_COUNT, EDT_PARAM_UNK);
  ocrEdtTemplateCreate(&put_tpl, put_edt, P_COUNT, 3);
  ocrEdtTemplateCreate(&get_tpl, get_edt, P_COUNT, 2);

  ocrEdtTemplateCreate(&land_tpl, land_edt, P_COUNT, 3);
  ocrEdtTemplateCreate(&complete_tpl, complete_edt, P_COUNT, 3);
  ocrGuid_t options; char *label;
  ocrDbCreate(&options, (void **)&label, strlen(network) + 1, DB_PROP_NONE, NULL_HINT, NO_ALLOC);
  strcpy(label, network); ocrDbRelease(options);
  u64 pv[P_COUNT] = {0};
  pv[P_OPTIONS] = mirror_guid_u64(options);
  pv[P_LAND_TPL] = mirror_guid_u64(land_tpl);
  pv[P_COMPLETE_TPL] = mirror_guid_u64(complete_tpl);
  pv[P_STORAGE] = local_mb * 1024 * 1024;
  pv[P_BLOCKS] = pv[P_STORAGE] / bytes + (pv[P_STORAGE] % bytes != 0);
  pv[P_SLOTS] = slots; pv[P_BYTES] = bytes; pv[P_ITERS] = iters; pv[P_NL] = nl;
  pv[P_ALL2ALL] = (u64)all2all; pv[P_NOLOCAL] = (u64)nolocal;
  pv[P_RANDOM] = distribution == 0;
  pv[P_RANGE] = mirror_guid_u64(range);
  pv[P_TURN_TPL] = mirror_guid_u64(turn_tpl);
  pv[P_RELAY_TPL] = mirror_guid_u64(relay_tpl);
  pv[P_TALLY_TPL] = mirror_guid_u64(tally_tpl);
  pv[P_PUT_TPL] = mirror_guid_u64(put_tpl);
  pv[P_GET_TPL] = mirror_guid_u64(get_tpl);

  /* The end and the barriers belong to the root, and exist before any rank
   * can reach them. */
  ocrHint_t h0;
  mirror_rank_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t final;
  ocrEdtCreate(&final, final_tpl, P_COUNT, pv, (u32)nl, NULL, EDT_PROP_NONE, &h0, NULL);
  pv[P_FINAL] = mirror_guid_u64(final);
  for (u64 b = 0; b < BARRIER_COUNT; ++b) {
    pv[P_BARRIER] = b;
    ocrGuid_t e;
    ocrEdtCreate(&e, barrier_tpl, P_COUNT, pv, (u32)nl, NULL, EDT_PROP_NONE, &h0, NULL);
    for (u64 q = 0; q < nl; ++q) {
      ocrGuid_t pt = entry_point(pv, b, q);
      mirror_edge_open(pt);
      ocrAddDependence(pt, e, (u32)q, DB_MODE_NULL);
    }
  }
  pv[P_BARRIER] = 0;

  mirror_spmd_fork(driver_tpl, pv, P_COUNT, P_RANK, nl, 0, NULL);
  return NULL_GUID;
}
