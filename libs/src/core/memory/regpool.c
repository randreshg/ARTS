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
 *     live only once every field is written (release-store of the state); a
 *     retired slot keeps exactly the fields published when its mapping was
 *     created, so retirement and reuse change nothing but the state.
 *   - A reader that observed a slot's state must know that the fields it
 *     then reads belong to the same generation of that slot.  State and
 *     fields are separate loads, so a slot rewritten between them would hand
 *     back a torn range — one generation's base with another's length — and
 *     resolve a pointer into the wrong registration.  A per-entry generation
 *     word is the gate: only a rewrite touches it, leaving it odd while the
 *     fields are in flux.  An ODD generation is a slot being published for a
 *     mapping nobody holds a pointer to yet, so a reader skips it; a
 *     generation that CHANGED across the read means the fields read may be a
 *     torn pair, so the reader reads the entry again.
 */

#include "arts/memory/regpool.h"

#include <errno.h> /* mmap failure diagnostics */
#include <stdatomic.h>
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
#include <mimalloc.h>
#include <mimalloc/types.h> /* arena geometry the slab sizes mirror */
#include <pthread.h>
#include <time.h>
#include <sched.h> /* CPU affinity mask — the node set's fallback source */
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
/* Base alignment for every mapping: >= the allocator's arena slice alignment
 * (so a slab is used in full) and huge-page friendly. */
#define REGPOOL_BASE_ALIGN ((size_t)2 * 1024 * 1024)
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
/* Largest object an arena serves out of whole chunks; a request above it is
 * cheaper in a mapping of its own (see regpool_alloc_direct).  Mirrors the
 * vendored allocator's chunk geometry, asserted below so drift is a compile
 * error rather than silent per-object waste. */
#define REGPOOL_MAX_ARENA_OBJ ((size_t)32 * 1024 * 1024)
_Static_assert(REGPOOL_MAX_ARENA_OBJ == MI_ARENA_MAX_CHUNK_OBJ_SIZE,
               "regpool's oversize threshold must track the allocator's "
               "largest chunk object");
/* Growth factor of a node's slab ladder, as a fraction (see
 * regpool_next_slab_locked). */
#define REGPOOL_GROWTH_NUM 3
#define REGPOOL_GROWTH_DEN 2
/* Payload alignment floor (matches the DB/CXL 64-byte payload invariant). */
#define REGPOOL_ALIGN_FLOOR ((size_t)64)
/* Held back from a node's availability estimate before a mapping is sized
 * against it — room for concurrent consumers, so a slab sized to the
 * estimate still lands where it was placed. */
#define REGPOOL_NODE_HEADROOM ((size_t)2 * 1024 * 1024 * 1024)

/* Lifecycle of one table entry.  An arena slab is created LIVE and stays
 * LIVE; the other two states belong to direct slabs. */
#define REGPOOL_SLOT_FREE 0u    /* mapping torn down, slot open for reuse;
                                 * the zero value, so an unwritten slot is
                                 * one readers skip                         */
#define REGPOOL_SLOT_RETIRED 1u /* mapping owned by nobody, kept for reuse   */
#define REGPOOL_SLOT_LIVE 2u    /* mapping owned by one allocation           */

/* One slab record.  The embedded public view is what lookups return; the extra
 * fields drive free() and growth. */
typedef struct regpool_slab_s {
  arts_regpool_mr_t mr;   /* public: base, len, mr, rkey, numa_node          */
  bool interleaved;       /* true = spread over a node set rather than
                           * placed on mr.numa_node, which is then -1: the
                           * two together are the three placements a mapping
                           * can have (a node, a set, or the kernel's
                           * choice)                                        */
  bool is_direct;         /* true = oversize single-allocation direct slab.
                           * Never changes across a slot's generations: an
                           * arena slab is never retired, and a free slot is
                           * only ever rewritten as a direct one — which is
                           * what lets the arena sweep read it after the
                           * state check alone                              */
  mi_arena_id_t arena;    /* backing arena (arena slabs only; NULL if direct) */
  size_t align;           /* alignment the mapping was created with           */
  _Atomic uint8_t state;  /* REGPOOL_SLOT_*; every transition is a
                           * release-store under g_lock, and only a FREE
                           * slot's fields are ever rewritten                */
  _Atomic uint32_t gen;   /* generation of this entry's fields, odd while a
                           * rewrite is in flux.  Bumped ONLY by the
                           * FREE -> LIVE rewrite: it is what lets a reader
                           * tell that the range it read belongs to the
                           * generation whose state it observed             */
} regpool_slab_t;

/* --- module state (guarded by g_lock except where marked atomic) --------- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* Waited on by a thread that wants a node grown while another thread is
 * already mapping a slab for that node, and broadcast when a grow ends. */
static pthread_cond_t g_grow_cv = PTHREAD_COND_INITIALIZER;
static bool g_inited;
/* g_domain, g_ep, g_slab_bytes and g_numa_nodes are read WITHOUT g_lock by
 * the mapping and allocation paths: they are written at init before any
 * allocation, and at unregister/cleanup, which are single-threaded by
 * contract (see the header). */
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

/* Relative access latency from each node to each other, row-major over
 * g_numa_nodes with the requesting node as the row.  Only the fallback ORDER
 * consults it, so the values' scale is immaterial — only their comparison
 * is.  g_distance_known false means the machine reported none, and every
 * node is then equidistant. */
static uint32_t g_node_distance[REGPOOL_MAX_NODES * REGPOOL_MAX_NODES];
static bool g_distance_known;

/* What arts_regpool_set_topology left for the next init to consume (0 = the
 * caller said nothing and init discovers it). */
static uint64_t g_req_node_set;
static unsigned g_req_node_count;

static regpool_slab_t g_slabs[REGPOOL_MAX_SLABS];
static atomic_size_t g_slab_count; /* release on append, acquire on read */

/* Monotonic memory-registration key source.  When the provider does NOT
 * negotiate FI_MR_PROV_KEY the application must supply a UNIQUE requested_key
 * per fi_mr_reg — registering a second slab with a duplicate key (e.g. 0)
 * fails with FI_ENOKEY.  A unique key is also harmless when the provider DOES
 * assign keys itself (it then ignores requested_key), so a monotone counter is
 * correct for either negotiation. */
static atomic_uint_least64_t g_mr_key_next;

