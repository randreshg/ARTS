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
#include "arts/system/topology.h"

#include "arts/system/placement.h"
#include "arts/system/print.h"

#include <hwloc.h>
#include <hwloc/helper.h>
#include <stdlib.h>
#include <string.h>

unsigned int num_numa_domains = 1;

static const char *role_char(enum arts_thread_role role) {
  switch (role) {
  case ARTS_ROLE_WORKER:
    return "W";
  case ARTS_ROLE_PROGRESS:
    return "P";
  default:
    return "?";
  }
}

void print_mask(struct thread_mask_s *threads, unsigned int num_threads) {
  (void)threads;
  (void)num_threads;
  ARTS_INFO(" Id  Role  GrpPos  NUMA  Pkg  Core    PU  Pin");
  for (unsigned int i = 0; i < num_threads; i++) {
    ARTS_INFO("%3u    %s    %3u     %3u   %3u   %3u   %3u    %1u",
              threads[i].id, role_char(threads[i].role), threads[i].group_pos,
              threads[i].numa_domain_id, threads[i].package_id,
              threads[i].core_id, threads[i].pu_id, threads[i].pin);
  }
}

/* Find the nearest NUMA domain for a PU by checking cpuset membership. */
static unsigned int find_numa_for_pu(hwloc_topology_t topology,
                                     hwloc_obj_t pu_obj) {
  hwloc_obj_t numa = NULL;
  while (
      (numa = hwloc_get_next_obj_by_type(topology, HWLOC_OBJ_NUMANODE, numa))) {
    if (hwloc_bitmap_isset(numa->cpuset, pu_obj->os_index)) {
      return numa->os_index;
    }
  }
  return 0; /* UMA or no NUMA node found */
}

/* Walk up from obj to the nearest ancestor of the given type. */
static hwloc_obj_t ancestor_by_type(hwloc_obj_t obj, hwloc_obj_type_t type) {
  for (hwloc_obj_t cur = obj->parent; cur; cur = cur->parent) {
    if (cur->type == type) {
      return cur;
    }
  }
  return NULL;
}

/* Read the machine's NUMA-node distances out of the loaded topology into the
 * flat row-major table `facts` carries.  hwloc reports the matrix over an
 * arbitrary object order, so every value is placed by the objects' OS
 * indices, never by their position in its array.  A machine that reports no
 * latency matrix leaves distance_known false, which its consumers read as
 * "every node equidistant" rather than as an error: distances only ORDER a
 * fallback, so not knowing them costs an ordering, not a placement. */
static void read_numa_distances(hwloc_topology_t topology,
                                struct arts_numa_facts_s *facts) {
  facts->distance_known = false;
  if (facts->node_count == 0 || facts->node_count > ARTS_MAX_NUMA_NODES) {
    return;
  }
  unsigned nr = 1;
  struct hwloc_distances_s *dist = NULL;
  if (hwloc_distances_get_by_type(topology, HWLOC_OBJ_NUMANODE, &nr, &dist,
                                  HWLOC_DISTANCES_KIND_FROM_OS |
                                      HWLOC_DISTANCES_KIND_MEANS_LATENCY,
                                  0) != 0 ||
      nr == 0 || dist == NULL) {
    return;
  }
  for (unsigned i = 0; i < dist->nbobjs; i++) {
    unsigned from = dist->objs[i]->os_index;
    if (from >= facts->node_count) {
      continue;
    }
    for (unsigned j = 0; j < dist->nbobjs; j++) {
      unsigned to = dist->objs[j]->os_index;
      if (to >= facts->node_count) {
        continue;
      }
      uint64_t v = dist->values[(size_t)i * dist->nbobjs + j];
      facts->distance[(size_t)from * facts->node_count + to] =
          (v > UINT32_MAX) ? UINT32_MAX : (uint32_t)v;
    }
  }
  facts->distance_known = true;
  hwloc_distances_release(topology, dist);
}

