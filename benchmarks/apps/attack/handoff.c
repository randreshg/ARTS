/*
 * handoff.c -- the event-ordered lattice engine the attack programs run on.
 *
 * L independent lattices advance through generations on one data block each.
 * Generation g's producer writes region (g % NREG), releases, and satisfies
 * ONE event that wakes both the generation's consumers (reading the region
 * just written) and the next producer (writing the NEXT region).  With at
 * least two regions the two sides run concurrently on byte-disjoint,
 * line-aligned spans, so the program is race-free at byte granularity by
 * event order alone -- yet a block-granular exclusive arm must serialize
 * them, and every generation forces a fresh read/write phase change:
 * producers and consumers rotate placement every generation, so there is
 * never a same-rank successor to amortize a resting grant, and the only
 * batching fodder a turn has is the fan-out the program asked for.  What this
 * measures is each configuration's cost of an interleaved handoff.
 *
 * Work is fixed: a lattice's producers run exactly the spec's generation
 * count and stop; no figure depends on a clock reaching a deadline.
 *
 * Timing: a lattice rotates ranks every generation, so no interval between
 * two of its generations can be measured without comparing two ranks' clocks.
 * The engine therefore reports no per-generation latency at all, and the two
 * figures it does report are single-clock by construction.  The release
 * bracket is one EDT's own pair of stamps.  The rate's denominator is the
 * span from the satisfy that releases every lattice to the collector's
 * arrival: both run on the rank that starts the job, the same placement
 * assumption the runtime's own end-to-end stamp rests on.  Its numerator is
 * the generations measured by ALL L lattices over that one span.
 *
 * Correctness: a producer is chain-ordered with every earlier producer, so
 * the region it overwrites must hold exactly the stamp from NREG generations
 * ago.  A consumer's snapshot is concurrent with LATER producers only; its
 * region stamp must be its own generation's or a later overwrite of the same
 * region slot (stamp = own (mod NREG), stamp >= own) -- anything else is a
 * lost or misdelivered write.  A failing consumer shuts the run down, so no
 * marker prints.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "attack/handoff.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- limits */
#define RH_MAX_LAT 256u
#define RH_NB 16u        /* coarse log2 buckets, 256ns .. >8ms */
#define RH_NAME_WORDS 4u /* the printed name, packed into params so it needs
                          * no pointer valid on the printing rank */
#define RH_NAME_CHARS (RH_NAME_WORDS * sizeof(u64))
#define RH_MIN_REGION 4160u /* stamp line + the fill span */

/* result-block layout, in u64 words */
#define RH_F_GENS_MEAS 0
#define RH_F_VIOL 1
#define RH_F_SUM_REL 2
#define RH_F_HREL 3
#define RH_RESULT_WORDS (RH_F_HREL + RH_NB)

/* paramv layout (producer, consumer and finisher share one shape) */
enum {
  P_NAME, /* RH_NAME_WORDS words */
  P_LAT = P_NAME + RH_NAME_WORDS,
  P_GEN,
  P_PREV_EVT, /* the event that woke this EDT; the producer it wakes destroys
               * it (a sticky event is torn down explicitly; 0 = the shared
               * start trigger, which the collector destroys) */
  P_PROD_TPL,
  P_CONS_TPL,
  P_FIN_TPL,
  P_COLLECTOR,
  P_SLOT,
  P_RESULT_DB,
  P_LATCH,
  P_DB,
  P_NREG,
  P_BYTES,
  P_EHW,
  P_EHR,
  P_ETH,
  P_FANOUT,
  P_PDS,
  P_KGENS, /* exact generation count per lattice */
  M_GENS_MEAS,
  M_VIOL,
  M_SUM_REL,
  M_HREL, /* RH_NB words */
  P_COUNT = M_HREL + RH_NB
};

/* collector params; its dependences are the L result blocks then the block
 * carrying the span's opening stamp, all RO */