/* Current arena a node's allocations draw from.  A grow publishes a fresh
 * arena here; per-thread heaps notice the change and re-bind. */
static _Atomic(mi_arena_id_t) g_node_arena[REGPOOL_MAX_NODES];

/* Per-node grow count, for the report (guarded by g_lock). */
static unsigned g_node_grow_count[REGPOOL_MAX_NODES];

/* Time a node's grows spent mapping (outside the lock, summed over grows)
 * and time threads spent waiting for a grow already in flight on their node
 * (inside the lock, summed over waits), for the report.  A wait is what an
 * allocation costs when it lands on an exhausted node behind the thread
 * that is mapping its next slab; the mapping time bounds it.  Guarded by
 * g_lock. */
static uint64_t g_node_grow_ns[REGPOOL_MAX_NODES];
static uint64_t g_node_wait_ns[REGPOOL_MAX_NODES];
static unsigned g_node_wait_count[REGPOOL_MAX_NODES];

static uint64_t regpool_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Size in bytes of the last arena slab actually MAPPED for a node, which is
 * what the next size is derived from (0 = none yet).  Guarded by g_lock. */
static size_t g_node_last_slab[REGPOOL_MAX_NODES];

/* Whether a node's memory has been carved yet.  A node is carved when a
 * thread on it first allocates, so an UNTRIED node is memory that exists and
 * has simply not been asked for; only a node the pool tried and could not
 * carve is REFUSED, and only a REFUSED node's threads are served from
 * another node's arena. */
#define REGPOOL_NODE_UNTRIED 0u
#define REGPOOL_NODE_CARVED 1u
#define REGPOOL_NODE_REFUSED 2u
static _Atomic uint8_t g_node_state[REGPOOL_MAX_NODES];

/* A grow in flight for this node (guarded by g_lock).  One at a time per
 * node: the mapping runs outside the lock, so the flag plus g_grow_cv is
 * what keeps a herd of threads from mapping a slab each. */
static bool g_node_growing[REGPOOL_MAX_NODES];

/* One warning per node exhaustion, not one per refused grow: a full node is
 * re-tried by every allocation that prefers it, and each retry would print.
 * Guarded by g_lock (set and cleared only inside a grow). */
static bool g_node_full_warned[REGPOOL_MAX_NODES];

/* Memoized serving node for a node with no arena of its own (refused at
 * init, not yet recovered): -1 = none chosen yet.  Placement is a
 * preference, never a reason to refuse memory that exists, so such a
 * node's threads are served from the fallback's arena on the fast path;
 * the node's own arena is re-checked on every allocation, so a later
 * successful grow reclaims its threads automatically and the memo goes
 * stale unused. */
static _Atomic int g_node_fallback[REGPOOL_MAX_NODES];

/* Diagnostic override: nodes whose bit is set report zero available bytes,
 * so the refusal, fallover and eviction paths are exercisable
 * deterministically without starving a machine.  Written under g_lock (by
 * the init-time parse of ARTS_REGPOOL_FORCE_FULL_NODES and by
 * arts_regpool_set_forced_full), read relaxed by the placement screens. */
static _Atomic uint64_t g_forced_full;

/* Publish a force-full mask.  Caller holds g_lock. */
static void regpool_store_forced_full_locked(uint64_t mask) {
  atomic_store_explicit(&g_forced_full, mask, memory_order_relaxed);
}

/* Read at init, like the force-full knob: print the pool's shape at cleanup. */
static bool g_report_enabled;

/* Kernels predating the populate advice report EINVAL; there a slab is
 * populated by a write per page instead, for the process lifetime.  Latched
 * on first sight, by mapping code that runs outside g_lock. */
static _Atomic bool g_populate_unsupported;

/* Per-thread allocator heap, bound to one exclusive arena at a time.  The
 * binding moves on exhaustion (see regpool_thread_bind); the superseded heap
 * is always deleted so its empty pages return to their arena for reuse. */
static __thread mi_heap_t *t_heap;
static __thread mi_arena_id_t t_arena;
static __thread int t_node = -1;

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

/* ------------------------------------------------------------------------- */
/* helpers                                                                     */
/* ------------------------------------------------------------------------- */

