/* SPDX-License-Identifier: Apache-2.0 */
#include "arts/fam/pool.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "arts.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/utils/malloc.h"

#define FAM_CLASS_COUNT 32u /* class c serves ARTS_FAM_GRANULE << c bytes */
#define FAM_NO_GRANULE 0xFFFFFFFFu
#define FAM_META_NONE 0xFFFFFFFFu
#define FAM_LIVE 1u
#define FAM_FREE 2u

/* How many times a request re-reads the whole slice before calling it
 * exhausted: the free lists, the cursor and the larger classes are three
 * independent observations, so a single pass that found none of them proves
 * only that no ONE of them was available at the moment it was read. */
#define FAM_ALLOC_PASSES 64u

static _Atomic uintptr_t g_pool_lo; /* 0 until init: contains() answers false */
static _Atomic uintptr_t g_pool_hi;

static struct {
  unsigned char *pool; /* the mapping, header first */
  uint64_t pool_bytes;
  unsigned char *slice; /* this rank's, page aligned */
  uint64_t slice_len;
  uint32_t granules;
  unsigned rank;
  unsigned nranks;
  _Atomic uint64_t cursor;                     /* next unbumped granule */
  _Atomic uint64_t free_head[FAM_CLASS_COUNT]; /* tag:32 | granule:32 */
  _Atomic uint32_t free_n[FAM_CLASS_COUNT];    /* for the fatal message */
  _Atomic uint32_t *next_free;                 /* per granule, DRAM */
  _Atomic uint32_t *meta;                      /* per granule, DRAM */
} g_fam;

static inline uint32_t fam_head_granule(uint64_t h) { return (uint32_t)h; }
static inline uint64_t fam_head_make(uint64_t old, uint32_t g) {
  return (((old >> 32) + 1u) << 32) | (uint64_t)g;
}
static inline uint32_t fam_meta_make(unsigned state, unsigned cls) {
  return (uint32_t)((state << 8) | cls);
}
static inline unsigned fam_meta_state(uint32_t m) { return (m >> 8) & 0xFFu; }
static inline unsigned fam_meta_cls(uint32_t m) { return m & 0xFFu; }

/* The class bound is in the loop condition, not inside it, so the shift can
 * never overflow and no statement after an ARTS_ERROR is unreachable: the
 * diagnostic aborts, and arts_abort's public declaration does not say
 * _Noreturn, so an unreachable `return` here would only invite a caller to
 * believe this can fail softly. */
static unsigned fam_class_of(size_t bytes) {
  uint64_t n = ARTS_FAM_GRANULE;
  unsigned c = 0;
  while (n < (uint64_t)bytes && c < FAM_CLASS_COUNT) {
    n <<= 1;
    c++;
  }
  if (c >= FAM_CLASS_COUNT) {
    ARTS_ERROR("arts_fam_alloc: %zu bytes exceeds any size class (the largest "
               "is %lluB)",
               bytes, (unsigned long long)ARTS_FAM_GRANULE
                          << (FAM_CLASS_COUNT - 1u));
  }
  return c;
}

/* The tag rises on both push and pop, so a pop whose reload lost a race
 * cannot complete against a head that was recycled meanwhile.  next_free is
 * read and written relaxed: the ordering that makes its value correct is the
 * release on this push paired with the acquire on the pop that observes it. */
static void fam_push(unsigned c, uint32_t g) {
  uint64_t head =
      atomic_load_explicit(&g_fam.free_head[c], memory_order_relaxed);
  /* Counted before the block is published, so that the pop which takes it can
   * only ever decrement a count this increment already carried: the other
   * order lets a diagnostic count wrap through UINT32_MAX. */
  atomic_fetch_add_explicit(&g_fam.free_n[c], 1u, memory_order_relaxed);
  for (;;) {
    atomic_store_explicit(&g_fam.next_free[g], fam_head_granule(head),
                          memory_order_relaxed);
    uint64_t want = fam_head_make(head, g);
    if (atomic_compare_exchange_weak_explicit(&g_fam.free_head[c], &head, want,
                                              memory_order_release,
                                              memory_order_relaxed)) {
      return;
    }
  }
}