enum {
  C_NAME, /* RH_NAME_WORDS words */
  C_PDS = C_NAME + RH_NAME_WORDS,
  C_L,
  C_NREG,
  C_BYTES,
  C_EHW,
  C_EHR,
  C_ETH,
  C_FANOUT,
  C_TRIGGER,
  C_COUNT
};

static inline u64 now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static inline void spin_ns(u64 us) {
  if (us == 0) return;
  u64 t0 = now_ns(), tgt = us * 1000ull;
  while (now_ns() - t0 < tgt)
    ;
}

static inline unsigned int hbucket(u64 ns) {
  u64 v = ns >> 8;
  if (v == 0) return 0;
  unsigned int b = 63u - (unsigned int)__builtin_clzll(v);
  return b >= RH_NB ? RH_NB - 1 : b;
}

static double hbucket_mid_us(unsigned int b) {
  double lo = (double)(256ull << b);
  return (b == 0 ? lo * 0.5 : lo * 1.5) / 1000.0;
}

static double hist_quantile_us(const u64 *h, double q) {
  u64 total = 0;
  for (unsigned int b = 0; b < RH_NB; b++) total += h[b];
  if (total == 0) return -1.0;
  double target = q * (double)total;
  u64 cum = 0;
  for (unsigned int b = 0; b < RH_NB; b++) {
    cum += h[b];
    if ((double)cum >= target) return hbucket_mid_us(b);
  }
  return hbucket_mid_us(RH_NB - 1);
}

static inline u64 guid_u64(ocrGuid_t g) { return (u64)g.guid; }
static inline ocrGuid_t u64_guid(u64 v) {
  ocrGuid_t g;
  g.guid = (intptr_t)v;
  return g;
}

static void pd_hint(ocrHint_t *h, u64 pd, ocrHintType_t type) {
  ocrHintInit(h, type);
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, pd, &aff);
  ocrSetHintValue(h,
                  type == OCR_HINT_EDT_T ? OCR_HINT_EDT_AFFINITY
                                         : OCR_HINT_DB_AFFINITY,
                  ocrAffinityToHintValue(aff));
}

static void name_pack(u64 *dst, const char *name) {
  char buf[RH_NAME_CHARS];
  size_t n = strlen(name);
  memset(buf, 0, sizeof(buf));
  memcpy(buf, name, n < sizeof(buf) ? n : sizeof(buf) - 1);
  memcpy(dst, buf, sizeof(buf));
}

/* out holds RH_NAME_CHARS + 1 bytes */
static void name_unpack(char *out, const u64 *src) {
  memcpy(out, src, RH_NAME_CHARS);
  out[RH_NAME_CHARS] = '\0';
}

/* words per region: the per-region span is floored to a 64-byte multiple, so
 * a generation's regions never share a line with its neighbours' */
static inline u64 region_words(u64 bytes, u64 nreg) {
  return ((bytes / nreg) & ~63ull) / sizeof(u64);
}