static inline size_t align_up_sz(size_t v, size_t a) {
  return (v + a - 1) & ~(a - 1);
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

/* NUMA node the calling thread currently runs on; fall back to node 0. */
static int regpool_current_node(void) {
  unsigned cpu = 0, node = 0;
  if (syscall(SYS_getcpu, &cpu, &node, NULL) == 0)
    return (int)node;
  return 0;
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
  (void)syscall(SYS_mbind, base, len, mode, &mask,
                (unsigned long)(sizeof(mask) * 8), 0UL);
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
  return g_node_distance[(size_t)from * g_numa_nodes + (size_t)to];
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
  uint64_t forced = atomic_load_explicit(&g_forced_full, memory_order_relaxed);
  if (node >= 0 && node < (int)REGPOOL_MAX_NODES &&
      (forced & (1ULL << (unsigned)node)) != 0) {
    return 0;
  }
  char path[64];
  snprintf(path, sizeof path, "/sys/devices/system/node/node%d/meminfo", node);
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return SIZE_MAX;
  size_t r = arts_regpool_parse_node_avail(f);
  fclose(f);
  return r;
}

/* Fault every page of a fresh anonymous mapping whose placement policy is
 * already set.  False means the range could not be made resident and the
 * caller must give the mapping up.
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

/* Map `len` bytes aligned to `align`, apply the NUMA placement preference
 * (`set` non-empty interleaves over it, else `node` is preferred — see
 * regpool_place), populate, and (when a domain is set) register.  Over-maps
 * by `align` and trims so the base is aligned regardless of what the kernel
 * hands back.  On success fills *out_* and returns true; on any failure
 * unwinds and returns false.
 *
 * Runs WITHOUT g_lock — it is tens of milliseconds of syscalls per slab, and
 * no allocation, free or grow may queue behind it — so everything it reads
 * is either immutable for the pool's lifetime or atomic. */
static bool regpool_map_slab(int node, uint64_t set, size_t len, size_t align,
                             void **out_base, struct fid_mr **out_mr,
                             uint64_t *out_rkey) {
  size_t over = len + align;
  void *raw = mmap(NULL, over, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

  /* Must run before fi_mr_reg() (which faults every page while pinning) or
   * any allocator first-touch — a policy set after pages are already
   * resident affects only future faults and silently fails to relocate this
   * range. */
  regpool_place(base, len, node, set);

  /* Populate every page NOW, so that a slab's residency is decided at
   * creation and the pool's resident set is its mapped capacity on every
   * transport.  A registration pins — and therefore faults — every page
   * anyway; without population an unregistered run's resident set would
   * depend on first touch and would no longer predict a registered run's.
   * The population's RESULT must be honored: it can stop early on a signal
   * (EINTR/EAGAIN) or fail outright (EFAULT and kin), and a partially
   * populated slab would fault its tail later.  Interruptions are retried
   * over the whole range (populated pages are cheap no-ops); a real failure
   * fails the map so the caller can step down or relocate.  The cost is
   * that a slab commits in full at creation. */
  if (!regpool_populate(base, len)) {
    munmap(base, len);
    return false;
  }

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
      /* Distinguishable from the mmap failure above: the mapping existed but
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
        fi_close(&mr->fid);
        munmap(base, len);
        return false;
      }
    }
    rkey = fi_mr_key(mr);
    if (ep != NULL && rkey == FI_KEY_NOTAVAIL) {
      ARTS_ERROR("regpool: MR key unavailable after enable — provider broke "
                 "the key-after-enable contract");
    }
  }

  *out_base = base;
  *out_mr = mr;
  *out_rkey = rkey;
  return true;
}

/* Read one entry's range without the lock: true, with *out_base / *out_len,
 * when the entry is live.  The generation word is what makes the answer
 * whole — see the module header's second publication rule.
 *
 * An odd generation means the entry is being rewritten for a mapping whose
 * base has not been handed to anyone yet, so it can own no address a caller
 * could be asking about: skip it rather than wait on a writer that may be
 * descheduled mid-rewrite.  A generation that CHANGED across the read is a
 * rewrite that completed under it, which says nothing about the entry's
 * current contents — re-read those. */
static bool regpool_slot_live_range(const regpool_slab_t *s,
                                    const char **out_base, size_t *out_len) {
  for (;;) {
    uint32_t g1 = atomic_load_explicit(&s->gen, memory_order_acquire);
    if ((g1 & 1u) != 0)
      return false;
    if (atomic_load_explicit(&s->state, memory_order_acquire) !=
        REGPOOL_SLOT_LIVE)
      return false;
    const char *base = (const char *)s->mr.base;
    size_t len = s->mr.len;
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&s->gen, memory_order_relaxed) != g1)
      continue; /* rewritten under us: the pair above may be torn */
    *out_base = base;
    *out_len = len;
    return true;
  }
}

/* Write an entry's fields and publish it live.  Caller holds g_lock and the
 * slot must be unreachable to readers — either past the published count, or
 * REGPOOL_SLOT_FREE.  The generation bump around the fields is what a
 * lock-free reader validates its range against. */
static void regpool_write_slot_locked(regpool_slab_t *s, void *base,
                                      size_t len, struct fid_mr *mr,
                                      uint64_t rkey, int node,
                                      bool interleaved, bool is_direct,
                                      mi_arena_id_t arena, size_t align) {
  uint32_t g = atomic_load_explicit(&s->gen, memory_order_relaxed);
  atomic_store_explicit(&s->gen, g + 1, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  s->mr.base = base;
  s->mr.len = len;
  s->mr.mr = mr;
  s->mr.rkey = rkey;
  s->mr.numa_node = node;
  s->interleaved = interleaved;
  s->is_direct = is_direct;
  s->arena = arena;
  s->align = align;
  atomic_store_explicit(&s->gen, g + 2, memory_order_release);
  atomic_store_explicit(&s->state, REGPOOL_SLOT_LIVE, memory_order_release);
}

/* Append a fully-formed slab record and publish it.  Caller holds g_lock.
 * Returns the record, or NULL if the table is full. */
static regpool_slab_t *regpool_append(void *base, size_t len, struct fid_mr *mr,
                                      uint64_t rkey, int node,
                                      bool interleaved, bool is_direct,
                                      mi_arena_id_t arena, size_t align) {
  size_t idx = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  if (idx >= REGPOOL_MAX_SLABS)
    return NULL;
  regpool_slab_t *s = &g_slabs[idx];
  atomic_store_explicit(&s->gen, 0, memory_order_relaxed);
  regpool_write_slot_locked(s, base, len, mr, rkey, node, interleaved,
                            is_direct, arena, align);
  /* Release: the fully-written record must be visible before the count that
   * exposes it to lock-free readers. */
  atomic_store_explicit(&g_slab_count, idx + 1, memory_order_release);
  return s;
}

/* Publish a fully-formed direct-slab record, preferring an already-published
 * free slot over growing the table.  Caller holds g_lock.  Only the
 * direct-slab path calls this: arena slabs are never retired, so every free
 * entry in the table is, and will again be, a direct slab. */
static regpool_slab_t *regpool_publish_direct_locked(void *base, size_t len,
                                                     struct fid_mr *mr,
                                                     uint64_t rkey, int node,
                                                     bool interleaved,
                                                     mi_arena_id_t arena,
                                                     size_t align) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_relaxed);
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    if (atomic_load_explicit(&s->state, memory_order_relaxed) !=
        REGPOOL_SLOT_FREE)
      continue;
    regpool_write_slot_locked(s, base, len, mr, rkey, node, interleaved,
                              /*is_direct=*/true, arena, align);
    return s;
  }
  return regpool_append(base, len, mr, rkey, node, interleaved, true, arena,
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
    if (atomic_load_explicit(&s->state, memory_order_relaxed) !=
        REGPOOL_SLOT_RETIRED)
      continue;
    if (s->mr.len != len || s->align < align)
      continue;
    hit = s;
    if (s->mr.numa_node == node)
      break;
  }
  if (hit != NULL)
    atomic_store_explicit(&hit->state, REGPOOL_SLOT_LIVE,
                          memory_order_release);
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
    if (atomic_load_explicit(&s->state, memory_order_relaxed) !=
        REGPOOL_SLOT_RETIRED)
      continue;
    if (s->mr.mr != NULL)
      fi_close(&s->mr.mr->fid);
    munmap(s->mr.base, s->mr.len);
    freed += s->mr.len;
    atomic_store_explicit(&s->state, REGPOOL_SLOT_FREE, memory_order_release);
  }
  return freed;
}

