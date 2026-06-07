/******************************************************************************
** ARTS distributed example: large remote datablock read.
**
** Node 0 creates a 1 MiB datablock filled with a known pattern. A reader EDT is
** placed on node 1 and made dependent on that datablock in read-only mode, so
** the runtime must transfer the whole datablock from node 0 to node 1 — the
** bulk data path of the transport (GASNet AM-chunk / one-sided RMA over RoCE).
** The reader checksums the payload and signals the result back to node 0, which
** verifies it. A successful PASS means the remote bulk transfer is byte-correct.
******************************************************************************/
#include "arts.h"

#define DB_INTS (256u * 1024u) /* 1 MiB datablock: exercises the bulk path */

void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t got = (uint64_t)depv[0].guid; /* checksum via signal_edt_value */
  uint64_t expect = paramv[0];
  arts_printf("[remote_db_read] checksum=%lu expected=%lu -> %s\n", got, expect,
              (got == expect) ? "PASS" : "FAIL");
  arts_shutdown();
}

void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  arts_guid_t done = (arts_guid_t)paramv[0];
  const int *data = (const int *)depv[0].ptr;
  uint64_t sum = 0;
  for (unsigned int i = 0; i < DB_INTS; i++) {
    sum += (uint64_t)data[i];
  }
  arts_printf("[remote_db_read] node %u read %u ints (%lu bytes) from remote "
              "DB, sum=%lu\n",
              arts_get_current_node(), DB_INTS,
              (uint64_t)DB_INTS * sizeof(int), sum);
  arts_signal_edt_value(done, 0, sum);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nodes = arts_get_total_nodes();
  unsigned int target = (nodes > 1) ? 1u : 0u;

  uint64_t expect = 0;
  for (unsigned int i = 0; i < DB_INTS; i++) {
    expect += i;
  }

  arts_guid_t done =
      arts_edt_create(done_edt, 1, &expect, 1, &(arts_hint_t){.route = 0});

  int *ptr;
  arts_guid_t db = arts_db_create((void **)&ptr, (uint64_t)DB_INTS * sizeof(int),
                                  ARTS_DB_DEFAULT, NULL);
  for (unsigned int i = 0; i < DB_INTS; i++) {
    ptr[i] = (int)i;
  }

  uint64_t arg = (uint64_t)done;
  arts_guid_t reader =
      arts_edt_create(reader_edt, 1, &arg, 1, &(arts_hint_t){.route = target});
  arts_printf("[remote_db_read] %u node(s); 1 MiB DB on node 0 -> reader on "
              "node %u\n",
              nodes, target);
  arts_signal_edt(reader, 0, db, DB_MODE_RO);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
