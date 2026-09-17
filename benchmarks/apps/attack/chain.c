/*
 * chain.c -- the closed-loop chain engine the attack programs run on.
 *
 * A chain is a self-perpetuating EDT sequence pinned to one PD: each EDT
 * acquires its target block in the chain's mode (RO for readers, RW for
 * writers), touches a bounded slice of the payload, optionally spins while
 * holding, releases EXPLICITLY, spins its think time, and creates its
 * successor.  The explicit release forces a coherence zero edge between
 * consecutive ops of one chain; overlap ACROSS chains on one node is
 * deliberate -- per-node batching is a protocol property, and the population
 * per rank is what controls it.
 *
 * Work is fixed: a chain runs exactly the op count its class was given, so no
 * figure here depends on a clock reaching a deadline.  A chain whose op count
 * runs away past AC_G_CAP is a failed cell, a runaway-loop safety net that is
 * not a termination path.
 *
 * Timing: a chain never leaves its PD, so all its timestamps come from one
 * monotonic clock; nothing compares clocks across PDs.  Each op yields
 *   acquire = this EDT's entry - the predecessor's post-think stamp
 *             (create + dispatch + acquire, incl. any protocol wait),
 *   release = the bracket around the explicit release
 *             (blocking arms pay their round/publish here),
 *   total   = entry-to-entry minus the realized spins
 *             (the class's whole per-op overhead; quantiles come from this).
 * Throughput denominators are the elapsed span on that one clock, never a
 * requested window: per chain, (entry of its last op) - (its trigger arrival),
 * and the reported span is the max over chains -- the slowest chain's wall.
 * A writer ring moves between PDs by construction, so it records none of these
 * figures: only its op count, which needs no clock.
 *
 * Every per-chain counter, sample sum and histogram lives in that chain's own
 * paramv: one EDT owns the vector, updates it with plain stores, and hands a
 * copy to its successor, so no memory is shared between EDTs and the figures
 * survive a hint the runtime chose to ignore.  The last EDT of a chain copies
 * the vector into the chain's result block, which the collector pools.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "attack/chain.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- limits */
#define AC_OCT 28u       /* octaves: [2^8, 2^36) ns */
#define AC_MANT 4u       /* linear sub-buckets per octave (19-25% width) */
#define AC_MANT_BITS 2u  /* log2(AC_MANT) */
#define AC_NB (AC_OCT * AC_MANT)
#define AC_SHIFT_MIN 8u  /* floor quantum: samples below it share bucket 0 */
#define AC_G_CAP 5000000ull
#define AC_NAME_WORDS 4u /* the printed name, packed into the collector's
                          * params so it needs no pointer valid on its rank */
#define AC_NAME_CHARS (AC_NAME_WORDS * sizeof(u64))
#define AC_MIN_BYTES 4160u /* counter line + the first-4KB touch region */

/* result-block layout, in u64 words */
#define AC_F_ROLE 0
#define AC_F_OPS_TOTAL 1
#define AC_F_OPS_MEAS 2
#define AC_F_VIOL 3
#define AC_F_CAP 4
#define AC_F_SUM_ACQ 5
#define AC_F_SUM_REL 6
#define AC_F_SUM_TOT 7
#define AC_F_USEFUL 8
#define AC_F_TRIG_NS 9
#define AC_F_LAST_NS 10
#define AC_F_HIST 11
#define AC_RESULT_WORDS (AC_F_HIST + 3u * AC_NB)

/* paramv layout (all chain EDTs, one shape) */
enum {
  P_ROLE, P_CHAIN, P_PD, P_DBIDX, P_TRIG_NS, P_PREV_STAMP, P_PREV_ENTRY,
  P_PREV_SPIN, P_LCG, P_STEP_TPL, P_FIN_TPL, P_COLLECTOR, P_SLOT, P_RESULT_DB,
  P_BLOCKS, P_EHOLD, P_ETHINK, P_BYTES, P_KOPS, P_COUPLED, P_LASTOP,
  P_OPS_TOTAL, P_OPS_MEAS, P_VIOL, P_CAP, P_USEFUL,
  P_SUM_ACQ, P_SUM_REL, P_SUM_TOT,
  /* writer ring, set on ring chains only (P_RING_LEN 0 everywhere else): the
   * length of the contiguous slot block it cycles, and the placement rule's
   * two numbers, so a step can reproduce the rule on a rank that never saw the
   * spec.  P_CHAIN is the ring's index, so its block starts at index x length. */
  P_RING_LEN, P_RING_BASE, P_RING_SPAN,
  P_LAST_SEEN, /* ATTACK_CHAIN_MAX_BLOCKS words */
  P_HIST = P_LAST_SEEN + ATTACK_CHAIN_MAX_BLOCKS, /* 3 x AC_NB words */
  P_DB0 = P_HIST + 3 * AC_NB,
  P_COUNT = P_DB0 + ATTACK_CHAIN_MAX_BLOCKS
};