static size_t regpool_evict_retired(size_t need) {
  pthread_mutex_lock(&g_lock);
  size_t freed = regpool_evict_retired_locked(need);
  pthread_mutex_unlock(&g_lock);
  return freed;
}

/* What the machine can still give a new slab: MemAvailable, with a fixed
 * fraction held back so the pool never races the rest of the process (and
 * the OS) to the last page.  A registration faults every page in, so sizing
 * past this turns a clean refusal into the OOM killer; an unregistered slab
 * is clamped by the same number because availability is a property of the
 * machine, not of whether the range will be registered.  0 on any parse
 * trouble — the caller treats that as "no clamp beyond the ladder itself". */
static size_t regpool_mem_available(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL)
    return 0;
  char line[128];
  size_t kb = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1)
      break;
  }
  fclose(f);
  return (kb >> 4) * 15 * 1024; /* 15/16 of it, in bytes */
}

/* One step down the ladder: half the size, carried back onto the granule and
 * never below the configured base slab.  Every candidate size stays a
 * granule multiple, which regpool_map_slab's tail trim and the allocator's
 * slice geometry both rely on. */
static size_t regpool_step_down(size_t want, size_t floor_bytes) {
  size_t half = (want / 2) & ~(REGPOOL_SLAB_GRANULE - 1);
  return half < floor_bytes ? floor_bytes : half;
}

/* Size of the next arena slab for `node`: half again the last slab actually
 * MAPPED there (the configured base slab when the node has none yet),
 * carried onto the granule and capped.  Deriving it from what was mapped
 * rather than from a step count is what keeps a clamped or halved grow from
 * skipping a rung.
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

/* Map one more arena slab for `node` and publish it as the node's current
 * arena.  Returns true when a slab appeared for the node — whether this call
 * mapped it or another thread's grow did.
 *
 * `seen_slabs` is the table length the caller last observed: the grow is
 * skipped, and reported as satisfied, once the table has moved past it,
 * which collapses a herd of threads that exhausted the same arena into a
 * single mapping (the losers retry allocation against the winner's slab
 * instead of each mapping one of their own).  Pass the current length to ask
 * for a slab unconditionally.  `carve` marks a node's first carve: then an
 * arena already published for the node is the answer whatever the length
 * says — the length is published before the arena it stands for, so a caller
 * that read the length after a peer's carve had completed would otherwise
 * carve a second slab.
 *
 * The mapping itself runs outside g_lock, so one grow per node at a time is
 * enforced by a flag and a condvar rather than by holding the lock: a second
 * thread waits, then re-checks the table and either finds the winner's slab
 * or grows in turn.
 *
 * Placement is a preference, never a reason to refuse memory that exists: a
 * node whose free pages cannot take even a base slab, or whose placed
 * mapping fails at the floor, is given a base slab placed by the kernel
 * instead, so the arena path cannot fail an allocation for want of memory on
 * one node. */
static bool regpool_grow_node(int node, size_t seen_slabs, bool carve) {
  pthread_mutex_lock(&g_lock);
  for (;;) {
    if (!g_inited) {
      pthread_mutex_unlock(&g_lock);
      return false;
    }
    if (atomic_load_explicit(&g_slab_count, memory_order_acquire) !=
        seen_slabs) {
      pthread_mutex_unlock(&g_lock);
      return true;
    }
    if (carve && atomic_load_explicit(&g_node_arena[node],
                                      memory_order_acquire) != NULL) {
      pthread_mutex_unlock(&g_lock);
      return true;
    }
    if (!g_node_growing[node])
      break;
    uint64_t w0 = regpool_now_ns();
    pthread_cond_wait(&g_grow_cv, &g_lock);
    g_node_wait_ns[node] += regpool_now_ns() - w0;
    g_node_wait_count[node]++;
  }
  g_node_growing[node] = true;
  uint64_t g0 = regpool_now_ns();
  size_t floor_bytes = g_slab_bytes;
  size_t want = regpool_next_slab_locked(node);
  pthread_mutex_unlock(&g_lock);

  /* Clamp to what the machine has right now: an overcommitting kernel
   * happily grants a mapping far beyond physical memory, and this slab is
   * populated in full at creation. */
  size_t mach_avail = regpool_mem_available();
  while (mach_avail != 0 && want > mach_avail && want > floor_bytes)
    want = regpool_step_down(want, floor_bytes);

  /* A slab carved FOR a node should land there, so its size is also clamped
   * to what the node itself can absorb (headroom held back for concurrent
   * consumers).  A node that cannot take even a base slab is not refused: it
   * gets an unplaced base slab below, and is warned about once per
   * exhaustion (cleared when a placed slab lands on it again). */
  size_t node_avail = regpool_node_avail_bytes(node);
  bool fits_node = true;
  if (node_avail != SIZE_MAX) {
    size_t usable = node_avail > REGPOOL_NODE_HEADROOM
                        ? node_avail - REGPOOL_NODE_HEADROOM
                        : 0;
    while (want > usable && want > floor_bytes)
      want = regpool_step_down(want, floor_bytes);
    fits_node = want <= usable;
  }

  void *base = NULL;
  struct fid_mr *mr = NULL;
  uint64_t rkey = 0;
  bool mapped = false;
  int placed = node;
  while (fits_node && !mapped) {
    mapped = regpool_map_slab(node, /*set=*/0, want, REGPOOL_BASE_ALIGN, &base,
                              &mr, &rkey);
    if (!mapped) {
      if (want <= floor_bytes)
        break;
      want = regpool_step_down(want, floor_bytes);
    }
  }
  if (!mapped) {
    want = floor_bytes;
    placed = -1;
    mapped = regpool_map_slab(-1, /*set=*/0, want, REGPOOL_BASE_ALIGN, &base,
                              &mr, &rkey);
  }

  pthread_mutex_lock(&g_lock);
  g_node_grow_ns[node] += regpool_now_ns() - g0;
  if (mapped && !g_inited) {
    /* Torn down while the mapping was in flight.  Teardown is single-
     * threaded by contract, so this is a violated contract rather than a
     * race the protocol handles — but the mapping must not be published
     * into a pool that no longer exists. */
    if (mr != NULL)
      fi_close(&mr->fid);
    munmap(base, want);
    g_node_growing[node] = false;
    pthread_cond_broadcast(&g_grow_cv);
    pthread_mutex_unlock(&g_lock);
    return false;
  }
  if (mapped) {
    /* Hand the pinned range to an exclusive arena.  is_committed=true (the
     * mapping is accessible — committed by mmap and already populated),
     * is_pinned=true (the arena must never decommit/purge/reset a registered
     * range), exclusive=true (only heaps created for this arena draw from
     * it — the confinement mechanism), is_zero=true (a fresh anonymous
     * mapping is kernel-zeroed and nothing between map and manage writes
     * into it: registration only pins, placement only sets policy), which
     * lets the allocator's zeroed-allocation path clear only recycled
     * blocks.
     *
     * No retry on refusal: the range is granule-sized and at most one
     * slot's worth, so nothing about it can be "too big" — the only refusal
     * left is an exhausted global arena table, which no smaller size cures.
     * That is the pool's genuine end of reach, reported loudly here and
     * fatally at the allocation that finds every node unable to grow. */
    mi_arena_id_t arena = NULL;
    if (!mi_manage_os_memory_ex(base, want, /*is_committed=*/true,
                                /*is_pinned=*/true, /*is_zero=*/true, placed,
                                /*exclusive=*/true, &arena)) {
      ARTS_WARN("regpool: allocator refused a %zu MiB slab — global arena "
                "table exhausted; no further growth is possible at any size",
                want >> 20);
      if (mr != NULL)
        fi_close(&mr->fid);
      munmap(base, want);
      mapped = false;
    } else if (regpool_append(base, want, mr, rkey, placed,
                              /*interleaved=*/false, false, arena,
                              REGPOOL_BASE_ALIGN) == NULL) {
      if (mr != NULL)
        fi_close(&mr->fid);
      /* Arena metadata now references this range; the OS reclaims it at
       * exit. */
      mapped = false;
    } else {
      g_node_last_slab[node] = want;
      g_node_grow_count[node]++;
      if (placed == node)
        g_node_full_warned[node] = false;
      atomic_store_explicit(&g_node_state[node], REGPOOL_NODE_CARVED,
                            memory_order_release);
      atomic_store_explicit(&g_node_arena[node], arena, memory_order_release);
    }
  }
  if (!mapped &&
      atomic_load_explicit(&g_node_arena[node], memory_order_relaxed) ==
          NULL) {
    atomic_store_explicit(&g_node_state[node], REGPOOL_NODE_REFUSED,
                          memory_order_release);
  }
  if (!fits_node && !g_node_full_warned[node]) {
    g_node_full_warned[node] = true;
    ARTS_WARN("regpool: node %d has %zu MiB free — no room for a %zu MiB "
              "slab there; its slab is placed by the kernel instead",
              node, node_avail >> 20, floor_bytes >> 20);
  }
  g_node_growing[node] = false;
  pthread_cond_broadcast(&g_grow_cv);
  pthread_mutex_unlock(&g_lock);
  return mapped;
}

