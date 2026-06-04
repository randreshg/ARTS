/******************************************************************************
** ARTS distributed example: read-only cross-node reduction.
**
** Every node produces a datablock (filled with node_id+1) and signals it, in
** read-only mode, into a collector EDT on node 0. The collector depends on one
** datablock per node, so the runtime must fetch each remote node's datablock to
** node 0 (remote read-only acquire over the transport) before the reduction
** runs. The collector sums all payloads and verifies the total.
**
** This is the RO counterpart of simple_reduction (which uses DB_MODE_EW and
** exposes a pre-existing exclusive-write remote-acquire hang); the read-only
** path is the common distributed-read pattern.
******************************************************************************/
#include "arts.h"

#define DB_INTS 8192u /* 32 KiB per node */

void collector_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t total = 0;
  for (unsigned int d = 0; d < depc; d++) {
    const int *p = (const int *)depv[d].ptr;
    for (unsigned int i = 0; i < DB_INTS; i++) {
      total += (uint64_t)p[i];
    }
  }
  uint64_t expect = paramv[0];
  arts_printf("[dist_reduction] reduced %u remote DBs, total=%lu expected=%lu "
              "-> %s\n",
              depc, total, expect, (total == expect) ? "PASS" : "FAIL");
  arts_shutdown();
}

void producer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t collector = (arts_guid_t)paramv[0];
  unsigned int node = arts_get_current_node();
  int *ptr;
  arts_guid_t db = arts_db_create((void **)&ptr, DB_INTS * sizeof(int),
                                  ARTS_DB_DEFAULT, NULL);
  for (unsigned int i = 0; i < DB_INTS; i++) {
    ptr[i] = (int)(node + 1);
  }
  arts_signal_edt(collector, node, db, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nodes = arts_get_total_nodes();

  uint64_t expect = 0;
  for (unsigned int n = 0; n < nodes; n++) {
    expect += (uint64_t)(n + 1) * DB_INTS;
  }

  arts_guid_t collector = arts_edt_create(collector_edt, 1, &expect, nodes,
                                          &(arts_hint_t){.route = 0});
  arts_printf("[dist_reduction] %u node(s) each contribute a 32 KiB DB (RO) to "
              "collector on node 0\n",
              nodes);
  for (unsigned int n = 0; n < nodes; n++) {
    uint64_t arg = (uint64_t)collector;
    arts_edt_create(producer_edt, 1, &arg, 0, &(arts_hint_t){.route = n});
  }
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
