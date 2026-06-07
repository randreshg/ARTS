/******************************************************************************
** GASNet RMA DB arena.
**
** Bump-pointer segment allocator with exact-size reuse. Allocation returns NULL
** when the arena cannot provide segment-resident storage.
******************************************************************************/
#include "arts/memory/db_arena.h"

#include <pthread.h>
#include <stdlib.h>

#include "arts/system/print.h"
#include "arts/transport/socket.h"
#include "arts/utils/malloc.h"

/* Over-align every block: covers the 16B DB alignment and any padding a caller
 * might want, and keeps payload = blk + STRIDE addressing trivial. */
#define ARTS_DB_ARENA_ALIGN ((size_t)64)
#define ARTS_DB_ARENA_STRIDE ARTS_DB_ARENA_ALIGN
#define ARTS_DB_ARENA_MAGIC 0xDB12BEEFu

/* Default RMA-worthwhile threshold (bytes of DB body). Tunable via env. */
#define ARTS_RMA_MIN_BYTES_DEFAULT ((uint64_t)4096)

typedef struct arts_arena_blk_s {
  uint64_t total;                /* bytes consumed from blk start */
  uint64_t payload_size;         /* last requested payload size */
  struct arts_arena_blk_s *next; /* free-list link when !in_use */
  uint32_t in_use;
  uint32_t magic;
} arts_arena_blk_t;

static char *g_base = NULL;   /* arena start (aligned) */
static char *g_end = NULL;    /* arena end (exclusive) */
static char *g_cursor = NULL; /* next bump address */
static arts_arena_blk_t *g_free = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_enabled = 0;
static uint64_t g_min_bytes = ARTS_RMA_MIN_BYTES_DEFAULT;

static uint64_t g_stats[ARTS_RMA_STAT__COUNT];