/* --------------------------------------------------------------- producer */
static ocrGuid_t prod_edt(u32 paramc, u64 *paramv, u32 depc,
                          ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 lat = paramv[P_LAT], g = paramv[P_GEN];
  u64 nreg = paramv[P_NREG], pds = paramv[P_PDS];
  if (paramv[P_PREV_EVT]) ocrEventDestroy(u64_guid(paramv[P_PREV_EVT]));

  u64 rw = region_words(paramv[P_BYTES], nreg);
  u64 *base = (u64 *)depv[0].ptr + (g % nreg) * rw;
  /* chain-ordered exact check: this region slot was last written NREG
   * generations ago */
  u64 old = __atomic_load_n((const uint64_t *)&base[0], __ATOMIC_RELAXED);
  u64 want_old = g >= nreg ? g + 1 - nreg : 0;
  if (old != want_old) paramv[M_VIOL]++;
  __atomic_store_n((uint64_t *)&base[0], g + 1, __ATOMIC_RELAXED);
  u64 fill = rw < 512 ? rw : 512;
  for (u64 i = 8; i < fill; i++) base[i] = g + 1;

  spin_ns(paramv[P_EHW]);
  u64 t1 = now_ns();
  ocrDbRelease(depv[0].guid);
  u64 rel = now_ns() - t1; /* one EDT's own clock, start to finish */

  paramv[M_SUM_REL] += rel;
  paramv[M_HREL + hbucket(rel)]++;
  paramv[M_GENS_MEAS]++;

  if (g + 1 >= paramv[P_KGENS]) {
    ocrHint_t h;
    pd_hint(&h, (lat + g) % pds, OCR_HINT_EDT_T);
    ocrGuid_t fin;
    ocrEdtCreate(&fin, u64_guid(paramv[P_FIN_TPL]), P_COUNT, paramv, 2, NULL,
                 EDT_PROP_NONE, &h, NULL);
    ocrAddDependence(u64_guid(paramv[P_RESULT_DB]), fin, 0, DB_MODE_RW);
    ocrAddDependence(u64_guid(paramv[P_LATCH]), fin, 1, DB_MODE_NULL);
    ocrEventSatisfySlot(u64_guid(paramv[P_LATCH]), NULL_GUID,
                        OCR_EVENT_LATCH_DECR_SLOT); /* the guard */
    return NULL_GUID;
  }

  /* FANOUT consumers for THIS generation and one successor for the next, all
   * woken by one event.  The successor takes the rank one past this producer
   * and the consumers the ranks after it, so a consumer shares the
   * successor's rank only where the fan-out spans every rank.  The think spin
   * precedes the creates; the handoff itself is gated by the satisfy, which
   * stays last. */
  spin_ns(paramv[P_ETH]);
  ocrGuid_t ev;
  ocrEventCreate(&ev, OCR_EVENT_STICKY_T, EVT_PROP_NONE);

  u64 cparams[P_COUNT];
  memcpy(cparams, paramv, sizeof(cparams));
  cparams[P_PREV_EVT] = 0; /* only the producer it wakes destroys the event */
  for (u64 k = 0; k < paramv[P_FANOUT]; k++) {
    ocrEventSatisfySlot(u64_guid(paramv[P_LATCH]), NULL_GUID,
                        OCR_EVENT_LATCH_INCR_SLOT); /* before the consumer
                                                     * exists to decrement it */
    ocrHint_t ch;
    pd_hint(&ch, (lat + g + 2 + k) % pds, OCR_HINT_EDT_T);
    ocrGuid_t cons;
    ocrEdtCreate(&cons, u64_guid(paramv[P_CONS_TPL]), P_COUNT, cparams, 2,
                 NULL, EDT_PROP_NONE, &ch, NULL);
    ocrAddDependence(u64_guid(paramv[P_DB]), cons, 0, DB_MODE_RO);
    ocrAddDependence(ev, cons, 1, DB_MODE_NULL);
  }

  u64 nparams[P_COUNT];
  memcpy(nparams, paramv, sizeof(nparams));
  nparams[P_GEN] = g + 1;
  nparams[P_PREV_EVT] = guid_u64(ev);
  ocrHint_t nh;
  pd_hint(&nh, (lat + g + 1) % pds, OCR_HINT_EDT_T);
  ocrGuid_t succ;
  ocrEdtCreate(&succ, u64_guid(paramv[P_PROD_TPL]), P_COUNT, nparams, 2, NULL,
               EDT_PROP_NONE, &nh, NULL);
  ocrAddDependence(u64_guid(paramv[P_DB]), succ, 0, DB_MODE_RW);
  ocrAddDependence(ev, succ, 1, DB_MODE_NULL);

  ocrEventSatisfy(ev, NULL_GUID);
  return NULL_GUID;
}