/* collector params; its dependences are A result blocks then C_BLOCKS data
 * blocks (RO), so it runs only after every chain's last release */
enum {
  C_NAME, /* AC_NAME_WORDS words */
  C_PDS = C_NAME + AC_NAME_WORDS,
  C_R, C_W, C_EHR, C_EHW, C_ETH, C_ETHW, C_BYTES,
  C_BLOCKS,     /* data-block dependences following the A result blocks */
  C_RHO_X10K,
  C_SOLE_OWNER, /* 1 = one block per chain, answerable to that chain alone */
  C_FIRST_TOUCH,
  C_TRIGGER,
  C_RING, /* number of writer rings; 0 = a chain per writer slot */
  C_COUNT
};

static inline u64 now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* busy spin; returns realized duration so the total-overhead figure can
 * subtract what was actually spun, not what was asked for */
static inline u64 spin_ns(u64 us) {
  if (us == 0) return 0;
  u64 t0 = now_ns(), tgt = us * 1000ull;
  while (now_ns() - t0 < tgt)
    ;
  return now_ns() - t0;
}

/* octave + AC_MANT-way linear mantissa over v = ns >> AC_SHIFT_MIN: the first
 * AC_MANT buckets are linear (v < AC_MANT), after that the bucket is the
 * octave of v times AC_MANT plus the AC_MANT_BITS bits below v's leading one.
 * Relative bucket width is constant above the linear region, which is what
 * lets a margin be judged against bucket resolution rather than an octave. */
static inline void hist_add(u64 *h, u64 ns) {
  u64 v = ns >> AC_SHIFT_MIN;
  unsigned int b;
  if (v < AC_MANT) {
    b = (unsigned int)v;
  } else {
    unsigned int hb = 63u - (unsigned int)__builtin_clzll(v);
    unsigned int mant =
        (unsigned int)((v >> (hb - AC_MANT_BITS)) & (AC_MANT - 1));
    b = (hb - AC_MANT_BITS + 1) * AC_MANT + mant;
    if (b >= AC_NB) b = AC_NB - 1;
  }
  h[b]++;
}

/* mid of one bucket, in ns (inverse of hist_add's binning) */
static double bucket_mid_ns(unsigned int b) {
  double unit = (double)(1ull << AC_SHIFT_MIN);
  if (b < AC_MANT) return ((double)b + 0.5) * unit;
  unsigned int oct = b / AC_MANT, mant = b % AC_MANT;
  double base = (double)(1ull << (oct + AC_MANT_BITS - 1)) * unit;
  double lo = base * (1.0 + (double)mant / AC_MANT);
  double hi = base * (1.0 + (double)(mant + 1) / AC_MANT);
  return (lo + hi) * 0.5;
}

static double hist_quantile_us(const u64 *h, double q) {
  u64 total = 0;
  for (unsigned int b = 0; b < AC_NB; b++) total += h[b];
  if (total == 0) return -1.0;
  double target = q * (double)total;
  u64 cum = 0;
  for (unsigned int b = 0; b < AC_NB; b++) {
    cum += h[b];
    if ((double)cum >= target) return bucket_mid_ns(b) / 1000.0;
  }
  return bucket_mid_ns(AC_NB - 1) / 1000.0;
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
  char buf[AC_NAME_CHARS];
  size_t n = strlen(name);
  memset(buf, 0, sizeof(buf));
  memcpy(buf, name, n < sizeof(buf) ? n : sizeof(buf) - 1);
  memcpy(dst, buf, sizeof(buf));
}

/* out holds AC_NAME_CHARS + 1 bytes */
static void name_unpack(char *out, const u64 *src) {
  memcpy(out, src, AC_NAME_CHARS);
  out[AC_NAME_CHARS] = '\0';
}

/* touch a bounded slice: the counter's own line, the first 4 KB, and one word
 * per 64 KB beyond -- app-side memory traffic stays near-constant while the
 * protocol still moves whole-block payloads.  The counter word is atomic
 * (same-PD RW writers are concurrent by contract; a write-through home
 * publishes into the served buffer in place, so a reader load can otherwise
 * tear). */
