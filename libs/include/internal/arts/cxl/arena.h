/******************************************************************************
 * Copyright 2019 Battelle Memorial Institute
 * Licensed under the Apache License, Version 2.0
 ******************************************************************************/
#ifndef ARTS_CXL_ARENA_H
#define ARTS_CXL_ARENA_H
#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "arts/cxl/lock.h"
#include "arts/cxl/wrapper.h"

extern unsigned int arts_global_rank_count;
extern unsigned int arts_global_rank_id;

/* ── Arena allocator (bump pointer on CXL global memory) ────────────────────
 */

typedef union {
  struct {
    char *base;
    char *head;
    char *max_size;
    bool initialized;
  };
  uint8_t pad[((sizeof(char *) * 3 + sizeof(bool) + CACHELINE_SIZE - 1) /
               CACHELINE_SIZE) *
              (CACHELINE_SIZE / sizeof(uint8_t))];
} arts_cxl_arena_t;

/* Maximum number of CXL devices supported for DB arena allocation. */
#define ARTS_CXL_MAX_DEVICES 16

/* ── Arena table (cache-line padded) ────────────────────────────────────────
 * The store's data-block arenas, created once by rank 0 and attached to by
 * every other rank; the lock serializes a bump across ranks. */

typedef union {
  struct {
    arts_cxl_arena_t
        *db_arenas[ARTS_CXL_MAX_DEVICES]; /**< One arena per CXL device. */
    unsigned int db_arena_count; /**< Number of active db_arenas entries. */
    arts_cxl_tournament_lock_t *lock;
  };
  uint8_t pad[((sizeof(arts_cxl_arena_t *) * ARTS_CXL_MAX_DEVICES +
                sizeof(unsigned int) + sizeof(arts_cxl_tournament_lock_t *) +
                CACHELINE_SIZE - 1) /
               CACHELINE_SIZE) *
              (CACHELINE_SIZE / sizeof(uint8_t))];
} arts_cxl_arena_table_t;

/* A flush covers whole cache lines, so the table must span exactly its own. */
#ifdef __cplusplus
static_assert(sizeof(arts_cxl_arena_table_t) % CACHELINE_SIZE == 0,
              "arts_cxl_arena_table_t must span whole cache lines");
#else
_Static_assert(sizeof(arts_cxl_arena_table_t) % CACHELINE_SIZE == 0,
               "arts_cxl_arena_table_t must span whole cache lines");
#endif

/* ── Arena operations ───────────────────────────────────────────────────────
 */

static inline void arts_cxl_arena_init(arts_cxl_arena_t **arena, size_t bytes) {
  *arena = (arts_cxl_arena_t *)GLOBAL_MALLOC(sizeof(arts_cxl_arena_t));
  char *memory = (char *)GLOBAL_MALLOC(bytes);
  (*arena)->base = memory;
  (*arena)->head = memory;
  (*arena)->initialized = true;
  (*arena)->max_size = memory + bytes;
  FLUSH_FENCE_PRODUCER(*arena, sizeof(arts_cxl_arena_t));
}

/**
 * arts_cxl_arena_init_dev — Allocate a DB arena on a specific CXL device.
 *
 * Uses GLOBAL_MALLOC_DEV to place the backing memory on @p dev_id.
 * The arena metadata struct itself is allocated with GLOBAL_MALLOC (any
 * device).
 */
static inline void arts_cxl_arena_init_dev(arts_cxl_arena_t **arena,
                                           size_t bytes, uint64_t dev_id) {
  *arena = (arts_cxl_arena_t *)GLOBAL_MALLOC(sizeof(arts_cxl_arena_t));
  char *memory = (char *)GLOBAL_MALLOC_DEV(bytes, dev_id);
  (*arena)->base = memory;
  (*arena)->head = memory;
  (*arena)->initialized = true;
  (*arena)->max_size = memory + bytes;
}

static inline void arts_cxl_arena_free(arts_cxl_arena_t *arena) {
  if (arena) {
    GLOBAL_FREE(arena->base);
    GLOBAL_FREE(arena);
  }
}

static inline void *arts_cxl_arena_malloc(arts_cxl_arena_t *arena,
                                          size_t bytes) {
  FLUSH_FENCE_CONSUMER(arena, sizeof(arts_cxl_arena_t));

  size_t aligned_bytes = ALIGN_UP(bytes, CACHELINE_SIZE);
  uintptr_t head_addr = (uintptr_t)arena->head;
  uintptr_t aligned_head = ALIGN_UP(head_addr, CACHELINE_SIZE);
  uintptr_t new_head = aligned_head + aligned_bytes;

  if (new_head > (uintptr_t)arena->max_size) {
    return NULL;
  }
  arena->head = (char *)new_head;
  FLUSH_FENCE_PRODUCER(arena, sizeof(arts_cxl_arena_t));
  return (void *)aligned_head;
}

