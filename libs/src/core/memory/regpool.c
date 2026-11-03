/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/* Registered slab pool.  See arts/memory/regpool.h for the contract.
 *
 * Layout of a slab: a large anonymous mapping placed by preference — one
 * NUMA node for an arena slab, the caller's whole node set for an oversize
 * one (see regpool_place) — and populated at creation, so a slab's
 * residency is decided when the slab is made and the pool's resident set is
 * its mapped capacity.  When a fabric domain is supplied the whole mapping
 * is pinned behind one memory-registration handle so a NIC can target any
 * byte with no per-alloc registration.  The mapping is then handed to an
 * allocator arena that is *exclusive* — the arena's heaps allocate only
 * inside it and, on exhaustion, return NULL instead of falling back to
 * unregistered OS memory.  That is the mechanism that keeps every returned
 * pointer inside a registered range.
 *
 * The slab table is grow-only: entries are never moved or removed, only
 * appended (publishing the new count with a release-store) or, for a direct
 * (oversize single-allocation) slab, retired and reused in place.  Lookups
 * take an acquire-load of the count and scan without a lock, so a lookup may
 * run concurrently with an allocation that is appending, retiring or reusing
 * a slab.  Records live in a fixed-capacity array so their addresses never
 * move, which is what lets arts_regpool_lookup hand back a stable pointer
 * into the table.
 *
 * A direct slab's whole mapping backs exactly one allocation, so a free
 * leaves that mapping owned by nobody — the same precondition an arena-slab
 * free already relies on: a caller frees only after its own uses and any
 * in-flight transport operation against that memory have completed, so no
 * lookup can legitimately still be resolving the address when the free runs.
 * Mapping, placing, populating and registering a range costs orders of
 * magnitude more than reusing one, so the free RETIRES the mapping instead
 * of tearing it down, and the next request of the same length and alignment
 * takes it over.  Retired mappings are released when memory or a table slot
 * is actually wanted, so what they hold is bounded by the workload's own
 * peak concurrent count per block shape, exactly as an arena's free space
 * is.
 *
 * Three states per entry — live (owned by an allocation), retired (mapped,
 * owned by no allocation) and free (torn down, slot open for reuse) — need
 * their own publication discipline, since an entry already counts toward the
 * table's published length.  Two rules carry it, and the lock-free reader
 * depends on both:
 *
 *   - Only a free slot's fields may be rewritten, and the slot is published
 *     live only once every field is written (release-store of its state
 *     word); a retired slot keeps exactly the fields published when its
 *     mapping was created, so retirement and reuse change nothing but the
 *     state.
 *   - A lock-free reader takes an entry's state and range in one load: a
 *     single-copy-atomic word (arts/memory/regpool_entry.h) cannot pair one
 *     rewrite's base with another's length, so a range a reader reads always
 *     belongs to the state it read with it.
 */

#include "arts/memory/regpool.h"

#include "arts/memory/regpool_entry.h"
#include "arts/utils/stdatomic_compat.h"
#include <errno.h>  /* mmap failure diagnostics */
#include <limits.h> /* UINT_MAX: the grow-end count wraps */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* posix_memalign / free — non-mimalloc fallback path */
#include <string.h>

#include "arts/system/print.h" /* ARTS_ERROR — fail loudly on confinement loss */

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h> /* struct fid_ep for endpoint-bound MRs */

/* Per-node availability estimate over one node's meminfo stream: the node's
 * free pages, and nothing else.
 *
 * A mapping placed on a node by preference takes that node's free pages and
 * spills onto other nodes past them; it does not reclaim the node's file
 * cache to stay local.  Reclaimable cache is therefore not room a local
 * mapping can take, and the estimate answers "how much of this mapping
 * would land on this node", never "can this mapping be made" — under a
 * placement preference it always can.
 *
 * Contract: SIZE_MAX when MemFree cannot be read (unknown must not veto
 * placement).  Allocator-independent and pure over the stream, so it is
 * testable in isolation. */
size_t arts_regpool_parse_node_avail(FILE *f) {
  char line[192];
  while (fgets(line, sizeof(line), f) != NULL) {
    unsigned long v;
    if (sscanf(line, "Node %*d MemFree: %lu", &v) == 1) {
      return (size_t)v * 1024;
    }
  }
  return SIZE_MAX;
}

/* ------------------------------------------------------------------------- */
/* The pool is meaningful only with an arena-capable allocator.  Without one   */
/* it degrades to inert stubs so the library still links in a system-malloc    */
/* configuration; init reports failure rather than pretending to register.     */
/* ------------------------------------------------------------------------- */
#ifdef ARTS_MALLOC_MIMALLOC

#include <ctype.h>
#include <dirent.h>
#include <execinfo.h> /* backtrace for the allocator's error report */
#include <mimalloc.h>
#include <mimalloc/types.h> /* arena geometry the slab sizes mirror */
#include "arts/memory/regpool_class.h"
#include "arts/utils/malloc.h" /* the request classes' table */
#include <pthread.h>
#include <time.h>
#include <sched.h> /* CPU affinity mask, and the calling thread's CPU and node */
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* MPOL_PREFERRED from the kernel's set_mempolicy ABI, declared locally so the
 * module needs neither libnuma nor <linux/mempolicy.h> (which can clash with
 * other headers). */
#ifndef ARTS_MPOL_PREFERRED
#define ARTS_MPOL_PREFERRED 1
#endif
/* MPOL_INTERLEAVE from the same ABI: round-robin every fault of the range
 * over the nodes in the mask. */
#ifndef ARTS_MPOL_INTERLEAVE
#define ARTS_MPOL_INTERLEAVE 3
#endif
/* madvise advice that faults a range in and REPORTS failure (Linux 5.14+),
 * declared locally for older toolchain headers. */
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif

#define REGPOOL_MAX_SLABS 4096u
#define REGPOOL_MAX_NODES 64u /* single-word NUMA bitmask covers node < 64 */
/* Base alignment for every mapping, huge-page friendly.  An arena slab's base
 * is aligned further, to the allocator's arena alignment
 * (regpool_arena_align). */
#define REGPOOL_BASE_ALIGN ((size_t)2 * 1024 * 1024)
_Static_assert(REGPOOL_BASE_ALIGN == (size_t)1
                                         << ARTS_REGPOOL_ENTRY_GRANULE_SHIFT,
               "every mapping's base and length must be whole granules of an "
               "entry word");
/* Allocator arena minimum; smaller slabs are rejected by the arena manager. */
#define REGPOOL_MIN_SLAB ((size_t)32 * 1024 * 1024)
/* Slab-size granule and ceiling, mirroring the vendored allocator's arena
 * geometry: a managed range is trimmed to 32 MiB slices, and one range
 * consumes one global arena-table slot per 16 GiB (a larger range is split
 * into that many sub-arenas, each holding its own slot).  Sizing slabs on
 * the granule loses nothing to trimming, and capping them at exactly the
 * per-slot maximum makes every grow cost exactly one slot — the table, not
 * the machine, is the scarce resource, and the pool's reach is free slots
 * times the cap on any machine.  Drift against the vendored allocator
 * surfaces as trimming waste or sub-arena splitting, both visible in the
 * pool-state dump on an exhausted allocation. */
#define REGPOOL_SLAB_GRANULE ((size_t)32 * 1024 * 1024)
#define REGPOOL_SLAB_CAP ((size_t)16 * 1024 * 1024 * 1024)
/* The part of a grow's slab one thread populates at a time (see
 * regpool_grow_node).  A thread that finds every part of a slab claimed
 * waits for at most one part in flight per populating thread, and the
 * registration. */
#define REGPOOL_POPULATE_CHUNK REGPOOL_BASE_ALIGN
_Static_assert(REGPOOL_SLAB_GRANULE % REGPOOL_POPULATE_CHUNK == 0 &&
                   REGPOOL_SLAB_CAP / REGPOOL_POPULATE_CHUNK <= UINT32_MAX,
               "a slab must be a whole number of population chunks, counted "
               "in 32 bits");
/* Arena-table slots the floor's sizing assumes the general-purpose heap
 * takes from the same table.  Nothing holds them back: the pool may take
 * them, and its arena growth ends where the table refuses (or where the
 * machine or the pool's own slab table runs out). */
#define REGPOOL_TABLE_RESERVE 8u
/* Largest object an arena serves out of whole chunks; a request above it is
 * cheaper in a mapping of its own (see regpool_alloc_direct).  Mirrors the
 * vendored allocator's chunk geometry, asserted below so drift is a compile
 * error rather than silent per-object waste. */
#define REGPOOL_MAX_ARENA_OBJ ((size_t)32 * 1024 * 1024)
_Static_assert(REGPOOL_MAX_ARENA_OBJ == MI_ARENA_MAX_CHUNK_OBJ_SIZE,
               "regpool's oversize threshold must track the allocator's "
               "largest chunk object");
/* Largest alignment an exclusive arena honours: the allocator serves a
 * larger one outside every arena, which a heap confined to one refuses, so
 * such a request takes a mapping of its own however small it is. */
#define REGPOOL_MAX_ARENA_ALIGN ((size_t)64 * 1024)
_Static_assert(REGPOOL_MAX_ARENA_ALIGN == MI_PAGE_MAX_OVERALLOC_ALIGN,
               "regpool's arena alignment limit must track the allocator's");
/* The largest arena one table slot holds is a full bitmap of chunks of
 * slices (MI_ARENA_MAX_SIZE, whose bitmap bound is private to the
 * allocator's sources). */
_Static_assert(REGPOOL_SLAB_GRANULE == MI_ARENA_MIN_SIZE &&
                   REGPOOL_SLAB_CAP == (size_t)MI_BCHUNK_BITS * MI_BCHUNK_BITS *
                                           MI_ARENA_SLICE_SIZE,
               "a slab must be a whole number of the allocator's arena "
               "granules and at most one arena-table slot's worth");
/* Growth factor of a node's slab ladder, as a fraction (see
 * regpool_next_slab_locked). */
#define REGPOOL_GROWTH_NUM 3
#define REGPOOL_GROWTH_DEN 2
/* Payload alignment floor (matches the DB/CXL 64-byte payload invariant). */
#define REGPOOL_ALIGN_FLOOR ((size_t)64)

/* Lifecycle of one table entry.  An arena slab is created LIVE and stays
 * LIVE; the other two states belong to direct slabs. */
/* Slot open for reuse, its mapping torn down — or still being torn down by
 * an eviction outside the lock, which publishes FREE first.  The zero value,
 * so an unwritten slot is one readers skip. */
#define REGPOOL_SLOT_FREE 0u
/* Mapping owned by nobody, kept for reuse. */
#define REGPOOL_SLOT_RETIRED 1u
/* Mapping in use: by one allocation (direct), or by its arena. */
#define REGPOOL_SLOT_LIVE 2u

/* One slab record.  The embedded public view is what lookups return; the extra
 * fields drive free() and growth. */
typedef struct regpool_slab_s {
  arts_regpool_mr_t mr;   /* public: base, len, mr, rkey, numa_node          */
  bool interleaved;       /* true = spread over a node set rather than
                           * placed on mr.numa_node, which is then -1: the
                           * two together are the three placements a mapping
                           * can have (a node, a set, or the kernel's
                           * choice)                                        */
  /* is_direct, arena and owner are written once, when the entry is
   * appended, and never again: an arena slab is never retired, and a free
   * slot is only ever rewritten as a direct one.  That is what lets the arena
   * sweep read them after the state check alone. */
  bool is_direct;         /* true = oversize single-allocation direct slab   */
  mi_arena_id_t arena;    /* backing arena (arena slabs only; NULL if direct) */
  int owner;              /* node whose grow made this arena slab — its
                           * placement, or the node it was made for when the
                           * kernel placed it; -1 for a direct slab        */
  size_t align;           /* alignment the mapping was created with           */
  /* The registration's close failed, so it may still take a write at its
   * key: the range is never unmapped, evicted or closed again, and the
   * process exit reclaims both.  Written under g_lock (false whenever the
   * entry is written, true when arts_regpool_unregister's close fails); read
   * under g_lock. */
  bool mr_kept;
} regpool_slab_t;

/* --- module state (guarded by g_lock except where marked atomic) --------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* Waited on by a thread that finds its node's grow in flight with no part
 * of the slab left for it to populate, that wants the process-wide turn of
 * a kernel-placed grow while another holds it, or that waits for retired
 * mappings another thread is tearing down; broadcast when a grow opens a
 * round of population, when a grow ends (which is also where the
 * arena-table latch is set) and when the last teardown in flight ends. */
static pthread_cond_t g_grow_cv = PTHREAD_COND_INITIALIZER;
static bool g_inited;
/* g_domain, g_ep, g_slab_bytes and g_numa_nodes are read WITHOUT g_lock by
 * the mapping and allocation paths: they are written at init before any
 * allocation, and at unregister and cleanup, while no thread allocates from
 * the pool or reads a slab's registration (see the header).  A free may run
 * concurrently with unregister: the free path reads none of them. */
static struct fid_domain *g_domain;
/* Non-NULL selects the endpoint-bound registration discipline
 * (FI_MR_ENDPOINT): every MR is bound to this endpoint and enabled after
 * registration, and the remote key is read only after the enable.  The
 * endpoint must outlive every registration — arts_regpool_unregister exists
 * so the transport can close all MRs before closing it. */
static struct fid_ep *g_ep;
static size_t g_slab_bytes;
static unsigned g_numa_nodes;

/* The nodes this rank's threads run on: the set an oversize mapping is
 * interleaved across.  Resolved at init from what the caller supplied, or
 * from the process's CPU affinity when it supplied nothing.  Read without
 * g_lock by the mapping paths, like g_numa_nodes and for the same reason. */
static uint64_t g_node_set;

/* Relative access latency from each node to each other, row-major with a
 * stride of REGPOOL_MAX_NODES and the requesting node as the row, over the
 * g_distance_nodes nodes the caller described.  Only the fallback ORDER
 * consults it, so the values' scale is immaterial — only their comparison
 * is.  g_distance_known false means the machine reported none, and every
 * node is then equidistant; a pair outside the described ones is farther
 * than every described pair. */
static uint32_t g_node_distance[REGPOOL_MAX_NODES * REGPOOL_MAX_NODES];
static bool g_distance_known;
static unsigned g_distance_nodes; /* rows (and columns) the caller gave */

/* What arts_regpool_set_topology left for the next init to consume (0 = the
 * caller said nothing and init discovers it). */
static uint64_t g_req_node_set;
static unsigned g_req_node_count;

static regpool_slab_t g_slabs[REGPOOL_MAX_SLABS];
/* Entry i's state and range (arts/memory/regpool_entry.h; REGPOOL_SLOT_* in
 * the state field), dense beside the records so a scan touches one line per
 * eight entries and no line a rewrite writes.  Every write is a store under
 * g_lock: a release store of the LIVE word, after the record's fields are
 * written, publishes them to a lock-free reader's acquire load; a transition
 * between LIVE and RETIRED keeps the range, and an eviction stores 0 (FREE).
 * Only a FREE entry's record is rewritten, and a range given back is
 * reusable only after the munmap that follows its FREE store, so a mapping
 * that reuses it is published after that store in g_lock's order and its
 * owner's lookups read the old entry FREE or later.  Loads under g_lock are
 * relaxed. */
static _Atomic(uint64_t) g_slab_word[REGPOOL_MAX_SLABS];
/* Published length of g_slabs.  An append, under g_lock, stores it with
 * release after writing the entry; a lock-free lookup or sweep acquires it
 * before reading the entries below it. */
static _Atomic(size_t) g_slab_count;

/* Monotonic memory-registration key source.  When the provider does NOT
 * negotiate FI_MR_PROV_KEY the application must supply a UNIQUE requested_key
 * per fi_mr_reg — registering a second slab with a duplicate key (e.g. 0)
 * fails with FI_ENOKEY.  A unique key is also harmless when the provider DOES
 * assign keys itself (it then ignores requested_key), so a monotone counter is
 * correct for either negotiation.  A ticket: only its own value. */
static _Atomic(uint_least64_t) g_mr_key_next;

/* The arena a node's threads draw from first.  Room in an exclusive arena is
 * per thread -- its free slices are shared, but a heap's pages serve only the
 * thread that owns the heap -- so a refusal is a fact about one thread and
 * never moves this word: only node-wide facts do.  Its writers: a grow of the
 * node, which publishes the arena it built with a release store; the first
 * cascade that serves a node with no arena from another node's, which names
 * that arena with a release CAS from NULL (re-publishing an arena its caller
 * acquired); and init and cleanup, which reset it to NULL.  So within a pool
 * lifetime the word is NULL at most until its first value and then takes
 * only arenas grown since, and an arena id is its arena's address, never
 * reused because arenas are never unloaded: no value recurs, within a
 * lifetime or across lifetimes, other than NULL.  A reader that binds a heap
 * to the value acquires. */
