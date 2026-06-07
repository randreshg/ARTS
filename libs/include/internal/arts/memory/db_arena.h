/******************************************************************************
** GASNet RMA DB arena.
**
** DB bodies allocated from this arena live inside the GASNet-EX registered
** segment and are valid one-sided RMA targets. Disabled, unsupported, or
** exhausted arena allocation falls back to heap storage and Medium-AM transfer.
******************************************************************************/
#ifndef ARTS_MEMORY_DB_ARENA_H
#define ARTS_MEMORY_DB_ARENA_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Initialize the DB arena from the active transport's RMA segment. Idempotent;
 * safe to call when no segment exists (stays disabled). Call once after the
 * transport segment is attached. Honors ARTS_RMA_DBMOVE (default on under a
 * GASNet segment; set to 0 to disable) and ARTS_RMA_MIN_BYTES (transfer size
 * threshold below which Medium-AM is used even when RMA-capable).
 */
void arts_db_arena_init(void);

/* True iff the RMA DB-move path is live. */
bool arts_db_rma_enabled(void);

/* Minimum DB body size (bytes) for which RMA is worthwhile; smaller transfers
 * take the Medium-AM path. */
uint64_t arts_db_rma_min_bytes(void);

/* Allocate/free segment-resident DB storage. arts_db_arena_alloc returns NULL
 * when the arena is disabled or exhausted (caller falls back to the heap).
 * Returned pointers are aligned to at least ARTS_DB_ARENA_ALIGN. */
void *arts_db_arena_alloc(size_t size);
void arts_db_arena_free(void *ptr);

/* True iff ptr was handed out by this arena. */
bool arts_db_arena_owns(const void *ptr);

/* RMA DB-move counters (per rank, process-lifetime). Lightweight evidence that
 * the intended path fired or why it fell back. */
typedef enum {
  ARTS_RMA_STAT_PUT_FIRED,      /* owner: a gex_RMA_Put completed */
  ARTS_RMA_STAT_OFFER_SENT,     /* owner: an RMA OFFER was emitted */
  ARTS_RMA_STAT_LANDED,         /* requester: a DONE installed an RMA DB */
  ARTS_RMA_STAT_FALLBACK_OFF,   /* RMA disabled / not capable */
  ARTS_RMA_STAT_FALLBACK_LOCAL, /* source body not segment-resident */
  ARTS_RMA_STAT_FALLBACK_SMALL, /* below size threshold */
  ARTS_RMA_STAT_FALLBACK_DEST,  /* requester could not register landing */
  ARTS_RMA_STAT__COUNT
} arts_rma_stat_t;

void arts_db_rma_stat_inc(arts_rma_stat_t which);
uint64_t arts_db_rma_stat_get(arts_rma_stat_t which);
/* Emit the counter vector via ARTS_INFO (called at cleanup; safe anytime). */
void arts_db_rma_stats_dump(void);

#ifdef __cplusplus
}
#endif
#endif