static u64 touch(void *ptr, u64 bytes, int is_writer, u64 *stamp_out) {
  volatile u64 *p = (volatile u64 *)ptr;
  u64 words = bytes / sizeof(u64);
  u64 v;
  if (is_writer) {
    v = __atomic_fetch_add((uint64_t *)&p[0], 1, __ATOMIC_RELAXED) + 1;
  } else {
    v = __atomic_load_n((const uint64_t *)&p[0], __ATOMIC_RELAXED);
  }
  u64 acc = 0;
  u64 first = bytes < 4096 ? words : 4096 / sizeof(u64);
  for (u64 i = 8; i < first; i++) { /* skip the counter's 64-byte line */
    if (is_writer)
      p[i] = v;
    else
      acc += p[i];
  }
  for (u64 off = 65536 / sizeof(u64); off < words; off += 65536 / sizeof(u64)) {
    if (is_writer)
      p[off] = v;
    else
      acc += p[off];
  }
  if (acc == 0xDEADBEEFDEADBEEFull) PRINTF("");
  *stamp_out = v;
  return acc;
}

/* ------------------------------------------------------------ chain step */
/* The per-op value oracle.  Shared block set: a reader's view of a block never
 * goes backwards, and a view that advanced since this chain's last visit is
 * one consumed version.  Sole owner: nobody else touches the block, so a
 * writer's counter advances by exactly one per op and a reader's never moves
 * at all. */
static inline void check_value(u64 *paramv, u64 role, u64 dbi, u64 stamp,
                               int sole_owner) {
  u64 *last = &paramv[P_LAST_SEEN + dbi];
  if (sole_owner) {
    if (role) {
      if (stamp != *last + 1) paramv[P_VIOL]++;
      *last = stamp; /* the writer's own count is what the next op expects */
    } else if (stamp != *last) {
      paramv[P_VIOL]++; /* a reader's block never moves: every wrong read counts */
    }
    return;
  }
  if (role) return;
  if (stamp < *last) {
    paramv[P_VIOL]++;
    return;
  }
  if (stamp > *last) paramv[P_USEFUL]++;
  *last = stamp;
}