static _Atomic(mi_arena_id_t) g_node_arena[REGPOOL_MAX_NODES];

/* Per-node grow count, for the report (guarded by g_lock). */
static unsigned g_node_grow_count[REGPOOL_MAX_NODES];

/* For the report, guarded by g_lock: the time a node's grows spent mapping
 * (summed over grows); the grow attempts that slept on the node's grow in
 * flight or on the process-wide turn of a kernel-placed grow, the times
 * they fell asleep and the time they slept; the attempts that populated
 * part of a slab another thread's grow reserved, the chunks they populated,
 * and every chunk populated for the node; the grows that read the node full
 * and mapped nothing, and the asks that took such a reading instead of
 * reading the node again (g_node_declined). */
static uint64_t g_node_grow_ns[REGPOOL_MAX_NODES];
static uint64_t g_node_wait_ns[REGPOOL_MAX_NODES];
static unsigned g_node_wait_count[REGPOOL_MAX_NODES];
static unsigned g_node_sleeps[REGPOOL_MAX_NODES];
static unsigned g_node_helpers[REGPOOL_MAX_NODES];
static unsigned g_node_help_chunks[REGPOOL_MAX_NODES];
static unsigned g_node_chunks[REGPOOL_MAX_NODES];
static unsigned g_node_declines[REGPOOL_MAX_NODES];
static unsigned g_node_decline_reuses[REGPOOL_MAX_NODES];

static uint64_t regpool_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Size in bytes of the largest arena slab MAPPED for a node, which is what
 * the next rung is derived from (0 = none yet).  Guarded by g_lock. */
static size_t g_node_last_slab[REGPOOL_MAX_NODES];

/* Allocations of a node's threads that a cascade served from another node's
 * arena because the node could not grow.  Diagnostic only, for the report:
 * only its own value. */
static _Atomic(unsigned) g_node_spills[REGPOOL_MAX_NODES];

/* Allocations of a node's threads that left the fast path for the exhaustion
 * cascade.  Diagnostic only, for the report: only its own value. */
static _Atomic(unsigned) g_node_cascades[REGPOOL_MAX_NODES];

/* Whether a node's memory has been carved yet.  A node is carved when a
 * thread on it first allocates (or another node's demand grows it), so an
 * UNTRIED node is memory that exists and has simply not been asked for; a
 * node the pool tried and could not carve is REFUSED, and its threads are
 * served from memory elsewhere until a later grow succeeds.  Written under
 * g_lock; a lock-free reader only decides whether to try a carve, which the
 * grow decides again under the lock, and the arena itself rides
 * g_node_arena: only its own value. */
#define REGPOOL_NODE_UNTRIED 0u
#define REGPOOL_NODE_CARVED 1u
#define REGPOOL_NODE_REFUSED 2u
static _Atomic(uint8_t) g_node_state[REGPOOL_MAX_NODES];

/* A grow in flight for this node (guarded by g_lock).  One at a time per
 * node: the mapping runs outside the lock, so the flag is what keeps a herd
 * of threads from mapping a slab each; the herd populates the one slab
 * instead (see regpool_grow_node). */
static bool g_node_growing[REGPOOL_MAX_NODES];

/* Grows of this node that have ended (guarded by g_lock): a grower knows its
 * own grow ended once the count moves past the value it read at the start. */
static unsigned g_node_grow_ends[REGPOOL_MAX_NODES];

/* Grows of every node that have ended (guarded by g_lock): the sequence a
 * decline record and a cascade's asks are compared by. */
static unsigned g_grows_ended;

/* One more than the table length at which a grow last read this node full
 * and mapped nothing, or 0, and that grow's end in g_grows_ended (guarded by
 * g_lock).  An own or peer ask made against that same length takes the
 * reading instead of reading the node and the machine again only when the
 * grow ended after its cascade's first ask (regpool_cascade_t), so a
 * cascade that begins after a declining grow ended reads the node again and
 * memory freed meanwhile is seen at most one cascade later.  The table
 * length only grows, so a reading of a length it has left is never
 * taken.  Mapping a slab for the node clears the record, and so do
 * giving retired mappings' memory back and changing the injected
 * exhaustion, either of which may let the node fit at the same length. */
static size_t g_node_declined[REGPOOL_MAX_NODES];
static unsigned g_node_declined_end[REGPOOL_MAX_NODES];

/* Times the records above were cleared (guarded by g_lock).  A grow records
 * its reading only when no clear came after it began reading, so a reading
 * that predates memory coming back is never recorded. */
static unsigned g_declines_cleared;

/* Caller holds g_lock. */
static void regpool_declines_clear_locked(void) {
  for (unsigned i = 0; i < REGPOOL_MAX_NODES; i++)
    g_node_declined[i] = 0;
  g_declines_cleared++;
}

/* A kernel-placed grow in flight in this process (guarded by g_lock).  Such
 * a slab is sized against the whole machine's availability, which concurrent
 * grows would each read before any of them lands and so spend several times
 * over; one at a time, each sized after the previous one is resident.  Other
 * processes on the machine, other ranks included, are outside the turn. */
static bool g_unplaced_growing;

/* Retired mappings being torn down outside g_lock (guarded by g_lock).
 * Their memory is not back until the teardown ends, so a request that looks
 * again for it waits for the count to reach zero on g_grow_cv.  Not reset by
 * init or cleanup: each eviction's increment and decrement pair up within
 * one call, so it reads zero whenever no eviction runs. */
static unsigned g_evicting;

/* One warning per outcome per node exhaustion, not one per refused grow: a
 * full node is re-tried by every allocation that prefers it, and each retry
 * would print.  One bit per outcome, because one exhaustion can pass through
 * several (a grow that declines, then the same node's kernel-placed one), and
 * the log must name each.  Guarded by g_lock (set and cleared only inside a
 * grow). */
#define REGPOOL_WARNED_SPILL 1u
#define REGPOOL_WARNED_UNPLACED 2u
#define REGPOOL_WARNED_NONE 4u
static uint8_t g_node_full_warned[REGPOOL_MAX_NODES];

#ifdef ARTS_DEBUG_INJECTION
/* Injected exhaustion: nodes whose bit is set report zero available bytes,
 * so the refusal, fallover and eviction paths are exercisable
 * deterministically without starving a machine.  Written under g_lock (by
 * the init-time parse of ARTS_REGPOOL_FORCE_FULL_NODES and by
 * arts_regpool_set_forced_full), read relaxed by the placement screens: only
 * its own value. */
static _Atomic(uint64_t) g_forced_full;

/* Publish a force-full mask.  Caller holds g_lock. */
static void regpool_store_forced_full_locked(uint64_t mask) {
  atomic_store_explicit(&g_forced_full, mask, memory_order_relaxed);
  regpool_declines_clear_locked();
}
#endif

/* Read at init: print the pool's shape at teardown. */
static bool g_report_enabled;

/* Kernels predating the populate advice report EINVAL; there a slab is
 * populated by a write per page instead, for the process lifetime.  Latched
 * on first sight, by mapping code that runs outside g_lock; the CAS winner
 * warns.  Only its own value. */
static _Atomic(bool) g_populate_unsupported;

/* Set once the allocator's arena table refuses a slab.  The table never
 * gives a slot back and a slab the pool hands it can be refused for no other
 * reason, so nothing later can be published: no later grow maps, populates or
 * registers anything.  Guarded by g_lock, with the count of refused slabs
 * for the report — at most one per grow already mapping when the first
 * refusal landed. */
static bool g_table_exhausted;
static unsigned g_table_refusals;

/* Arena-table slots the pool's arena slabs hold (a direct slab is a plain
 * mapping and holds none).  Never reset: an arena slab outlives the pool.
 * Guarded by g_lock. */
static unsigned g_pool_slots;

/* A heap of the calling thread and the exclusive arena it draws from. */
typedef struct {
  mi_heap_t *heap;
  mi_arena_id_t arena;
} regpool_binding_t;

/* A request class's binding on the calling thread, and the memo value it
 * answers for: g_node_arena[memo_node] held memo_seen when this thread last
 * bound that value for the class or was refused by it.  The binding is the
 * memo itself, or the arena that served the class instead after the memo
 * refused it.  Never a NULL memo value, which recurs across lifetimes: -1
 * then.  Room in an exclusive arena is per thread and per class (a page
 * serves one block size of one thread's heap; only free slices are shared),
 * so one class's refusal moves only that class's binding. */
typedef struct {
  uint64_t key; /* 0: an empty slot */
  int memo_node;
  mi_arena_id_t memo_seen;
  regpool_binding_t bound;
} regpool_class_t;

/* The calling thread's classes, an open-addressed table that only grows:
 * its keys are bounded by the allocator's block sizes times the alignments,
 * so it never needs to forget one.  From the runtime's general allocator,
 * never from this pool, and freed when the thread exits. */
typedef struct {
  unsigned cap, used; /* cap a power of two */
  regpool_class_t slot[];
} regpool_classes_t;
static __thread regpool_classes_t *t_classes;

/* Per-(thread, arena) heap cache.  A heap, once created for an arena, is
 * never deleted while the runtime runs: mi_heap_delete migrates live pages
 * and returns all-free pages to the arena, and that page-retirement path
 * races with lock-free cross-thread frees landing on the same pages (the
 * abandon-vs-free window).  A cached live heap keeps owning its pages, so
 * frees from any thread take mimalloc's ordinary supported path, and a later
 * re-bind to the same arena reuses the heap (no stranded blocks, no
 * footprint ratchet — the concerns that motivated deletion — since the heap
 * remains reachable and allocatable). */
#define REGPOOL_THREAD_HEAP_SLOTS 512
typedef struct {
  mi_arena_id_t arena;
  mi_heap_t *heap;
} regpool_theap_slot_t;
static __thread regpool_theap_slot_t t_heap_cache[REGPOOL_THREAD_HEAP_SLOTS];
static __thread unsigned t_heap_cache_count;

/* Binds that found the arena with no room for the thread's part of its heap,
 * since this pool's init (cleanup resets it, as it does the spill counts).
 * Diagnostic only, for the report: only its own value. */
static _Atomic(unsigned) g_theap_refusals;

/* ------------------------------------------------------------------------- */
/* allocator error report                                                      */
/* ------------------------------------------------------------------------- */

/* Creating the calling thread's part of a heap bound to an exclusive arena
 * (the allocator's theap) takes a slice of that arena.  With no slice free
 * the allocator raises exactly one ENOMEM and then one EFAULT and leaves the
 * part uncreated, and nothing between the two can detect corruption: that
 * pair is the arena having no room for this thread, which is legal.  The
 * allocator dependency is exactly that sentence, re-audited whenever the
 * allocator changes.  A bind opens this window around the creation alone; the
 * error report accepts an EFAULT only inside it and only after its ENOMEM,
 * and the bind treats any state but OPEN as no room.  Written only by its own
 * thread. */
#define REGPOOL_WINDOW_CLOSED 0u
#define REGPOOL_WINDOW_OPEN 1u
#define REGPOOL_WINDOW_OOM 2u
#define REGPOOL_WINDOW_REFUSED 3u
static __thread uint8_t t_theap_window;

static size_t regpool_put_str(char *b, size_t n, size_t cap, const char *s) {
  while (*s != '\0' && n < cap)
    b[n++] = *s++;
  return n;
}

static size_t regpool_put_uint(char *b, size_t n, size_t cap, unsigned v) {
  char d[12];
  int k = 0;
  do {
    d[k++] = (char)('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && k < (int)sizeof d);
  while (k > 0 && n < cap)
    b[n++] = d[--k];
  return n;
}

/* Runs on, and prints the backtrace of, the thread that detected the error,
 * which is not always the thread whose call caused it: in a release build a
 * double free is often found by the owner's later collection.  It runs inside
 * the allocator, possibly with the allocator's locks held and its metadata
 * corrupt, so it allocates nothing from that allocator and takes no allocator
 * or stdio lock: write, a backtrace warmed up at installation, abort. */
_Noreturn static void regpool_allocator_abort(int err) {
  /* write is a cancellation point, and a thread that missed the shutdown's
   * join deadline is cancelled: the report must reach abort. */
  int old_state;
  (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
  const char *what = err == EAGAIN   ? "double free"
                     : err == EINVAL ? "invalid pointer or argument"
                     : err == EFAULT ? "heap metadata corrupted or unobtainable"
                                     : "unexpected error";
  char b[160];
  size_t n = 0;
  n = regpool_put_str(b, n, sizeof b, "[ARTS] allocator error: ");
  n = regpool_put_str(b, n, sizeof b, what);
  n = regpool_put_str(b, n, sizeof b, " (code ");
  n = regpool_put_uint(b, n, sizeof b, (unsigned)err);
  n = regpool_put_str(b, n, sizeof b, ", rank ");
  n = regpool_put_uint(b, n, sizeof b, arts_global_rank_id);
  /* thread_id is unique within the rank; a thread the runtime did not
   * create reads 0 and "worker", and only its backtrace tells it apart. */
  n = regpool_put_str(b, n, sizeof b, ", thread ");
  n = regpool_put_uint(b, n, sizeof b, arts_thread_info.thread_id);
  n = regpool_put_str(b, n, sizeof b,
                      arts_thread_info.role == ARTS_ROLE_PROGRESS ? " (progress "
                                                                  : " (worker ");
  n = regpool_put_uint(b, n, sizeof b, arts_thread_info.group_pos);
  n = regpool_put_str(b, n, sizeof b, "))\n");
  (void)!write(STDERR_FILENO, b, n);
  void *frames[32];
  backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
  abort();
}

static void regpool_allocator_error(int err, void *arg) {
  (void)arg;
  if (err == ENOMEM || err == EOVERFLOW) {
    /* A refused request: NULL goes back to its caller, which owns it. */
    if (err == ENOMEM && t_theap_window == REGPOOL_WINDOW_OPEN)
      t_theap_window = REGPOOL_WINDOW_OOM;
    return;
  }
  if (err == EFAULT && t_theap_window == REGPOOL_WINDOW_OOM) {
    t_theap_window = REGPOOL_WINDOW_REFUSED;
    return;
  }
  regpool_allocator_abort(err);
}

/* Before any allocation, every error the allocator detects reaches the report
 * above, and the allocator prints nothing itself: the pool's exhaustion is a
 * NULL return it would print as an error.  What reaches the report is what the
 * allocator detects.  A release build of it detects a cross-thread double free
 * only when both frees come from threads other than the block's owner, the
 * owner did not collect the page between them and the page is not full, and
 * reports it when the owner collects the page; plus a double or invalid free
 * of a whole arena block.  A debug build marks a freed block in its padding
 * and reports a second free of it at that free, whichever threads make the
 * two, while the block's page is in use.  Once the page is released (at the
 * first free always for a block above the largest binned size, which has a
 * page of its own, and at or after it for any block whose page that free
 * emptied) the second is an invalid pointer, or, when the page's memory
 * already serves another page, whatever that page's state makes of it, if
 * anything, unrelated to the double free.  It adds an invalid or unaligned
 * free and a write into a block's padding, which it may report as a double
 * free when the write leaves the padding's check word intact.  Neither sees
 * a double free of a block allocated again between the two frees. */
__attribute__((constructor)) static void regpool_install_error_report(void) {
  /* The first backtrace loads the unwinder, which allocates: not inside the
   * report. */
  void *warm[1];
  (void)backtrace(warm, 1);
  mi_option_disable(mi_option_show_errors);
  mi_register_error(regpool_allocator_error, NULL);
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                     */
/* ------------------------------------------------------------------------- */

static inline size_t align_up_sz(size_t v, size_t a) {
  return (v + a - 1) & ~(a - 1);
}

/* An arena slab's base alignment.  The allocator moves the start of a range
 * it manages up to its arena alignment, giving up the head, and refuses the
 * range when less than that alignment is left; a slab based on it is used in
 * full.  A power of two, from the linked allocator itself. */
static size_t regpool_arena_align(void) {
  size_t a = mi_arena_min_alignment();
  return a > REGPOOL_BASE_ALIGN ? a : REGPOOL_BASE_ALIGN;
}

/* Count NUMA nodes from sysfs; fall back to 1 (single node / no sysfs). */
static unsigned regpool_detect_nodes(void) {
  DIR *d = opendir("/sys/devices/system/node");
  if (d == NULL)
    return 1;
  unsigned n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "node", 4) == 0 &&
        isdigit((unsigned char)e->d_name[4]))
      n++;
  }
  closedir(d);
  return n ? n : 1;
}

/* The CPU the calling thread last asked its node for, and that node: the
 * node is asked for again only when the thread runs on another CPU.  An
 * online CPU keeps its node; a CPU number removed and added again may come
 * back on another node, which costs locality until the thread moves.  Written
 * only by its own thread. */
static __thread int t_cpu = -1;
static __thread int t_cpu_node;

/* NUMA node the calling thread currently runs on; fall back to node 0.  The
 * CPU number is a load from the thread's restartable-sequence area where the
 * C library registered one; the node is a system call. */
static int regpool_current_node(void) {
  int cpu = sched_getcpu();
  if (cpu >= 0 && cpu == t_cpu)
    return t_cpu_node;
  unsigned c = 0, n = 0;
  if (syscall(SYS_getcpu, &c, &n, NULL) != 0)
    return 0;
  t_cpu = (int)c;
  t_cpu_node = (int)n;
  return (int)n;
}

/* Best-effort NUMA placement, as a preference in both of its shapes.  A
 * memory policy only governs *future* faults on the range — it does not
 * migrate pages already resident, and this call passes no MPOL_MF_MOVE
 * (nothing should be resident yet to move).  Callers must therefore invoke
 * this before the range is populated by any means (prefaulting,
 * pinning/registration, or first touch) or the policy is a no-op that
 * silently leaves the range on whatever node absorbed the earlier fault.
 *
 * A non-empty `set` INTERLEAVES the range over those nodes: every fault goes
 * to the next member in turn, so one object is served by every member's
 * memory controller and its placement does not depend on which thread
 * created it.  Otherwise the range is PREFERRED on `node`.  Both spill onto
 * other nodes once the target has no free pages, so neither can fail a
 * mapping for want of memory on one node.  A failure (single-node kernel, no
 * permission, unsupported) is not an error — the range simply keeps the
 * process's default policy. */
static void regpool_place(void *base, size_t len, int node, uint64_t set) {
  if (g_numa_nodes <= 1)
    return;
  unsigned long mask;
  int mode;
  if (set != 0) {
    mask = (unsigned long)set;
    mode = ARTS_MPOL_INTERLEAVE;
  } else if (node >= 0 && node < (int)REGPOOL_MAX_NODES) {
    mask = 1UL << (unsigned)node;
    mode = ARTS_MPOL_PREFERRED;
  } else {
    return;
  }
  /* The kernel reads maxnode - 1 bits of the mask, so a whole word of nodes
   * takes one more than its width. */
  (void)syscall(SYS_mbind, base, len, mode, &mask,
                (unsigned long)(sizeof(mask) * 8 + 1), 0UL);
}

/* Every node the process may run on, from its CPU affinity mask and the
 * nodes' CPU lists: the answer to "which nodes are this rank's" when the
 * caller supplied no set.  0 when it cannot be determined, which the caller
 * reads as "all of them". */
static uint64_t regpool_affinity_node_set(unsigned nodes) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof allowed, &allowed) != 0)
    return 0;
  uint64_t set = 0;
  for (unsigned n = 0; n < nodes && n < REGPOOL_MAX_NODES; n++) {
    char path[80];
    snprintf(path, sizeof path, "/sys/devices/system/node/node%u/cpulist", n);
    FILE *f = fopen(path, "r");
    if (f == NULL)
      continue;
    char line[4096];
    if (fgets(line, sizeof line, f) != NULL) {
      /* A cpulist is comma-separated singletons and lo-hi ranges; one CPU of
       * the node inside the mask is enough to claim the node. */
      const char *p = line;
      while (*p != '\0' && (set & (1ULL << n)) == 0) {
        char *end = NULL;
        long lo = strtol(p, &end, 10);
        if (end == p)
          break;
        long hi = lo;
        if (*end == '-') {
          p = end + 1;
          hi = strtol(p, &end, 10);
        }
        for (long c = lo; c <= hi; c++) {
          if (c >= 0 && c < CPU_SETSIZE && CPU_ISSET((size_t)c, &allowed)) {
            set |= 1ULL << n;
            break;
          }
        }
        if (*end != ',')
          break;
        p = end + 1;
      }
    }
    fclose(f);
  }
  return set;
}

