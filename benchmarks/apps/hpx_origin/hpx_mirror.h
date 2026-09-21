/* Idioms shared by the OCR mirrors of HPX programs: the placement an HPX
 * program states, the layout its distribution policy uses, its option
 * syntax, the fork that gives every rank a driver, and the rendezvous that
 * carries a value from a producer on one rank to a consumer another rank
 * created. */
#ifndef HPX_MIRROR_H
#define HPX_MIRROR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "ocr.h"
#include "extensions/ocr-affinity.h"
#include "extensions/ocr-labeling.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static inline u64 mirror_guid_u64(ocrGuid_t g) { return (u64)g.guid; }
static inline ocrGuid_t mirror_u64_guid(u64 v) { ocrGuid_t g; g.guid = (intptr_t)v; return g; }

/* A task runs where the origin's task ran; a block lives where the origin's
 * component or buffer lived.  Both are stated, never left to a build
 * default. */
static inline void mirror_rank_hint(ocrHint_t *h, u64 rank, ocrHintType_t type) {
  ocrHintInit(h, type);
  ocrGuid_t aff;
  ocrAffinityGetAt(AFFINITY_PD, rank, &aff);
  ocrSetHintValue(h, type == OCR_HINT_EDT_T ? OCR_HINT_EDT_AFFINITY : OCR_HINT_DB_AFFINITY,
                  ocrAffinityToHintValue(aff));
}

/* A block produced for a consumer elsewhere is stated to live where it is
 * written, which is where the origin's value sits before it travels. */
static inline void mirror_here_hint(ocrHint_t *h, ocrHintType_t type) {
  ocrHintInit(h, type);
  ocrGuid_t aff;
  ocrAffinityGetCurrent(&aff);
  ocrSetHintValue(h, type == OCR_HINT_EDT_T ? OCR_HINT_EDT_AFFINITY : OCR_HINT_DB_AFFINITY,
                  ocrAffinityToHintValue(aff));
}

static inline u64 mirror_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (u64)now.tv_sec * 1000000000ULL + (u64)now.tv_nsec;
}

static inline u64 mirror_rank(u64 nl) {
  ocrGuid_t cur, at;
  ocrAffinityGetCurrent(&cur);
  for (u64 i = 0; i < nl; ++i) {
    ocrAffinityGetAt(AFFINITY_PD, i, &at);
    if (ocrGuidIsEq(cur, at)) return i;
  }
  return 0;
}

/* Contiguous groups of ceil(items/locs) items, one per locality when sparse. */
static inline u64 mirror_owner(u64 i, u64 items, u64 nl) {
  if (items < nl) return i;
  u64 per = (items + nl - 1) / nl;
  u64 o = i / per;
  return o < nl - 1 ? o : nl - 1;
}

/* A latch created at zero is never carried to zero: a phase that can be
 * empty takes max(n,1) and one explicit decrement from its creator. */
static inline ocrGuid_t mirror_latch(u64 n) {
  ocrEventParams_t params;
  params.EVENT_LATCH.counter = n ? n : 1;
  ocrGuid_t latch;
  ocrEventCreateParams(&latch, OCR_EVENT_LATCH_T, EVT_PROP_NONE, &params);
  return latch;
}

/* --key=value or --key value.  Returns the value, or NULL; `*bad` is raised
 * when the key is PRESENT and has no value, which is malformed and must not
 * be reported as absence -- a malformed option is usage, never a silent
 * default.
 *
 * What decides the three cases is the character after the key's own
 * characters: `=` introduces the value, end-of-string means the next
 * argument is the value (and, if there is no next argument, that the key was
 * given without one), and anything else means this argument is simply a
 * DIFFERENT option that happens to start with the same characters -- so the
 * scan moves on.  That last case is not a typo to reject: option sets in
 * this family legitimately contain a key and a longer key built on it
 * (`--nx` beside `--nx_block`, `--n` beside `--nt`), and rejecting the
 * longer one would make a program refuse its own correct arguments. */