/* --------------------------------------------------------------- consumer */
static ocrGuid_t cons_edt(u32 paramc, u64 *paramv, u32 depc,
                          ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 g = paramv[P_GEN], nreg = paramv[P_NREG];
  u64 rw = region_words(paramv[P_BYTES], nreg);
  const u64 *base = (const u64 *)depv[0].ptr + (g % nreg) * rw;
  u64 stamp = __atomic_load_n((const uint64_t *)&base[0], __ATOMIC_RELAXED);
  /* concurrent-with-later-writers legality: own stamp or a later overwrite
   * of the same region slot */
  if (stamp < g + 1 || (stamp - (g + 1)) % nreg != 0) {
    char name[RH_NAME_CHARS + 1];
    name_unpack(name, &paramv[P_NAME]);
    PRINTF("%s-ORACLE-FAIL lat=%lu gen=%lu stamp=%lu\n", name,
           (unsigned long)paramv[P_LAT], (unsigned long)g,
           (unsigned long)stamp);
    ocrShutdown(); /* idempotent: several failing consumers are fine */
    return NULL_GUID;
  }
  u64 acc = 0;
  u64 fill = rw < 512 ? rw : 512;
  for (u64 i = 8; i < fill; i++) acc += base[i];
  if (acc == 0xDEADBEEFDEADBEEFull) PRINTF("");
  spin_ns(paramv[P_EHR]);
  ocrDbRelease(depv[0].guid);
  ocrEventSatisfySlot(u64_guid(paramv[P_LATCH]), NULL_GUID,
                      OCR_EVENT_LATCH_DECR_SLOT);
  return NULL_GUID;
}

/* --------------------------------------------------------------- finisher */
/* Runs once its lattice's latch has fired, so every consumer of every
 * generation has released.  The latch is not destroyed here: a latch carries
 * the persistence of a single-fire event and is reclaimed when its post-slot
 * triggers, which is the edge this EDT waited on. */
static ocrGuid_t fin_edt(u32 paramc, u64 *paramv, u32 depc,
                         ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 *r = (u64 *)depv[0].ptr;
  r[RH_F_GENS_MEAS] = paramv[M_GENS_MEAS];
  r[RH_F_VIOL] = paramv[M_VIOL];
  r[RH_F_SUM_REL] = paramv[M_SUM_REL];
  for (unsigned int b = 0; b < RH_NB; b++)
    r[RH_F_HREL + b] = paramv[M_HREL + b];
  /* release BEFORE wiring: the satisfy travels to the collector's rank and
   * its acquire runs there asynchronously -- nothing else orders that read
   * behind this EDT's epilogue */
  ocrGuid_t rdb = depv[0].guid;
  ocrDbRelease(rdb);
  ocrAddDependence(rdb, u64_guid(paramv[P_COLLECTOR]), (u32)paramv[P_SLOT],
                   DB_MODE_RO);
  return NULL_GUID;
}