static uint32_t fam_pop(unsigned c) {
  uint64_t head =
      atomic_load_explicit(&g_fam.free_head[c], memory_order_acquire);
  for (;;) {
    uint32_t g = fam_head_granule(head);
    if (g == FAM_NO_GRANULE) {
      return FAM_NO_GRANULE;
    }
    uint32_t nx =
        atomic_load_explicit(&g_fam.next_free[g], memory_order_relaxed);
    uint64_t want = fam_head_make(head, nx);
    if (atomic_compare_exchange_weak_explicit(&g_fam.free_head[c], &head, want,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      atomic_fetch_sub_explicit(&g_fam.free_n[c], 1u, memory_order_relaxed);
      return g;
    }
  }
}

/* CAS rather than fetch_add so the cursor never passes the slice's end: an
 * overshoot that is later given back would refuse a request that had room. */
static uint32_t fam_bump(unsigned c) {
  uint64_t need = (uint64_t)1u << c;
  uint64_t cur = atomic_load_explicit(&g_fam.cursor, memory_order_relaxed);
  for (;;) {
    if (cur + need > (uint64_t)g_fam.granules) {
      return FAM_NO_GRANULE;
    }
    if (atomic_compare_exchange_weak_explicit(&g_fam.cursor, &cur, cur + need,
                                              memory_order_relaxed,
                                              memory_order_relaxed)) {
      return (uint32_t)cur;
    }
  }
}

/* An empty class is served by halving the smallest larger block there is, so
 * a slice whose size mix changed between phases does not die with most of
 * itself free.  A granule interior to a just-popped block is this thread's
 * alone, so the halves' metadata is published with a release store and no CAS.
 * Halves are never rejoined: the residual exhaustion mode is many small live
 * blocks and then a large request, which the fatal message names. */
static uint32_t fam_split_down(unsigned c) {
  for (unsigned k = c + 1u; k < FAM_CLASS_COUNT; k++) {
    uint32_t g = fam_pop(k);
    if (g == FAM_NO_GRANULE) {
      continue;
    }
    unsigned j = k;
    while (j > c) {
      j--;
      uint32_t buddy = g + (uint32_t)((uint32_t)1u << j);
      atomic_store_explicit(&g_fam.meta[buddy], fam_meta_make(FAM_FREE, j),
                            memory_order_release);
      fam_push(j, buddy);
    }
    return g;
  }
  return FAM_NO_GRANULE;
}

static void fam_exhausted(size_t bytes, unsigned c) {
  char detail[512];
  int off = snprintf(detail, sizeof(detail),
                     "cursor %llu of %u granules; free blocks at or above "
                     "this class:",
                     (unsigned long long)atomic_load_explicit(
                         &g_fam.cursor, memory_order_relaxed),
                     g_fam.granules);
  if (off < 0) {
    off = 0;
  }
  for (unsigned k = c; k < FAM_CLASS_COUNT && (size_t)off < sizeof(detail);
       k++) {
    uint32_t n = atomic_load_explicit(&g_fam.free_n[k], memory_order_relaxed);
    if (n) {
      int w = snprintf(detail + off, sizeof(detail) - (size_t)off, " %u x %lluB",
                       n, (unsigned long long)ARTS_FAM_GRANULE << k);
      if (w < 0) {
        break;
      }
      off += w;
    }
  }
  ARTS_ERROR("arts_fam_alloc: this rank's slice cannot serve %zu bytes "
             "(class %u, %lluB blocks) - raise fam_pool_mb. %s",
             bytes, c, (unsigned long long)ARTS_FAM_GRANULE << c, detail);
}

bool arts_fam_contains(const void *p) {
  uintptr_t lo = atomic_load_explicit(&g_pool_lo, memory_order_acquire);
  if (!lo) {
    return false;
  }
  uintptr_t hi = atomic_load_explicit(&g_pool_hi, memory_order_acquire);
  uintptr_t a = (uintptr_t)p;
  return a >= lo && a < hi;
}

/* slice_len and nranks are written once in arts_fam_init, on the main thread,
 * before a worker exists, and never again; every caller of this reaches it
 * afterwards, so the plain reads are correct and the pool bounds carry the
 * ordering. */
unsigned arts_fam_owner_of(const void *p) {
  uintptr_t lo = atomic_load_explicit(&g_pool_lo, memory_order_acquire);
  uintptr_t hi = atomic_load_explicit(&g_pool_hi, memory_order_acquire);
  uintptr_t a = (uintptr_t)p;
  if (!lo || a < lo + ARTS_FAM_HEADER_BYTES || a >= hi) {
    ARTS_ERROR("arts_fam_owner_of: %p is not a slot of the pool", p);
  }
  /* The slices need not tile the pool exactly: what a page-multiple slice
   * length leaves over is a tail that is never handed out and names no
   * slice. */
  uint64_t index = (a - lo - ARTS_FAM_HEADER_BYTES) / g_fam.slice_len;
  if (index >= (uint64_t)g_fam.nranks) {
    ARTS_ERROR("arts_fam_owner_of: %p is not a slot of the pool", p);
  }
  return (unsigned)index;
}

void arts_fam_init(unsigned rank, unsigned nranks) {
  void *base = NULL;
  uint64_t bytes = 0;
  arts_fam_backend_map(rank, nranks, &base, &bytes);
  if (!base || bytes <= (uint64_t)ARTS_FAM_HEADER_BYTES) {
    ARTS_ERROR("arts_fam_init: backend returned no pool");
  }
  if ((uintptr_t)base % ARTS_FAM_PAGE) {
    ARTS_ERROR("arts_fam_init: the pool's base %p is not page aligned", base);
  }

  g_fam.pool = (unsigned char *)base;
  g_fam.pool_bytes = bytes;
  g_fam.rank = rank;
  g_fam.nranks = nranks;

  const struct arts_fam_header_s *h = (const struct arts_fam_header_s *)base;
  if (h->magic != ARTS_FAM_MAGIC || h->nranks != nranks || h->bytes != bytes ||
      nranks == 0) {
    ARTS_ERROR("arts_fam_init: pool header does not describe this run");
  }
  if (h->slice_len == 0 || h->slice_len % ARTS_FAM_PAGE) {
    ARTS_ERROR("arts_fam_init: slice length %llu is not a non-zero multiple "
               "of %u bytes",
               (unsigned long long)h->slice_len, ARTS_FAM_PAGE);
  }
  /* Divided rather than multiplied so the bound itself cannot overflow. */
  if (h->slice_len > (bytes - (uint64_t)ARTS_FAM_HEADER_BYTES) / nranks) {
    ARTS_ERROR("arts_fam_init: %u slices of %llu bytes do not fit the %llu "
               "bytes the pool has past its header page",
               nranks, (unsigned long long)h->slice_len,
               (unsigned long long)(bytes - (uint64_t)ARTS_FAM_HEADER_BYTES));
  }
  g_fam.slice_len = h->slice_len;
  g_fam.slice =
      g_fam.pool + ARTS_FAM_HEADER_BYTES + (uint64_t)rank * h->slice_len;
  g_fam.granules = (uint32_t)(g_fam.slice_len / ARTS_FAM_GRANULE);

  atomic_store_explicit(&g_fam.cursor, 0u, memory_order_relaxed);
  for (unsigned c = 0; c < FAM_CLASS_COUNT; c++) {
    atomic_store_explicit(&g_fam.free_head[c], (uint64_t)FAM_NO_GRANULE,
                          memory_order_relaxed);
    atomic_store_explicit(&g_fam.free_n[c], 0u, memory_order_relaxed);
  }
  g_fam.next_free = (_Atomic uint32_t *)arts_calloc(g_fam.granules,
                                                    sizeof(*g_fam.next_free));
  g_fam.meta =
      (_Atomic uint32_t *)arts_malloc((size_t)g_fam.granules * sizeof(*g_fam.meta));
  if (!g_fam.next_free || !g_fam.meta) {
    ARTS_ERROR("arts_fam_init: no DRAM for %u granules of allocator state",
               g_fam.granules);
  }
  for (uint32_t g = 0; g < g_fam.granules; g++) {
    atomic_store_explicit(&g_fam.meta[g], FAM_META_NONE, memory_order_relaxed);
  }

  atomic_store_explicit(&g_pool_hi, (uintptr_t)g_fam.pool + bytes,
                        memory_order_release);
  atomic_store_explicit(&g_pool_lo, (uintptr_t)g_fam.pool,
                        memory_order_release);
}

void arts_fam_fini(void) {
  /* Single-threaded by contract: every runtime thread has joined before this
   * runs, so nothing can be allocating, freeing or asking about the pool
   * while it tears down.  The bounds are still cleared before the backing
   * goes away, so that the order alone is fail-closed. */
  atomic_store_explicit(&g_pool_lo, 0u, memory_order_release);
  atomic_store_explicit(&g_pool_hi, 0u, memory_order_release);
  arts_free(g_fam.next_free);
  arts_free(g_fam.meta);
  if (g_fam.pool) {
    arts_fam_backend_unmap(g_fam.pool, g_fam.pool_bytes);
  }
  /* The atomic members are cleared atomically rather than by one bulk write:
   * a memset over an _Atomic object is a data race in the abstract machine
   * even where no thread is left to observe it. */
  atomic_store_explicit(&g_fam.cursor, 0u, memory_order_relaxed);
  for (unsigned c = 0; c < FAM_CLASS_COUNT; c++) {
    atomic_store_explicit(&g_fam.free_head[c], (uint64_t)FAM_NO_GRANULE,
                          memory_order_relaxed);
    atomic_store_explicit(&g_fam.free_n[c], 0u, memory_order_relaxed);
  }
  g_fam.pool = NULL;
  g_fam.pool_bytes = 0;
  g_fam.slice = NULL;
  g_fam.slice_len = 0;
  g_fam.granules = 0;
  g_fam.rank = 0;
  g_fam.nranks = 0;
  g_fam.next_free = NULL;
  g_fam.meta = NULL;
}

void *arts_fam_alloc(size_t bytes) {
  if (!bytes) {
    bytes = 1;
  }
  unsigned c = fam_class_of(bytes);
  uint32_t g = FAM_NO_GRANULE;
  for (unsigned pass = 0; pass < FAM_ALLOC_PASSES && g == FAM_NO_GRANULE;
       pass++) {
    if (pass) {
      arts_runtime_idle_pause();
    }
    g = fam_pop(c);
    if (g == FAM_NO_GRANULE) {
      g = fam_bump(c);
    }
    if (g == FAM_NO_GRANULE) {
      g = fam_split_down(c);
    }
  }
  if (g == FAM_NO_GRANULE) {
    fam_exhausted(bytes, c);
  }
  atomic_store_explicit(&g_fam.meta[g], fam_meta_make(FAM_LIVE, c),
                        memory_order_release);
  void *p = g_fam.slice + (uint64_t)g * ARTS_FAM_GRANULE;
  arts_fam_backend_poison(p, (size_t)ARTS_FAM_GRANULE << c);
  return p;
}

void arts_fam_free(void *p) {
  if (!p) {
    return;
  }
  uintptr_t a = (uintptr_t)p;
  uintptr_t lo = (uintptr_t)g_fam.slice;
  if (!g_fam.slice || a < lo || a >= lo + g_fam.slice_len ||
      (a - lo) % ARTS_FAM_GRANULE) {
    ARTS_ERROR("arts_fam_free: %p is not a block of this rank's slice", p);
  }
  uint32_t g = (uint32_t)((a - lo) / ARTS_FAM_GRANULE);
  uint32_t m = atomic_load_explicit(&g_fam.meta[g], memory_order_acquire);
  for (;;) {
    if (m == FAM_META_NONE) {
      ARTS_ERROR("arts_fam_free: %p does not start a block", p);
    }
    if (fam_meta_state(m) == FAM_FREE) {
      ARTS_ERROR("arts_fam_free: %p was already freed", p);
    }
    unsigned c = fam_meta_cls(m);
    if (atomic_compare_exchange_weak_explicit(&g_fam.meta[g], &m,
                                              fam_meta_make(FAM_FREE, c),
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      fam_push(c, g);
      return;
    }
  }
}
