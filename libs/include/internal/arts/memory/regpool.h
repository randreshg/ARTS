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
#ifndef ARTS_MEMORY_REGPOOL_H
#define ARTS_MEMORY_REGPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* FILE — the node-availability parser is stream-pure */

/* Registered slab pool.
 *
 * Payload buffers destined for one-sided RDMA must live in memory that is
 * pinned and pre-registered with the fabric, so the NIC can target it with no
 * per-allocation registration.  The pool carves large slabs, each placed on
 * one NUMA node by preference and populated at creation, pins each behind a
 * memory-registration handle, and hands the slab to an allocator arena; every
 * pointer it returns therefore falls inside exactly one registered slab and
 * can be resolved back to its registration handle and remote key.
 *
 * Confinement is a hard invariant: a pointer that cannot be resolved by
 * arts_regpool_lookup would be an unregistered address the NIC cannot reach,
 * so the pool fails loudly rather than return one.
 *
 * Shape of the memory it holds, which is what a caller sizing a run needs:
 * a node's base slab is carved when a thread on that node first allocates
 * (or another node's demand grows it), the node's next slab is half again
 * the largest one mapped for it, rounded up to the allocator's 32 MiB
 * granule (up to a per-slab ceiling), and every slab is resident from
 * creation — so the pool's resident set is its mapped capacity, not its
 * live payload.  An oversize payload gets a dedicated mapping, which a free
 * retires for reuse rather than unmaps; retired mappings are released when
 * memory or a table slot is wanted.
 *
 * Where a block lands: an arena slab is placed on the node whose threads
 * carved it, so a small payload is served by the memory of the node that
 * allocated it.  An oversize payload is INTERLEAVED across the caller's
 * whole node set instead — one object that every thread sweeps has no one
 * owning node, so spreading it puts every memory controller of the set
 * behind it and makes its placement independent of which thread happened to
 * create it.  Neither is a requirement.  A node with no room for a slab is
 * full, and its threads are served from memory that exists before any is
 * mapped where the kernel chooses: its own arenas, then other nodes'
 * arenas, nearest first, then a slab placed on the nearest node that can
 * take one (the caller's set before any other), then the memory retired
 * oversize mappings hold, and only when none of these serves does the
 * kernel place a slab.  An oversize mapping the set cannot absorb goes to
 * the nearest single node that can, and failing all of them is placed by
 * the kernel.
 *
 * Reach: every arena slab holds one slot of the allocator's process-wide
 * arena table, which never gives a slot back.  Once the table refuses a
 * slab, no arena grows for the rest of the process: an arena-path request
 * is served only from the slabs the pool has, and one none of them can
 * serve is fatal.  An oversize request still gets a mapping of its own,
 * which holds no slot. */

/* Forward declarations of the libfabric object handles keep fabric headers out
 * of every includer of this header.  When the runtime is built without the OFI
 * transport, or the pool is initialized with a NULL domain, these stay opaque
 * and unused: `mr` is NULL and `rkey` is 0. */
struct fid_domain;
struct fid_ep;
struct fid_mr;

/* One registered slab.  A slab backs either an allocator arena (many
 * allocations) or a single oversize direct allocation.  `arts_regpool_lookup`
 * returns a pointer to the record covering a payload pointer; the record
 * exposes the registration handle and remote key that describe the enclosing
 * pinned range. */
typedef struct arts_regpool_mr_s {
  void *base;        /* first byte of the registered range                    */
  size_t len;        /* length of the registered range                        */
  struct fid_mr *mr; /* registration handle; NULL when unregistered           */
  uint64_t rkey;     /* remote key for one-sided RDMA; 0 when unregistered     */
  int numa_node;     /* NUMA node the range is placed on (-1 = unplaced)      */
} arts_regpool_mr_t;