/* Relative latency from `from` to `to`; every node is equidistant when the
 * machine reported no distances, which reduces the fallback order to free
 * memory alone. */
static uint32_t regpool_node_distance(int from, int to) {
  if (!g_distance_known || g_numa_nodes == 0)
    return (from == to) ? 0u : 1u;
  if (from < 0 || to < 0 || (unsigned)from >= g_numa_nodes ||
      (unsigned)to >= g_numa_nodes)
    return (from == to) ? 0u : 1u;
  /* A pair the caller's table does not describe is farther than every pair
   * it does, and equidistant with the other undescribed ones. */
  if ((unsigned)from >= g_distance_nodes || (unsigned)to >= g_distance_nodes)
    return (from == to) ? 0u : UINT32_MAX;
  return g_node_distance[(size_t)from * REGPOOL_MAX_NODES + (size_t)to];
}

static bool regpool_node_in_set(int node) {
  if (g_node_set == 0)
    return true;
  if (node < 0 || node >= (int)REGPOOL_MAX_NODES)
    return false;
  return (g_node_set & (1ULL << (unsigned)node)) != 0;
}

/* Bytes of one NUMA node a mapping placed there could land on (see
 * arts_regpool_parse_node_avail for the estimate), or SIZE_MAX when the
 * kernel does not expose it (no sysfs, single-node) — unknown must not
 * veto placement. */
static size_t regpool_node_avail_bytes(int node) {
#ifdef ARTS_DEBUG_INJECTION
  uint64_t forced = atomic_load_explicit(&g_forced_full, memory_order_relaxed);
  if (node >= 0 && node < (int)REGPOOL_MAX_NODES &&
      (forced & (1ULL << (unsigned)node)) != 0) {
    return 0;
  }
#endif
  char path[64];
  snprintf(path, sizeof path, "/sys/devices/system/node/node%d/meminfo", node);
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return SIZE_MAX;
  size_t r = arts_regpool_parse_node_avail(f);
  fclose(f);
  return r;
}

/* Fault every page of `len` bytes of a fresh anonymous mapping whose
 * placement policy is already set.  False means the range could not be made
 * resident and the caller must give the mapping up.
 *
 * Every slab is populated in full before it is registered or handed to the
 * allocator, so its residency is decided when it is made and the pool's
 * resident set is its mapped capacity on every transport: a registration
 * pins, and so faults, every page anyway, and without population an
 * unregistered run's resident set would depend on first touch.  An
 * interruption (EINTR, EAGAIN) is retried over the same range, whose
 * populated pages are cheap no-ops; any other failure fails it, since a
 * partly populated slab would fault its tail later.
 *
 * Without the advice a write per page populates and places the range
 * identically, from this thread alone and with no effect on any other CPU,
 * but it cannot report: a range its node cannot back ends in the kernel's
 * out-of-memory handling rather than in a failed map.  The advice fares no
 * better there — population walks the ordinary fault path either way — so
 * what keeps a slab within its node is the size it is mapped at, not the
 * result read here. */
static bool regpool_populate(void *base, size_t len) {
  if (!atomic_load_explicit(&g_populate_unsupported, memory_order_relaxed)) {
    for (int tries = 0;; tries++) {
      if (madvise(base, len, MADV_POPULATE_WRITE) == 0) {
        return true;
      }
      if (errno == EINVAL) {
        /* A fresh anonymous mapping admits no other reading of EINVAL than
         * a kernel without the advice. */
        break;
      }
      if ((errno != EINTR && errno != EAGAIN) || tries >= 1000) {
        ARTS_WARN("regpool: populate(%zu MiB) failed: %s", len >> 20,
                  strerror(errno));
        return false;
      }
    }
    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(
            &g_populate_unsupported, &expected, true, memory_order_relaxed,
            memory_order_relaxed)) {
      ARTS_WARN("regpool: MADV_POPULATE_WRITE unsupported by this kernel — "
                "slabs are populated by a write per page");
    }
  }
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  volatile unsigned char *bytes = (volatile unsigned char *)base;
  for (size_t off = 0; off < len; off += page) {
    bytes[off] = 0;
  }
  return true;
}

/* Close a slab's registration (NULL, an unregistered slab, closes trivially).
 * A close the provider refuses leaves the registration live, able to take a
 * write at its key, so on false the caller keeps the range mapped and never
 * closes it again: the process exit reclaims both. */
static bool regpool_mr_close(struct fid_mr *mr) {
  if (mr == NULL) {
    return true;
  }
  int rc = fi_close(&mr->fid);
  if (rc != 0) {
    ARTS_WARN("regpool: fi_close(mr) failed: %s; its range stays mapped and "
              "registered until the process exits",
              fi_strerror(-rc));
    return false;
  }
  return true;
}

/* Reserve `len` bytes aligned to `align` and set the NUMA placement
 * preference on them (`set` non-empty interleaves over it, else `node` is
 * preferred — see regpool_place).  Reserves `len + align` of address space
 * with no access and no commit charge, trims it to the aligned `len`
 * whatever the kernel hands back, and only then makes that range writable;
 * that range is commit-charged only under strict overcommit, and the slab
 * sizing, not the map, keeps it within memory.  No page of the range is
 * resident on return.  On failure unwinds and returns false.
 *
 * Runs without g_lock, so everything it reads is either immutable for the
 * pool's lifetime or atomic; so do regpool_populate and regpool_register. */
static bool regpool_reserve(int node, uint64_t set, size_t len, size_t align,
                            void **out_base) {
  size_t over = len + align;
  void *raw = mmap(NULL, over, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) {
    ARTS_WARN("regpool: mmap(%zu MiB) failed: %s", over >> 20,
              strerror(errno));
    return false;
  }

  uintptr_t aligned = ((uintptr_t)raw + (align - 1)) & ~(uintptr_t)(align - 1);
  size_t head = (size_t)(aligned - (uintptr_t)raw);
  size_t tail = over - head - len;
  if (head)
    munmap(raw, head);
  if (tail)
    munmap((void *)(aligned + len), tail);
  void *base = (void *)aligned;
  if (mprotect(base, len, PROT_READ | PROT_WRITE) != 0) {
    ARTS_WARN("regpool: mprotect(%zu MiB) failed: %s", len >> 20,
              strerror(errno));
    munmap(base, len);
    return false;
  }

  /* Before any population, registration (which faults every page while
   * pinning) or allocator first touch: a policy set after a page is resident
   * governs only later faults and silently fails to relocate this range. */
  regpool_place(base, len, node, set);
  *out_base = base;
  return true;
}

/* Register a populated range when a domain is set (a NULL domain registers
 * nothing: *out_mr NULL, *out_rkey 0).  On failure the range is given up:
 * unmapped, or left mapped and forgotten until the process exit when a
 * registration made for it does not close. */
static bool regpool_register(void *base, size_t len, struct fid_mr **out_mr,
                             uint64_t *out_rkey) {
  struct fid_mr *mr = NULL;
  uint64_t rkey = 0;
  struct fid_domain *domain = g_domain;
  struct fid_ep *ep = g_ep;
  if (domain != NULL) {
    uint64_t requested_key =
        atomic_fetch_add_explicit(&g_mr_key_next, 1, memory_order_relaxed);
    int rc = fi_mr_reg(domain, base, len,
                       FI_SEND | FI_RECV | FI_READ | FI_WRITE | FI_REMOTE_WRITE,
                       0, requested_key, 0, &mr, NULL);
    if (rc != 0) {
      /* Distinguishable from a reservation failure: the mapping existed but
       * the fabric refused to pin it — on providers that lock pages this is
       * typically the locked-memory limit (RLIMIT_MEMLOCK), not RAM. */
      ARTS_WARN("regpool: fi_mr_reg(%zu MiB) failed: %s", len >> 20,
                fi_strerror((int)-rc));
      munmap(base, len);
      return false;
    }
    if (ep != NULL) {
      /* Endpoint-bound discipline: the region becomes usable only after it
       * is bound to the endpoint and enabled, and with provider-assigned
       * keys the key exists only after the enable. */
      rc = fi_mr_bind(mr, &ep->fid, 0);
      if (rc == 0) {
        rc = fi_mr_enable(mr);
      }
      if (rc != 0) {
        ARTS_WARN("regpool: fi_mr_bind/enable(%zu MiB) failed: %s", len >> 20,
                  fi_strerror((int)-rc));
        if (regpool_mr_close(mr)) {
          munmap(base, len);
        }
        return false;
      }
    }
    rkey = fi_mr_key(mr);
    if (ep != NULL && rkey == FI_KEY_NOTAVAIL) {
      ARTS_ERROR("regpool: MR key unavailable after enable — provider broke "
                 "the key-after-enable contract");
    }
  }
  *out_mr = mr;
  *out_rkey = rkey;
  return true;
}

/* Reserve, populate and register one range on the calling thread alone:
 * an oversize mapping, which no other thread waits on.  On success fills
 * *out_* and returns true; on any failure unwinds and returns false. */
static bool regpool_map_slab(int node, uint64_t set, size_t len, size_t align,
                             void **out_base, struct fid_mr **out_mr,
                             uint64_t *out_rkey) {
  void *base = NULL;
  if (!regpool_reserve(node, set, len, align, &base))
    return false;
  if (!regpool_populate(base, len)) {
    munmap(base, len);
    return false;
  }
  if (!regpool_register(base, len, out_mr, out_rkey))
    return false;
  *out_base = base;
  return true;
}

/* Write an entry's per-mapping fields and publish it live.  Caller holds
 * g_lock and the slot must be unreachable to readers — either past the
 * published count, or REGPOOL_SLOT_FREE.  A range the entry word cannot name
 * is refused before anything is written. */
static void regpool_write_slot_locked(regpool_slab_t *s, void *base, size_t len,
                                      struct fid_mr *mr, uint64_t rkey,
                                      int node, bool interleaved,
                                      size_t align) {
  uint64_t w = arts_regpool_entry_word(REGPOOL_SLOT_LIVE, base, len);
  s->mr.base = base;
  s->mr.len = len;
  s->mr.mr = mr;
  s->mr.rkey = rkey;
  s->mr.numa_node = node;
  s->interleaved = interleaved;
  s->align = align;
  s->mr_kept = false;
  atomic_store_explicit(&g_slab_word[s - g_slabs], w, memory_order_release);
}

/* Append a fully-formed slab record and publish it.  Caller holds g_lock.
 * Returns the record, or NULL if the table is full. */
static regpool_slab_t *regpool_append(void *base, size_t len, struct fid_mr *mr,
                                      uint64_t rkey, int node, bool interleaved,
                                      bool is_direct, mi_arena_id_t arena,
                                      int owner, size_t align) {
  size_t idx = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  if (idx >= REGPOOL_MAX_SLABS)
    return NULL;
  regpool_slab_t *s = &g_slabs[idx];
  s->is_direct = is_direct;
  s->arena = arena;
  s->owner = owner;
  regpool_write_slot_locked(s, base, len, mr, rkey, node, interleaved, align);
  /* Release: the fully-written record must be visible before the count that
   * exposes it to lock-free readers. */
  atomic_store_explicit(&g_slab_count, idx + 1, memory_order_release);
  return s;
}

/* Publish a fully-formed direct-slab record, preferring an already-published
 * free slot over growing the table.  Caller holds g_lock.  Only the
 * direct-slab path calls this: arena slabs are never retired, so every free
 * entry in the table is, and will again be, a direct slab, and a reused one
 * keeps the is_direct, arena and owner it was appended with. */
static regpool_slab_t *regpool_publish_direct_locked(void *base, size_t len,
                                                     struct fid_mr *mr,
                                                     uint64_t rkey, int node,
                                                     bool interleaved,
                                                     size_t align) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) != REGPOOL_SLOT_FREE)
      continue;
    regpool_write_slot_locked(s, base, len, mr, rkey, node, interleaved,
                              align);
    return s;
  }
  return regpool_append(base, len, mr, rkey, node, interleaved,
                        /*is_direct=*/true, /*arena=*/NULL, /*owner=*/-1,
                        align);
}

/* Claim a retired mapping that satisfies (len, align), preferring one placed
 * on `node`.  Caller holds g_lock.  Reuse changes no field, so a reader that
 * observes the slot live sees exactly what was published when the mapping
 * was created. */
static regpool_slab_t *regpool_claim_retired_locked(size_t len, size_t align,
                                                    int node) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  regpool_slab_t *hit = NULL;
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) != REGPOOL_SLOT_RETIRED)
      continue;
    if (s->mr.len != len || s->align < align || s->mr_kept)
      continue;
    hit = s;
    if (s->mr.numa_node == node)
      break;
  }
  if (hit != NULL) {
    size_t i = (size_t)(hit - g_slabs);
    atomic_store_explicit(
        &g_slab_word[i],
        arts_regpool_entry_with_state(
            atomic_load_explicit(&g_slab_word[i], memory_order_relaxed),
            REGPOOL_SLOT_LIVE),
        memory_order_release);
  }
  return hit;
}

