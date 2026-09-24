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
 * page: the strict oracle's privacy is page-granular, so a page that held two
 * ranks' lines would let one rank's write-back carry the other's. */
#define ARTS_FAM_PAGE 4096u
#define ARTS_FAM_HEADER_BYTES ARTS_FAM_PAGE

/* Ceiling on what one run's pool may cost the host it runs on, checked at
 * config load against that machine's own free memory: a pool may claim only a
 * fraction of what is free, leaving the runtime, the ranks and the page cache
 * the rest. */
#define ARTS_FAM_BUDGET_CAP_MB 2048u

/* The byte a fresh block is filled with, so that a read before the first write
 * is reproducible on every rank instead of reading the object's zeroes.  Spelled
 * here and nowhere else: strict.c writes it and the test that checks it reads
 * it, and two spellings of one oracle value is a test that can pass while the
 * code changed. */
#define ARTS_FAM_POISON_BYTE 0xA5u

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

/* Which rank's slice holds p, as a pure function of the address: the slice
 * index.  A slot's owner is therefore never a field and never a wire member --
 * the address alone says it, so no two-field publication can tear.  p must be
 * a pool address at or after the header page; anything else is fatal, because
 * every caller reaches this with a slot address and a wrong answer here would
 * address a message to the wrong rank. */
unsigned arts_fam_owner_of(const void *p);

/* Refuses, at config load and on every rank, a configuration this build's
 * backend cannot serve or this host cannot afford. */
void arts_fam_config_check(const struct arts_config_s *config);

/* Boot hooks.  arts_fam_boot_prepare runs on EVERY rank before anything else
 * maps and before the launcher forks: a backend whose region is inherited has
 * no later chance to create it, and a rank that mapped after the fabric and
 * the registered pool could find its fixed address taken while another rank
 * did not.  arts_fam_boot_child_exec runs in a forked child immediately before
 * it execs.  arts_fam_boot_launched runs on the rank that spawned the others
 * as soon as they are forked: a handoff a backend placed in the environment is
 * dropped there, before any thread that reads the environment exists, and a
 * rank that adopted the region has already dropped its own.  The two device
 * hooks ride the address exchange, the one round every rank already performs
 * before a worker thread exists. */
void arts_fam_boot_prepare(const struct arts_config_s *config);
void arts_fam_boot_child_exec(void);
void arts_fam_boot_launched(void);
void arts_fam_device_publish(uint64_t *base, uint64_t *size);
void arts_fam_device_record(unsigned from_rank, uint64_t base, uint64_t size);

/* The seam between the allocator and where the memory comes from.  Exactly one
 * backend TU is compiled into a build, and it defines every member: no TU that
 * a build compiles may name a symbol from the one it does not. */
void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes);
void arts_fam_backend_unmap(void *base, uint64_t bytes);
void arts_fam_backend_flush(const void *p, size_t bytes, bool producer);
void arts_fam_backend_poison(void *p, size_t bytes);
void arts_fam_backend_hold(const void *p, size_t bytes, bool hold);
bool arts_fam_backend_strict(void);
void arts_fam_backend_config_check(const struct arts_config_s *config);

/* Whether this rank runs with the second coherency domain emulated, so
 * that a test can assert the mode it is running under instead of passing
 * vacuously in the other one.  It is a per-rank fact of the rank's own parsed
 * config, decided before the first mapping; a backend that has no such mode
 * answers false. */
static inline bool arts_fam_strict(void) { return arts_fam_backend_strict(); }

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
 * Under strict mode that fence is necessary and still not sufficient for a
 * word that LIVES IN THE POOL: nothing refreshes such a word except a consumer
 * flush, and that flush reloads the whole line it sits in, discarding any
 * unflushed write this rank made anywhere in that line.  A word two parties
 * poll to coordinate therefore belongs in DRAM, where the runtime's own
 * protocol words are, and never in the pool. */