/* --------------------------------------------------------------- collector */
static ocrGuid_t collector_edt(u32 paramc, u64 *paramv, u32 depc,
                               ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  /* depv: the L result blocks, then the block carrying the opening stamp.
   * The span closes on the clock that opened it -- both EDTs run on the rank
   * that starts the job. */
  u64 L = paramv[C_L];
  u64 span_ns = now_ns() - *(const u64 *)depv[L].ptr;
  char name[RH_NAME_CHARS + 1];
  name_unpack(name, &paramv[C_NAME]);

  /* the start trigger lingers until destroyed; every lattice has consumed it
   * by the time the last result lands here */
  ocrEventDestroy(u64_guid(paramv[C_TRIGGER]));

  u64 prel[RH_NB];
  memset(prel, 0, sizeof(prel));
  u64 gens_meas = 0, viol = 0;
  double sum_rel = 0;
  for (u64 l = 0; l < L; l++) {
    const u64 *r = (const u64 *)depv[l].ptr;
    gens_meas += r[RH_F_GENS_MEAS];
    viol += r[RH_F_VIOL];
    sum_rel += (double)r[RH_F_SUM_REL];
    for (unsigned int b = 0; b < RH_NB; b++) prel[b] += r[RH_F_HREL + b];
  }
  if (viol) {
    PRINTF("%s-ORACLE-FAIL kind=producer viol=%lu\n", name,
           (unsigned long)viol);
    ocrShutdown();
    return NULL_GUID;
  }
  if (span_ns == 0) { /* a rate needs a span; a zero one is a broken clock,
                       * not a fast run */
    PRINTF("%s-ORACLE-FAIL kind=span gens=%lu\n", name,
           (unsigned long)gens_meas);
    ocrShutdown();
    return NULL_GUID;
  }

  /* fixed work names no window: the denominator is the span every lattice
   * ran inside, and the numerator the generations all L of them measured */
  double span_s = (double)span_ns / 1e9;
  PRINTF("%s OK PDS=%lu L=%lu NREG=%lu BYTES=%lu EHW=%lu EHR=%lu ETH=%lu "
         "FANOUT=%lu GENS=%lu GXPUT=%.1f "
         "WREL_P50=%.2f WREL_P99=%.2f WREL_MEAN=%.2f\n",
         name, (unsigned long)paramv[C_PDS], (unsigned long)L,
         (unsigned long)paramv[C_NREG], (unsigned long)paramv[C_BYTES],
         (unsigned long)paramv[C_EHW], (unsigned long)paramv[C_EHR],
         (unsigned long)paramv[C_ETH], (unsigned long)paramv[C_FANOUT],
         (unsigned long)gens_meas, (double)gens_meas / span_s,
         hist_quantile_us(prel, 0.50), hist_quantile_us(prel, 0.99),
         gens_meas ? sum_rel / 1000.0 / (double)gens_meas : -1.0);
  ocrShutdown();
  return NULL_GUID;
}

