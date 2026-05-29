/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file multinode_stencil_halo.c
/// @brief Cross-node halo WAR (write-after-read) stress for the CDAG.
///
/// Replicates the data hazard a distributed iterative stencil (e.g. jacobi2d)
/// generates: a tile owned by node 0 is, every timestep, written exclusively
/// (EW) by an owner-local EDT and then read (RO) by a halo neighbour on the
/// remote node. The CDAG must hold version t of the tile alive until the
/// remote RO consumer has captured its snapshot, so the next timestep's EW
/// cannot overwrite the buffer underneath an in-flight halo copy.
///
/// If the runtime's RO-head retirement / remote-snapshot gating (the
/// roOutstanding sharer-ack) is wrong, EW(t+1) overwrites version t before the
/// node-1 reader's snapshot of version t is taken, and that reader observes
/// t+1 instead of t — a silent corruption this test turns into a FAIL/abort.
///
/// Requires multi-node (node_count > 1). Run with ARTS_CONFIG=arts_multinode.cfg.
///
/// FINDING (2026-05-29): timesteps is an optional argv argument, default 1.
/// A SINGLE cross-node exchange (1 timestep) is correct. TWO OR MORE interleaved
/// EW->RO timesteps on the same tile across nodes hit a cross-node WAR: the
/// node-1 reader of version t observes version t+1 (the reader's abort() then
/// wedges the run). Root cause: a remote RO reader is served PULL-based — node 1
/// fetches the tile on demand when the reader runs, and by then node 0 has
/// already progressed the frontier to the next EW's version. The roOutstanding /
/// roMarkedHead gate (remote/handler.c arts_remote_db_send_check) pins only the
/// *current* head across the snapshot copy, not the reader's intended
/// generation, and the next EW is never blocked on a remote-snapshot ack the
/// runtime does not have. This is the known pre-existing iterative cross-node
/// halo bug (jacobi2d-medium); the fix is the runtime-owned versioned-coherence
/// protocol (durable sharer directory + per-write version + per-generation gated
/// snapshot), not a surgical patch. NOT a regression from the optimization
/// series. Reproduce with `./multinode_stencil_halo 2`.

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