/* ── Arena table lifecycle ──────────────────────────────────────────────────
 */

/**
 * arts_cxl_arena_table_create — Create the arena table with DB arenas
 * allocated on specific devices, and publish it.
 *
 * @param dev_ids   Array of device IDs to allocate arenas on.
 * @param dev_count Number of devices (length of dev_ids).
 *
 * For the static strategy, pass a single-element array with the chosen device.
 * For round-robin, pass all device IDs.
 */
static inline arts_cxl_arena_table_t *
arts_cxl_arena_table_create(const uint64_t *dev_ids, unsigned int dev_count) {
  assert(dev_count > 0 && dev_count <= ARTS_CXL_MAX_DEVICES &&
         "dev_count must be in [1, ARTS_CXL_MAX_DEVICES]");

  arts_cxl_arena_table_t *t =
      (arts_cxl_arena_table_t *)SHARED_MALLOC(sizeof(arts_cxl_arena_table_t));

  assert(t && "Allocated CXL arena table pointer is valid");

  /* WORKAROUND: GLOBAL_CXL_MALLOC_DEV returns a stack pointer on the device
   * library; revert once fixed. */
  (void)dev_ids;
  for (unsigned int i = 0; i < dev_count; i++) {
    arts_cxl_arena_init(&t->db_arenas[i], ARTS_CXL_DB_ARENA_SIZE_BYTES);
  }
  for (unsigned int i = dev_count; i < ARTS_CXL_MAX_DEVICES; i++) {
    t->db_arenas[i] = NULL;
  }
  t->db_arena_count = dev_count;

  t->lock = arts_cxl_tournament_lock_new(arts_global_rank_count);

  FLUSH_FENCE_PRODUCER(t, sizeof(arts_cxl_arena_table_t));
  SHARED_MALLOC_INITIALIZED(t);
  return t;
}

/** The table rank 0 published, as every other rank attaches to it. */
static inline arts_cxl_arena_table_t *arts_cxl_arena_table_attach(void) {
  arts_cxl_arena_table_t *t = (arts_cxl_arena_table_t *)LAST_SHARED_MALLOC(
      sizeof(arts_cxl_arena_table_t));
  FLUSH_FENCE_CONSUMER(t, sizeof(arts_cxl_arena_table_t));
  return t;
}

/** Only rank 0, which created the table, frees it. */
static inline void arts_cxl_arena_table_free(arts_cxl_arena_table_t *t) {
  if (!arts_global_rank_id) {
    for (unsigned int i = 0; i < t->db_arena_count; i++) {
      arts_cxl_arena_free(t->db_arenas[i]);
    }
    arts_cxl_tournament_lock_delete(t->lock);
    SHARED_FREE(t);
  }
}

/** True when every one of the table's arenas is present and initialized. */
static inline bool arts_cxl_arena_table_ready(arts_cxl_arena_table_t *t) {
  FLUSH_FENCE_CONSUMER(t, sizeof(arts_cxl_arena_table_t));
  if (t->db_arena_count == 0) {
    return false;
  }
  for (unsigned int i = 0; i < t->db_arena_count; i++) {
    if (!t->db_arenas[i]) {
      return false;
    }
    FLUSH_FENCE_CONSUMER(t->db_arenas[i], sizeof(arts_cxl_arena_t));
    if (!t->db_arenas[i]->initialized) {
      return false;
    }
  }
  return true;
}

/* ── Locked DB arena malloc (device-indexed) ────────────────────────────────
 */

/**
 * arts_cxl_arena_table_malloc — Allocate from a specific device's DB arena.
 *
 * @param t          The arena table.
 * @param local_lock Per-node pthread mutex for local serialization.
 * @param size       Allocation size in bytes.
 * @param dev_idx    Index into t->db_arenas[] (0-based).
 *                   Must be < t->db_arena_count.
 */
static inline void *arts_cxl_arena_table_malloc(arts_cxl_arena_table_t *t,
                                                pthread_mutex_t *local_lock,
                                                size_t size,
                                                unsigned int dev_idx) {
  assert(dev_idx < t->db_arena_count && "dev_idx out of range");
  arts_cxl_tournament_lock_acquire(t->lock, local_lock, arts_global_rank_id);
  arts_cxl_arena_t *arena = t->db_arenas[dev_idx];
  FLUSH_FENCE_CONSUMER(arena, sizeof(arts_cxl_arena_t));
  void *ptr = arts_cxl_arena_malloc(arena, size);
  FLUSH_FENCE_PRODUCER(arena, sizeof(arts_cxl_arena_t));
  arts_cxl_tournament_lock_release(t->lock, local_lock, arts_global_rank_id);
  return ptr;
}

#ifdef __cplusplus
}
#endif
#endif /* ARTS_CXL_ARENA_H */