static inline void arts_fam_flush_producer(const void *p, size_t bytes) {
  arts_fam_backend_flush(p, bytes, true);
}
static inline void arts_fam_flush_consumer(const void *p, size_t bytes) {
  arts_fam_backend_flush(p, bytes, false);
}

/* Strict-mode hooks.  A range registered here is one this rank currently
 * holds, which is the only range the oracle's random write-back may touch.
 * No-ops when strict mode is off.
 *
 * The order is reload, then hold: a range is consumer-flushed BEFORE it is
 * registered, and a block is reloaded whole before its first write.  A hold --
 * or a producer flush of part of a block -- over lines this rank never
 * reloaded republishes whatever its pages captured over the previous writer's
 * bytes.  Once a range is registered the other direction is forbidden: a
 * consumer flush that covers a held line is fatal, because it would discard a
 * write only this rank has.
 *
 * A write-back moves a whole 64-byte line and is not atomic: the medium
 * carries no atomicity above eight bytes either, so a reader of bytes in a
 * line it holds no right to may see a mixture of two writers' values. */
static inline void arts_fam_strict_hold(const void *p, size_t bytes) {
  arts_fam_backend_hold(p, bytes, true);
}
static inline void arts_fam_strict_unhold(const void *p, size_t bytes) {
  arts_fam_backend_hold(p, bytes, false);
}

#else /* no fabric-attached memory in this build */

/* The six entry points the arm asks or tells on paths that exist in both
 * builds keep their names and signatures and answer inertly, so no call site
 * carries a conditional.  Every other entry point is absent on purpose: there
 * is no pool to allocate from, and a silent no-op would be a wrong answer. */
static inline bool arts_fam_contains(const void *p) {
  (void)p;
  return false;
}
static inline bool arts_fam_strict(void) { return false; }
static inline void arts_fam_flush_producer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
static inline void arts_fam_flush_consumer(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
static inline void arts_fam_strict_hold(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}
static inline void arts_fam_strict_unhold(const void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}

#endif /* ARTS_FAM */

/* Module-internal: the strict oracle, which exists wherever the store is one
 * host's memory -- the inherited mapping, and the device backend over the
 * vendored fake library -- and nowhere else.  Named from the backend TU and
 * from nowhere else but the read-only query below, which is what makes a
 * build over a real device library's freedom from these symbols structural
 * rather than inspected.  The poison is written through the backing as well
 * as the private view: a block just allocated has no holder and no reload
 * yet, so this is the one write outside a producer flush and the sampled
 * eviction that can reach the backing, and it can meet no other copy of its
 * lines. */
#if defined(ARTS_FAM) &&                                                       \
    (defined(ARTS_FAM_BACKEND_SHM) || defined(ARTS_FAM_DEVICE_VENDORED))
#define ARTS_FAM_HAS_STRICT 1
void *arts_fam_strict_map(int fd, void *want, uint64_t bytes);
#ifdef ARTS_FAM_BACKEND_DEVICE
/* Re-maps [base, base + bytes) of the device library's shared mapping private
 * at its own address, with the same backing object shared elsewhere; nothing
 * outside that range is touched.  Returns base. */
void *arts_fam_strict_remap(void *base, uint64_t bytes);
#endif
void *arts_fam_strict_shared_base(void);
void arts_fam_strict_unmap(void *base, uint64_t bytes);
void arts_fam_strict_flush(const void *p, size_t bytes, bool producer);
void arts_fam_strict_poison(void *p, size_t bytes);
void arts_fam_strict_hold_range(const void *p, size_t bytes, bool hold);
/* True when any line of [p, p+bytes) the oracle tracks is currently held.
 * Read-only -- claims, releases and moves no byte -- so a teardown walk may
 * call it to find a hold whose matching unhold never ran. */
bool arts_fam_strict_range_held(const void *p, size_t bytes);
#endif

#ifdef __cplusplus
}
#endif
#endif /* ARTS_FAM_POOL_H */