/* Is `a` a worse candidate than `b` for a mapping requested from `from`?
 * Three keys, in order: a node outside this rank's set comes after every one
 * inside it (a mapping the rank's own memory controllers cannot serve is the
 * last resort, however near it looks); then distance, because a farther node
 * costs every access for the mapping's whole life; then free memory, which
 * only breaks ties between equals.  Ascending node index — the order this
 * replaces — encodes none of that. */
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
    if (av != SIZE_MAX &&
        (av <= REGPOOL_NODE_HEADROOM || av - REGPOOL_NODE_HEADROOM < len))
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
 * vetoes (the whole set is offered then). */
static uint64_t regpool_interleave_set(size_t len) {
  uint64_t set = g_node_set;
  if (set == 0 || g_numa_nodes <= 1)
    return set;
  uint64_t usable_set = 0;
  size_t sum = 0;
  for (unsigned i = 0; i < g_numa_nodes && i < REGPOOL_MAX_NODES; i++) {
    if ((set & (1ULL << i)) == 0)
      continue;
    size_t av = regpool_node_avail_bytes((int)i);
    if (av == SIZE_MAX)
      return set;
    size_t usable =
        (av > REGPOOL_NODE_HEADROOM) ? av - REGPOOL_NODE_HEADROOM : 0;
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
static void *regpool_alloc_direct(size_t size, size_t align, int node,
                                  bool zero) {
  size_t a = align > REGPOOL_BASE_ALIGN ? align : REGPOOL_BASE_ALIGN;
  size_t len = align_up_sz(size, a);
  void *base = NULL;
  struct fid_mr *mr = NULL;
  uint64_t rkey = 0;

  pthread_mutex_lock(&g_lock);
  regpool_slab_t *reused = regpool_claim_retired_locked(len, a, node);
  pthread_mutex_unlock(&g_lock);
  if (reused != NULL) {
    /* The block is the caller's now, and a retired mapping holds whatever
     * its previous owner wrote — where a fresh mapping is kernel-zeroed. */
    if (zero)
      memset(reused->mr.base, 0, size);
    return reused->mr.base;
  }

  int used_node = node;
  bool interleaved = false;
  bool mapped = regpool_map_placed(len, a, node, &used_node, &interleaved,
                                   &base, &mr, &rkey);
  if (!mapped && regpool_evict_retired(len) != 0) {
    /* Retired mappings hold memory no allocation owns: give it back before
     * concluding that nowhere can place this one. */
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
  regpool_slab_t *s = regpool_publish_direct_locked(
      base, len, mr, rkey, used_node, interleaved, NULL, a);
  if (s == NULL) {
    /* The table is full, and every retired entry is holding a slot no
     * allocation owns. */
    regpool_evict_retired_locked(SIZE_MAX);
    s = regpool_publish_direct_locked(base, len, mr, rkey, used_node,
                                      interleaved, NULL, a);
  }
  pthread_mutex_unlock(&g_lock);
  if (s == NULL) {
    if (mr != NULL)
      fi_close(&mr->fid);
    munmap(base, len);
    return NULL;
  }
  /* base is aligned to `a` >= requested align, so it satisfies the request. */
  return base;
}

/* Re-bind the calling thread's heap to `arena` via the per-thread heap
 * cache: switch to the arena's cached heap, creating it on first use.  See
 * the cache's comment for why heaps are never deleted mid-run. */
static bool regpool_thread_bind(mi_arena_id_t arena, int node) {
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
      return false;
    if (t_heap_cache_count < REGPOOL_THREAD_HEAP_SLOTS) {
      t_heap_cache[t_heap_cache_count].arena = arena;
      t_heap_cache[t_heap_cache_count].heap = h;
      t_heap_cache_count++;
    }
    /* Cache overflow leaves the heap uncached but live: correctness is
     * unaffected, a re-bind simply creates another heap. */
  }
  t_heap = h;
  t_arena = arena;
  t_node = node;
  return true;
}

/* Sweep the existing arena slabs newest-first (the newest is the least
 * likely to be fully consumed), re-binding the thread's heap to each
 * candidate and attempting the allocation.  `node_filter` < 0 admits every
 * node's arenas — the locality-fallback pass; NUMA placement is a
 * preference, never a reason to refuse memory that exists.  Skips the arena
 * that already refused this request. */
static void *regpool_sweep_arenas(size_t size, size_t align, bool zero,
                                  int node_filter, mi_arena_id_t refused) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  for (size_t i = n; i-- > 0;) {
    regpool_slab_t *s = &g_slabs[i];
    /* An arena entry is written once and never rewritten, so the state
     * check alone makes its fields safe to read. */
    if (atomic_load_explicit(&s->state, memory_order_acquire) !=
        REGPOOL_SLOT_LIVE)
      continue;
    if (s->is_direct)
      continue;
    if (s->arena == NULL || s->arena == refused)
      continue;
    if (node_filter >= 0 && s->mr.numa_node != node_filter)
      continue;
    if (!regpool_thread_bind(s->arena, (int)s->mr.numa_node))
      return NULL;
    void *p = zero ? mi_heap_zalloc_aligned(t_heap, size, align)
                   : mi_heap_malloc_aligned(t_heap, size, align);
    if (p != NULL)
      return p;
  }
  return NULL;
}

/* Exhaustion slow path.  A heap can only draw from the single arena it is
 * bound to, so recovery is a re-binding cascade: (1) sweep this node's
 * existing arenas (space freed into an earlier arena is reachable only
 * through a heap bound to it), (2) grow this node, (3) drop the locality
 * preference — sweep every node's arenas, then grow any other node.  Loops
 * until the allocation succeeds or every node's grow fails at the
 * map/registration level; only that is genuine exhaustion.  A transient
 * miss (a peer raced away a fresh slab) re-enters the cascade.
 * `refused` names the one arena that already failed this request (NULL
 * when none was tried — entry from a node with no arena of its own), so
 * the sweeps skip exactly the arena known to be exhausted and no other. */
static void *regpool_alloc_arena_slow(size_t size, size_t align, bool zero,
                                      int node, mi_arena_id_t refused) {
  for (;;) {
    size_t seen = atomic_load_explicit(&g_slab_count, memory_order_acquire);
    void *p = regpool_sweep_arenas(size, align, zero, node, refused);
    if (p != NULL)
      return p;

    if (regpool_grow_node(node, seen, /*carve=*/false)) {
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
      if (!regpool_thread_bind(cur, node)) {
        ARTS_WARN("regpool: heap re-bind failed for node %d", node);
        return NULL;
      }
      p = zero ? mi_heap_zalloc_aligned(t_heap, size, align)
               : mi_heap_malloc_aligned(t_heap, size, align);
      if (p != NULL)
        return p;
      continue; /* raced away — re-enter the cascade */
    }

    /* This node cannot grow: fall back across nodes before failing. */
    p = regpool_sweep_arenas(size, align, zero, -1, refused);
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
       * so a forced relocation costs the least access latency available. */
      grew = regpool_grow_node(
          order[o], atomic_load_explicit(&g_slab_count, memory_order_acquire),
          /*carve=*/false);
    }
    if (!grew) {
      ARTS_WARN("regpool: no node can grow (%zu-byte alloc, node %d)", size,
                node);
      return NULL;
    }
    /* The grown node's fresh slab is found by the next sweep pass. */
  }
}