/* Tear down retired mappings until `need` bytes have been given back
 * (SIZE_MAX releases every one) and return the bytes released.  Caller holds
 * g_lock.  A retired entry's memory is owned by no allocation, so the
 * teardown is safe while the slot still reads RETIRED — readers skip it —
 * and the slot is offered to reuse only afterwards. */
static size_t regpool_evict_retired_locked(size_t need) {
  size_t freed = 0;
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  for (size_t i = 0; i < n && freed < need; i++) {
    regpool_slab_t *s = &g_slabs[i];
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) != REGPOOL_SLOT_RETIRED ||
        s->mr_kept)
      continue;
    /* A registration that does not close keeps its range mapped: the slot is
     * released, the range forgotten and left to the process exit. */
    if (regpool_mr_close(s->mr.mr)) {
      munmap(s->mr.base, s->mr.len);
      freed += s->mr.len;
    }
    atomic_store_explicit(&g_slab_word[i], 0, memory_order_release);
  }
  if (freed != 0)
    regpool_declines_clear_locked();
  return freed;
}

/* The same, one mapping at a time with g_lock held only to take it out of
 * the table: the slot reads FREE before the lock is dropped (readers skip
 * it, a publish may rewrite it), and the range is unregistered and unmapped
 * after, so nothing queues on g_lock behind the teardown.  The
 * range stays mapped until its own munmap, so nothing mapped meanwhile can
 * overlap it.  One pass over the table as it stood at the call; before
 * returning it waits for every teardown another request has in flight, so
 * a caller that looks again afterwards sees the memory given back, whoever
 * gave it. */
static size_t regpool_evict_retired(size_t need) {
  size_t freed = 0;
  pthread_mutex_lock(&g_lock);
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  for (size_t i = 0; i < n && freed < need; i++) {
    regpool_slab_t *s = &g_slabs[i];
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) != REGPOOL_SLOT_RETIRED ||
        s->mr_kept)
      continue;
    void *base = s->mr.base;
    size_t len = s->mr.len;
    struct fid_mr *mr = s->mr.mr;
    atomic_store_explicit(&g_slab_word[i], 0, memory_order_release);
    g_evicting++;
    pthread_mutex_unlock(&g_lock);
    /* As in the locked form: a range whose registration does not close stays
     * mapped, forgotten by the table. */
    bool gave = regpool_mr_close(mr);
    if (gave) {
      munmap(base, len);
      freed += len;
    }
    pthread_mutex_lock(&g_lock);
    if (gave)
      regpool_declines_clear_locked();
    if (--g_evicting == 0)
      pthread_cond_broadcast(&g_grow_cv);
  }
  while (g_evicting != 0)
    pthread_cond_wait(&g_grow_cv, &g_lock);
  pthread_mutex_unlock(&g_lock);
  return freed;
}

/* What the machine can still give a new slab: MemAvailable, with a fixed
 * fraction held back so the pool never races the rest of the process (and
 * the OS) to the last page.  A registration faults every page in, so sizing
 * past this turns a clean refusal into the OOM killer; an unregistered slab
 * is clamped by the same number because availability is a property of the
 * machine, not of whether the range will be registered.  SIZE_MAX when it
 * cannot be read — no clamp beyond the ladder itself — which a machine that
 * reports nothing available (0) never reads as. */
static size_t regpool_mem_available(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL)
    return SIZE_MAX;
  char line[128];
  size_t kb = 0;
  bool found = false;
  while (!found && fgets(line, sizeof(line), f) != NULL)
    found = sscanf(line, "MemAvailable: %zu kB", &kb) == 1;
  fclose(f);
  if (!found)
    return SIZE_MAX;
  return (kb >> 4) * 15 * 1024; /* 15/16 of it, in bytes */
}

/* One step down the ladder: half the size, carried back onto the granule and
 * never below the configured base slab.  Every candidate size stays a
 * granule multiple, which regpool_reserve's tail trim, a grow's population
 * chunks and the allocator's slice geometry all rely on. */
static size_t regpool_step_down(size_t want, size_t floor_bytes) {
  size_t half = (want / 2) & ~(REGPOOL_SLAB_GRANULE - 1);
  return half < floor_bytes ? floor_bytes : half;
}

/* Size of the next arena slab for `node`: half again the largest slab
 * MAPPED there (the configured base slab when the node has none yet),
 * carried onto the granule and capped.  Deriving it from what was mapped
 * rather than from a step count is what keeps a clamped or halved grow from
 * skipping a rung; deriving it from the largest is what keeps one clamped
 * grow from sending the ladder back down, after which every rung would
 * spend a table slot on a slab the clamp no longer asks to be small.
 *
 * A geometric ladder overshoots demand by its factor in the worst case and
 * by about (1 + factor)/2 on average, so a smaller factor wastes less
 * memory but spends more rungs.  What fixes both the factor and the base is
 * the allocator's arena table: a bounded GLOBAL process resource shared with
 * the default heap that backs ordinary allocations, of which one managed
 * range costs one slot per REGPOOL_SLAB_CAP of length.  A capped slab
 * therefore costs exactly one slot, the pool's reach is the table's free
 * slots times the cap on any machine, and the number of rungs a node may
 * spend climbing there is what the factor has to respect.
 *
 * Caller holds g_lock. */
static size_t regpool_next_slab_locked(int node) {
  size_t last = g_node_last_slab[node];
  if (last == 0)
    return g_slab_bytes;
  if (last >= REGPOOL_SLAB_CAP)
    return REGPOOL_SLAB_CAP;
  size_t want = align_up_sz(last * REGPOOL_GROWTH_NUM / REGPOOL_GROWTH_DEN,
                            REGPOOL_SLAB_GRANULE);
  return want > REGPOOL_SLAB_CAP ? REGPOOL_SLAB_CAP : want;
}

/* Smallest slab a clamped grow may step down to: the configured base, raised
 * to this pool's share of what the machine can still give (`mach_avail`)
 * over the table slots the pool may still take.  A slot is the scarce
 * resource: a slab below the share spends one on less than the memory a
 * slot is left to cover, and enough of them end the pool's reach while the
 * machine still has room.  The slots the general-purpose heap will take are
 * not knowable in advance, so the pool counts its own against the table
 * minus a fixed reserve; the share is a lower bound on a clamp, never a
 * size the pool maps unasked. */
static size_t regpool_floor_bytes(size_t mach_avail, unsigned slots_held) {
  size_t budget = (MI_MAX_ARENAS > REGPOOL_TABLE_RESERVE)
                      ? MI_MAX_ARENAS - REGPOOL_TABLE_RESERVE
                      : 1;
  size_t left = budget > slots_held ? budget - slots_held : 1;
  size_t share = mach_avail == SIZE_MAX
                     ? 0
                     : (mach_avail / left) & ~(REGPOOL_SLAB_GRANULE - 1);
  size_t floor_bytes = share > g_slab_bytes ? share : g_slab_bytes;
  return floor_bytes > REGPOOL_SLAB_CAP ? REGPOOL_SLAB_CAP : floor_bytes;
}

/* Whose demand a grow serves, which decides what it may do when the node is
 * full (its free pages cannot take the node's floor). */
typedef enum {
  /* The node's own threads: nothing, and the node is warned full.  Its
   * demand is then served from memory that exists — other nodes' arenas,
   * then slabs placed on other nodes, then retired oversize mappings given
   * back — before anything is mapped where the kernel chooses. */
  REGPOOL_GROW_OWN,
  /* Another node's threads, asked in its stead: nothing. */
  REGPOOL_GROW_PEER,
  /* The node's own threads once every other node declined: a slab the
   * kernel places, sized by the node's own rung.  It carries no placement
   * policy and only the node's own threads populate it, so its pages follow
   * that node's fallback order. */
  REGPOOL_GROW_LAST,
} regpool_grow_mode_t;

/* A node's grow in flight: what its grower decided, and the round of
 * population open now.  A round is one reserved range whose placement is
 * set; every thread that finds the grow in flight claims chunks of it and
 * populates them, and the thread whose chunk ends the round registers the
 * range, hands it to the allocator and publishes it, or gives the range up
 * and opens the next round a step down, or ends the grow.  The plain fields
 * are written by the grower before its first round opens and under g_lock at
 * each open, and read under g_lock or by the round's finisher, which joined
 * the round under g_lock like every claimer; only that finisher's next open
 * or the grow's end comes before a rewrite. */
typedef struct {
  regpool_grow_mode_t mode;
  int placed;        /* node the slab is placed on, or -1: the kernel's */
  bool fits_node;    /* the node's free pages could take the slab */
  bool superseded;   /* a kernel-placed grow found another slab first */
  size_t seen_slabs; /* the table length the grow was decided against */
  unsigned cleared;  /* g_declines_cleared when it began reading */
  size_t floor_bytes;
  size_t node_avail; /* the readings the sizing took, for the warnings */
  size_t mach_avail;
  uint64_t t0; /* when its mapping began, for the report */
  bool open;   /* a round of this grow is open */
  char *base;
  size_t len;
  uint32_t nchunks;
  /* The round's generation in the high half, never reused for the node, and
   * its next unclaimed chunk in the low half, at most nchunks.  Stored under
   * g_lock at an open; a claim is a CAS that keeps the generation, so a
   * thread holding a round that has ended claims nothing.  Relaxed: a
   * round's fields reach a claimer through g_lock, and what a chunk's
   * population did reaches the finisher through done. */
  _Atomic(uint64_t) claim;
  /* The round's chunks whose population has ended.  Stored 0 under g_lock at
   * an open; each claimer adds one per chunk, acq_rel, after the chunk's
   * population and its store to failed.  The add that reaches nchunks is
   * the last in the word's order: its thread finishes the round, and its
   * acquire reads every chunk's store to failed. */
  _Atomic(uint32_t) done;
  /* A chunk of the round failed to populate.  Stored false under g_lock at
   * an open, and true, relaxed, by that chunk's claimer before its add to
   * done; a claimer that reads it true skips its chunk, which is moot. */
  _Atomic(bool) failed;
} regpool_grow_t;
static regpool_grow_t g_grow[REGPOOL_MAX_NODES];

/* One cascade's asks: the readings they may take instead of reading a node
 * are those of grows that ended after the first of them (`grows_ended`, read
 * under g_lock when `started` is set).  An ask outside a cascade is a
 * cascade of one. */
typedef struct {
  bool started;
  unsigned grows_ended;
} regpool_cascade_t;

/* Whether grow end `a` came after grow end `b`, modulo the count's wrap. */
static bool regpool_ended_after(unsigned a, unsigned b) {
  return a - b - 1u < UINT_MAX / 2u;
}

/* A thread's copy of an open round, taken under g_lock. */
typedef struct {
  uint32_t gen;
  char *base;
  uint32_t nchunks;
} regpool_round_t;

typedef enum {
  REGPOOL_GROW_MAPPED,  /* the slab was handed to an arena */
  REGPOOL_GROW_REFUSED, /* the arena table refused the slab */
  REGPOOL_GROW_NOTHING, /* nothing was mapped */
} regpool_grow_outcome_t;

/* Sleep on g_grow_cv, counted for `node`'s report (*slept: this attempt
 * slept before).  Caller holds g_lock. */
static void regpool_sleep_locked(int node, bool *slept) {
  if (!*slept) {
    *slept = true;
    g_node_wait_count[node]++;
  }
  g_node_sleeps[node]++;
  uint64_t t0 = regpool_now_ns();
  pthread_cond_wait(&g_grow_cv, &g_lock);
  g_node_wait_ns[node] += regpool_now_ns() - t0;
}

/* End `node`'s grow: publish the slab a MAPPED outcome handed to `arena`,
 * record what the grow read, warn, and wake every thread that waits on it.
 * Runs on the thread that ends the grow's last round, or on the grower when
 * it opened none. */
static void regpool_grow_end(int node, regpool_grow_outcome_t outcome,
                             void *base, size_t len, struct fid_mr *mr,
                             uint64_t rkey, mi_arena_id_t arena) {
  regpool_grow_t *g = &g_grow[node];
  bool mapped = outcome == REGPOOL_GROW_MAPPED;
  pthread_mutex_lock(&g_lock);
  g_grows_ended++;
  g_node_grow_ns[node] += regpool_now_ns() - g->t0;
  if (g->placed < 0)
    g_unplaced_growing = false;
  if (mapped)
    g_pool_slots++;
  if (outcome == REGPOOL_GROW_REFUSED) {
    g_table_refusals++;
    if (!g_table_exhausted) {
      g_table_exhausted = true;
      ARTS_WARN("regpool: the allocator's arena table is full (%u slots held "
                "by the pool) — no further growth on any node",
                g_pool_slots);
    }
  }
  if (mapped && !g_inited) {
    /* Torn down while the mapping was in flight.  Teardown is single-
     * threaded by contract, so this is a violated contract rather than a
     * race the protocol handles — but the mapping must not be published
     * into a pool that no longer exists.  The arena's metadata references
     * the range, so it is left to the process exit like every arena slab. */
    (void)regpool_mr_close(mr);
  } else {
    if (mapped) {
      if (regpool_append(base, len, mr, rkey, g->placed,
                         /*interleaved=*/false, false, arena, node,
                         regpool_arena_align()) == NULL) {
        (void)regpool_mr_close(mr);
        /* Arena metadata now references this range; the OS reclaims it at
         * exit. */
        mapped = false;
      } else {
        if (len > g_node_last_slab[node])
          g_node_last_slab[node] = len;
        g_node_grow_count[node]++;
        if (g->placed == node)
          g_node_full_warned[node] = 0;
        atomic_store_explicit(&g_node_state[node], REGPOOL_NODE_CARVED,
                              memory_order_release);
        atomic_store_explicit(&g_node_arena[node], arena, memory_order_release);
      }
    }
    if (!mapped &&
        atomic_load_explicit(&g_node_state[node], memory_order_relaxed) !=
            REGPOOL_NODE_CARVED) {
      atomic_store_explicit(&g_node_state[node], REGPOOL_NODE_REFUSED,
                            memory_order_release);
    }
    if (mapped) {
      g_node_declined[node] = 0;
    } else if (!g->fits_node) {
      if (g->cleared == g_declines_cleared) {
        g_node_declined[node] = g->seen_slabs + 1;
        g_node_declined_end[node] = g_grows_ended;
      }
      g_node_declines[node]++;
    }
    /* Warned once per outcome per exhaustion (cleared when a placed slab
     * lands on the node again), by the node's own demand: a peer's request
     * it declines costs its own threads nothing, and a kernel-placed grow
     * that saw another slab appear leaves the cascade to look at it first. */
    uint8_t warn = 0;
    if (!g->fits_node && g->mode == REGPOOL_GROW_OWN)
      warn = REGPOOL_WARNED_SPILL;
    else if (!g->fits_node && g->mode == REGPOOL_GROW_LAST)
      warn = (g->placed == -1 && mapped)              ? REGPOOL_WARNED_UNPLACED
             : (!g->superseded && !g_table_exhausted) ? REGPOOL_WARNED_NONE
                                                      : 0;
    if (warn != 0 && (g_node_full_warned[node] & warn) == 0) {
      g_node_full_warned[node] |= warn;
      if (warn == REGPOOL_WARNED_SPILL)
        ARTS_WARN("regpool: node %d has %zu MiB free — no room for a %zu MiB "
                  "slab there; its threads are served from memory elsewhere",
                  node, g->node_avail >> 20, g->floor_bytes >> 20);
      else if (warn == REGPOOL_WARNED_UNPLACED)
        ARTS_WARN("regpool: no node has room for a %zu MiB slab; node %d's "
                  "slab is placed by the kernel",
                  g->floor_bytes >> 20, node);
      else if (g->mach_avail != SIZE_MAX)
        ARTS_WARN("regpool: no slab of %zu MiB or more could be mapped for "
                  "node %d (the machine can give %zu MiB); an allocation no "
                  "existing arena can serve fails",
                  g->floor_bytes >> 20, node, g->mach_avail >> 20);
      else
        ARTS_WARN("regpool: no slab of %zu MiB or more could be mapped for "
                  "node %d; an allocation no existing arena can serve fails",
                  g->floor_bytes >> 20, node);
    }
  }
  g->open = false;
  g_node_growing[node] = false;
  g_node_grow_ends[node]++;
  pthread_cond_broadcast(&g_grow_cv);
  pthread_mutex_unlock(&g_lock);
}

/* Reserve the range of `node`'s next round, stepping down from `want` while
 * a reservation fails, and open its population.  False when nothing could
 * be reserved down to the grow's floor. */
