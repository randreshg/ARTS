/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file multinode_stencil_halo.c
/// @brief Cross-node halo write-after-read stress for the CDAG.
///
/// A tile owned by node 0 is, each timestep, written EW by an owner-local EDT
/// then read RO by a halo neighbour on node 1. The reader of version t must
/// observe version t, so the next EW cannot overwrite the tile until node 1 has
/// captured its snapshot. Timesteps is an optional argv (default 1).
///
/// Requires multi-node. Run with ARTS_CONFIG=arts_multinode.cfg.

#include "arts.h"
#include <stdlib.h>

#define DEFAULT_TIMESTEPS 1

/// EW writer: stamps the tile with this timestep's value.
void halo_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int *tile = (int *)depv[0].ptr;
  if (tile) {
    tile[0] = (int)paramv[0];
  }
}

/// Remote RO halo reader: must observe exactly this timestep's value. Seeing a
/// later timestep's value means the next EW overwrote version t before this
/// snapshot was captured (cross-node WAR hazard).
void halo_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const int *tile = (const int *)depv[0].ptr;
  int expected = (int)paramv[0];
  if (tile && tile[0] == expected) {
    arts_printf("  PASS: halo reader (node %u) saw version %d\n",
                arts_get_current_node(), expected);
  } else {
    arts_printf("  FAIL: halo reader expected version %d, saw %d "
                "(cross-node WAR: next EW overwrote the tile)\n",
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

  // Optional argv[1] = number of timesteps (default 1). >= 2 reproduces the
  // known iterative cross-node halo deadlock (see file header).
  int timesteps = DEFAULT_TIMESTEPS;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv && argv[1]) {
      int t = atoi(argv[1]);
      if (t > 0) timesteps = t;
    }
  }

  arts_printf("=== multinode_stencil_halo (%d timesteps) ===\n", timesteps);

  if (arts_get_total_nodes() < 2) {
    arts_printf("  SKIP: requires >= 2 nodes\n");
    arts_shutdown();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  // Tile owned by node 0.
  void *ptr = NULL;
  arts_guid_t tile =
      arts_db_create(&ptr, sizeof(int), ARTS_DB_DEFAULT, &(arts_hint_t){.route = 0});
  ((int *)ptr)[0] = 0;
  arts_db_release(tile);

  // Build the per-timestep CDAG: EW(node 0, writes t) then RO(node 1, expects t).
  // The frontier serialises these in registration order, so RO(t) reads the
  // generation EW(t) produced, and EW(t+1) follows RO(t).
  for (int t = 1; t <= timesteps; t++) {
    uint64_t val = (uint64_t)t;

    arts_guid_t w = arts_edt_create_with_epoch(halo_writer, 1, &val, 1, epoch,
                                               &(arts_hint_t){.route = 0});
    arts_add_dependence(tile, w, 0, DB_MODE_EW);

    arts_guid_t r = arts_edt_create_with_epoch(halo_reader, 1, &val, 1, epoch,
                                               &(arts_hint_t){.route = 1});
    arts_add_dependence(tile, r, 0, DB_MODE_RO);
  }

  // Completion is driven by the epoch finish-EDT (shut -> arts_shutdown), the
  // proven multinode pattern; main_edt simply returns after wiring the CDAG.
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