/* Arena path: allocate from the calling thread's heap; on exhaustion enter
 * the re-binding cascade above.  A node whose memory has not been carved yet
 * carves it here, on its first allocation.  A node with no arena that the
 * pool already tried and could not carve is served from a memoized fallback
 * node's arena on this same fast path — the node's own slot is re-checked
 * every call, so a later successful grow reclaims its threads
 * automatically. */
static void *regpool_alloc_arena(size_t size, size_t align, bool zero,
                                 int node) {
  mi_arena_id_t cur = atomic_load_explicit(&g_node_arena[node],
                                           memory_order_acquire);
  int home = node;
  if (cur == NULL && atomic_load_explicit(&g_node_state[node],
                                          memory_order_acquire) ==
                         REGPOOL_NODE_UNTRIED) {
    if (regpool_grow_node(node,
                          atomic_load_explicit(&g_slab_count,
                                               memory_order_acquire),
                          /*carve=*/true)) {
      cur = atomic_load_explicit(&g_node_arena[node], memory_order_acquire);
    }
  }
  if (cur == NULL) {
    int fb = atomic_load_explicit(&g_node_fallback[node],
                                  memory_order_acquire);
    if (fb >= 0) {
      cur = atomic_load_explicit(&g_node_arena[fb], memory_order_acquire);
      home = fb;
    }
    if (cur == NULL) {
      for (unsigned o = 0; o < g_numa_nodes; o++) {
        mi_arena_id_t a =
            atomic_load_explicit(&g_node_arena[o], memory_order_acquire);
        if (a != NULL) {
          cur = a;
          home = (int)o;
          atomic_store_explicit(&g_node_fallback[node], (int)o,
                                memory_order_release);
          break;
        }
      }
    }
    if (cur == NULL) {
      /* No node has an arena yet: let the cascade try to grow this one
       * (nothing was tried, so nothing is refused). */
      return regpool_alloc_arena_slow(size, align, zero, node, NULL);
    }
  }
  if (t_heap == NULL || t_arena != cur || t_node != home) {
    if (!regpool_thread_bind(cur, home))
      return NULL;
  }

  void *p = zero ? mi_heap_zalloc_aligned(t_heap, size, align)
                 : mi_heap_malloc_aligned(t_heap, size, align);
  if (p != NULL)
    return p;
  /* The preferred node (not the fallback) leads the cascade, so a starved
   * node is re-probed for growth exactly when serving capacity runs out.
   * A memo that just failed to serve is dropped first: the cascade may
   * settle on a different node, and a dead memo would otherwise re-route
   * every later allocation through this slow path. */
  if (home != node) {
    atomic_store_explicit(&g_node_fallback[node], -1, memory_order_release);
  }
  return regpool_alloc_arena_slow(size, align, zero, node, t_arena);
}