static bool regpool_round_open(int node, size_t want) {
  regpool_grow_t *g = &g_grow[node];
  size_t align = regpool_arena_align();
  for (;;) {
    void *base = NULL;
    if (regpool_reserve(g->placed, /*set=*/0, want, align, &base)) {
      pthread_mutex_lock(&g_lock);
      uint64_t gen =
          (atomic_load_explicit(&g->claim, memory_order_relaxed) >> 32) + 1;
      g->base = base;
      g->len = want;
      g->nchunks = (uint32_t)(want / REGPOOL_POPULATE_CHUNK);
      atomic_store_explicit(&g->done, 0, memory_order_relaxed);
      atomic_store_explicit(&g->failed, false, memory_order_relaxed);
      atomic_store_explicit(&g->claim, gen << 32, memory_order_relaxed);
      g->open = true;
      pthread_cond_broadcast(&g_grow_cv);
      pthread_mutex_unlock(&g_lock);
      return true;
    }
    if (want <= g->floor_bytes)
      return false;
    want = regpool_step_down(want, g->floor_bytes);
  }
}

/* End the round of `node`'s grow whose last chunk the caller populated:
 * register the range, hand it to an exclusive arena and end the grow with
 * it; when a chunk or the registration failed, give the range up and open
 * the next round a step down, or end the grow with nothing at the floor. */
static void regpool_round_end(int node) {
  regpool_grow_t *g = &g_grow[node];
  void *base = g->base;
  size_t len = g->len;
  struct fid_mr *mr = NULL;
  uint64_t rkey = 0;
  bool ok = !atomic_load_explicit(&g->failed, memory_order_relaxed);
  if (!ok) {
    ARTS_WARN("regpool: a %zu MiB slab for node %d could not be populated",
              len >> 20, node);
    munmap(base, len);
  } else {
    ok = regpool_register(base, len, &mr, &rkey);
  }
  if (ok) {
    /* is_committed=true (committed by mmap and already populated),
     * is_pinned=true (the arena must never decommit/purge/reset a
     * registered range), is_zero=true (a fresh anonymous mapping is
     * kernel-zeroed and nothing between map and manage writes into it:
     * registration only pins, placement only sets policy), exclusive=true
     * (only heaps created for this arena draw from it — the confinement
     * mechanism). */
    mi_arena_id_t arena = NULL;
    if (mi_manage_os_memory_ex(base, len, /*is_committed=*/true,
                               /*is_pinned=*/true, /*is_zero=*/true, g->placed,
                               /*exclusive=*/true, &arena)) {
      regpool_grow_end(node, REGPOOL_GROW_MAPPED, base, len, mr, rkey, arena);
    } else {
      if (regpool_mr_close(mr))
        munmap(base, len);
      regpool_grow_end(node, REGPOOL_GROW_REFUSED, NULL, 0, NULL, 0, NULL);
    }
    return;
  }
  if (len > g->floor_bytes &&
      regpool_round_open(node, regpool_step_down(len, g->floor_bytes)))
    return;
  regpool_grow_end(node, REGPOOL_GROW_NOTHING, NULL, 0, NULL, 0, NULL);
}

/* Populate chunks of round `r` of `node`'s grow until none is left to
 * claim, and end the round when the caller's chunk is its last.  Runs
 * without g_lock.  Returns the chunks the caller populated (a chunk claimed
 * after a failure in its round is not populated). */
static unsigned regpool_round_work(int node, const regpool_round_t *r) {
  regpool_grow_t *g = &g_grow[node];
  unsigned mine = 0;
  uint64_t c = atomic_load_explicit(&g->claim, memory_order_relaxed);
  for (;;) {
    if ((uint32_t)(c >> 32) != r->gen || (uint32_t)c >= r->nchunks)
      return mine;
    if (!atomic_compare_exchange_weak_explicit(
            &g->claim, &c, c + 1, memory_order_relaxed, memory_order_relaxed))
      continue;
    char *part = r->base + (size_t)(uint32_t)c * REGPOOL_POPULATE_CHUNK;
    if (!atomic_load_explicit(&g->failed, memory_order_relaxed)) {
      if (regpool_populate(part, REGPOOL_POPULATE_CHUNK))
        mine++;
      else
        atomic_store_explicit(&g->failed, true, memory_order_relaxed);
    }
    if (atomic_fetch_add_explicit(&g->done, 1, memory_order_acq_rel) + 1 ==
        r->nchunks) {
      regpool_round_end(node);
      return mine;
    }
    c = atomic_load_explicit(&g->claim, memory_order_relaxed);
  }
}

/* One step of a thread that finds `node`'s grow in flight, entered and left
 * with g_lock held: populate chunks of the open round while one is left
 * unclaimed and the caller may populate it, else sleep until the grow opens
 * a round or ends.  A kernel-placed round carries no placement policy, so
 * only its grower and the node's own threads (`here` is the caller's node)
 * populate it.  Returns the chunks the caller populated. */
static unsigned regpool_grow_join_locked(int node, int here, bool grower,
                                         bool *slept) {
  regpool_grow_t *g = &g_grow[node];
  uint64_t c = atomic_load_explicit(&g->claim, memory_order_relaxed);
  if (g->open && (uint32_t)c < g->nchunks &&
      (grower || g->placed >= 0 || here == node)) {
    regpool_round_t r = {(uint32_t)(c >> 32), g->base, g->nchunks};
    pthread_mutex_unlock(&g_lock);
    unsigned n = regpool_round_work(node, &r);
    pthread_mutex_lock(&g_lock);
    return n;
  }
  regpool_sleep_locked(node, slept);
  return 0;
}

/* Map one more arena slab for `node` and publish it as the node's current
 * arena.  Returns true when the table grew since the caller's reading —
 * this call's slab, or any entry published since, an oversize allocation's
 * mapping included — and false when it did not.
 *
 * `seen_slabs` is the table length the caller last observed: the grow is
 * skipped, and reported as satisfied, once the table has moved past it,
 * which collapses a herd of threads that exhausted the same arena into a
 * single mapping (the losers retry allocation against the winner's slab
 * instead of each mapping one of their own).  Pass the current length to ask
 * for a slab unconditionally.  `carve` marks a node's first carve: then an
 * arena the node already draws from (its own, or one its demand was moved
 * onto) is the answer whatever the length says — the length is published
 * before the arena it stands for, so a caller that read the length after a
 * peer's carve had completed would otherwise carve a second slab.  `asks`
 * is the caller's cascade (regpool_cascade_t).
 *
 * One grow per node is in flight at a time, and the mapping runs outside
 * g_lock.  The grower sizes the slab, reserves its range and sets its
 * placement (two availability reads and a few system calls whatever the
 * slab's size), then opens the range's population as chunks.  Every thread
 * that finds the grow in flight, the grower included, claims chunks and
 * populates them; the thread whose chunk ends the round registers the range
 * and publishes the slab, so the registration stays one per slab.  A thread
 * sleeps only while no round is open (the grower sizing, waiting for the
 * kernel-placed turn or reserving, or a failed round's finisher reserving
 * the next), once every chunk is claimed, for the chunks in flight and the
 * registration, or, on a kernel-placed slab, which carries no placement
 * policy and so is populated only by its grower and the node's own threads,
 * as any other thread.  Nothing that sleeps or populates holds g_lock.  A
 * waiter whose table length the slab moved returns true; one whose grow
 * ended with nothing grows in turn, unless that grow read the node full at
 * its length and ended after its cascade's first ask (g_node_declined).
 *
 * A node whose free pages cannot take its floor is FULL, and gets no slab
 * placed on it: one sized for what it has left spends a table slot on a
 * fraction of what the slot must cover.  What the grow does instead depends
 * on `mode` (above); an own or peer ask at a length the node was read full
 * at, by a grow that ended after its cascade's first ask, takes that
 * reading.  Only REGPOOL_GROW_LAST maps a slab the kernel places, sized by
 * what the machine can still give — the node figures count free pages
 * alone, so a machine whose every node is full by them may still give
 * reclaimable cache, which only the machine's figure counts — and such a
 * grow is the cascade's last step, after every arena and every other node
 * was asked, so it maps whenever the machine can still give a slab.
 *
 * The allocator's arena table refuses a slab only when it is full (the
 * range is granule-sized, based on the arena alignment, committed and at
 * most one slot's worth, so nothing else about it can be refused), and a
 * full table never gets a slot back.  The first refusal is therefore
 * latched: no later grow maps, populates or registers anything, and the
 * caller's cascade either finds room in an existing arena or fails the
 * allocation loudly. */
static bool regpool_grow_node(int node, size_t seen_slabs, bool carve,
                              regpool_grow_mode_t mode,
                              regpool_cascade_t *asks) {
  regpool_grow_t *g = &g_grow[node];
  int here = regpool_current_node();
  unsigned helped = 0, own = 0;
  bool slept = false;
  bool answer = false;
  pthread_mutex_lock(&g_lock);
  if (!asks->started) {
    asks->started = true;
    asks->grows_ended = g_grows_ended;
  }
  for (;;) {
    if (!g_inited)
      goto out;
    if (atomic_load_explicit(&g_slab_count, memory_order_acquire) !=
            seen_slabs ||
        (carve && atomic_load_explicit(&g_node_arena[node],
                                       memory_order_acquire) != NULL)) {
      answer = true;
      goto out;
    }
    if (!g_node_growing[node])
      break;
    helped += regpool_grow_join_locked(node, here, /*grower=*/false, &slept);
  }
  /* Consulted only once no grow of this node is in flight: a slab such a
   * grow publishes moves the table and answers the caller above, even when
   * the table filled after it.  A full slab table ends growth the same way,
   * before a slab is mapped and handed to an arena it could not be
   * published for. */
  if (g_table_exhausted || seen_slabs >= REGPOOL_MAX_SLABS) {
    if (atomic_load_explicit(&g_node_state[node], memory_order_relaxed) !=
        REGPOOL_NODE_CARVED)
      atomic_store_explicit(&g_node_state[node], REGPOOL_NODE_REFUSED,
                            memory_order_release);
    goto out;
  }
  if (mode != REGPOOL_GROW_LAST && g_node_declined[node] == seen_slabs + 1 &&
      regpool_ended_after(g_node_declined_end[node], asks->grows_ended)) {
    g_node_decline_reuses[node]++;
    goto out;
  }
  g_node_growing[node] = true;
  g->cleared = g_declines_cleared;
  unsigned my_end = g_node_grow_ends[node];
  uint64_t g0 = regpool_now_ns();
  size_t rung = regpool_next_slab_locked(node);
  size_t want = rung;
  unsigned slots_held = g_pool_slots;
  pthread_mutex_unlock(&g_lock);

  /* Clamp to what the machine has right now: an overcommitting kernel
   * happily grants a mapping far beyond physical memory, and this slab is
   * populated in full at creation.  The floor bounds how far the clamp steps
   * a rung down; it never raises one, so a run that needs little maps
   * little. */
  size_t mach_avail = regpool_mem_available();
  size_t floor_bytes = regpool_floor_bytes(mach_avail, slots_held);
  if (floor_bytes > want)
    floor_bytes = want;
  while (mach_avail != SIZE_MAX && want > mach_avail && want > floor_bytes)
    want = regpool_step_down(want, floor_bytes);

  /* A slab placed on a node must land there, so its size is also clamped
   * to what the node itself can absorb (headroom held back for concurrent
   * consumers). */
  size_t node_avail = regpool_node_avail_bytes(node);
  bool fits_node = true;
  if (node_avail != SIZE_MAX) {
    size_t usable = node_avail > ARTS_REGPOOL_NODE_HEADROOM
                        ? node_avail - ARTS_REGPOOL_NODE_HEADROOM
                        : 0;
    while (want > usable && want > floor_bytes)
      want = regpool_step_down(want, floor_bytes);
    fits_node = want <= usable;
  }

  bool unplaced = !fits_node && mode == REGPOOL_GROW_LAST &&
                  (mach_avail == SIZE_MAX || mach_avail >= floor_bytes);
  bool map = fits_node || unplaced;
  bool superseded = false;
  if (map) {
    /* A refusal elsewhere may have landed while this grow sized its slab
     * (or waited its turn), and mapping now would only be refused again.
     * A kernel-placed slab is also not mapped once any slab has appeared
     * since the caller looked: the cascade's last step must see it first. */
    pthread_mutex_lock(&g_lock);
    if (unplaced && g_unplaced_growing && !g_table_exhausted) {
      uint64_t t0 = regpool_now_ns();
      while (g_unplaced_growing && !g_table_exhausted)
        regpool_sleep_locked(node, &slept);
      g0 += regpool_now_ns() - t0;
    }
    superseded =
        unplaced &&
        atomic_load_explicit(&g_slab_count, memory_order_relaxed) != seen_slabs;
    if (g_table_exhausted || superseded)
      map = unplaced = false;
    else if (unplaced)
      g_unplaced_growing = true;
    slots_held = g_pool_slots;
    pthread_mutex_unlock(&g_lock);
  }
  if (unplaced) {
    /* Sized only now, floor included: the previous kernel-placed slab is
     * resident and the machine's availability says so. */
    mach_avail = regpool_mem_available();
    floor_bytes = regpool_floor_bytes(mach_avail, slots_held);
    if (floor_bytes > rung)
      floor_bytes = rung;
    want = rung;
    while (mach_avail != SIZE_MAX && want > mach_avail && want > floor_bytes)
      want = regpool_step_down(want, floor_bytes);
    map = mach_avail == SIZE_MAX || want <= mach_avail;
  }

  g->mode = mode;
  g->placed = unplaced ? -1 : node;
  g->fits_node = fits_node;
  g->superseded = superseded;
  g->seen_slabs = seen_slabs;
  g->floor_bytes = floor_bytes;
  g->node_avail = node_avail;
  g->mach_avail = mach_avail;
  g->t0 = g0;
  if (!map || !regpool_round_open(node, want))
    regpool_grow_end(node, REGPOOL_GROW_NOTHING, NULL, 0, NULL, 0, NULL);

  /* The contract is "a slab appeared", not "this grow mapped one": a slab
   * another grow published must send the caller's cascade round again, not
   * read as a node that cannot grow. */
  pthread_mutex_lock(&g_lock);
  while (g_node_grow_ends[node] == my_end)
    own += regpool_grow_join_locked(node, here, /*grower=*/true, &slept);
  answer = g_inited && atomic_load_explicit(&g_slab_count,
                                            memory_order_relaxed) != seen_slabs;
out:
  if (helped != 0) {
    g_node_helpers[node]++;
    g_node_help_chunks[node] += helped;
  }
  g_node_chunks[node] += helped + own;
  pthread_mutex_unlock(&g_lock);
  return answer;
}

/* Is `a` a worse candidate than `b` for a mapping requested from `from`?
 * Three keys, in order: a node outside this rank's set comes after every one
 * inside it (a mapping the rank's own memory controllers cannot serve is the
 * last resort, however near it looks); then distance, because a farther node
 * costs every access for the mapping's whole life; then free memory, which
 * only breaks ties between equals. */
static bool regpool_node_worse(int a, int b, int from, const size_t *avail) {
  bool a_in = regpool_node_in_set(a);
  bool b_in = regpool_node_in_set(b);
  if (a_in != b_in)
    return !a_in;
  uint32_t da = regpool_node_distance(from, a);
  uint32_t db = regpool_node_distance(from, b);
  if (da != db)
    return da > db;
  return avail[a] < avail[b];
}

unsigned int arts_regpool_node_order(int from, int *out, unsigned int max) {
  if (out == NULL || max == 0)
    return 0;
  unsigned n = g_numa_nodes ? g_numa_nodes : 1u;
  if (n > REGPOOL_MAX_NODES)
    n = REGPOOL_MAX_NODES;
  if (from < 0 || (unsigned)from >= n)
    from = 0;
  /* One availability reading per node for the whole sort: a comparator that
   * re-read it could order the same pair both ways as the machine moves. */
  size_t avail[REGPOOL_MAX_NODES];
  for (unsigned i = 0; i < n; i++)
    avail[i] = regpool_node_avail_bytes((int)i);
  int order[REGPOOL_MAX_NODES];
  unsigned cnt = 0;
  order[cnt++] = from;
  for (unsigned i = 0; i < n; i++) {
    if ((int)i != from)
      order[cnt++] = (int)i;
  }
  /* The requesting node stays first whatever the keys say — it is the one
   * node whose locality the caller actually asked for. */
  for (unsigned k = 2; k < cnt; k++) {
    int v = order[k];
    unsigned j = k;
    while (j > 1 && regpool_node_worse(order[j - 1], v, from, avail)) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }
  unsigned w = (cnt < max) ? cnt : max;
  for (unsigned i = 0; i < w; i++)
    out[i] = order[i];
  return w;
}

/* Screen the nodes by availability and map an exactly sized mapping on the
 * first that can absorb it, in the candidate order above.  Placement is a
 * preference, so a mapping is worth making wherever it lands; the screen
 * only picks WHERE (unknown availability never vetoes).  A candidate that
 * passes the screen can still fail the map itself, which just moves on to
 * the next.  Runs without g_lock. */
