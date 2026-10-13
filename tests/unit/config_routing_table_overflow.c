/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_config_create_routing_table never writes past the table it allocates.
 *
 * The table holds exactly config->nodes rows.  A node list that names more
 * hosts than that (here nodes = 2 and the list "a,b,c", on the remote-launcher
 * path) must stop after the last row instead of writing a third: a sanitizer
 * build reports such a write, and every build checks that the two rows hold
 * the list's first two hosts.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  struct arts_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.master_boot = true; /* SSH parser path */
  cfg.nodes = 2;          /* calloc 2 entries ... */

  /* ... but the list parses to 3 hosts.  Config load refuses a node_count
     that disagrees with the list, so the parser is driven directly. */
  char node_list[] = "a,b,c";

  struct arts_config_s *p = &cfg;
  arts_config_create_routing_table(&p, node_list);

  /* table[2] does not exist, so a write to it is visible only to a sanitizer;
     the rows that do exist must be the list's first hosts, in order. */
  if (cfg.table == NULL) {
    fprintf(stderr, "FAIL routing_table_overflow: table not allocated\n");
    return 1;
  }
  if (cfg.table_length != 2 || cfg.table[0].ip_address == NULL ||
      cfg.table[1].ip_address == NULL ||
      strcmp(cfg.table[0].ip_address, "a") != 0 ||
      strcmp(cfg.table[1].ip_address, "b") != 0) {
    fprintf(stderr, "FAIL routing_table_overflow: rows are not \"a\", \"b\"\n");
    return 1;
  }

  /* Free only the in-bounds (allocated) entries. */
  for (unsigned int i = 0; i < cfg.nodes; i++) {
    if (cfg.table[i].ip_address) {
      arts_free(cfg.table[i].ip_address);
    }
    if (cfg.table[i].ports) {
      arts_free(cfg.table[i].ports);
    }
  }
  arts_free(cfg.table);

  printf("PASS config_routing_table_overflow: no OOB write past nodes (%u)\n",
         cfg.nodes);
  return 0;
}