/* ------------------------------------------------------------------------- */
/* public API                                                                  */
/* ------------------------------------------------------------------------- */

bool arts_regpool_init(struct fid_domain *domain_or_null,
                       struct fid_ep *ep_or_null, size_t slab_bytes,
                       unsigned int numa_nodes) {
  pthread_mutex_lock(&g_lock);
  if (g_inited) {
    pthread_mutex_unlock(&g_lock);
    return false;
  }

  /* The pool drives arena growth off NULL returns from exhausted exclusive
   * arenas — an intended control-flow signal, not a failure.  The allocator
   * would otherwise print each exhaustion as an "out of memory" diagnostic.
   * This option gates the allocator's diagnostic messages generally (error
   * and warning prints in debug builds), not just the exhaustion one;
   * disabling it silences all of them, but it is purely a print gate — it
   * does not affect the allocator's safety aborts on genuine metadata
   * corruption, which are unconditional. */
  mi_option_disable(mi_option_show_errors);

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
    atomic_store_explicit(&g_node_fallback[i], -1, memory_order_relaxed);
    atomic_store_explicit(&g_node_state[i], REGPOOL_NODE_UNTRIED,
                          memory_order_relaxed);
    g_node_last_slab[i] = 0;
    g_node_grow_count[i] = 0;
    g_node_full_warned[i] = false;
    g_node_growing[i] = false;
  }
  g_report_enabled = getenv("ARTS_REGPOOL_REPORT") != NULL;
  {
    uint64_t forced = 0;
    const char *ff = getenv("ARTS_REGPOOL_FORCE_FULL_NODES");
    if (ff != NULL && ff[0] != '\0') {
      const char *p = ff;
      while (*p != '\0') {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
          /* A diagnostic knob's whole value is determinism: a silently
           * dropped token would make its absence look like a pass. */
          ARTS_WARN("regpool: unparsable node list token ignored: \"%s\"", p);
          break;
        }
        if (v >= 0 && v < (long)REGPOOL_MAX_NODES) {
          forced |= 1ULL << (unsigned)v;
        } else {
          ARTS_WARN("regpool: node %ld out of range in force-full list", v);
        }
        if (*end != ',')
          break;
        p = end + 1;
      }
      if (forced != 0)
        ARTS_WARN("regpool: diagnostic override — nodes mask 0x%llx treated "
                  "as full",
                  (unsigned long long)forced);
    }
    regpool_store_forced_full_locked(forced);
  }
  g_inited = true;
  pthread_mutex_unlock(&g_lock);

  /* Carving is lazy: a node's memory is mapped when a thread on it first
   * allocates, so nothing here maps for a node that may never be used.  What
   * init still owes is proof that the pool can map at all — so the
   * initializing thread's own node is carved now and, if that node cannot be
   * carved, the others in index order until one can.  ZERO carved nodes is
   * an init failure, loudly. */
  int first = regpool_current_node();
  if ((unsigned)first >= numa_nodes)
    first = 0;
  bool any = regpool_grow_node(
      first, atomic_load_explicit(&g_slab_count, memory_order_acquire),
      /*carve=*/true);
  for (unsigned i = 0; i < numa_nodes && !any; i++) {
    if ((int)i == first)
      continue;
    any = regpool_grow_node(
        (int)i, atomic_load_explicit(&g_slab_count, memory_order_acquire),
        /*carve=*/true);
  }

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
      if (atomic_load_explicit(&s->state, memory_order_relaxed) !=
          REGPOOL_SLOT_LIVE)
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
            "grow_ms=%llu waits=%u wait_ms=%llu\n",
            node,
            st == REGPOOL_NODE_CARVED
                ? "carved"
                : (st == REGPOOL_NODE_REFUSED ? "refused" : "untried"),
            arenas, mapped >> 20, g_node_grow_count[node],
            (unsigned long long)(g_node_grow_ns[node] / 1000000ull),
            g_node_wait_count[node],
            (unsigned long long)(g_node_wait_ns[node] / 1000000ull));
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
    uint8_t st = atomic_load_explicit(&s->state, memory_order_relaxed);
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
          "[REGPOOL] total: slabs=%zu/%u mapped=%zu unplaced=%zu base=%zu\n",
          n, REGPOOL_MAX_SLABS, total >> 20, unplaced >> 20,
          g_slab_bytes >> 20);
}

void arts_regpool_report(FILE *out) {
  if (out == NULL)
    return;
  pthread_mutex_lock(&g_lock);
  regpool_report_locked(out);
  pthread_mutex_unlock(&g_lock);
  fflush(out);
}

void arts_regpool_set_forced_full(uint64_t node_mask) {
  pthread_mutex_lock(&g_lock);
  regpool_store_forced_full_locked(node_mask);
  pthread_mutex_unlock(&g_lock);
}

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
  if (node_count > REGPOOL_MAX_NODES)
    node_count = REGPOOL_MAX_NODES;
  g_req_node_count = node_count;
  g_req_node_set = node_set;
  g_distance_known = false;
  if (distance != NULL && node_count > 0) {
    /* Copied, not referenced: the table outlives whatever the caller built
     * it in, and nothing here may follow a pointer into a freed topology. */
    memcpy(g_node_distance, distance,
           (size_t)node_count * node_count * sizeof(*g_node_distance));
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
    /* A free slot was already closed and unmapped by the eviction that
     * released it; its mr/base/len are stale and must not be touched
     * again.  A retired one is still mapped and registered. */
    if (atomic_load_explicit(&s->state, memory_order_relaxed) ==
        REGPOOL_SLOT_FREE)
      continue;
    if (s->mr.mr != NULL)
      fi_close(&s->mr.mr->fid);
    /* Direct slabs are the pool's own mappings and are unmapped here.  Arena
     * slabs are owned by the allocator's arena registry; the vendored allocator
     * exposes no public arena-unload, so unmapping one out from under it would
     * dangle its metadata.  The OS reclaims those ranges at process exit — the
     * pool's lifetime is the process lifetime. */
    if (s->is_direct)
      munmap(s->mr.base, s->mr.len);
    atomic_store_explicit(&s->state, REGPOOL_SLOT_FREE, memory_order_release);
  }
  atomic_store_explicit(&g_slab_count, 0, memory_order_release);
  for (unsigned i = 0; i < REGPOOL_MAX_NODES; i++) {
    atomic_store_explicit(&g_node_arena[i], NULL, memory_order_release);
    atomic_store_explicit(&g_node_fallback[i], -1, memory_order_release);
    atomic_store_explicit(&g_node_state[i], REGPOOL_NODE_UNTRIED,
                          memory_order_release);
    g_node_last_slab[i] = 0;
    g_node_grow_count[i] = 0;
    g_node_full_warned[i] = false;
    g_node_growing[i] = false;
  }
  regpool_store_forced_full_locked(0);
  g_report_enabled = false;
  g_domain = NULL;
  g_ep = NULL;
  g_slab_bytes = 0;
  g_numa_nodes = 0;
  g_node_set = 0;
  g_req_node_set = 0;
  g_req_node_count = 0;
  g_distance_known = false;
  g_inited = false;
  pthread_mutex_unlock(&g_lock);
}