static bool regpool_map_screened(size_t len, size_t a, int node,
                                 int *out_node, void **out_base,
                                 struct fid_mr **out_mr, uint64_t *out_rkey) {
  int order[REGPOOL_MAX_NODES];
  unsigned cnt = arts_regpool_node_order(node, order, REGPOOL_MAX_NODES);
  for (unsigned k = 0; k < cnt; k++) {
    int cand = order[k];
    size_t av = regpool_node_avail_bytes(cand);
    if (av != SIZE_MAX && (av <= ARTS_REGPOOL_NODE_HEADROOM ||
                           av - ARTS_REGPOOL_NODE_HEADROOM < len))
      continue;
    if (regpool_map_slab(cand, /*set=*/0, len, a, out_base, out_mr, out_rkey)) {
      *out_node = cand;
      return true;
    }
  }
  return false;
}

/* Which members of this rank's node set an interleaved mapping of `len`
 * bytes should spread over: the ones that can still absorb pages, provided
 * their estimates TOGETHER cover the length — an interleave puts an equal
 * share on each member, so what matters is the set's capacity, not any one
 * node's.  0 means "do not place it interleaved"; unknown availability never
 * vetoes (a member whose estimate is unknown counts as room), while a
 * member known to have no room is left out whatever the others report. */
static uint64_t regpool_interleave_set(size_t len) {
  uint64_t set = g_node_set;
  if (set == 0)
    return set;
  uint64_t usable_set = 0;
  size_t sum = 0;
  for (unsigned i = 0; i < g_numa_nodes && i < REGPOOL_MAX_NODES; i++) {
    if ((set & (1ULL << i)) == 0)
      continue;
    size_t av = regpool_node_avail_bytes((int)i);
    size_t usable = (av == SIZE_MAX) ? SIZE_MAX
                    : (av > ARTS_REGPOOL_NODE_HEADROOM)
                        ? av - ARTS_REGPOOL_NODE_HEADROOM
                        : 0;
    if (usable == 0)
      continue;
    usable_set |= 1ULL << i;
    sum = (sum > SIZE_MAX - usable) ? SIZE_MAX : sum + usable;
  }
  return (sum >= len) ? usable_set : 0;
}

/* Place one oversize mapping: interleaved over the rank's node set when that
 * set can absorb it, else on the nearest single node that can.  On success
 * *out_interleaved says which of the two happened and *out_node is the node
 * a single-node placement chose (untouched otherwise).  Runs without
 * g_lock. */
static bool regpool_map_placed(size_t len, size_t a, int node, int *out_node,
                               bool *out_interleaved, void **out_base,
                               struct fid_mr **out_mr, uint64_t *out_rkey) {
  uint64_t set = regpool_interleave_set(len);
  if (set != 0 && (set & (set - 1)) == 0) {
    /* A set of one node: spreading over it is preferring it, and saying
     * "this node" keeps the placement legible where saying "spread" would
     * name nothing. */
    int only = __builtin_ctzll(set);
    if (regpool_map_slab(only, /*set=*/0, len, a, out_base, out_mr,
                         out_rkey)) {
      *out_interleaved = false;
      *out_node = only;
      return true;
    }
  } else if (set != 0 &&
             regpool_map_slab(-1, set, len, a, out_base, out_mr, out_rkey)) {
    *out_interleaved = true;
    return true;
  }
  *out_interleaved = false;
  return regpool_map_screened(len, a, node, out_node, out_base, out_mr,
                              out_rkey);
}

/* Oversize path: a dedicated mapping that backs exactly one allocation, kept
 * in the same table so lookups resolve it.  Two thresholds make a dedicated
 * mapping the cheaper home (see the caller): above the allocator's largest
 * chunk object a request is carved from whole chunks and wastes up to a
 * chunk per object, and above half a slab two such requests cannot share a
 * slab — while an exactly sized mapping wastes nothing either way.
 *
 * A mapping outlives the allocation it backs (it is retired, not unmapped),
 * so the syscalls are paid once per distinct block shape rather than once
 * per allocation.
 *
 * Placement differs from an arena slab's on purpose: an object this big is
 * one object many threads sweep, so it has no owning node and is
 * INTERLEAVED over the caller's whole node set — every memory controller of
 * the set serves a share of it, and its placement no longer depends on which
 * thread created it.  Only when the set cannot absorb it does the mapping
 * fall back to a single node, and then to the kernel's own choice.  A
 * reused retired mapping keeps whatever placement it was created with:
 * re-binding cannot move pages that are already resident. */
static void *regpool_alloc_direct(size_t size, size_t align, int node) {
  size_t a = align > REGPOOL_BASE_ALIGN ? align : REGPOOL_BASE_ALIGN;
  size_t len = align_up_sz(size, a);
  void *base = NULL;
  struct fid_mr *mr = NULL;
  uint64_t rkey = 0;

  pthread_mutex_lock(&g_lock);
  regpool_slab_t *reused = regpool_claim_retired_locked(len, a, node);
  pthread_mutex_unlock(&g_lock);
  if (reused != NULL) {
    return reused->mr.base;
  }

  int used_node = node;
  bool interleaved = false;
  bool mapped = regpool_map_placed(len, a, node, &used_node, &interleaved,
                                   &base, &mr, &rkey);
  if (!mapped) {
    /* Retired mappings hold memory no allocation owns: give it back before
     * concluding that nowhere can place this one — and look again even when
     * a concurrent request was the one that gave it back. */
    (void)regpool_evict_retired(len);
    mapped = regpool_map_placed(len, a, node, &used_node, &interleaved, &base,
                                &mr, &rkey);
  }
  if (!mapped) {
    used_node = -1;
    mapped = regpool_map_slab(-1, /*set=*/0, len, a, &base, &mr, &rkey);
  }
  if (!mapped)
    return NULL;
  /* An interleaved mapping belongs to no single node: the flag is its
   * placement and numa_node carries the value that says "not one node". */
  if (interleaved)
    used_node = -1;

  pthread_mutex_lock(&g_lock);
  regpool_slab_t *s = regpool_publish_direct_locked(base, len, mr, rkey,
                                                    used_node, interleaved, a);
  if (s == NULL) {
    /* The table is full, and every retired entry is holding a slot no
     * allocation owns. */
    regpool_evict_retired_locked(SIZE_MAX);
    s = regpool_publish_direct_locked(base, len, mr, rkey, used_node,
                                      interleaved, a);
  }
  pthread_mutex_unlock(&g_lock);
  if (s == NULL) {
    if (regpool_mr_close(mr)) {
      munmap(base, len);
    }
    return NULL;
  }
  /* base is aligned to `a` >= requested align, so it satisfies the request. */
  return base;
}

typedef enum {
  REGPOOL_BOUND,        /* *out now draws from the arena */
  REGPOOL_BIND_NO_ROOM, /* the arena has no room for this thread's part of
                         * its heap; *out is unchanged */
  REGPOOL_BIND_FAILED,  /* no heap could be made at all */
} regpool_bind_t;

/* Bind *out to the calling thread's heap for `arena` via the per-thread heap
 * cache: the arena's cached heap, created on first use.  See the cache's
 * comment for why heaps are never deleted mid-run.  The heap's part for this
 * thread is made here, inside the error report's window.  While the thread has
 * not begun exiting a binding names only a heap whose part exists, so no
 * allocation makes one; the allocator's exit hook destroys every part a
 * binding names. */
static regpool_bind_t regpool_thread_bind(mi_arena_id_t arena,
                                          regpool_binding_t *out) {
  mi_heap_t *h = NULL;
  for (unsigned i = 0; i < t_heap_cache_count; i++) {
    if (t_heap_cache[i].arena == arena) {
      h = t_heap_cache[i].heap;
      break;
    }
  }
  if (h == NULL) {
    h = mi_heap_new_in_arena(arena);
    if (h == NULL)
      return REGPOOL_BIND_FAILED;
    if (t_heap_cache_count < REGPOOL_THREAD_HEAP_SLOTS) {
      t_heap_cache[t_heap_cache_count].arena = arena;
      t_heap_cache[t_heap_cache_count].heap = h;
      t_heap_cache_count++;
    }
    /* Cache overflow leaves the heap uncached but live: correctness is
     * unaffected, a re-bind simply creates another heap. */
  }
  t_theap_window = REGPOOL_WINDOW_OPEN;
  (void)mi_heap_theap(h);
  bool refused = t_theap_window != REGPOOL_WINDOW_OPEN;
  t_theap_window = REGPOOL_WINDOW_CLOSED;
  if (refused) {
    atomic_fetch_add_explicit(&g_theap_refusals, 1, memory_order_relaxed);
    return REGPOOL_BIND_NO_ROOM;
  }
  out->heap = h;
  out->arena = arena;
  return REGPOOL_BOUND;
}

/* Sweep the existing arena slabs made for `owner` newest-first (the newest
 * is the least likely to be fully consumed), binding the thread's heap for
 * each candidate and attempting the allocation.  Skips the arena that
 * already refused this request.  On success *w is the binding that served. */
static void *regpool_sweep_arenas(size_t size, size_t align, int owner,
                                  mi_arena_id_t refused,
                                  regpool_binding_t *w) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  for (size_t i = n; i-- > 0;) {
    regpool_slab_t *s = &g_slabs[i];
    /* An arena entry is written once and never rewritten, so the state
     * check alone makes its fields safe to read. */
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_acquire)) != REGPOOL_SLOT_LIVE)
      continue;
    if (s->is_direct)
      continue;
    if (s->arena == NULL || s->arena == refused)
      continue;
    if (s->owner != owner)
      continue;
    regpool_bind_t b = regpool_thread_bind(s->arena, w);
    if (b == REGPOOL_BIND_FAILED)
      return NULL;
    if (b == REGPOOL_BIND_NO_ROOM)
      continue;
    void *p = mi_heap_malloc_aligned(w->heap, size, align);
    if (p != NULL)
      return p;
  }
  return NULL;
}

/* The locality-fallback pass for a node that cannot grow: every other node's
 * arenas, in the order a mapping for `node` would try the nodes — nearest
 * first, since a block lives where it is first allocated and the node that
 * serves it costs every access for the block's whole life.  A slab the
 * kernel placed ranks with the node it was made for; where its pages
 * actually are is the kernel's choice.  Placement is a preference, never a
 * reason to refuse memory that exists. */
static void *regpool_sweep_other_nodes(size_t size, size_t align, int node,
                                       mi_arena_id_t refused,
                                       regpool_binding_t *w) {
  int order[REGPOOL_MAX_NODES];
  unsigned cnt = arts_regpool_node_order(node, order, REGPOOL_MAX_NODES);
  for (unsigned o = 0; o < cnt; o++) {
    if (order[o] == node)
      continue; /* swept by the caller just before */
    void *p = regpool_sweep_arenas(size, align, order[o], refused, w);
    if (p != NULL)
      return p;
  }
  return NULL;
}

/* The cascade's first resort: the node's own arenas, placed on it or by the
 * kernel for it. */
static void *regpool_serve_own(size_t size, size_t align, int node,
                               mi_arena_id_t refused, regpool_binding_t *w) {
  return regpool_sweep_arenas(size, align, node, refused, w);
}

/* The cascade's locality fallback: another node's arena serves the request,
 * counted as a spill.  A node with no arena of its own takes that arena as
 * its memo, the first time only; a CAS that finds the memo set leaves it. */
static void *regpool_serve_elsewhere(size_t size, size_t align, int node,
                                     mi_arena_id_t refused,
                                     regpool_binding_t *w) {
  void *p = regpool_sweep_other_nodes(size, align, node, refused, w);
  if (p != NULL) {
    atomic_fetch_add_explicit(&g_node_spills[node], 1, memory_order_relaxed);
    mi_arena_id_t expect = NULL;
    (void)atomic_compare_exchange_strong_explicit(&g_node_arena[node], &expect,
                                                  w->arena,
                                                  memory_order_release,
                                                  memory_order_relaxed);
  }
  return p;
}

/* One more attempt on the arena that refused this request (NULL: none). */
static void *regpool_retry_refused(size_t size, size_t align,
                                   mi_arena_id_t refused,
                                   regpool_binding_t *w) {
  if (refused == NULL || regpool_thread_bind(refused, w) != REGPOOL_BOUND)
    return NULL;
  return mi_heap_malloc_aligned(w->heap, size, align);
}

/* Exhaustion slow path.  A heap can only draw from the single arena it is
 * bound to, so recovery is a re-binding cascade: (1) sweep the arenas made
 * for this node, placed on it or by the kernel (space freed into an earlier
 * arena is reachable only through a heap bound to it), (2) grow this node
 * by a slab placed on it, (3) drop the locality preference — sweep the other
 * nodes' arenas nearest-first, then grow the other nodes nearest-first, each
 * by a slab placed on it, and draw from the slab that appeared at once, (4)
 * once every arena and every other node declined and the retired oversize
 * mappings were given back (where a grow could still map), try the arena
 * that refused once more and grow this node by a slab the kernel places.
 * Memory that exists is used before any is mapped where the kernel chooses,
 * whatever the machine's state.  Loops until the allocation succeeds or
 * nothing can be mapped; only that is genuine exhaustion.  A transient miss
 * (a peer raced away a fresh slab) re-enters the cascade.  `refused` names
 * the one arena that already failed this request for this thread — the
 * thread's binding, or NULL when the node has no arena — so the sweeps skip
 * exactly that arena and no other.
 *
 * The arena that serves becomes the calling thread's binding and nobody
 * else's: other threads of the node may still have room where this one has
 * none, so only a grow moves them.  A miss proves an arena exhausted only for
 * this thread and for requests of its size, so a thread moved off its node's
 * memory goes back to it the next time this cascade runs for it, the node's
 * own arenas first. */
static void *regpool_cascade(size_t size, size_t align, int node,
                             mi_arena_id_t refused, regpool_binding_t *w) {
  bool evicted = false;
  regpool_cascade_t asks = {false, 0};
  for (;;) {
    size_t seen = atomic_load_explicit(&g_slab_count, memory_order_acquire);
    void *p = regpool_serve_own(size, align, node, refused, w);
    if (p != NULL)
      return p;

    if (regpool_grow_node(node, seen, /*carve=*/false, REGPOOL_GROW_OWN,
                          &asks)) {
      mi_arena_id_t cur =
          atomic_load_explicit(&g_node_arena[node], memory_order_acquire);
      if (cur == NULL) {
        /* "Someone else grew" was another node's slab and this node still
         * has no arena.  A NULL arena id must never reach a heap bind: it
         * addresses the allocator's unmanaged default space, outside every
         * registered slab — the confinement the pool exists to provide.
         * Re-enter the cascade; the fresh slab is found by the sweeps. */
        continue;
      }
      regpool_bind_t b = regpool_thread_bind(cur, w);
      if (b == REGPOOL_BIND_FAILED) {
        ARTS_WARN("regpool: heap re-bind failed for node %d", node);
        return NULL;
      }
      if (b == REGPOOL_BIND_NO_ROOM)
        continue; /* the fresh slab was taken whole by others */
      p = mi_heap_malloc_aligned(w->heap, size, align);
      if (p != NULL)
        return p;
      continue; /* raced away — re-enter the cascade */
    }

    p = regpool_serve_elsewhere(size, align, node, refused, w);
    if (p != NULL)
      return p;
    bool grew = false;
    int order[REGPOOL_MAX_NODES];
    unsigned cnt = arts_regpool_node_order(node, order, REGPOOL_MAX_NODES);
    for (unsigned o = 0; o < cnt && !grew; o++) {
      if (order[o] == node)
        continue; /* the requesting node led the cascade above */
      /* A node with no arena is not skipped: an uncarved node is memory
       * that exists and has simply not been asked for yet.  Nearest first,
       * so a forced relocation costs the least access latency available,
       * and each node grows only a slab placed on itself.
       * The length this pass began from, not the current one: a slab
       * published since the sweeps above is one they did not see, and it
       * must send the cascade round again rather than read as a node that
       * cannot grow. */
      grew = regpool_grow_node(order[o], seen, /*carve=*/false,
                               REGPOOL_GROW_PEER, &asks);
    }
    if (grew) {
      /* Whatever appeared, every node's arenas are swept at once, this
       * node's first, before this node's own grow, which declined a moment
       * ago, is asked again. */
      p = regpool_serve_own(size, align, node, refused, w);
      if (p == NULL)
        p = regpool_serve_elsewhere(size, align, node, refused, w);
      if (p != NULL)
        return p;
      continue; /* raced away — re-enter the cascade */
    }
    /* Retired oversize mappings hold memory no allocation owns: it is given
     * back once before anything is mapped where the kernel chooses, and the
     * cascade looks again, since this node or another may now take a placed
     * slab — also when a concurrent request was the one that gave it back.
     * Where no grow can map any more (the arena table refused, or the slab
     * table is full) the retired mappings would only be lost to the oversize
     * path that reuses them, so they are kept; the cascade still looks
     * again, since frees may have refilled an arena. */
    if (!evicted) {
      evicted = true;
      pthread_mutex_lock(&g_lock);
      bool can_grow =
          !g_table_exhausted &&
          atomic_load_explicit(&g_slab_count, memory_order_relaxed) <
              REGPOOL_MAX_SLABS;
      pthread_mutex_unlock(&g_lock);
      if (can_grow)
        (void)regpool_evict_retired(SIZE_MAX);
      continue;
    }
    /* Every sweep skips the arena that refused this request, and frees
     * since may have refilled it: it is tried once more before the kernel
     * places a slab, and before the request is given up. */
    p = regpool_retry_refused(size, align, refused, w);
    if (p != NULL)
      return p;
    /* Every arena and every other node declined: the kernel places this
     * node's slab, so the cascade fails only when nothing can be mapped —
     * the machine has nothing left to give, the arena table refused, the
     * slab table is full, or the mapping itself failed. */
    if (!regpool_grow_node(node, seen, /*carve=*/false, REGPOOL_GROW_LAST,
                           &asks)) {
      p = regpool_retry_refused(size, align, refused, w);
      if (p != NULL)
        return p;
      ARTS_WARN("regpool: no node can grow (%zu-byte alloc, node %d)", size,
                node);
      return NULL;
    }
    /* This node's fresh slab is found by the next sweep pass. */
  }
}

