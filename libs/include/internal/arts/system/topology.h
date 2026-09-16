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

#ifndef ARTS_SYSTEM_TOPOLOGY_H
#define ARTS_SYSTEM_TOPOLOGY_H
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Abstract Machine Model -- topology discovery and thread-to-PU mapping.
 *
 * Uses hwloc to enumerate PUs in Package -> Core -> PU order,
 * then assigns threads with configurable stride.
 * NUMA is a memory property, not a CPU tree level -- looked up per PU.
 *
 * Output: thread_mask_s[] -- one entry per thread, combining HW topology
 * info and SW thread assignment.  Consumed by arts_runtime_private_init().
 */

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "arts/system/config.h"

/* Widest NUMA node count the facts below describe.  A single-word node mask
 * covers node < 64, which is also the registered pool's own ceiling. */
#define ARTS_MAX_NUMA_NODES 64u

/* What the memory subsystem needs to know about the machine's NUMA
 * topology, discovered alongside the thread placement because the same
 * traversal already answers it:
 *
 *   node_count      NUMA nodes on the machine.
 *   rank_nodes      bitmask of the nodes carrying at least one of THIS
 *                   rank's threads — the set an object no single thread owns
 *                   should be spread across.
 *   distance_known  false when the machine reports no distances, leaving
 *                   every node equidistant.
 *   distance        node_count x node_count relative access latencies,
 *                   row-major with the requesting node as the row (stride
 *                   node_count, smallest value on the diagonal). */
struct arts_numa_facts_s {
  unsigned int node_count;
  uint64_t rank_nodes;
  bool distance_known;
  uint32_t distance[ARTS_MAX_NUMA_NODES * ARTS_MAX_NUMA_NODES];
};

/* Thread roles.  A thread has exactly one role.  After the transport cutover
 * there is no dedicated sender: every producing thread injects its own outbound
 * traffic straight onto the fabric.  A PROGRESS thread reaps fabric completions
 * (dispatching inbound messages) and drains the self-loopback — the sole inbound
 * coherence processor on a multi-node run. */
enum arts_thread_role {
  ARTS_ROLE_WORKER = 0, /* Executes EDTs from work-stealing deque */
  ARTS_ROLE_PROGRESS,   /* Reaps fabric completions + drains self-loopback */
  ARTS_ROLE_MAX
};

/* Per-thread descriptor (output of get_thread_mask).
 * Combines HW topology info + thread assignment.
 * Consumed by arts_runtime_private_init(). */
struct thread_mask_s {
  unsigned int id;             /* sequential 0..total_threads-1 */
  unsigned int pu_id;          /* hwloc PU os_index (== Linux CPU number) */
  unsigned int core_id;        /* hwloc CORE os_index */
  unsigned int package_id;     /* hwloc PACKAGE os_index */
  unsigned int numa_domain_id; /* nearest NUMA domain */
  enum arts_thread_role role;
  unsigned int group_pos; /* 0-based index within role group */
  bool pin;
};

/* Compute this rank's thread placement into `flat` (config->thread_count
 * entries) and, when `facts` is non-NULL, the machine's NUMA facts the same
 * traversal discovers.  Both are read from the machine once, here, so no
 * other module re-discovers the topology for itself. */
void get_thread_mask(struct arts_config_s *config, struct thread_mask_s *flat,
                     struct arts_numa_facts_s *facts);
void print_mask(struct thread_mask_s *threads, unsigned int num_threads);

#ifdef __cplusplus
}
#endif

#endif /* ARTS_SYSTEM_TOPOLOGY_H */