void arts_regpool_unregister(void) {
  pthread_mutex_lock(&g_lock);
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  size_t closed = 0;
  for (size_t i = 0; i < n; i++) {
    regpool_slab_t *s = &g_slabs[i];
    /* A retired mapping is still registered — only a free slot has nothing
     * left to close. */
    if (atomic_load_explicit(&s->state, memory_order_relaxed) ==
        REGPOOL_SLOT_FREE) {
      continue;
    }
    if (s->mr.mr != NULL) {
      int rc = fi_close(&s->mr.mr->fid);
      if (rc != 0) {
        ARTS_WARN("regpool: unregister fi_close(mr) failed: %s",
                  fi_strerror(-rc));
      }
      s->mr.mr = NULL;
      s->mr.rkey = 0;
      closed++;
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
}

bool arts_regpool_grow(int numa_node) {
  if (numa_node < 0 || (unsigned)numa_node >= g_numa_nodes)
    numa_node = 0;
  /* The current table length asks for a slab unconditionally — this entry
   * point is a request to grow, not a reaction to an exhausted arena. */
  return regpool_grow_node(
      numa_node, atomic_load_explicit(&g_slab_count, memory_order_acquire),
      /*carve=*/false);
}

static void *regpool_alloc_common(size_t size, size_t align, bool zero) {
  if (size == 0)
    return NULL;
  size_t slab = g_slab_bytes;
  if (slab == 0)
    return NULL; /* pool not initialized */
  if (align < REGPOOL_ALIGN_FLOOR)
    align = REGPOOL_ALIGN_FLOOR;

  int node = regpool_current_node();
  if ((unsigned)node >= g_numa_nodes)
    node = 0;

  /* A request the arena would serve out of whole chunks, or that no two of
   * which could share a slab, is cheaper in a mapping of its own; the rest
   * draw from the node's arena, which grows on exhaustion. */
  size_t direct_above = slab / 2;
  if (direct_above > REGPOOL_MAX_ARENA_OBJ)
    direct_above = REGPOOL_MAX_ARENA_OBJ;
  void *p = (size > direct_above)
                ? regpool_alloc_direct(size, align, node, zero)
                : regpool_alloc_arena(size, align, zero, node);

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

void *arts_regpool_alloc_aligned(size_t size, size_t align) {
  return regpool_alloc_common(size, align, /*zero=*/false);
}

/* Zeroed variant: the allocator clears only blocks recycled from dirty
 * pages — fresh slab memory is kernel-zeroed and declared so at manage
 * time, so the common create-then-initialize pattern skips a full payload
 * memset (and the page faults it forces) on the caller's critical path. */
void *arts_regpool_zalloc_aligned(size_t size, size_t align) {
  return regpool_alloc_common(size, align, /*zero=*/true);
}

void arts_regpool_free(void *p) {
  if (p == NULL)
    return;
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL)
    return; /* not ours (or already reclaimed) */
  regpool_slab_t *s =
      (regpool_slab_t *)((char *)m - offsetof(regpool_slab_t, mr));
  if (!s->is_direct) {
    mi_free(p);
    return;
  }
  if (p != s->mr.base) {
    /* A direct mapping backs exactly one allocation, handed out at the
     * mapping's base; an interior pointer is not something this pool ever
     * returned, and retiring on it would retire another owner's block. */
    ARTS_ERROR("regpool: free of %p, an interior pointer of the direct "
               "mapping at %p", p, s->mr.base);
  }

  /* Oversize direct slab: its mapping backs exactly this one allocation, so
   * this free leaves the mapping owned by nobody (the same
   * uses-complete-and-transport-quiesced precondition an arena free already
   * relies on).  Retire it rather than unmap it: it stays mapped, placed,
   * populated and registered for the next request of the same shape, and
   * the freed pointer stops resolving because the slot is no longer live. */
  pthread_mutex_lock(&g_lock);
  if (atomic_load_explicit(&s->state, memory_order_relaxed) ==
      REGPOOL_SLOT_LIVE) {
    atomic_store_explicit(&s->state, REGPOOL_SLOT_RETIRED,
                          memory_order_release);
  }
  pthread_mutex_unlock(&g_lock);
}

const arts_regpool_mr_t *arts_regpool_lookup(const void *p) {
  size_t n = atomic_load_explicit(&g_slab_count, memory_order_acquire);
  const char *cp = (const char *)p;
  for (size_t i = 0; i < n; i++) {
    const regpool_slab_t *s = &g_slabs[i];
    const char *base;
    size_t len;
    if (!regpool_slot_live_range(s, &base, &len))
      continue;
    if (cp >= base && cp < base + len)
      return &s->mr;
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
  return true;
}
void arts_regpool_cleanup(void) {}
void arts_regpool_unregister(void) {}
void arts_regpool_report(FILE *out) {
  /* No slabs, no nodes, no direct mappings to describe — say so rather than
   * print a shape this build does not have. */
  if (out != NULL)
    fprintf(out, "[REGPOOL] no arena allocator: pass-through allocation\n");
}
void arts_regpool_set_forced_full(uint64_t node_mask) {
  /* No placement to override in this build. */
  (void)node_mask;
}
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
void *arts_regpool_zalloc_aligned(size_t size, size_t align) {
  void *p = arts_regpool_alloc_aligned(size, align);
  if (p != NULL)
    memset(p, 0, size);
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