static pthread_once_t g_classes_once = PTHREAD_ONCE_INIT;
/* Frees a thread's classes when it exits; made once per process. */
static pthread_key_t g_classes_key;

/* What t_classes holds once the thread's exit hook has freed its table. */
static regpool_classes_t g_classes_gone;

/* Runs on the exiting thread, after the hooks of keys made before the pool's,
 * the allocator's own among them.  The table is freed here, so an allocation
 * a later hook makes is refused by name rather than reading it. */
static void regpool_classes_free(void *t) {
  t_classes = &g_classes_gone;
  arts_free(t);
}

static void regpool_classes_key_make(void) {
  if (pthread_key_create(&g_classes_key, regpool_classes_free) != 0)
    ARTS_ERROR("regpool: no thread-exit hook for the request classes");
}

static regpool_class_t *regpool_class_slot(regpool_classes_t *t, uint64_t key) {
  unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (t->cap - 1);
  while (t->slot[i].key != 0 && t->slot[i].key != key)
    i = (i + 1) & (t->cap - 1);
  return &t->slot[i];
}

/* The calling thread's entry for the class of (size, align), made on the
 * class's first request with no binding. */
static regpool_class_t *regpool_class_of(size_t size, size_t align) {
  uint64_t key = arts_regpool_class_key(size, align);
  regpool_classes_t *t = t_classes;
  if (t == &g_classes_gone)
    ARTS_ERROR("regpool: allocation on a thread whose exit hooks have run");
  if (t != NULL) {
    regpool_class_t *c = regpool_class_slot(t, key);
    if (c->key == key)
      return c;
  }
  if (t == NULL || 4 * (t->used + 1) > 3 * t->cap) {
    unsigned cap = t == NULL ? 64u : 2 * t->cap;
    regpool_classes_t *n = arts_calloc(1, sizeof *n + cap * sizeof n->slot[0]);
    n->cap = cap;
    if (t != NULL) {
      for (unsigned i = 0; i < t->cap; i++) {
        if (t->slot[i].key != 0)
          *regpool_class_slot(n, t->slot[i].key) = t->slot[i];
      }
      n->used = t->used;
      arts_free(t);
    }
    (void)pthread_once(&g_classes_once, regpool_classes_key_make);
    if (pthread_setspecific(g_classes_key, n) != 0)
      ARTS_ERROR("regpool: no thread-exit hook for the request classes");
    t_classes = t = n;
  }
  regpool_class_t *c = regpool_class_slot(t, key);
  c->key = key;
  c->memo_node = -1;
  c->memo_seen = NULL;
  c->bound.heap = NULL;
  c->bound.arena = NULL;
  t->used++;
  return c;
}

/* The cascade for a request of class `c` that the memo value `seen` (NULL:
 * the node had none) or the class's binding refused.  The arena that serves
 * becomes the class's binding, answering `seen`: the class draws from it
 * until the memo moves past `seen` or the arena refuses the class in turn. */
static void *regpool_alloc_arena_slow(size_t size, size_t align, int node,
                                      regpool_class_t *c,
                                      mi_arena_id_t refused,
                                      mi_arena_id_t seen) {
  atomic_fetch_add_explicit(&g_node_cascades[node], 1, memory_order_relaxed);
  regpool_binding_t w = {NULL, NULL};
  void *p = regpool_cascade(size, align, node, refused, &w);
  if (p != NULL) {
    c->bound = w;
    c->memo_node = seen != NULL ? node : -1;
    c->memo_seen = seen;
  }
  return p;
}

/* Arena path: allocate from the node's memo, or from the arena that served
 * the request's class on this thread when that memo value refused it; on
 * exhaustion enter the re-binding cascade above, which re-probes the node's
 * own growth first.  A node whose memory has not been carved yet carves it
 * here, on its first allocation. */
static void *regpool_alloc_arena(size_t size, size_t align, int node) {
  mi_arena_id_t cur =
      atomic_load_explicit(&g_node_arena[node], memory_order_acquire);
  if (cur == NULL && atomic_load_explicit(&g_node_state[node],
                                          memory_order_acquire) ==
                         REGPOOL_NODE_UNTRIED) {
    regpool_cascade_t asks = {false, 0};
    if (regpool_grow_node(
            node, atomic_load_explicit(&g_slab_count, memory_order_acquire),
            /*carve=*/true, REGPOOL_GROW_OWN, &asks)) {
      cur = atomic_load_explicit(&g_node_arena[node], memory_order_acquire);
    }
  }
  regpool_class_t *c = regpool_class_of(size, align);
  if (cur == NULL)
    return regpool_alloc_arena_slow(size, align, node, c, NULL, NULL);
  if (c->memo_node != node || c->memo_seen != cur) {
    regpool_binding_t w;
    regpool_bind_t b = regpool_thread_bind(cur, &w);
    if (b == REGPOOL_BIND_FAILED)
      return NULL;
    if (b == REGPOOL_BIND_NO_ROOM)
      return regpool_alloc_arena_slow(size, align, node, c, cur, cur);
    c->bound = w;
    c->memo_node = node;
    c->memo_seen = cur;
  }

  void *p = mi_heap_malloc_aligned(c->bound.heap, size, align);
  if (p != NULL)
    return p;
  return regpool_alloc_arena_slow(size, align, node, c, c->bound.arena, cur);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                  */
/* ------------------------------------------------------------------------- */

bool arts_regpool_init(struct fid_domain *domain_or_null,
                       struct fid_ep *ep_or_null, size_t slab_bytes,
                       unsigned int numa_nodes) {
  /* regpool_reserve's trim needs a power of two. */
  size_t arena_align = regpool_arena_align();
  if ((arena_align & (arena_align - 1)) != 0)
    ARTS_ERROR("regpool: arena alignment %zu is not a power of two",
               arena_align);

  pthread_mutex_lock(&g_lock);
  if (g_inited) {
    pthread_mutex_unlock(&g_lock);
    return false;
  }

  /* An explicit argument wins (a test naming its own topology), then what
   * the caller discovered, then what the pool can count for itself. */
  if (numa_nodes == 0)
    numa_nodes = g_req_node_count;
  if (numa_nodes == 0)
    numa_nodes = regpool_detect_nodes();
  if (numa_nodes > REGPOOL_MAX_NODES)
    numa_nodes = REGPOOL_MAX_NODES;

  /* The configured slab is carried on the allocator's slice granule, and no
   * single slab exceeds one table slot's worth — see REGPOOL_SLAB_GRANULE /
   * REGPOOL_SLAB_CAP.  A machine that must reach the table's full extent
   * with fewer ladder steps raises the configured slab, not the cap. */
  size_t slab = align_up_sz(slab_bytes, REGPOOL_SLAB_GRANULE);
  if (slab < REGPOOL_MIN_SLAB)
    slab = REGPOOL_MIN_SLAB;
  if (slab > REGPOOL_SLAB_CAP)
    slab = REGPOOL_SLAB_CAP;

  g_domain = domain_or_null;
  g_ep = ep_or_null;
  g_slab_bytes = slab;
  g_numa_nodes = numa_nodes;
  /* Resolve the node set an oversize mapping interleaves over.  A caller
   * that named none is asking for "wherever this process may run", which is
   * what its CPU affinity says; a machine that will not say falls back to
   * every node, so the spread is never narrower than the truth. */
  {
    uint64_t set = g_req_node_set;
    if (set == 0)
      set = regpool_affinity_node_set(numa_nodes);
    uint64_t all = (numa_nodes >= 64) ? ~(uint64_t)0
                                      : (((uint64_t)1 << numa_nodes) - 1);
    set &= all;
    g_node_set = (set != 0) ? set : all;
  }
  atomic_store_explicit(&g_slab_count, 0, memory_order_relaxed);
  for (unsigned i = 0; i < REGPOOL_MAX_NODES; i++) {
    atomic_store_explicit(&g_node_arena[i], NULL, memory_order_relaxed);
    atomic_store_explicit(&g_node_state[i], REGPOOL_NODE_UNTRIED,
                          memory_order_relaxed);
    atomic_store_explicit(&g_node_spills[i], 0, memory_order_relaxed);
    atomic_store_explicit(&g_node_cascades[i], 0, memory_order_relaxed);
    g_node_last_slab[i] = 0;
    g_node_grow_count[i] = 0;
    g_node_grow_ns[i] = 0;
    g_node_wait_ns[i] = 0;
    g_node_wait_count[i] = 0;
    g_node_sleeps[i] = 0;
    g_node_helpers[i] = 0;
    g_node_help_chunks[i] = 0;
    g_node_chunks[i] = 0;
    g_node_declines[i] = 0;
    g_node_decline_reuses[i] = 0;
    g_node_declined[i] = 0;
    g_node_full_warned[i] = 0;
    g_node_growing[i] = false;
  }
  g_unplaced_growing = false;
  g_report_enabled = getenv("ARTS_REGPOOL_REPORT") != NULL;
#ifdef ARTS_DEBUG_INJECTION
  {
    uint64_t forced = 0;
    const char *ff = getenv("ARTS_REGPOOL_FORCE_FULL_NODES");
    if (ff != NULL && ff[0] != '\0') {
      const char *p = ff;
      while (*p != '\0') {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        /* A token dropped would make the run read as one that exercised
         * the exhaustion it asked for. */
        const char *comma = strchr(p, ',');
        int len = comma != NULL ? (int)(comma - p) : (int)strlen(p);
        if (end == p || (*end != ',' && *end != '\0')) {
          ARTS_ERROR("regpool: ARTS_REGPOOL_FORCE_FULL_NODES token \"%.*s\" "
                     "is not a node number",
                     len, p);
        }
        if (v < 0 || v >= (long)g_numa_nodes) {
          ARTS_ERROR("regpool: ARTS_REGPOOL_FORCE_FULL_NODES names node %ld, "
                     "but this machine has %u",
                     v, g_numa_nodes);
        }
        forced |= 1ULL << (unsigned)v;
        if (*end != ',')
          break;
        p = end + 1;
      }
      if (forced != 0)
        ARTS_WARN("regpool: injected exhaustion — nodes mask 0x%llx treated "
                  "as full",
                  (unsigned long long)forced);
    }
    regpool_store_forced_full_locked(forced);
  }
#endif
  g_inited = true;
  pthread_mutex_unlock(&g_lock);

  /* Carving is lazy: a node's memory is mapped when a thread on it first
   * allocates, so nothing here maps for a node that may never be used.  What
   * init still owes is proof that the pool can map at all — so the
   * initializing thread's own node is carved now and, if that node cannot be
   * carved, the others nearest-first until one can: the node carved instead
   * is the one the initializing node's threads are then served from.  ZERO
   * carved nodes is an init failure, loudly. */
  int first = regpool_current_node();
  if ((unsigned)first >= numa_nodes)
    first = 0;
  int order[REGPOOL_MAX_NODES];
  unsigned cnt = arts_regpool_node_order(first, order, REGPOOL_MAX_NODES);
  bool any = false;
  regpool_cascade_t asks = {false, 0};
  for (unsigned o = 0; o < cnt && !any; o++) {
    regpool_grow_mode_t mode =
        order[o] == first ? REGPOOL_GROW_OWN : REGPOOL_GROW_PEER;
    any = regpool_grow_node(
        order[o], atomic_load_explicit(&g_slab_count, memory_order_acquire),
        /*carve=*/true, mode, &asks);
  }
  if (!any)
    any = regpool_grow_node(
        first, atomic_load_explicit(&g_slab_count, memory_order_acquire),
        /*carve=*/true, REGPOOL_GROW_LAST, &asks);

  if (!any) {
    arts_regpool_cleanup();
    return false;
  }
  return true;
}

/* Print the pool's shape: one line per node, one for the direct mappings,
 * one total.  Caller holds g_lock — the counts must be consistent with each
 * other, and taking the lock is also what lets the fields be read straight
 * rather than through the lock-free accessor. */
static void regpool_report_locked(FILE *out) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  size_t total = 0;
  unsigned direct_live = 0, direct_retired = 0, free_slots = 0;
  size_t direct_live_bytes = 0, direct_retired_bytes = 0;

  for (unsigned node = 0; node < g_numa_nodes; node++) {
    unsigned arenas = 0;
    size_t mapped = 0;
    for (size_t i = 0; i < n; i++) {
      const regpool_slab_t *s = &g_slabs[i];
      if (arts_regpool_entry_state(atomic_load_explicit(
              &g_slab_word[i], memory_order_relaxed)) != REGPOOL_SLOT_LIVE)
        continue;
      if (s->is_direct || s->mr.numa_node != (int)node)
        continue;
      arenas++;
      mapped += s->mr.len;
    }
    uint8_t st = atomic_load_explicit(&g_node_state[node],
                                      memory_order_relaxed);
    fprintf(out,
            "[REGPOOL] node %u: state=%s arenas=%u mapped=%zu grows=%u "
            "grow_ms=%llu waits=%u wait_ms=%llu spills=%u cascades=%u "
            "sleeps=%u helpers=%u help_chunks=%u chunks=%u declines=%u "
            "decline_reuses=%u\n",
            node,
            st == REGPOOL_NODE_CARVED
                ? "carved"
                : (st == REGPOOL_NODE_REFUSED ? "refused" : "untried"),
            arenas, mapped >> 20, g_node_grow_count[node],
            (unsigned long long)(g_node_grow_ns[node] / 1000000ull),
            g_node_wait_count[node],
            (unsigned long long)(g_node_wait_ns[node] / 1000000ull),
            atomic_load_explicit(&g_node_spills[node], memory_order_relaxed),
            atomic_load_explicit(&g_node_cascades[node], memory_order_relaxed),
            g_node_sleeps[node], g_node_helpers[node], g_node_help_chunks[node],
            g_node_chunks[node], g_node_declines[node],
            g_node_decline_reuses[node]);
  }

  size_t unplaced = 0;
  /* Where the direct mappings landed: per node, plus one bucket for the
   * interleaved ones and one for those the kernel placed.  A payload many
   * threads sweep is served by whichever memory controllers hold it, so its
   * placement is part of a run's shape. */
  size_t direct_node_bytes[REGPOOL_MAX_NODES + 1] = {0};
  unsigned direct_node_count[REGPOOL_MAX_NODES + 1] = {0};
  size_t direct_ileave_bytes = 0;
  unsigned direct_ileave_count = 0;
  for (size_t i = 0; i < n; i++) {
    const regpool_slab_t *s = &g_slabs[i];
    unsigned st = arts_regpool_entry_state(
        atomic_load_explicit(&g_slab_word[i], memory_order_relaxed));
    if (st == REGPOOL_SLOT_FREE) {
      free_slots++;
      continue;
    }
    total += s->mr.len;
    if (!s->is_direct) {
      if (s->mr.numa_node < 0)
        unplaced += s->mr.len;
      continue;
    }
    if (st == REGPOOL_SLOT_LIVE) {
      direct_live++;
      direct_live_bytes += s->mr.len;
    } else {
      direct_retired++;
      direct_retired_bytes += s->mr.len;
    }
    if (s->interleaved) {
      direct_ileave_bytes += s->mr.len;
      direct_ileave_count++;
      continue;
    }
    unsigned bucket = (s->mr.numa_node >= 0 &&
                       s->mr.numa_node < (int)REGPOOL_MAX_NODES)
                          ? (unsigned)s->mr.numa_node
                          : REGPOOL_MAX_NODES;
    direct_node_bytes[bucket] += s->mr.len;
    direct_node_count[bucket]++;
  }
  fprintf(out,
          "[REGPOOL] direct: live=%u/%zu retired=%u/%zu free_slots=%u\n",
          direct_live, direct_live_bytes >> 20, direct_retired,
          direct_retired_bytes >> 20, free_slots);
  if (direct_live + direct_retired > 0) {
    fprintf(out, "[REGPOOL] direct placement:");
    if (direct_ileave_count != 0)
      fprintf(out, " interleaved=%u/%zu", direct_ileave_count,
              direct_ileave_bytes >> 20);
    for (unsigned b = 0; b <= REGPOOL_MAX_NODES; b++) {
      if (direct_node_count[b] == 0)
        continue;
      if (b == REGPOOL_MAX_NODES)
        fprintf(out, " unplaced=%u/%zu", direct_node_count[b],
                direct_node_bytes[b] >> 20);
      else
        fprintf(out, " node%u=%u/%zu", b, direct_node_count[b],
                direct_node_bytes[b] >> 20);
    }
    fprintf(out, "\n");
  }
  fprintf(out,
          "[REGPOOL] total: slabs=%zu/%u mapped=%zu unplaced=%zu base=%zu "
          "arena_slots=%u/%u table_refusals=%u theap_refusals=%u\n",
          n, REGPOOL_MAX_SLABS, total >> 20, unplaced >> 20, g_slab_bytes >> 20,
          g_pool_slots, (unsigned)MI_MAX_ARENAS, g_table_refusals,
          atomic_load_explicit(&g_theap_refusals, memory_order_relaxed));
}