static inline const char *mirror_option_raw(void *argdb, u64 argc, const char *key, int *bad) {
  size_t klen = strlen(key);
  *bad = 0;
  for (u64 i = 1; i < argc; ++i) {
    const char *a = getArgv(argdb, i);
    if (strncmp(a, key, klen) != 0) continue;
    if (a[klen] == '=') return a + klen + 1;
    if (a[klen] != '\0') continue;    /* a longer key, not this one */
    if (i + 1 < argc) return getArgv(argdb, i + 1);
    *bad = 1;    /* the key is there and its value is not */
    return NULL;
  }
  return NULL;
}
/* 1 found, 0 absent, -1 malformed. */
static inline int mirror_option_u64(void *argdb, u64 argc, const char *key, u64 *out) {
  int bad;
  const char *v = mirror_option_raw(argdb, argc, key, &bad);
  if (v == NULL) return bad ? -1 : 0;
  char *end;
  unsigned long long x = strtoull(v, &end, 10);
  if (*v == '\0' || *end != '\0') return -1;
  *out = (u64)x;
  return 1;
}
static inline int mirror_option_f64(void *argdb, u64 argc, const char *key, double *out) {
  int bad;
  const char *v = mirror_option_raw(argdb, argc, key, &bad);
  if (v == NULL) return bad ? -1 : 0;
  char *end;
  double x = strtod(v, &end);
  if (*v == '\0' || *end != '\0') return -1;
  *out = x;
  return 1;
}
static inline int mirror_option_str(void *argdb, u64 argc, const char *key, char *out, size_t n) {
  int bad;
  const char *v = mirror_option_raw(argdb, argc, key, &bad);
  if (v == NULL) return bad ? -1 : 0;
  if (strlen(v) >= n) return -1;
  strcpy(out, v);
  return 1;
}
/* A boolean option that takes a value, in the spellings the origins' option
 * library accepts, compared without regard to case: an empty value, `1`,
 * `true`, `yes` or `on` is true; `0`, `false`, `no` or `off` is false;
 * anything else is malformed.  Not for a presence flag, which takes no value:
 * given as the last argument it would read as malformed, and anywhere else
 * it would take the next argument as its value. */
static inline int mirror_option_bool(void *argdb, u64 argc, const char *key, int *out) {
  static const char *const truths[] = {"", "1", "true", "yes", "on"};
  static const char *const falsehoods[] = {"0", "false", "no", "off"};
  int bad;
  const char *v = mirror_option_raw(argdb, argc, key, &bad);
  if (v == NULL) return bad ? -1 : 0;
  for (size_t i = 0; i < sizeof truths / sizeof truths[0]; ++i)
    if (strcasecmp(v, truths[i]) == 0) { *out = 1; return 1; }
  for (size_t i = 0; i < sizeof falsehoods / sizeof falsehoods[0]; ++i)
    if (strcasecmp(v, falsehoods[i]) == 0) { *out = 0; return 1; }
  return -1;
}

/* A count derived from the arguments is folded against its ceiling one factor
 * at a time: a product that had already wrapped would pass the very test that
 * exists to reject it. */
static inline int mirror_fits(u64 *acc, u64 factor, u64 cap) {
  if (factor == 0 || *acc > cap / factor) return 0;
  *acc *= factor;
  return 1;
}

/* The entropy a random_device draw reads here; the wall clock and the
 * process identity are the fallback, so an unseeded run is never a fixed
 * sequence. */
static inline u64 mirror_draw_seed(void) {
  uint32_t s;
  if (getrandom(&s, sizeof s, 0) == (ssize_t)sizeof s) return s;
  return (u64)time(NULL) ^ (u64)getpid();
}

