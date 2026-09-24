/* SPDX-License-Identifier: Apache-2.0
 *
 * Fabric-attached memory: one pool per run, one slice per rank, at the same
 * address in every rank.  A rank allocates only from its own slice and keeps
 * every byte of allocator state in its own DRAM, so a create takes no
 * cross-host lock; the pool itself carries payload and nothing else. */
#ifndef ARTS_FAM_POOL_H
#define ARTS_FAM_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A flush's unit, and therefore the alignment of every block: with no
 * coherence between hosts, flushing a line two blocks share would carry one
 * block's bytes over the other's.  A slice's length divided by this is also the
 * allocator's index space, and the config check is what keeps that count below
 * the 32-bit sentinel the free lists reserve for "no granule". */
#define ARTS_FAM_GRANULE 64u

/* A slice's base and length are page multiples, and the header takes a whole
 * page, so no page holds bytes of two ranks' slices. */
#define ARTS_FAM_PAGE 4096u
#define ARTS_FAM_HEADER_BYTES ARTS_FAM_PAGE

#define ARTS_FAM_MAGIC 0x46414D5F41525453ULL

struct arts_config_s;

/* Immutable for the run's whole life, written once before any rank can map. */
struct arts_fam_header_s {
  uint64_t magic;
  uint64_t bytes;     /* the whole pool, header included */
  uint64_t slice_len; /* per rank, a multiple of ARTS_FAM_PAGE */
  uint32_t nranks;
  uint32_t reserved;
};
#ifndef __cplusplus
_Static_assert(sizeof(struct arts_fam_header_s) <= ARTS_FAM_HEADER_BYTES,
               "the pool header must fit the page reserved for it");
#endif

#ifdef ARTS_FAM

void arts_fam_init(unsigned rank, unsigned nranks);
void arts_fam_fini(void);

/* Never returns NULL: exhaustion is fatal and names fam_pool_mb, so a caller
 * that null-checks the result is checking a state that cannot exist. */
void *arts_fam_alloc(size_t bytes);

/* Double-free detection is best effort: nothing quarantines a granule, so a
 * stale free that arrives after the block was handed out again is undetectable
 * and frees the new holder's block. */
void arts_fam_free(void *p);
bool arts_fam_contains(const void *p);

/* The pool's base, where its header lives, or NULL outside the window between
 * arts_fam_init and arts_fam_fini. */
const void *arts_fam_pool_base(void);

/* Which rank's slice holds p, as a pure function of the address: the slice
 * index.  A slot's owner is therefore never a field and never a wire member --
 * the address alone says it, so no two-field publication can tear.  p must be
 * a pool address at or after the header page; anything else is fatal, because
 * every caller reaches this with a slot address and a wrong answer here would
 * address a message to the wrong rank. */
unsigned arts_fam_owner_of(const void *p);

/* Refuses, at config load and on every rank, a configuration this build's
 * backend cannot serve.  A pool is never refused for its size. */
void arts_fam_config_check(const struct arts_config_s *config);

/* Boot hook, on EVERY rank before the fabric and the registered pool exist:
 * it settles the pool size from the rank's own parsed config.  The arena itself is taken at the address exchange, the one round
 * every rank already performs before a worker thread exists: rank 0 takes it
 * and publishes its base, and every other rank records what rank 0 named. */
void arts_fam_boot_prepare(const struct arts_config_s *config);
void arts_fam_device_publish(uint64_t *base, uint64_t *size);
void arts_fam_device_record(unsigned from_rank, uint64_t base, uint64_t size);

/* The seam between the allocator and where the memory comes from.  Exactly one
 * backend TU is compiled into a build, and it defines every member: no TU that
 * a build compiles may name a symbol from the one it does not. */
void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes);
void arts_fam_backend_unmap(void *base, uint64_t bytes);
void arts_fam_backend_flush(const void *p, size_t bytes, bool producer);
void arts_fam_backend_config_check(const struct arts_config_s *config);

/* Both flushes end in a fence and a compiler memory barrier: the producer's
 * is a store fence, so its write-backs precede whatever tells a peer to read
 * them; the consumer's is a full fence, so the loads that follow it see the
 * invalidated lines.  The producer flush is therefore not a StoreLoad
 * barrier: a releaser that publishes its bytes and then LOADS a peer's word
 * to decide whether it may stand down must place an
 * atomic_thread_fence(memory_order_seq_cst) between the publication and that
 * load.  The flush does not stand in for it.  An empty range sweeps no line
 * and still ends in its fence.
 *
 * The pool is not coherent across hosts, so nothing but a consumer flush
 * refreshes a word that lives in it: a word two parties poll to coordinate
 * belongs in DRAM, where the runtime's own protocol words are, and never in
 * the pool.  A write-back moves a whole 64-byte line and is not atomic: the
 * medium carries no atomicity above eight bytes either, so a reader of bytes
 * in a line it holds no right to may see a mixture of two writers' values. */
static inline void arts_fam_flush_producer(const void *p, size_t bytes) {
  arts_fam_backend_flush(p, bytes, true);
}
static inline void arts_fam_flush_consumer(const void *p, size_t bytes) {
  arts_fam_backend_flush(p, bytes, false);
}

#ifdef ARTS_FAM_FLUSH_RECORDER
/* Test-only: with this defined, the adapter appends every flush it performs
 * to a fixed per-rank ring, in the order the flushes were entered.  A ring
 * that fills stops recording and says so; it never wraps, so a record once
 * readable never changes. */
#define ARTS_FAM_FLUSH_RECORD_CAP 4096u
struct arts_fam_flush_record_s {
  uint64_t seq; /* position in this rank's order, from 0 */
  uintptr_t addr;
  size_t bytes;
  bool producer;
  unsigned role; /* the flushing thread's enum arts_thread_role */
};
/* How many flushes this rank has entered, overflow included. */
uint64_t arts_fam_flush_record_count(void);
/* Copies record i into *out; false while it is not yet complete, or past the
 * ring's capacity. */
bool arts_fam_flush_record_get(uint64_t i,
                               struct arts_fam_flush_record_s *out);
#endif /* ARTS_FAM_FLUSH_RECORDER */

#else /* no fabric-attached memory in this build */

/* The three entry points the arm asks or tells on paths that exist in both
 * builds keep their names and signatures and answer inertly, so no call site
 * carries a conditional.  Every other entry point is absent on purpose: there
 * is no pool to allocate from, and a silent no-op would be a wrong answer. */
static inline bool arts_fam_contains(const void *p) {
  (void)p;
  return false;
}
static inline void arts_fam_flush_producer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
static inline void arts_fam_flush_consumer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}

#endif /* ARTS_FAM */

#ifdef __cplusplus
}
#endif
#endif /* ARTS_FAM_POOL_H */
