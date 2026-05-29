/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/// @file multinode_remote_writer.c
/// @brief Reproducer for the remaining cross-node gap: remote-WRITER ordering.
///
/// A tile owned by node 0 is written EW by an EDT on node 1 (remote to the
/// owner) then read RO by an EDT on node 0 each timestep; the node-0 reader of
/// version t must observe t. This is the DUAL of the (fixed) remote-RO-reader
/// case: here the writer is forwarded + late-pulled, so the local reader can
/// register and run before the remote write lands, observing the pre-write
/// value (reader abort()s). The fix needs the remote writer registered on the
/// owner's frontier in CDAG order AND its update-return completing the frontier
/// progression — the latter is not yet wired, so this reproducer currently
/// fails. Not in the auto-run suite. Timesteps is an optional argv (default 1).

#include "arts.h"
#include <stdlib.h>

#define DEFAULT_TIMESTEPS 1

void rw_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int *tile = (int *)depv[0].ptr;
  if (tile) {
    tile[0] = (int)paramv[0];
  }
}

void rw_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const int *tile = (const int *)depv[0].ptr;
  int expected = (int)paramv[0];
  if (tile && tile[0] == expected) {
    arts_printf("  PASS: reader (node %u) saw version %d\n",
                arts_get_current_node(), expected);
  } else {
    arts_printf("  FAIL: reader expected version %d, saw %d "
                "(remote writer update not propagated/ordered)\n",
                expected, tile ? tile[0] : -1);
    abort();
  }
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;

  int timesteps = DEFAULT_TIMESTEPS;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv && argv[1]) {
      int t = atoi(argv[1]);
      if (t > 0) timesteps = t;
    }
  }

  arts_printf("=== multinode_remote_writer (%d timesteps) ===\n", timesteps);
  if (arts_get_total_nodes() < 2) {
    arts_printf("  SKIP: requires >= 2 nodes\n");
    arts_shutdown();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  void *ptr = NULL;
  arts_guid_t tile =
      arts_db_create(&ptr, sizeof(int), ARTS_DB_DEFAULT, &(arts_hint_t){.route = 0});
  ((int *)ptr)[0] = 0;
  arts_db_release(tile);

  // Per timestep: EW writer on node 1 (remote) writes t, RO reader on node 0
  // (local owner) must observe t.
  for (int t = 1; t <= timesteps; t++) {
    uint64_t val = (uint64_t)t;
    arts_guid_t w = arts_edt_create_with_epoch(rw_writer, 1, &val, 1, epoch,
                                               &(arts_hint_t){.route = 1});
    arts_add_dependence(tile, w, 0, DB_MODE_EW);

    arts_guid_t r = arts_edt_create_with_epoch(rw_reader, 1, &val, 1, epoch,
                                               &(arts_hint_t){.route = 0});
    arts_add_dependence(tile, r, 0, DB_MODE_RO);
  }
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