/* mt19937, for a mirror whose origin draws from one. */
typedef struct { uint32_t mt[624]; int idx; } mirror_mt19937_t;
static inline void mirror_mt_seed(mirror_mt19937_t *s, uint32_t seed) {
  s->mt[0] = seed;
  for (int i = 1; i < 624; ++i)
    s->mt[i] = 1812433253u * (s->mt[i - 1] ^ (s->mt[i - 1] >> 30)) + (uint32_t)i;
  s->idx = 624;
}
static inline uint32_t mirror_mt_next(mirror_mt19937_t *s) {
  if (s->idx >= 624) {
    for (int i = 0; i < 624; ++i) {
      uint32_t y = (s->mt[i] & 0x80000000u) | (s->mt[(i + 1) % 624] & 0x7fffffffu);
      s->mt[i] = s->mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
    }
    s->idx = 0;
  }
  uint32_t y = s->mt[s->idx++];
  y ^= y >> 11; y ^= (y << 7) & 0x9d2c5680u; y ^= (y << 15) & 0xefc60000u; y ^= y >> 18;
  return y;
}

/* A full-width 32-bit engine uses multiply-high downscaling with rejection.
 * The bound is nonzero; even a singleton interval consumes one engine draw. */
static inline uint32_t mirror_mt_bounded(mirror_mt19937_t *s, uint32_t bound) {
  uint64_t product = (uint64_t)mirror_mt_next(s) * bound;
  uint32_t low = (uint32_t)product;
  if (low < bound) {
    uint32_t threshold = (uint32_t)(-bound) % bound;
    while (low < threshold) {
      product = (uint64_t)mirror_mt_next(s) * bound;
      low = (uint32_t)product;
    }
  }
  return (uint32_t)(product >> 32);
}

/* The SPMD fork.  A program whose driver runs on every locality has one
 * driver per rank; the main task is rank 0's, so it creates the others'.
 * paramv[rank_slot] carries the rank each driver is for.  The created EDTs
 * are returned when the caller has dependences to add to them; a driver
 * created with depc 0 starts as soon as it is created. */
static inline void mirror_spmd_fork(ocrGuid_t tpl, u64 *pv, u32 paramc, u32 rank_slot,
                                    u64 nl, u32 depc, ocrGuid_t *out) {
  for (u64 r = 0; r < nl; ++r) {
    pv[rank_slot] = r;
    ocrHint_t h;
    mirror_rank_hint(&h, r, OCR_HINT_EDT_T);
    ocrGuid_t e;
    ocrEdtCreate(&e, tpl, paramc, pv, depc, NULL, EDT_PROP_NONE, &h, NULL);
    if (out) out[r] = e;
  }
}

/* A rendezvous point between a producer on one rank and a consumer that
 * another rank created.  Both sides derive the same GUID from a reserved
 * range and an ordinal, so neither has to learn the other's names; the index
 * gives each point exactly one producer and one consumer and never aliases two
 * units of work onto one point.  Whether a point lands on its consumer or on
 * the rank that reserved the range is the runtime's choice of how a labeled
 * range is homed, and only the hop count depends on it.  One producer, one
 * consumer, one generation: an ordinal names a unit of work and is never
 * reused across a lifetime boundary. */
static inline ocrGuid_t mirror_edge(ocrGuid_t range, u64 ordinal, u64 consumer_rank, u64 nl) {
  ocrGuid_t g;
  ocrGuidFromIndex(&g, range, ordinal * nl + consumer_rank);
  return g;
}

/* Both sides open the point before using it.  A runtime that reports a
 * taken label tells whoever arrives second that the first one made it; one
 * that installs first-wins silently tells nobody.  Either is the expected
 * outcome and not an error. */
static inline void mirror_edge_open(ocrGuid_t evt) {
  ocrGuid_t g = evt;
  u8 err = ocrEventCreate(&g, OCR_EVENT_STICKY_T,
                          GUID_PROP_IS_LABELED | GUID_PROP_CHECK | EVT_PROP_TAKES_ARG);
  if (err && err != OCR_EGUIDEXISTS) {
    PRINTF("mirror: rendezvous point could not be opened (%u)\n", (unsigned)err);
    ocrShutdown();
  }
}

#endif /* HPX_MIRROR_H */
