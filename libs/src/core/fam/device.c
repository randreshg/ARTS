/* SPDX-License-Identifier: Apache-2.0
 *
 * One global allocation per run, taken by rank 0 and carved into equal
 * slices.  Allocation on this medium is mediated and expensive, so it happens
 * exactly once; every subsequent placement is software. */
#include <MemOps.h>
#include <SharedAlloc.h>

#include <string.h>

#include "arts.h"
#include "arts/fam/pool.h"
#include "arts/system/config.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"

static void *g_arena;
static uint64_t g_arena_bytes;
static uint64_t g_recorded_base;
static uint64_t g_recorded_bytes;
static unsigned g_pool_mb;

/* The pool size this rank's own config names, settled in
 * arts_fam_boot_prepare.  Zero means that hook never ran, which nothing later
 * would recognise as such: it would read as a rank asking for a zero-byte
 * pool. */
static uint64_t fam_pool_bytes(void) {
  if (g_pool_mb == 0) {
    ARTS_ERROR("fam: the pool size was never settled - arts_fam_boot_prepare "
               "must run on every rank before anything maps");
  }
  return (uint64_t)g_pool_mb * 1024u * 1024u;
}

static uint64_t fam_slice_len(uint64_t bytes, unsigned nranks) {
  uint64_t usable = bytes - ARTS_FAM_HEADER_BYTES;
  uint64_t per = usable / nranks;
  return per - (per % ARTS_FAM_PAGE);
}

static void fam_take_arena(unsigned nranks) {
  uint64_t bytes = fam_pool_bytes();
  void *base = GLOBAL_CXL_MALLOC((size_t)bytes);
  if (!base) {
    ARTS_ERROR("fam: the device library returned no arena for %llu bytes - "
               "raise or lower fam_pool_mb",
               (unsigned long long)bytes);
  }
  if (!IS_FAM_PTR(base)) {
    ARTS_ERROR("fam: the arena the device library returned is not "
               "fabric-attached memory");
  }
  struct arts_fam_header_s *h = (struct arts_fam_header_s *)base;
  memset(h, 0, sizeof(*h));
  h->magic = ARTS_FAM_MAGIC;
  h->bytes = bytes;
  h->slice_len = fam_slice_len(bytes, nranks);
  h->nranks = nranks;
  FLUSH_FENCE_PRODUCER(h, sizeof(*h));
  g_arena = base;
  g_arena_bytes = bytes;
}

void arts_fam_boot_prepare(const struct arts_config_s *config) {
  /* The arena is taken at the exchange, which is where its base can be told
   * to the other ranks; only the size is known this early. */
  g_pool_mb = config->fam_pool_mb;
}

void arts_fam_boot_child_exec(void) {}

void arts_fam_boot_launched(void) {}

void arts_fam_device_publish(uint64_t *base, uint64_t *size) {
  if (arts_global_rank_id != 0) {
    *base = 0;
    *size = 0;
    return;
  }
  if (!g_arena) {
    fam_take_arena(arts_global_rank_count);
  }
  *base = (uint64_t)(uintptr_t)g_arena;
  *size = g_arena_bytes;
}

void arts_fam_device_record(unsigned from_rank, uint64_t base, uint64_t size) {
  if (from_rank != 0 || base == 0) {
    return;
  }
  g_recorded_base = base;
  g_recorded_bytes = size;
}

/* A window into this backend's file statics, for fam_device_frame alone. */
uint64_t arts_fam_device_recorded_base(void) { return g_recorded_base; }

void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes) {
  (void)rank;
  /* A one-rank run has no exchange to ride, so it takes the arena here. */
  if (nranks == 1) {
    if (!g_arena) {
      fam_take_arena(1);
    }
    *out_base = g_arena;
    *out_bytes = g_arena_bytes;
    return;
  }
  if (arts_global_rank_id == 0) {
    *out_base = g_arena;
    *out_bytes = g_arena_bytes;
    return;
  }
  /* Everything below is a peer's word taken off the wire, and the consumer
   * flush at the end of this branch is the first thing that touches it, so
   * every property the arena must have is established before that, not after.
   * The size is checked against this rank's OWN configured pool size for the
   * same reason the inherited backend checks its adopted object's: every rank
   * parses its own cfg, so two ranks can name different fam_pool_mb values,
   * and only the allocating rank's is the size the arena actually has. */
  if (!g_recorded_base) {
    ARTS_ERROR("fam: no arena arrived with the address exchange");
  }
  if (g_recorded_base % ARTS_FAM_PAGE) {
    ARTS_ERROR("fam: the arena the exchange named (0x%llx) is not aligned to "
               "the pool's %u-byte page",
               (unsigned long long)g_recorded_base, ARTS_FAM_PAGE);
  }
  void *base = (void *)(uintptr_t)g_recorded_base;
  if (!IS_FAM_PTR(base)) {
    ARTS_ERROR("fam: the arena the exchange named (%p) is not fabric-attached "
               "memory",
               base);
  }
  uint64_t want = fam_pool_bytes();
  if (g_recorded_bytes != want) {
    ARTS_ERROR("fam: the arena is %llu bytes, but this rank's fam_pool_mb=%u "
               "asks for %llu - every rank of a run must name the same value",
               (unsigned long long)g_recorded_bytes, g_pool_mb,
               (unsigned long long)want);
  }
  *out_base = base;
  *out_bytes = g_recorded_bytes;
  FLUSH_FENCE_CONSUMER(*out_base, sizeof(struct arts_fam_header_s));
}

void arts_fam_backend_unmap(void *base, uint64_t bytes) {
  (void)bytes;
  /* Only where no peer can still hold it: the shutdown protocol drains
   * messages, not mappings, so there is no point at which the allocating rank
   * knows every peer has stopped reading.  Past one rank the arena is left to
   * process teardown. */
  if (base && base == g_arena && arts_global_rank_count == 1) {
    GLOBAL_CXL_FREE(base);
  }
  g_arena = NULL;
  g_arena_bytes = 0;
}

void arts_fam_backend_flush(const void *p, size_t bytes, bool producer) {
  /* The device library's own sweep over the range, then the fence pool.h's
   * contract puts at the end of both flushes: a store fence with a compiler
   * barrier for the producer, and a full fence for the consumer, whose
   * invalidations must precede the loads that follow -- a load-ordering
   * property a store fence does not give. */
  if (producer) {
    FLUSH_FENCE_PRODUCER(p, bytes);
    __asm__ __volatile__("sfence" ::: "memory");
  } else {
    FLUSH_FENCE_CONSUMER(p, bytes);
    __asm__ __volatile__("mfence" ::: "memory");
  }
}

/* The device has the second coherency domain the strict oracle emulates, so
 * both hooks are nothing here, and the predicate is always false -- and
 * because all three are DEFINED here, a DEVICE build never names a strict
 * symbol at all. */
void arts_fam_backend_poison(void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
void arts_fam_backend_hold(const void *p, size_t bytes, bool hold) {
  (void)p;
  (void)bytes;
  (void)hold;
}
bool arts_fam_backend_strict(void) { return false; }

void arts_fam_backend_config_check(const struct arts_config_s *config) {
  if (config->fam_strict) {
    ARTS_ERROR("fam_strict emulates a second coherency domain, which this "
               "build's memory already has; remove fam_strict from the cfg");
  }
}