static inline char *align_up_ptr(char *p, size_t a) {
  return (char *)(((uintptr_t)p + a - 1) & ~(uintptr_t)(a - 1));
}
static inline uint64_t round_up_u64(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

void arts_db_arena_init(void) {
  pthread_mutex_lock(&g_lock);
  if (g_base) { /* already initialized */
    pthread_mutex_unlock(&g_lock);
    return;
  }

  const char *off = getenv("ARTS_RMA_DBMOVE");
  if (off && (off[0] == '0' || off[0] == 'n' || off[0] == 'N')) {
    ARTS_INFO("RMA DB-move: disabled by ARTS_RMA_DBMOVE=%s", off);
    pthread_mutex_unlock(&g_lock);
    return;
  }
  if (!arts_transport_rma_capable()) {
    pthread_mutex_unlock(&g_lock);
    return; /* TCP/rsocket: silent, RMA simply unavailable */
  }

  uint64_t seg_bytes = 0;
  void *seg = arts_transport_segment_base(&seg_bytes);
  if (!seg || seg_bytes < (ARTS_DB_ARENA_STRIDE * 4)) {
    ARTS_INFO("RMA DB-move: no usable segment (base=%p bytes=%lu); "
              "using Medium-AM fallback",
              seg, (unsigned long)seg_bytes);
    pthread_mutex_unlock(&g_lock);
    return;
  }

  const char *minenv = getenv("ARTS_RMA_MIN_BYTES");
  if (minenv) {
    char *endp = NULL;
    unsigned long long v = strtoull(minenv, &endp, 10);
    if (endp != minenv) {
      g_min_bytes = (uint64_t)v;
    }
  }

  g_base = align_up_ptr((char *)seg, ARTS_DB_ARENA_ALIGN);
  uint64_t usable = seg_bytes - (uint64_t)(g_base - (char *)seg);
  g_end = g_base + (usable & ~(uint64_t)(ARTS_DB_ARENA_ALIGN - 1));
  g_cursor = g_base;
  g_free = NULL;
  g_enabled = 1;

  /* Route arena pointers away from libc free() at the single malloc chokepoint.
   */
  arts_malloc_register_foreign(arts_db_arena_owns, arts_db_arena_free);

  ARTS_INFO("RMA DB-move: arena live over segment [%p,%p) %lu MiB, "
            "min_bytes=%lu (ARTS_USE_GASNET, transport=%s)",
            (void *)g_base, (void *)g_end,
            (unsigned long)((g_end - g_base) >> 20), (unsigned long)g_min_bytes,
            arts_transport_kind_name());
  pthread_mutex_unlock(&g_lock);
}

bool arts_db_rma_enabled(void) { return g_enabled != 0; }

uint64_t arts_db_rma_min_bytes(void) { return g_min_bytes; }

bool arts_db_arena_owns(const void *ptr) {
  if (!g_enabled || !ptr) {
    return false;
  }
  const char *p = (const char *)ptr;
  return p >= g_base && p < g_end;
}

void *arts_db_arena_alloc(size_t size) {
  if (!g_enabled || size == 0) {
    return NULL;
  }
  uint64_t need = (uint64_t)ARTS_DB_ARENA_STRIDE +
                  round_up_u64((uint64_t)size, ARTS_DB_ARENA_ALIGN);

  pthread_mutex_lock(&g_lock);

  /* Exact-size reuse: unlink the first free block whose footprint matches. */
  arts_arena_blk_t **pp = &g_free;
  while (*pp) {
    if ((*pp)->total == need) {
      arts_arena_blk_t *blk = *pp;
      *pp = blk->next;
      blk->next = NULL;
      blk->in_use = 1;
      blk->payload_size = size;
      pthread_mutex_unlock(&g_lock);
      return (char *)blk + ARTS_DB_ARENA_STRIDE;
    }
    pp = &(*pp)->next;
  }

  /* Bump. */
  if ((uint64_t)(g_end - g_cursor) < need) {
    pthread_mutex_unlock(&g_lock);
    return NULL; /* exhausted -> caller uses heap */
  }
  arts_arena_blk_t *blk = (arts_arena_blk_t *)g_cursor;
  g_cursor += need;
  pthread_mutex_unlock(&g_lock);

  blk->total = need;
  blk->payload_size = size;
  blk->next = NULL;
  blk->in_use = 1;
  blk->magic = ARTS_DB_ARENA_MAGIC;
  return (char *)blk + ARTS_DB_ARENA_STRIDE;
}

void arts_db_arena_free(void *ptr) {
  if (!ptr) {
    return;
  }
  arts_arena_blk_t *blk =
      (arts_arena_blk_t *)((char *)ptr - ARTS_DB_ARENA_STRIDE);
  if (blk->magic != ARTS_DB_ARENA_MAGIC) {
    ARTS_ERROR("arts_db_arena_free: corrupt or non-arena pointer %p", ptr);
    return;
  }
  pthread_mutex_lock(&g_lock);
  blk->in_use = 0;
  blk->next = g_free;
  g_free = blk;
  pthread_mutex_unlock(&g_lock);
}

void arts_db_rma_stat_inc(arts_rma_stat_t which) {
  if ((unsigned)which < (unsigned)ARTS_RMA_STAT__COUNT) {
    __atomic_fetch_add(&g_stats[which], 1, __ATOMIC_RELAXED);
  }
}

uint64_t arts_db_rma_stat_get(arts_rma_stat_t which) {
  if ((unsigned)which < (unsigned)ARTS_RMA_STAT__COUNT) {
    return __atomic_load_n(&g_stats[which], __ATOMIC_RELAXED);
  }
  return 0;
}

void arts_db_rma_stats_dump(void) {
  if (!g_enabled && g_stats[ARTS_RMA_STAT_FALLBACK_OFF] == 0) {
    return;
  }
  ARTS_INFO("RMA DB-move stats: put_fired=%lu offer_sent=%lu landed=%lu "
            "fb_off=%lu fb_local=%lu fb_small=%lu fb_dest=%lu",
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_PUT_FIRED),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_OFFER_SENT),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_LANDED),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_FALLBACK_OFF),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_FALLBACK_LOCAL),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_FALLBACK_SMALL),
            (unsigned long)arts_db_rma_stat_get(ARTS_RMA_STAT_FALLBACK_DEST));
}
