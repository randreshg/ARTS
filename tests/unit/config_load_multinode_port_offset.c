/* SPDX-License-Identifier: Apache-2.0
 *
 * Local multi-node port auto-offset discipline (config_compute_derived +
 * config_setup_local).
 *
 * Every rank of a `launcher=local` run with node_count > 1 shares the
 * loopback interface, so the loader must:
 *   - mark config->shared_pu_pool == true (all nodes share PUs on 127.0.0.1),
 *   - force every table[i].ip_address to "127.0.0.1",
 *   - and give each node a DISJOINT port block:
 *     table[i].ports[j] == ports[j] + i*port_count.
 * A regression here makes every local multinode run collide on the same TCP
 * ports and flake.
 *
 * Registered multinode for 2n_9w_1p/4n_4w_1p/2n_8w_2p; each variant's
 * ARTS_CONFIG points at configs/local/test/<variant>.cfg, which names no
 * ports, so the loader chooses the base block itself.  The test does NOT
 * start the runtime — it calls arts_config_load() directly and inspects the
 * resolved table, so it binds no ports and never oversubscribes.  config.c is
 * protocol-agnostic, so this runs meaningfully in every coherence build.  The
 * table offset is checked against whatever base config->ports holds after
 * load, so the assertion is independent of the base chosen.
 */

#include "arts.h"
#include "arts/system/config.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  struct arts_config_s config;
  arts_config_load(&config);

  /* Single-node cfg (or a build that resolved to 1 node) cannot exercise the
     offset path — skip cleanly rather than false-fail. */
  if (config.table_length <= 1) {
    printf("SKIP config_load_multinode_port_offset: single node "
           "(table_length=%u)\n",
           config.table_length);
    arts_config_destroy(&config);
    return 0;
  }

  int ok = 1;

  if (!config.shared_pu_pool) {
    printf("FAIL config_load_multinode_port_offset: shared_pu_pool not set for "
           "local multi-node\n");
    ok = 0;
  }
  if (config.ports == NULL || config.ports_count == 0) {
    printf(
        "FAIL config_load_multinode_port_offset: no ports resolved\n");
    arts_config_destroy(&config);
    return 1;
  }
  if (config.port_count != config.ports_count) {
    printf("FAIL config_load_multinode_port_offset: port_count=%u != "
           "ports_count=%u\n",
           config.port_count, config.ports_count);
    ok = 0;
  }
  if (config.table == NULL) {
    printf("FAIL config_load_multinode_port_offset: routing table is NULL\n");
    arts_config_destroy(&config);
    return 1;
  }

  for (unsigned int i = 0; i < config.table_length; i++) {
    if (config.table[i].ip_address == NULL ||
        strcmp(config.table[i].ip_address, "127.0.0.1") != 0) {
      printf("FAIL config_load_multinode_port_offset: node %u ip=%s expected "
             "127.0.0.1\n",
             i,
             config.table[i].ip_address ? config.table[i].ip_address
                                        : "(null)");
      ok = 0;
      continue;
    }
    if (config.table[i].ports == NULL) {
      printf("FAIL config_load_multinode_port_offset: node %u has no ports\n",
             i);
      ok = 0;
      continue;
    }
    for (unsigned int j = 0; j < config.port_count; j++) {
      unsigned int expected = config.ports[j] + (i * config.port_count);
      if (config.table[i].ports[j] != expected) {
        printf("FAIL config_load_multinode_port_offset: node %u port[%u]=%u "
               "expected %u (base=%u + %u*%u)\n",
               i, j, config.table[i].ports[j], expected,
               config.ports[j], i, config.port_count);
        ok = 0;
      }
    }
  }

  /* Cross-node disjointness: no two nodes may share a port (the whole point of
     the offset).  Verified implicitly by the formula above (offset =
     i*port_count with port_count contiguous ports per node), but assert the
     first ports are strictly increasing across nodes as a belt-and-suspenders
     check. */
  for (unsigned int i = 1; i < config.table_length; i++) {
    if (config.table[i].ports[0] <= config.table[i - 1].ports[0]) {
      printf("FAIL config_load_multinode_port_offset: node %u base port %u not "
             "above node %u base port %u\n",
             i, config.table[i].ports[0], i - 1, config.table[i - 1].ports[0]);
      ok = 0;
    }
  }

  unsigned int n = config.table_length;
  arts_config_destroy(&config);

  if (ok) {
    printf("PASS config_load_multinode_port_offset %u nodes 127.0.0.1 "
           "offset-disjoint ports\n",
           n);
    return 0;
  }
  return 1;
}
