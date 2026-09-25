/* SPDX-License-Identifier: Apache-2.0
 *
 * The CXL store: where a data block's bytes rest in a CXL build.  Slots come
 * from the shared arena rank 0 creates and every rank attaches to; a slot is
 * minted once per block by its creator, named by its address alone, and never
 * reclaimed within a run (the arena is a bump allocator).  A flush's unit is
 * a cache line, so every slot is 64-byte aligned. */
#ifndef ARTS_CXL_STORE_H
#define ARTS_CXL_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARTS_CXL_GRANULE 64u

#ifdef ARTS_USE_CXL

/* Never returns NULL: exhaustion is fatal and names the arena size. */
void *arts_cxl_store_alloc(size_t bytes);
bool arts_cxl_store_contains(const void *p);

/* The device library's own sweep over [p, p + bytes), then the fence the two
 * ownership edges rely on: a store fence with a compiler barrier for the
 * producer, a full fence for the consumer, whose invalidations must precede
 * the loads that follow -- a load-ordering property a store fence does not
 * give.  Neither is a StoreLoad barrier for anything else. */
void arts_cxl_store_flush(const void *p, size_t bytes, bool producer);

static inline void arts_cxl_flush_producer(const void *p, size_t bytes) {
  arts_cxl_store_flush(p, bytes, true);
}
static inline void arts_cxl_flush_consumer(const void *p, size_t bytes) {
  arts_cxl_store_flush(p, bytes, false);
}

#ifdef ARTS_CXL_FLUSH_RECORDER
/* Every flush this rank performed, in order, for the test that pins the two
 * edges; fixed capacity, later flushes are dropped from the record. */
#define ARTS_CXL_FLUSH_RECORD_CAP 4096u
struct arts_cxl_flush_record_s {
  uint64_t seq;
  uintptr_t addr;
  size_t bytes;
  bool producer;
};
uint64_t arts_cxl_flush_record_count(void);
bool arts_cxl_flush_record_get(uint64_t i, struct arts_cxl_flush_record_s *out);
#endif

#else /* !ARTS_USE_CXL */

/* A build with no CXL store knows no slot; the call sites are the same. */
static inline bool arts_cxl_store_contains(const void *p) {
  (void)p;
  return false;
}
static inline void arts_cxl_flush_producer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
static inline void arts_cxl_flush_consumer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}

#endif /* ARTS_USE_CXL */

#ifdef __cplusplus
}
#endif
#endif /* ARTS_CXL_STORE_H */