/* Initialize the pool: detect NUMA topology (numa_nodes==0 auto-detects) and
 * carve one base slab for the initializing thread's own node — placing it,
 * registering it against `domain_or_null` and handing it to that node's
 * allocator arena.  Every other node is carved when a thread on it first
 * allocates.  A full node — no room for its slab — is refused with a warning
 * and left without an arena: init carves the nearest node that can take a slab
 * instead, its threads are served from there, and the node joins the pool when
 * a later demand-time grow succeeds.  When no node can, the slab is placed by
 * the kernel.  Only ZERO carved nodes fails the init, which is what proves at
 * init that the pool can map at all.  A NULL domain skips registration
 * (single-node runs / unit tests) while carving arenas identically, so
 * allocation behavior is unchanged.  A non-NULL `ep_or_null` selects the
 * endpoint-bound registration discipline some providers require
 * (FI_MR_ENDPOINT): each slab MR is bound to that endpoint and enabled after
 * registration, and its remote key is read only after the enable — the endpoint
 * must already be enabled, and it must outlive every registered slab (see
 * arts_regpool_unregister).  `slab_bytes` is rounded up to the allocator's
 * minimum arena granularity.  Returns false if already initialized or a slab
 * could not be mapped/registered. */
bool arts_regpool_init(struct fid_domain *domain_or_null,
                       struct fid_ep *ep_or_null, size_t slab_bytes,
                       unsigned int numa_nodes);

/* Hand the pool the machine's NUMA facts, which a caller that has already
 * discovered the topology knows better than the pool can find out by itself.
 * Call BEFORE arts_regpool_init; the values are consumed there.
 *
 *   node_count  NUMA nodes on the machine (0 = let the pool count them).
 *   node_set    the nodes carrying this caller's threads.  An oversize
 *               object is interleaved across the members of this set that
 *               can take a share of it, so it is served by memory
 *               controllers the caller owns; only when the set cannot absorb
 *               it does it go to a single node, nearest first and the
 *               set's own before any other, and then to the kernel's choice.
 *               0 = every node the process may run on (derived from the CPU
 *               affinity mask).
 *   distance    node_count x node_count relative access latencies, row-major
 *               with the requesting node as the row and the smallest value
 *               on the diagonal; copied here.  NULL = every node
 *               equidistant, which leaves free memory the only thing
 *               ordering the fallback.  A pair outside node_count (an init
 *               that counts more nodes) is farther than every pair the
 *               table describes.
 *
 * Cleared by arts_regpool_cleanup: a later init with no fresh call sees only
 * what the pool can discover on its own.  Calling it AFTER init is a fatal
 * error: the node set is consumed at init and every mapping since has been
 * placed against it, so changing it would leave the pool's record of its own
 * placement describing memory it does not have. */
void arts_regpool_set_topology(unsigned int node_count, uint64_t node_set,
                               const uint32_t *distance);

/* The order in which nodes are tried for a mapping requested from `from`:
 * `from` itself first, then the nodes inside the caller's node set before any
 * outside it, nearer before farther, and between equals the one with more
 * free memory.  Writes at most `max` node numbers into `out` and returns how
 * many it wrote.  Exposed so the order itself is testable. */
unsigned int arts_regpool_node_order(int from, int *out, unsigned int max);

/* Release pool bookkeeping and unregister every slab.  Must be called only at
 * teardown with no thread still allocating from the pool. */
void arts_regpool_cleanup(void);

/* Close every live slab registration and detach the pool from the fabric
 * (domain and endpoint references cleared): later allocations still succeed
 * but are no longer fabric-registered, and later cleanup skips the closed
 * handles.  Exists for endpoint-bound registrations, whose MRs hold
 * references the endpoint cannot close under — the transport calls this
 * before closing its endpoint.  Single-threaded teardown only, like
 * arts_regpool_cleanup. */
void arts_regpool_unregister(void);

/* Allocate `size` bytes aligned to at least `align` (floored to the payload
 * alignment invariant) from the calling thread's NUMA-local arena, growing the
 * pool on demand.  An oversize request, or one aligned past the 64 KiB an
 * arena honours, takes a dedicated mapping instead (at least 2 MiB, resident
 * like every slab), reusing a retired one of the same length and an
 * alignment at least as strict when there is one.
 * NUMA placement is a preference throughout: a request is never refused for
 * want of memory on one node.  The returned pointer is guaranteed to resolve
 * through arts_regpool_lookup; a pointer that escaped the registered slabs is
 * a fatal error. */