void get_thread_mask(struct arts_config_s *config, struct thread_mask_s *flat,
                     struct arts_numa_facts_s *facts) {
  /* Init hwloc topology */
  hwloc_topology_t topology;
  if (hwloc_topology_init(&topology) < 0) {
    ARTS_ERROR("hwloc_topology_init() failed");
  }
  if (hwloc_topology_load(topology) < 0) {
    ARTS_ERROR("hwloc_topology_load() failed");
  }

  /* Oversubscription check (accounts for PU offset in local multi-node) */
  unsigned int total_pus = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
  if (total_pus == 0) {
    ARTS_ERROR("hwloc detected 0 PUs — cannot assign thread topology");
    return;
  }
  unsigned int pu_offset = 0;
  if (config->shared_pu_pool) {
    pu_offset = config->my_rank * config->thread_count;
    ARTS_INFO("Local multi-node rank %u: PU offset %u (threads %u)",
              config->my_rank, pu_offset, config->thread_count);
  }
  if (pu_offset + config->thread_count > total_pus) {
    ARTS_ERROR("Rank %u: PU range [%u..%u) exceeds available PUs (%u)",
               config->my_rank, pu_offset, pu_offset + config->thread_count,
               total_pus);
    return;
  }

  /* Collect the machine's PUs; the placement itself is the pure core in
     placement.c (unit-tested against synthetic topologies). */
  struct arts_pu_desc_s *pus = malloc(total_pus * sizeof(*pus));
  for (unsigned int i = 0; i < total_pus; i++) {
    hwloc_obj_t pu = hwloc_get_obj_by_type(topology, HWLOC_OBJ_PU, i);
    hwloc_obj_t core = ancestor_by_type(pu, HWLOC_OBJ_CORE);
    hwloc_obj_t pkg = ancestor_by_type(pu, HWLOC_OBJ_PACKAGE);
    pus[i].pu_os_index = pu->os_index;
    pus[i].core_os_index = core ? core->os_index : 0;
    pus[i].pkg_os_index = pkg ? pkg->os_index : 0;
    pus[i].numa_id = find_numa_for_pu(topology, pu);
  }

  struct arts_placement_s *placed =
      malloc(config->thread_count * sizeof(*placed));
  if (!arts_placement_compute(pus, total_pus, pu_offset,
                              config->worker_thread_count,
                              config->progress_thread_count, placed)) {
    ARTS_ERROR("Rank %u: thread placement failed (%u threads, offset %u, "
               "%u PUs)",
               config->my_rank, config->thread_count, pu_offset, total_pus);
  }

  for (unsigned int t = 0; t < config->thread_count; t++) {
    flat[t].id = t;
    flat[t].pu_id = placed[t].pu_os_index;
    flat[t].core_id = placed[t].core_os_index;
    flat[t].package_id = placed[t].pkg_os_index;
    flat[t].numa_domain_id = placed[t].numa_id;
    flat[t].role = placed[t].role;
    flat[t].group_pos = placed[t].group_pos;
    flat[t].pin = config->pin_threads;
  }

  /* NUMA domain count for public API */
  unsigned int mem = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);
  num_numa_domains = mem ? mem : 1;

  if (facts != NULL) {
    memset(facts, 0, sizeof(*facts));
    facts->node_count = (num_numa_domains > ARTS_MAX_NUMA_NODES)
                            ? ARTS_MAX_NUMA_NODES
                            : num_numa_domains;
    /* The rank's nodes are exactly the ones its placed threads sit on: the
     * slice is contiguous in the base order, but which nodes that spans is a
     * property of the machine, so it is read off the placement rather than
     * derived from the slice's bounds. */
    for (unsigned int t = 0; t < config->thread_count; t++) {
      unsigned int nid = placed[t].numa_id;
      if (nid < facts->node_count) {
        facts->rank_nodes |= (uint64_t)1 << nid;
      }
    }
    read_numa_distances(topology, facts);
  }

  free(placed);
  free(pus);

  hwloc_topology_destroy(topology);
}