static ocrGuid_t step_run(u64 *paramv, ocrEdtDep_t depv[], int sole_owner) {
  u64 t0 = now_ns();
  u64 role = paramv[P_ROLE];
  u64 dbi = paramv[P_DBIDX]; /* index of the block THIS op acquired */
  u64 blocks = paramv[P_BLOCKS];
  u64 ring = paramv[P_RING_LEN]; /* non-zero only on a rotating writer chain */
  /* the span anchors here; a ring spans no single clock and reports none */
  if (!ring && paramv[P_TRIG_NS] == 0) paramv[P_TRIG_NS] = t0;

  /* the op: touch, hold, explicit release (the zero edge), think */
  u64 stamp = 0;
  touch(depv[0].ptr, paramv[P_BYTES], role != 0, &stamp);
  check_value(paramv, role, dbi, stamp, sole_owner);
  u64 spun = spin_ns(paramv[P_EHOLD]);
  u64 t1 = now_ns();
  ocrDbRelease(depv[0].guid);
  u64 t2 = now_ns();
  spun += spin_ns(paramv[P_ETHINK]);

  /* record: acquire and release belong to THIS op; the total closes the
   * PREVIOUS op (entry-to-entry minus its realized spins).  A ring step's
   * predecessor ran on another rank, so every interval here would be a
   * difference of two clocks: the ring keeps its sums and histograms empty
   * and the collector reports the class as it reports an absent one. */
  if (!ring) {
    if (paramv[P_PREV_STAMP]) {
      u64 acq = t0 - paramv[P_PREV_STAMP];
      hist_add(&paramv[P_HIST], acq);
      paramv[P_SUM_ACQ] += acq;
    }
    u64 rel = t2 - t1;
    hist_add(&paramv[P_HIST + AC_NB], rel);
    paramv[P_SUM_REL] += rel;
    if (paramv[P_PREV_ENTRY]) {
      u64 cyc = t0 - paramv[P_PREV_ENTRY];
      u64 tot = cyc > paramv[P_PREV_SPIN] ? cyc - paramv[P_PREV_SPIN] : 0;
      hist_add(&paramv[P_HIST + 2 * AC_NB], tot);
      paramv[P_SUM_TOT] += tot;
    }
    paramv[P_OPS_MEAS]++;
  }
  u64 total = ++paramv[P_OPS_TOTAL];

  /* A coupled consumer's work is the produced STREAM, not raw reads: it
   * terminates on having SEEN the stream's final counter value (the
   * producers' exact-count writes make that value known a priori).
   * Terminating on raw reads would dissolve the coupling; terminating on
   * observed-advance counts can strand a consumer that missed overwritten
   * versions after production ends -- the final value alone is durable, so
   * awaiting it is both coupled and stall-free. */
  u64 progress = (!role && paramv[P_COUPLED]) ? paramv[P_LAST_SEEN + dbi]
                                              : total;
  int done = progress >= paramv[P_KOPS];
  if (total >= AC_G_CAP) {
    paramv[P_CAP] = 1;
    done = 1;
  }

  if (done) {
    if (!ring) paramv[P_LASTOP] = t0;
    ocrHint_t hf; /* the finisher belongs to the rank this step ran on */
    pd_hint(&hf, paramv[P_PD], OCR_HINT_EDT_T);
    ocrGuid_t fin;
    ocrEdtCreate(&fin, u64_guid(paramv[P_FIN_TPL]), P_COUNT, paramv, 1, NULL,
                 EDT_PROP_NONE, &hf, NULL);
    ocrAddDependence(u64_guid(paramv[P_RESULT_DB]), fin, 0, DB_MODE_RW);
    return NULL_GUID;
  }

  /* next block: seeded walk over the block set (identical across arms); a
   * coupled chain and a single-block chain stay put */
  u64 next_dbi = dbi;
  if (blocks > 1 && !paramv[P_COUPLED]) {
    paramv[P_LCG] =
        paramv[P_LCG] * 6364136223846793005ull + 1442695040888963407ull;
    next_dbi = (paramv[P_LCG] >> 33) % blocks;
  }
  /* the successor inherits this EDT's own vector: paramv is private to this
   * instance and ocrEdtCreate copies it, so the handoff fields are set in
   * place and nothing below reads what they replaced */
  paramv[P_DBIDX] = next_dbi;
  if (ring) {
    /* the turn goes to the rank the placement rule gives this ring's next
     * slot, the next in its block; the successor is created after this step's
     * release, so a ring holds one write at a time and the class at most one
     * per ring */
    u64 slot = paramv[P_CHAIN] * ring + total % ring;
    paramv[P_PD] = paramv[P_RING_BASE] + slot % paramv[P_RING_SPAN];
  } else {
    u64 t3 = now_ns();
    paramv[P_PREV_STAMP] = t3;
    paramv[P_PREV_ENTRY] = t0;
    paramv[P_PREV_SPIN] = spun;
  }
  ocrHint_t h;
  pd_hint(&h, paramv[P_PD], OCR_HINT_EDT_T);
  ocrGuid_t succ;
  ocrEdtCreate(&succ, u64_guid(paramv[P_STEP_TPL]), P_COUNT, paramv, 1,
               NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(u64_guid(paramv[P_DB0 + next_dbi]), succ, 0,
                   role ? DB_MODE_RW : DB_MODE_RO);
  return NULL_GUID;
}

/* one template shape per layout; the starter instance carries depc=2
 * (block+trigger), successors depc=1 (block).  depv[1] is never read. */
static ocrGuid_t step_shared_edt(u32 paramc, u64 *paramv, u32 depc,
                                 ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  return step_run(paramv, depv, 0);
}

static ocrGuid_t step_private_edt(u32 paramc, u64 *paramv, u32 depc,
                                  ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  return step_run(paramv, depv, 1);
}

/* Untimed first-touch write: exercise ownership once from this chain's own
 * rank, then hand off to the role.  It anchors nothing and counts as no op --
 * the successor's entry anchors the span -- so a reader's block carries
 * exactly one write for the whole run. */
static ocrGuid_t first_touch_edt(u32 paramc, u64 *paramv, u32 depc,
                                 ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 dbi = paramv[P_DBIDX];
  u64 stamp = 0;
  touch(depv[0].ptr, paramv[P_BYTES], 1, &stamp);
  ocrDbRelease(depv[0].guid);
  paramv[P_LAST_SEEN + dbi] = stamp; /* this EDT's own vector: see step_run */
  ocrHint_t h;
  pd_hint(&h, paramv[P_PD], OCR_HINT_EDT_T);
  ocrGuid_t succ;
  ocrEdtCreate(&succ, u64_guid(paramv[P_STEP_TPL]), P_COUNT, paramv, 1,
               NULL, EDT_PROP_NONE, &h, NULL);
  ocrAddDependence(u64_guid(paramv[P_DB0 + dbi]), succ, 0,
                   paramv[P_ROLE] ? DB_MODE_RW : DB_MODE_RO);
  return NULL_GUID;
}

/* ------------------------------------------------------------- finisher */
static ocrGuid_t fin_edt(u32 paramc, u64 *paramv, u32 depc,
                         ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  u64 *r = (u64 *)depv[0].ptr;
  r[AC_F_ROLE] = paramv[P_ROLE];
  r[AC_F_OPS_TOTAL] = paramv[P_OPS_TOTAL];
  r[AC_F_OPS_MEAS] = paramv[P_OPS_MEAS];
  r[AC_F_VIOL] = paramv[P_VIOL];
  r[AC_F_CAP] = paramv[P_CAP];
  r[AC_F_USEFUL] = paramv[P_USEFUL];
  r[AC_F_TRIG_NS] = paramv[P_TRIG_NS];
  r[AC_F_LAST_NS] = paramv[P_LASTOP];
  for (unsigned int k = 0; k < 3; k++) {
    r[AC_F_SUM_ACQ + k] = paramv[P_SUM_ACQ + k];
    for (unsigned int b = 0; b < AC_NB; b++)
      r[AC_F_HIST + k * AC_NB + b] = paramv[P_HIST + k * AC_NB + b];
  }
  /* release BEFORE wiring: the satisfy travels to the collector's rank and
   * its acquire runs there asynchronously -- nothing else orders that read
   * behind this EDT's epilogue */
  ocrGuid_t rdb = depv[0].guid;
  ocrDbRelease(rdb);
  ocrAddDependence(rdb, u64_guid(paramv[P_COLLECTOR]), (u32)paramv[P_SLOT],
                   DB_MODE_RO);
  return NULL_GUID;
}

/* ------------------------------------------------------------ collector */
static ocrGuid_t collector_edt(u32 paramc, u64 *paramv, u32 depc,
                               ocrEdtDep_t depv[]) {
  (void)paramc;
  (void)depc;
  char name[AC_NAME_CHARS + 1];
  name_unpack(name, &paramv[C_NAME]);
  u64 R = paramv[C_R], W = paramv[C_W], rings = paramv[C_RING];
  /* the rings are the whole writer class, so result blocks and dependence
   * slots follow the chain count while W stays the slot count */
  u64 A = R + (rings ? rings : W);
  u64 nblk = paramv[C_BLOCKS];
  int sole_owner = paramv[C_SOLE_OWNER] != 0;

  /* the start trigger lingers until destroyed; every starter has consumed it
   * by the time the last chain's result lands here */
  ocrEventDestroy(u64_guid(paramv[C_TRIGGER]));

  u64 pooled[2][3][AC_NB]; /* class x figure x bucket */
  double sum[2][3] = {{0}};
  u64 ops_meas[2] = {0, 0}, ops_total_w = 0, viol = 0, cap = 0, final_bad = 0;
  u64 useful_sum = 0, max_span_ns = 0, final_sum = 0;
  double cn[2] = {0, 0}, cs[2] = {0, 0}, cq[2] = {0, 0}; /* chain-ops CV */
  memset(pooled, 0, sizeof(pooled));
  for (u64 i = 0; i < A; i++) {
    const u64 *r = (const u64 *)depv[i].ptr;
    unsigned int c = r[AC_F_ROLE] ? 1 : 0;
    ops_meas[c] += r[AC_F_OPS_MEAS];
    if (!c) useful_sum += r[AC_F_USEFUL];
    double co = (double)r[AC_F_OPS_MEAS];
    cn[c] += 1.0;
    cs[c] += co;
    cq[c] += co * co;
    if (c) ops_total_w += r[AC_F_OPS_TOTAL];
    viol += r[AC_F_VIOL];
    cap += r[AC_F_CAP];
    u64 span = r[AC_F_LAST_NS] - r[AC_F_TRIG_NS];
    if (span > max_span_ns) max_span_ns = span;
    for (unsigned int k = 0; k < 3; k++) {
      sum[c][k] += (double)r[AC_F_SUM_ACQ + k];
      for (unsigned int b = 0; b < AC_NB; b++)
        pooled[c][k][b] += r[AC_F_HIST + k * AC_NB + b];
    }
    if (sole_owner) {
      /* each block answers to its own chain: its counter is that chain's
       * writes plus the one untimed first touch, if the program asked for
       * one */
      const u64 *blk = (const u64 *)depv[A + i].ptr;
      u64 want = (c ? r[AC_F_OPS_TOTAL] : 0) + paramv[C_FIRST_TOUCH];
      u64 got = __atomic_load_n((const uint64_t *)&blk[0], __ATOMIC_RELAXED);
      final_sum += got;
      if (got != want) final_bad++;
    }
  }
  if (!sole_owner) {
    for (u64 d = 0; d < nblk; d++) {
      const u64 *p = (const u64 *)depv[A + d].ptr;
      final_sum += __atomic_load_n((const uint64_t *)&p[0], __ATOMIC_RELAXED);
    }
    if (final_sum != ops_total_w) final_bad++;
  }
  /* dispersion of per-chain throughput inside one cell: the convoy /
   * fairness signal (order effects show here before they show in rep-to-rep
   * variance) */
  double cv[2] = {-1.0, -1.0};
  for (unsigned int c = 0; c < 2; c++) {
    if (cn[c] >= 2.0 && cs[c] > 0.0) {
      double mean = cs[c] / cn[c];
      double var = cq[c] / cn[c] - mean * mean;
      cv[c] = var > 0.0 ? sqrt(var) / mean : 0.0;
    }
  }

  if (cap) {
    PRINTF("%s-CAP-HIT chains=%lu\n", name, (unsigned long)cap);
    ocrShutdown();
    return NULL_GUID;
  }
  if (viol) {
    PRINTF("%s-ORACLE-FAIL kind=value viol=%lu\n", name, (unsigned long)viol);
    ocrShutdown();
    return NULL_GUID;
  }
  if (final_bad) {
    PRINTF("%s-ORACLE-FAIL kind=final bad=%lu wops=%lu got=%lu\n", name,
           (unsigned long)final_bad, (unsigned long)ops_total_w,
           (unsigned long)final_sum);
    ocrShutdown();
    return NULL_GUID;
  }

  /* fixed work names no window: the denominator is the slowest chain's wall */
  double span_s = (double)max_span_ns / 1e9;
  if (span_s <= 0.0) span_s = 1e-9;
  double rx = (double)ops_meas[0] / span_s;
  /* a ring measures no span, so its steps have no rate; the exact step count
   * is the class's op count */
  double wx = rings ? -1.0 : (double)ops_meas[1] / span_s;
  u64 wops = rings ? ops_total_w : ops_meas[1];
#define QQ(c, k, q) hist_quantile_us(pooled[c][k], q)
#define MEAN(c, k) \
  (ops_meas[c] ? sum[c][k] / 1000.0 / (double)ops_meas[c] : -1.0)
  PRINTF(
      "%s OK PDS=%lu R=%lu W=%lu EHR=%lu EHW=%lu ETH=%lu ETHW=%lu BYTES=%lu "
      "D=%lu RHO=%.4f ROPS=%lu WOPS=%lu RXPUT=%.1f WXPUT=%.1f "
      "RTOT_P50=%.2f RTOT_P90=%.2f RTOT_P99=%.2f "
      "WTOT_P50=%.2f WTOT_P90=%.2f WTOT_P99=%.2f "
      "RACQ_P99=%.2f RREL_P99=%.2f WACQ_P99=%.2f WREL_P99=%.2f "
      "RACQ_MEAN=%.2f RREL_MEAN=%.2f RTOT_MEAN=%.2f "
      "WACQ_MEAN=%.2f WREL_MEAN=%.2f WTOT_MEAN=%.2f "
      "RCV=%.3f WCV=%.3f RUSE=%.1f FINAL=%lu\n",
      name, (unsigned long)paramv[C_PDS], (unsigned long)R, (unsigned long)W,
      (unsigned long)paramv[C_EHR], (unsigned long)paramv[C_EHW],
      (unsigned long)paramv[C_ETH], (unsigned long)paramv[C_ETHW],
      (unsigned long)paramv[C_BYTES], (unsigned long)nblk,
      (double)paramv[C_RHO_X10K] / 10000.0, (unsigned long)ops_meas[0],
      (unsigned long)wops, rx, wx, QQ(0, 2, 0.50), QQ(0, 2, 0.90),
      QQ(0, 2, 0.99), QQ(1, 2, 0.50), QQ(1, 2, 0.90), QQ(1, 2, 0.99),
      QQ(0, 0, 0.99), QQ(0, 1, 0.99), QQ(1, 0, 0.99), QQ(1, 1, 0.99),
      MEAN(0, 0), MEAN(0, 1), MEAN(0, 2), MEAN(1, 0), MEAN(1, 1), MEAN(1, 2),
      cv[0], cv[1], (double)useful_sum / span_s, (unsigned long)final_sum);
#undef QQ
#undef MEAN
  ocrShutdown();
  return NULL_GUID;
}

/* ------------------------------------------------------------ entry point */
ocrGuid_t attack_chain_run(const attack_chain_spec_t *spec, u64 pd_count) {
  const char *name = spec->name ? spec->name : "ATTACK";
  int sole_owner = (spec->layout == ATTACK_CHAIN_PRIVATE);
  if (pd_count == 0) pd_count = 1;
  u64 bytes = spec->bytes < AC_MIN_BYTES ? AC_MIN_BYTES : spec->bytes;

  u64 homes = spec->homes ? (spec->homes > pd_count ? pd_count : spec->homes)
                          : 1;
  u64 zone_lo = pd_count > homes ? homes : 0; /* actors: [zone_lo, pd_count) */
  u64 zone = pd_count - zone_lo;
  if (sole_owner) { /* a private block is homed off its chain's rank; no rank
                     * is reserved for homes, so every rank runs chains */
    zone_lo = 0;
    zone = pd_count;
  }
  u64 eff_r = (spec->reader_ranks == 0 || spec->reader_ranks > zone)
                  ? zone
                  : spec->reader_ranks;
  u64 eff_w = (spec->writer_ranks == 0 || spec->writer_ranks > zone)
                  ? zone
                  : spec->writer_ranks;
  u64 R = spec->readers_total ? spec->readers_total
                              : spec->readers_per_rank * zone;
  u64 W = spec->writers_total ? spec->writers_total
                              : spec->writers_per_rank * zone;
  /* the rings replace the class's chains; W stays the slot count, which is
   * what their step counts are built from */
  u64 rings = spec->writer_rings;
  u64 wchains = rings ? rings : W;
  u64 A = R + wchains; /* chains: result blocks and collector slots */
  /* what one chain can reach, and how many blocks the collector re-checks */
  u64 view = sole_owner ? 1 : spec->blocks;
  u64 n_data = sole_owner ? A : spec->blocks;

  /* A coupled reader waits on a block's counter and a first touch writes one
   * that nobody else moves, so each belongs to exactly one layout: coupling a
   * sole owner would spin a reader to the cap, and first-touching a shared
   * block would break the exact-count oracle. */
  if (A == 0 || A > ATTACK_CHAIN_MAX_CHAINS || view > ATTACK_CHAIN_MAX_BLOCKS ||
      view == 0 || (R && spec->ops_r == 0) || (W && spec->ops_w == 0) ||
      (!sole_owner && spec->first_touch_write) ||
      (sole_owner && spec->coupled) ||
      (rings && (sole_owner || W == 0 || W % rings != 0))) {
    PRINTF("%s-ORACLE-FAIL kind=args R=%lu W=%lu D=%lu\n", name,
           (unsigned long)R, (unsigned long)W, (unsigned long)n_data);
    ocrShutdown();
    return NULL_GUID;
  }

  /* shared block set: homed round-robin over [0, homes) */
  u64 db_guids[ATTACK_CHAIN_MAX_BLOCKS] = {0};
  if (!sole_owner) {
    for (u64 d = 0; d < n_data; d++) {
      ocrHint_t dh;
      pd_hint(&dh, d % homes, OCR_HINT_DB_T);
      ocrGuid_t g;
      u64 *p;
      ocrDbCreate(&g, (void **)&p, bytes, DB_PROP_NONE, &dh, NO_ALLOC);
      memset(p, 0, bytes);
      ocrDbRelease(g); /* create takes an implicit hold; under EXCL every
                        * first op would otherwise park behind this EDT */
      db_guids[d] = guid_u64(g);
    }
  }

  ocrGuid_t trigger;
  ocrEventCreate(&trigger, OCR_EVENT_STICKY_T, EVT_PROP_NONE);

  ocrGuid_t step_tpl, start_tpl, fin_tpl, coll_tpl;
  ocrEdt_t step_fn = sole_owner ? step_private_edt : step_shared_edt;
  ocrEdtTemplateCreate(&step_tpl, step_fn, P_COUNT, 1);
  ocrEdtTemplateCreate(&start_tpl,
                       spec->first_touch_write ? first_touch_edt : step_fn,
                       P_COUNT, 2);
  ocrEdtTemplateCreate(&fin_tpl, fin_edt, P_COUNT, 1);
  ocrEdtTemplateCreate(&coll_tpl, collector_edt, C_COUNT, (u32)(A + n_data));

  u64 cparams[C_COUNT];
  memset(cparams, 0, sizeof(cparams));
  name_pack(&cparams[C_NAME], name);
  cparams[C_PDS] = pd_count;
  cparams[C_R] = R;
  cparams[C_W] = W;
  cparams[C_EHR] = spec->hold_r_us;
  cparams[C_EHW] = spec->hold_w_us;
  cparams[C_ETH] = spec->think_r_us;
  cparams[C_ETHW] = spec->think_w_us;
  cparams[C_BYTES] = bytes;
  cparams[C_BLOCKS] = n_data;
  cparams[C_RHO_X10K] = (u64)((double)W / (double)(R + W) * 10000.0 + 0.5);
  cparams[C_SOLE_OWNER] = sole_owner;
  cparams[C_FIRST_TOUCH] = spec->first_touch_write ? 1 : 0;
  cparams[C_TRIGGER] = guid_u64(trigger);
  cparams[C_RING] = rings;

  ocrHint_t h0;
  pd_hint(&h0, 0, OCR_HINT_EDT_T);
  ocrGuid_t collector;
  ocrEdtCreate(&collector, coll_tpl, C_COUNT, cparams, (u32)(A + n_data), NULL,
               EDT_PROP_NONE, &h0, NULL);
  if (!sole_owner)
    for (u64 d = 0; d < n_data; d++)
      ocrAddDependence(u64_guid(db_guids[d]), collector, (u32)(A + d),
                       DB_MODE_RO);

  u64 r_idx = 0, w_idx = 0;
  for (u64 c = 0; c < A; c++) {
    int is_writer = c < wchains; /* ids: writers first, then readers */
    u64 pd;
    if (sole_owner) {
      pd = c % pd_count;
    } else if (is_writer) {
      /* the placement rule, applied to this chain's first slot: slot c for a
       * chain per slot, the head of ring c's block for a ring */
      u64 slot = rings ? c * (W / rings) : w_idx++;
      pd = zone_lo + (slot % eff_w); /* bottom of the zone upward */
    } else {
      pd = pd_count - 1 - (r_idx++ % eff_r); /* top downward */
    }

    u64 own_db = 0;
    if (sole_owner) {
      ocrHint_t dh;
      pd_hint(&dh, (pd + 1) % pd_count, OCR_HINT_DB_T);
      /* always one rank away: the required communication is zero, the wire
       * distance is not */
      ocrGuid_t db;
      u64 *p;
      ocrDbCreate(&db, (void **)&p, bytes, DB_PROP_NONE, &dh, NO_ALLOC);
      memset(p, 0, bytes);
      ocrDbRelease(db);
      ocrAddDependence(db, collector, (u32)(A + c), DB_MODE_RO);
      own_db = guid_u64(db);
    }

    ocrHint_t rh;
    pd_hint(&rh, pd, OCR_HINT_DB_T);
    ocrGuid_t rdb;
    u64 *rp;
    ocrDbCreate(&rdb, (void **)&rp, AC_RESULT_WORDS * sizeof(u64),
                DB_PROP_NONE, &rh, NO_ALLOC);
    memset(rp, 0, AC_RESULT_WORDS * sizeof(u64));
    ocrDbRelease(rdb);

    u64 lcg = 0x9E3779B97F4A7C15ull + c * 0xBF58476D1CE4E5B9ull + 1;
    u64 first_db = 0;
    if (!sole_owner)
      first_db = spec->coupled ? c % view
                 : view > 1   ? (lcg >> 33) % view
                              : 0;
    u64 params[P_COUNT];
    memset(params, 0, sizeof(params));
    params[P_ROLE] = is_writer ? 1 : 0;
    params[P_CHAIN] = c;
    params[P_PD] = pd;
    params[P_DBIDX] = first_db;
    params[P_LCG] = lcg;
    params[P_STEP_TPL] = guid_u64(step_tpl);
    params[P_FIN_TPL] = guid_u64(fin_tpl);
    params[P_COLLECTOR] = guid_u64(collector);
    params[P_SLOT] = c;
    params[P_RESULT_DB] = guid_u64(rdb);
    params[P_BLOCKS] = view;
    params[P_EHOLD] = is_writer ? spec->hold_w_us : spec->hold_r_us;
    params[P_ETHINK] = is_writer ? spec->think_w_us : spec->think_r_us;
    params[P_BYTES] = bytes;
    params[P_KOPS] = is_writer ? (rings ? (W / rings) * spec->ops_w
                                        : spec->ops_w)
                               : spec->ops_r;
    params[P_COUPLED] = spec->coupled ? 1 : 0;
    if (rings && is_writer) {
      params[P_RING_LEN] = W / rings;
      params[P_RING_BASE] = zone_lo;
      params[P_RING_SPAN] = eff_w;
    }
    if (sole_owner)
      params[P_DB0] = own_db;
    else
      for (u64 d = 0; d < view; d++) params[P_DB0 + d] = db_guids[d];

    ocrHint_t eh;
    pd_hint(&eh, pd, OCR_HINT_EDT_T);
    ocrGuid_t starter;
    ocrEdtCreate(&starter, start_tpl, P_COUNT, params, 2, NULL, EDT_PROP_NONE,
                 &eh, NULL);
    ocrAddDependence(u64_guid(params[P_DB0 + first_db]), starter, 0,
                     (is_writer || spec->first_touch_write) ? DB_MODE_RW
                                                            : DB_MODE_RO);
    ocrAddDependence(trigger, starter, 1, DB_MODE_NULL);
  }

  ocrEventSatisfy(trigger, NULL_GUID);
  return NULL_GUID;
}