void *arts_regpool_alloc_aligned(size_t size, size_t align);

/* Return a pointer previously obtained from arts_regpool_alloc_aligned.  The
 * caller must have finished with the memory, transport operations against it
 * included: the pointer stops resolving through arts_regpool_lookup here.
 * An oversize block is freed by the pointer that was returned for it; an
 * interior pointer of such a block is a fatal error. */
void arts_regpool_free(void *p);

/* Resolve `p` to the registered slab that contains it, or NULL if `p` lies in
 * no registered slab (an escaped pointer).  Lock-free; safe to call
 * concurrently with allocation.  The record it returns describes the
 * allocation `p` belongs to and is valid for as long as the caller keeps that
 * allocation alive: a pointer the caller does not own may resolve to a slot
 * that is being reused for another mapping. */
const arts_regpool_mr_t *arts_regpool_lookup(const void *p);

/* Map, register, and publish one additional slab placed on `numa_node`,
 * half again the size of the largest one mapped for that node (a node
 * outside the pool's count means node 0).  Exposed so callers can pre-grow;
 * the allocation path grows on its own, and only it and init fall back to a
 * slab the kernel places.  Returns true when the pool's table grew since the
 * call began — this call's slab, or any entry a concurrent grow or oversize
 * allocation published — and false otherwise: the node is full (its
 * threads are served from other nodes' memory), the mapping failed, or the
 * allocator's arena table has refused a slab, after which no grow maps
 * again. */
bool arts_regpool_grow(int numa_node);

/* Availability estimate over one NUMA node's meminfo stream: the node's free
 * pages.  Under a placement preference a mapping takes a node's free pages
 * and spills past them rather than reclaiming that node's file cache, so the
 * estimate answers how much of a mapping would land on the node, never
 * whether it can be made.  SIZE_MAX when MemFree cannot be read (unknown
 * must not veto placement).  Pure over the stream — exposed for hermetic
 * testing. */
size_t arts_regpool_parse_node_avail(FILE *f);

/* Held back from a node's availability estimate before a mapping is sized
 * against it — room for concurrent consumers, so a slab sized to the
 * estimate still lands where it was placed.  A node whose estimate does not
 * exceed a slab plus this is full: its threads are served from other
 * nodes' memory, and a slab is placed by the kernel only when no node can
 * take one. */
#define ARTS_REGPOOL_NODE_HEADROOM ((size_t)2 * 1024 * 1024 * 1024)

/* Diagnostic override, the programmatic form of the
 * ARTS_REGPOOL_FORCE_FULL_NODES environment list: every node whose bit is
 * set in `node_mask` reports zero availability to later placements, so the
 * refusal, fallover and eviction paths are reachable deterministically
 * without starving a machine.  A mask of 0 clears it.  It acts on an
 * initialized pool: init sets the mask from the environment list, and
 * cleanup clears it.  A diagnostic, never a policy: it changes where memory
 * is placed, never whether an allocation is served (placement is a
 * preference, so a request refused by every node falls back to an unplaced
 * mapping). */
void arts_regpool_set_forced_full(uint64_t node_mask);

/* Print the pool's shape to `out`: per node its carve state, arena count,
 * mapped MiB (of the arena slabs placed on it), grow count (of the slabs grown
 * for it, wherever they landed), the milliseconds its grows spent mapping, the
 * number and milliseconds of waits for a grow in flight (on the node, or for
 * the process-wide turn of a kernel-placed grow) and spills (times its
 * allocations were moved onto another node's arena because it could not grow);
 * the direct mappings live, retired, where they landed (per node, interleaved,
 * or unplaced) and the free slots; and the totals, with the arena-table slots
 * the pool's slabs hold and the slabs the table refused.  Takes the pool lock,
 * so the lines are consistent with each other.  Printed to stderr at cleanup
 * when ARTS_REGPOOL_REPORT is set in the environment, and before the fatal
 * error of an allocation the pool could not satisfy. */
void arts_regpool_report(FILE *out);

#ifdef __cplusplus
}
#endif
#endif /* ARTS_MEMORY_REGPOOL_H */