/* ------------------------------------------------------------ entry point */
ocrGuid_t attack_handoff_run(const attack_handoff_spec_t *spec, u64 pd_count) {
  const char *name = spec->name ? spec->name : "ATTACK";
  if (pd_count == 0) pd_count = 1;
  u64 L = spec->lattices, nreg = spec->regions, gens = spec->gens;
  /* fewer than two regions would put a generation's consumers and the next
   * producer on the same bytes, which no event order separates */
  if (L == 0 || L > RH_MAX_LAT || nreg < 2 || gens == 0) {
    PRINTF("%s-ORACLE-FAIL kind=args L=%lu NREG=%lu GENS=%lu\n", name,
           (unsigned long)L, (unsigned long)nreg, (unsigned long)gens);
    ocrShutdown();
    return NULL_GUID;
  }
  u64 bytes = spec->bytes;
  if (region_words(bytes, nreg) * sizeof(u64) < RH_MIN_REGION)
    bytes = nreg * 4224; /* keep every region a full stamp + fill span */
  /* at least one consumer per generation, and never more than there are
   * ranks: one rank is a legitimate cell, where every consumer is local to
   * the next producer and an exclusive arm still blocks it behind them */
  u64 fanout = spec->fanout ? spec->fanout : 1;
  if (fanout > pd_count) fanout = pd_count;

  ocrGuid_t trigger;
  ocrEventCreate(&trigger, OCR_EVENT_STICKY_T, EVT_PROP_NONE);

  /* the starter and every successor have the same shape (block + the event
   * that opens their generation), so one producer template serves both */
  ocrGuid_t prod_tpl, cons_tpl, fin_tpl, coll_tpl;
  ocrEdtTemplateCreate(&prod_tpl, prod_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&cons_tpl, cons_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&fin_tpl, fin_edt, P_COUNT, 2);
  ocrEdtTemplateCreate(&coll_tpl, collector_edt, C_COUNT, (u32)(L + 1));

  u64 cparams[C_COUNT];
  memset(cparams, 0, sizeof(cparams));
  name_pack(&cparams[C_NAME], name);
  cparams[C_PDS] = pd_count;
  cparams[C_L] = L;
  cparams[C_NREG] = nreg;
  cparams[C_BYTES] = bytes;
  cparams[C_EHW] = spec->hold_w_us;
  cparams[C_EHR] = spec->hold_r_us;
  cparams[C_ETH] = spec->think_us;
  cparams[C_FANOUT] = fanout;
  cparams[C_TRIGGER] = guid_u64(trigger);

  ocrHint_t h0;
  pd_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t collector;
  ocrEdtCreate(&collector, coll_tpl, C_COUNT, cparams, (u32)(L + 1), NULL,
               EDT_PROP_NONE, &h0, NULL);

  for (u64 l = 0; l < L; l++) {
    u64 home = l % pd_count;
    ocrHint_t dh;
    pd_hint(&dh, home, OCR_HINT_DB_T);
    ocrGuid_t db;
    u64 *p;
    ocrDbCreate(&db, (void **)&p, bytes, DB_PROP_NONE, &dh, NO_ALLOC);
    memset(p, 0, bytes);
    ocrDbRelease(db); /* create takes an implicit hold; under an exclusive arm
                       * every first generation would park behind this EDT */

    ocrGuid_t rdb;
    u64 *rp;
    ocrDbCreate(&rdb, (void **)&rp, RH_RESULT_WORDS * sizeof(u64),
                DB_PROP_NONE, &dh, NO_ALLOC);
    memset(rp, 0, RH_RESULT_WORDS * sizeof(u64));
    ocrDbRelease(rdb);

    ocrGuid_t latch;
    ocrEventCreate(&latch, OCR_EVENT_LATCH_T, EVT_PROP_NONE);
    ocrEventSatisfySlot(latch, NULL_GUID,
                        OCR_EVENT_LATCH_INCR_SLOT); /* the guard: the last
                                                     * producer decrements it */

    u64 params[P_COUNT];
    memset(params, 0, sizeof(params));
    name_pack(&params[P_NAME], name);
    params[P_LAT] = l;
    params[P_PROD_TPL] = guid_u64(prod_tpl);
    params[P_CONS_TPL] = guid_u64(cons_tpl);
    params[P_FIN_TPL] = guid_u64(fin_tpl);
    params[P_COLLECTOR] = guid_u64(collector);
    params[P_SLOT] = l;
    params[P_RESULT_DB] = guid_u64(rdb);
    params[P_LATCH] = guid_u64(latch);
    params[P_DB] = guid_u64(db);
    params[P_NREG] = nreg;
    params[P_BYTES] = bytes;
    params[P_EHW] = spec->hold_w_us;
    params[P_EHR] = spec->hold_r_us;
    params[P_ETH] = spec->think_us;
    params[P_FANOUT] = fanout;
    params[P_PDS] = pd_count;
    params[P_KGENS] = gens;

    ocrHint_t eh;
    pd_hint(&eh, home, OCR_HINT_EDT_T);
    ocrGuid_t starter;
    ocrEdtCreate(&starter, prod_tpl, P_COUNT, params, 2, NULL, EDT_PROP_NONE,
                 &eh, NULL);
    ocrAddDependence(db, starter, 0, DB_MODE_RW);
    ocrAddDependence(trigger, starter, 1, DB_MODE_NULL);
  }

  /* The rate's span opens with the satisfy below and closes when the
   * collector runs, both on this rank.  The opening stamp reaches the
   * collector as a block because an EDT's params are sealed at its create,
   * and the collector's create must precede the lattices that name it. */
  ocrHint_t sh;
  pd_hint(&sh, 0, OCR_HINT_DB_T);
  ocrGuid_t startdb;
  u64 *stamp;
  ocrDbCreate(&startdb, (void **)&stamp, sizeof(u64), DB_PROP_NONE, &sh,
              NO_ALLOC);
  *stamp = now_ns();
  ocrDbRelease(startdb);
  ocrEventSatisfy(trigger, NULL_GUID);
  ocrAddDependence(startdb, collector, (u32)L, DB_MODE_RO);
  return NULL_GUID;
}
