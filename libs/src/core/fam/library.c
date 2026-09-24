/* SPDX-License-Identifier: Apache-2.0
 *
 * The runtime's one adapter to the device library API, for both backends:
 * one global allocation per run, taken by rank 0 and carved into equal
 * slices.  Allocation on this medium is mediated and expensive, so it happens
 * exactly once; every subsequent placement is software.
 *
 * The library maps one region at one fixed address in every process that
 * loads it, which is what lets a pool address mean the same thing in every
 * rank.  Under SHM that region is one host's shared memory with one fixed
 * name, so a host runs one FAM run at a time: a second run would find the
 * first one's region live. */
#include <MemOps.h>
#include <SharedAlloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARTS_FAM_FLUSH_RECORDER
#include <stdatomic.h>

#include "arts/runtime_state.h"
#endif

#include "arts.h"
#include "arts/fam/pool.h"
#include "arts/system/config.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"

static void *g_arena;
static void *g_arena_alloc;
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
  /* The device library promises no alignment beyond a cache line, and the
   * pool needs a page-aligned base: one page more is taken and the base is
   * aligned up inside it, so [base, base + bytes) always lies within the
   * allocation and the size every rank is told is exactly the pool's. */
  void *alloc = GLOBAL_CXL_MALLOC((size_t)(bytes + ARTS_FAM_PAGE));
  if (!alloc) {
    ARTS_ERROR("fam: the device library returned no arena for %llu bytes - "
               "raise or lower fam_pool_mb",
               (unsigned long long)(bytes + ARTS_FAM_PAGE));
  }
  void *base = (void *)(((uintptr_t)alloc + ARTS_FAM_PAGE - 1) &
                        ~(uintptr_t)(ARTS_FAM_PAGE - 1));
  if (!IS_FAM_PTR(base) ||
      !IS_FAM_PTR((char *)base + bytes - 1)) {
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
  g_arena_alloc = alloc;
  g_arena_bytes = bytes;
}

void arts_fam_boot_prepare(const struct arts_config_s *config) {
  /* The arena is taken at the exchange, which is where its base can be told
   * to the other ranks; only the size is known this early. */
  g_pool_mb = config->fam_pool_mb;
}

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

/* A window into this adapter's file statics, for fam_device_frame alone. */
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
   * The size is checked against this rank's OWN configured pool size: every
   * rank parses its own cfg, so two ranks can name different fam_pool_mb
   * values, and only the allocating rank's is the size the arena has. */
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
  arts_fam_backend_flush(*out_base, sizeof(struct arts_fam_header_s), false);
}

void arts_fam_backend_unmap(void *base, uint64_t bytes) {
  (void)bytes;
  /* Only where no peer can still hold it: the shutdown protocol drains
   * messages, not mappings, so there is no point at which the allocating rank
   * knows every peer has stopped reading.  Past one rank the arena is left to
   * process teardown. */
  if (base && base == g_arena && arts_global_rank_count == 1) {
    GLOBAL_CXL_FREE(g_arena_alloc);
  }
  g_arena = NULL;
  g_arena_alloc = NULL;
  g_arena_bytes = 0;
}

#ifdef ARTS_FAM_FLUSH_RECORDER
/* A slot is published by its stamp, seq + 1, stored after the rest of the
 * record, so a reader that sees the stamp sees the whole record. */
static struct {
  struct arts_fam_flush_record_s rec;
  _Atomic uint64_t stamp;
} g_flush_ring[ARTS_FAM_FLUSH_RECORD_CAP];
static _Atomic uint64_t g_flush_next;

static void fam_flush_record(const void *p, size_t bytes, bool producer) {
  uint64_t seq =
      atomic_fetch_add_explicit(&g_flush_next, 1u, memory_order_relaxed);
  if (seq >= ARTS_FAM_FLUSH_RECORD_CAP) {
    return;
  }
  g_flush_ring[seq].rec = (struct arts_fam_flush_record_s){
      .seq = seq,
      .addr = (uintptr_t)p,
      .bytes = bytes,
      .producer = producer,
      .role = (unsigned)arts_thread_info.role};
  atomic_store_explicit(&g_flush_ring[seq].stamp, seq + 1u,
                        memory_order_release);
}

uint64_t arts_fam_flush_record_count(void) {
  return atomic_load_explicit(&g_flush_next, memory_order_acquire);
}

bool arts_fam_flush_record_get(uint64_t i,
                               struct arts_fam_flush_record_s *out) {
  if (i >= ARTS_FAM_FLUSH_RECORD_CAP ||
      atomic_load_explicit(&g_flush_ring[i].stamp, memory_order_acquire) !=
          i + 1u) {
    return false;
  }
  *out = g_flush_ring[i].rec;
  return true;
}
#endif /* ARTS_FAM_FLUSH_RECORDER */

void arts_fam_backend_flush(const void *p, size_t bytes, bool producer) {
#ifdef ARTS_FAM_FLUSH_RECORDER
  fam_flush_record(p, bytes, producer);
#endif
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

#ifdef ARTS_FAM_BACKEND_SHM
/* MemAvailable, in MB, or 0 when it cannot be read. */
static uint64_t fam_mem_available_mb(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) {
    return 0;
  }
  char line[256];
  unsigned long long kb = 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
      break;
    }
  }
  (void)fclose(f);
  return (uint64_t)(kb / 1024u);
}

/* The region is one host's memory, so every rank must share that host.  A
 * remote launcher puts each rank on its own host, so past one rank only the
 * local launcher can run.  The launcher is decided by handle_launcher, where a
 * scheduler's variables in the environment override the cfg and an absent key
 * defaults to ssh, so the message names whichever decided it. */
void arts_fam_backend_config_check(const struct arts_config_s *config) {
  /* The region is this host's RAM, so a pool past what the host has free is
   * worth a word; it is never refused, since the host fails loudly if memory
   * really runs out. */
  uint64_t avail_mb = fam_mem_available_mb();
  if (avail_mb && (uint64_t)config->fam_pool_mb > avail_mb) {
    ARTS_WARN("fam: fam_pool_mb=%u exceeds this host's MemAvailable of %llu MB",
              config->fam_pool_mb, (unsigned long long)avail_mb);
  }
  unsigned nranks = config->table_length ? config->table_length : 1u;
  if (nranks == 1u ||
      (config->launcher && strcmp(config->launcher, "local") == 0)) {
    return;
  }
  static const char *const deciders[] = {"SLURM_PROCID", "SLURM_NNODES",
                                         "LSB_HOSTS", "LSB_MCPU_HOSTS",
                                         "FLUX_TASK_RANK"};
  const char *decider = NULL;
  for (unsigned i = 0; i < sizeof(deciders) / sizeof(deciders[0]); i++) {
    if (getenv(deciders[i])) {
      decider = deciders[i];
      break;
    }
  }
  ARTS_ERROR("fam: this build's fabric-attached memory is the vendored fake "
             "library, one host's shared memory, so a run of more than one "
             "rank needs launcher=local; this run has %u ranks under "
             "launcher=%s%s%s",
             nranks, config->launcher ? config->launcher : "(unset)",
             decider ? ", decided by the environment variable " : "",
             decider ? decider : "");
}
#else
/* A device library places no constraint of its own on a run's shape. */
void arts_fam_backend_config_check(const struct arts_config_s *config) {
  (void)config;
}
#endif