void arts_regpool_report(FILE *out) {
  if (out == NULL)
    return;
  pthread_mutex_lock(&g_lock);
  regpool_report_locked(out);
  pthread_mutex_unlock(&g_lock);
  fflush(out);
}

#ifdef ARTS_DEBUG_INJECTION
void arts_regpool_set_forced_full(uint64_t node_mask) {
  pthread_mutex_lock(&g_lock);
  if (g_numa_nodes < 64u && (node_mask >> g_numa_nodes) != 0) {
    ARTS_ERROR("regpool: force-full mask 0x%llx names a node this machine "
               "(%u nodes) lacks",
               (unsigned long long)node_mask, g_numa_nodes);
  }
  regpool_store_forced_full_locked(node_mask);
  pthread_mutex_unlock(&g_lock);
}
#endif

void arts_regpool_set_topology(unsigned int node_count, uint64_t node_set,
                               const uint32_t *distance) {
  pthread_mutex_lock(&g_lock);
  if (g_inited) {
    /* The values are CONSUMED by init: the node set is resolved there and
     * every mapping since has been placed against it.  A later call would
     * change where allocations land without moving anything already resident,
     * so the pool's own record of its placement would stop describing its
     * memory.  Fail loudly rather than half-apply. */
    pthread_mutex_unlock(&g_lock);
    ARTS_ERROR("regpool: topology set after init — the node set is consumed "
               "at init and cannot be changed under live mappings");
    return;
  }
  unsigned stride = node_count; /* the caller's table rows */
  if (node_count > REGPOOL_MAX_NODES)
    node_count = REGPOOL_MAX_NODES;
  g_req_node_count = node_count;
  g_req_node_set = node_set;
  g_distance_known = false;
  if (distance != NULL && node_count > 0) {
    /* Copied, not referenced: the table outlives whatever the caller built
     * it in, and nothing here may follow a pointer into a freed topology.
     * Rows keep a fixed stride, so an init that counts the nodes differently
     * still reads the distance the caller gave for each pair. */
    for (unsigned i = 0; i < node_count; i++) {
      for (unsigned j = 0; j < node_count; j++) {
        uint32_t d = distance[(size_t)i * stride + j];
        /* UINT32_MAX is kept for a pair the table does not describe. */
        g_node_distance[(size_t)i * REGPOOL_MAX_NODES + j] =
            d < UINT32_MAX ? d : UINT32_MAX - 1;
      }
    }
    g_distance_nodes = node_count;
    g_distance_known = true;
  }
  pthread_mutex_unlock(&g_lock);
}

void arts_regpool_cleanup(void) {
  pthread_mutex_lock(&g_lock);
  if (g_report_enabled)
    regpool_report_locked(stderr);
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    /* A free slot's mr/base/len are stale and must not be touched again: the
     * eviction that released it closed and unmapped the range, or, when the
     * provider refused that close, left it mapped, registered and forgotten
     * until the process exit.  A retired one is still mapped and
     * registered. */
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) == REGPOOL_SLOT_FREE)
      continue;
    /* A registration whose close failed, at unregister or here, may still
     * take a write: it is not closed again and its range is not unmapped,
     * both left to the process exit. */
    bool closed = !s->mr_kept && regpool_mr_close(s->mr.mr);
    /* Direct slabs are the pool's own mappings and are unmapped here.  Arena
     * slabs are owned by the allocator's arena registry; the vendored allocator
     * exposes no public arena-unload, so unmapping one out from under it would
     * dangle its metadata.  The OS reclaims those ranges at process exit — the
     * pool's lifetime is the process lifetime. */
    if (s->is_direct && closed)
      munmap(s->mr.base, s->mr.len);
    atomic_store_explicit(&g_slab_word[i], 0, memory_order_release);
  }
  atomic_store_explicit(&g_slab_count, 0, memory_order_release);
  for (unsigned i = 0; i < REGPOOL_MAX_NODES; i++) {
    atomic_store_explicit(&g_node_arena[i], NULL, memory_order_release);
    atomic_store_explicit(&g_node_state[i], REGPOOL_NODE_UNTRIED,
                          memory_order_release);
    atomic_store_explicit(&g_node_spills[i], 0, memory_order_relaxed);
    atomic_store_explicit(&g_node_cascades[i], 0, memory_order_relaxed);
    g_node_last_slab[i] = 0;
    g_node_grow_count[i] = 0;
    g_node_grow_ns[i] = 0;
    g_node_wait_ns[i] = 0;
    g_node_wait_count[i] = 0;
    g_node_sleeps[i] = 0;
    g_node_helpers[i] = 0;
    g_node_help_chunks[i] = 0;
    g_node_chunks[i] = 0;
    g_node_declines[i] = 0;
    g_node_decline_reuses[i] = 0;
    g_node_declined[i] = 0;
    g_node_full_warned[i] = 0;
    g_node_growing[i] = false;
  }
  g_unplaced_growing = false;
  atomic_store_explicit(&g_theap_refusals, 0, memory_order_relaxed);
  /* The table-slot count and the refusal latch are not reset: arena slabs
   * outlive the pool and keep their slots, so both describe the process's
   * arena table, which no cleanup gives back. */
#ifdef ARTS_DEBUG_INJECTION
  regpool_store_forced_full_locked(0);
#endif
  g_report_enabled = false;
  g_domain = NULL;
  g_ep = NULL;
  g_slab_bytes = 0;
  g_numa_nodes = 0;
  g_node_set = 0;
  g_req_node_set = 0;
  g_req_node_count = 0;
  g_distance_known = false;
  g_distance_nodes = 0;
  g_inited = false;
  pthread_mutex_unlock(&g_lock);
}

/* The teardown's report when ARTS_REGPOOL_REPORT was set at init, for a
 * teardown that leaves the pool to the process exit instead of cleaning it up
 * (cleanup prints the same report itself). */
void arts_regpool_report_if_requested(void) {
  pthread_mutex_lock(&g_lock);
  if (g_report_enabled)
    regpool_report_locked(stderr);
  pthread_mutex_unlock(&g_lock);
}

bool arts_regpool_unregister(void) {
  pthread_mutex_lock(&g_lock);
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  size_t closed = 0;
  bool all_closed = true;
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    /* A retired mapping is still registered.  A free slot names nothing to
     * close: its range was closed at its eviction, or forgotten there when the
     * provider refused the close. */
    if (arts_regpool_entry_state(atomic_load_explicit(
            &g_slab_word[i], memory_order_relaxed)) == REGPOOL_SLOT_FREE) {
      continue;
    }
    if (s->mr_kept) {
      all_closed = false;
      continue;
    }
    if (s->mr.mr == NULL) {
      continue;
    }
    if (regpool_mr_close(s->mr.mr)) {
      s->mr.mr = NULL;
      s->mr.rkey = 0;
      closed++;
    } else {
      /* Kept with its handle and key: it is still registered. */
      s->mr_kept = true;
      all_closed = false;
    }
  }
  /* Detach from the fabric: a slab mapped after this point registers
   * nothing (NULL-domain behavior), rather than touching a domain or
   * endpoint the transport is about to close. */
  g_domain = NULL;
  g_ep = NULL;
  pthread_mutex_unlock(&g_lock);
  if (closed != 0) {
    ARTS_INFO("regpool: closed %zu slab registrations ahead of endpoint "
              "teardown",
              closed);
  }
  return all_closed;
}

bool arts_regpool_grow(int numa_node) {
  if (numa_node < 0 || (unsigned)numa_node >= g_numa_nodes)
    numa_node = 0;
  /* The current table length asks for a slab unconditionally — this entry
   * point is a request to grow, not a reaction to an exhausted arena. */
  regpool_cascade_t asks = {false, 0};
  return regpool_grow_node(
      numa_node, atomic_load_explicit(&g_slab_count, memory_order_acquire),
      /*carve=*/false, REGPOOL_GROW_OWN, &asks);
}

void *arts_regpool_alloc_aligned(size_t size, size_t align) {
  if (size == 0)
    return NULL;
  size_t slab = g_slab_bytes;
  if (slab == 0)
    return NULL; /* pool not initialized */
  if (align < REGPOOL_ALIGN_FLOOR)
    align = REGPOOL_ALIGN_FLOOR;
  if ((align & (align - 1)) != 0)
    ARTS_ERROR("regpool: alignment %zu is not a power of two", align);

  int node = regpool_current_node();
  if ((unsigned)node >= g_numa_nodes)
    node = 0;

  /* A request the arena would serve out of whole chunks, or that no two of
   * which could share a slab, is cheaper in a mapping of its own, and one
   * aligned past what an arena honours can only have one; the rest draw
   * from the node's arena, which grows on exhaustion. */
  size_t direct_above = slab / 2;
  if (direct_above > REGPOOL_MAX_ARENA_OBJ)
    direct_above = REGPOOL_MAX_ARENA_OBJ;
  void *p = (size > direct_above || align > REGPOOL_MAX_ARENA_ALIGN)
                ? regpool_alloc_direct(size, align, node)
                : regpool_alloc_arena(size, align, node);

  /* Fail loudly: an allocation that could not be satisfied even after a grow
   * cannot be papered over — the payload it would back has nowhere to live.
   * Print the pool's shape first so exhaustion is distinguishable from an
   * allocator-path defect in the field. */
  if (p == NULL) {
    arts_regpool_report(stderr);
    ARTS_ERROR("regpool: could not satisfy %zu-byte allocation (align %zu)",
               size, align);
  }

  /* Confinement guard.  An external arena's contiguity is not contractually
   * guaranteed by the allocator, so a returned pointer that resolves to no
   * registered slab would be an address the NIC cannot reach — a correctness
   * failure, not a soft error. */
  if (arts_regpool_lookup(p) == NULL)
    ARTS_ERROR("regpool: allocation %p (size %zu) escaped all registered slabs",
               p, size);
  return p;
}

void arts_regpool_free(void *p) {
  if (p == NULL)
    return;
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL) {
    /* A miss is a foreign pointer, a free after cleanup, or a second free of
     * an oversize block, whose first free stopped it resolving: only a
     * pointer the pool returned may be freed, once, before cleanup. */
    ARTS_ERROR("regpool: free of %p, which no live slab holds", p);
  }
  regpool_slab_t *s =
      (regpool_slab_t *)((char *)m - offsetof(regpool_slab_t, mr));
  if (!s->is_direct) {
    mi_free(p);
    return;
  }
  /* Oversize direct slab: its mapping backs exactly this one allocation, so
   * this free leaves the mapping owned by nobody (the same
   * uses-complete-and-transport-quiesced precondition an arena free already
   * relies on).  Retire it rather than unmap it: it stays mapped, placed,
   * populated and registered for the next request of the same shape, and
   * the freed pointer stops resolving because the slot is no longer live.
   * The slot is judged again under the lock: another free of the same block
   * may have retired it since the lookup, and only a live slot whose mapping
   * starts at p is this block. */
  pthread_mutex_lock(&g_lock);
  size_t i = (size_t)(s - g_slabs);
  uint64_t w = atomic_load_explicit(&g_slab_word[i], memory_order_relaxed);
  bool mine =
      arts_regpool_entry_state(w) == REGPOOL_SLOT_LIVE && s->mr.base == p;
  if (mine) {
    atomic_store_explicit(
        &g_slab_word[i], arts_regpool_entry_with_state(w, REGPOOL_SLOT_RETIRED),
        memory_order_release);
  }
  pthread_mutex_unlock(&g_lock);
  if (!mine) {
    ARTS_ERROR("regpool: free of %p, which is not a live oversize block's "
               "start (freed twice, or an interior pointer)", p);
  }
}

const arts_regpool_mr_t *arts_regpool_lookup(const void *p) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    uint64_t w = atomic_load_explicit(&g_slab_word[i], memory_order_acquire);
    if (arts_regpool_entry_state(w) != REGPOOL_SLOT_LIVE)
      continue;
    if ((uintptr_t)p - arts_regpool_entry_base(w) < arts_regpool_entry_len(w))
      return &g_slabs[i].mr;
  }
  return NULL;
}

#else /* !ARTS_MALLOC_MIMALLOC — no arena allocator: fall back to a plain
       * system aligned allocator instead of an inert (always-failing) stub.
       * Every caller in this configuration still routes through this API
       * and expects working memory back, so init must succeed; what it
       * cannot provide without an arena allocator is slabs, NUMA placement, or
       * fabric registration.  arts_regpool_lookup therefore always misses
       * (no registered range exists to resolve to) and a non-NULL domain is
       * silently ignored — callers that need RDMA-targetable memory require
       * the arena-allocator build; this build only guarantees plain,
       * symmetric alloc/free. */

bool arts_regpool_init(struct fid_domain *domain_or_null,
                       struct fid_ep *ep_or_null, size_t slab_bytes,
                       unsigned int numa_nodes) {
  (void)domain_or_null;
  (void)ep_or_null;
  (void)slab_bytes;
  (void)numa_nodes;
#ifdef ARTS_DEBUG_INJECTION
  /* No placement here for the list to exhaust. */
  const char *ff = getenv("ARTS_REGPOOL_FORCE_FULL_NODES");
  if (ff != NULL && ff[0] != '\0') {
    ARTS_ERROR("regpool: ARTS_REGPOOL_FORCE_FULL_NODES is set, but this "
               "build's pool places no memory (ARTS_MALLOC=system)");
  }
#endif
  return true;
}
void arts_regpool_cleanup(void) {}
bool arts_regpool_unregister(void) { return true; }
void arts_regpool_report_if_requested(void) {}
void arts_regpool_report(FILE *out) {
  /* No slabs, no nodes, no direct mappings to describe — say so rather than
   * print a shape this build does not have. */
  if (out != NULL)
    fprintf(out, "[REGPOOL] no arena allocator: pass-through allocation\n");
}
#ifdef ARTS_DEBUG_INJECTION
void arts_regpool_set_forced_full(uint64_t node_mask) {
  /* No placement here for the mask to exhaust. */
  if (node_mask != 0) {
    ARTS_ERROR("regpool: a force-full mask was set, but this build's pool "
               "places no memory (ARTS_MALLOC=system)");
  }
}
#endif
void arts_regpool_set_topology(unsigned int node_count, uint64_t node_set,
                               const uint32_t *distance) {
  /* Nothing places memory here, so the topology has nothing to steer. */
  (void)node_count;
  (void)node_set;
  (void)distance;
}
unsigned int arts_regpool_node_order(int from, int *out, unsigned int max) {
  /* One candidate, and it is wherever the system allocator puts things. */
  (void)from;
  if (out == NULL || max == 0)
    return 0;
  out[0] = 0;
  return 1;
}
void *arts_regpool_alloc_aligned(size_t size, size_t align) {
  if (size == 0) {
    return NULL;
  }
  size_t a = align < sizeof(void *) ? sizeof(void *) : align;
  void *p = NULL;
  if (posix_memalign(&p, a, size) != 0) {
    return NULL;
  }
  return p;
}
void arts_regpool_free(void *p) { free(p); }
const arts_regpool_mr_t *arts_regpool_lookup(const void *p) {
  (void)p;
  return NULL;
}
bool arts_regpool_grow(int numa_node) {
  /* No slab concept in this build — honestly report "nothing was grown"
   * rather than claim success for a no-op. */
  (void)numa_node;
  return false;
}

#endif /* ARTS_MALLOC_MIMALLOC */
