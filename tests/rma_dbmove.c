/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**    https://www.apache.org/licenses/LICENSE-2.0
******************************************************************************/

/// @file rma_dbmove.c
/// @brief GASNet RMA DB-move coverage.
///
/// Single-rank runs check arena residency and opt-out behavior. Two-rank runs
/// check cross-node DB delivery and whether the RMA landing path fired.

#include "arts.h"
#include "arts/memory/db_arena.h"
#include <string.h>

#define RMA_N 8192 /* 32 KiB body, above the default RMA threshold */
#define RMA_SENTINEL 0x5A5A /* writeback marker */
static inline int rma_pattern(unsigned i) { return (int)(i * 2654435761u); }

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

/* Two-rank EW writer on the NON-owner node. The owner-resident DB is delivered
 * here as the writer's private working copy via the full-send path. */
void rma_ew_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  int *tile = (int *)depv[0].ptr;
  bool ok = (tile != NULL);
  for (unsigned i = 0; ok && i < RMA_N; i++) {
    if (tile[i] != rma_pattern(i)) {
      ok = false;
    }
  }
  if (!ok) {
    arts_printf(
        "  FAIL: EW writer (node %u) received wrong DB body (tile=%p)\n",
        arts_get_current_node(), (void *)tile);
    arts_abort(1);
    return;
  }

  uint64_t landed = arts_db_rma_stat_get(ARTS_RMA_STAT_LANDED);
  bool resident = arts_db_arena_owns(tile);
  if (arts_db_rma_enabled() && landed > 0 && resident) {
    arts_printf("  PASS: cross-node 32KiB DB correct via RMA zero-copy "
                "(landed=%lu, segment-resident landing, memcpy skipped)\n",
                (unsigned long)landed);
  } else {
    arts_printf("  PASS: cross-node 32KiB DB correct via Medium-AM fallback "
                "(enabled=%d landed=%lu resident=%d)\n",
                (int)arts_db_rma_enabled(), (unsigned long)landed,
                (int)resident);
  }
  arts_db_rma_stats_dump();

  tile[0] = RMA_SENTINEL; /* mutate; writeback returns to the owner */
}

/* Owner-local RO reader: confirms the remote EW writeback propagated in order.
 */
void rma_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const int *tile = (const int *)depv[0].ptr;
  if (tile && tile[0] == RMA_SENTINEL && tile[1] == rma_pattern(1)) {
    arts_printf("  PASS: owner-local reader saw EW writeback (sentinel + body "
                "intact)\n");
  } else {
    arts_printf("  FAIL: owner-local reader did not see EW writeback "
                "(tile[0]=%d expected=%d)\n",
                tile ? tile[0] : -1, RMA_SENTINEL);
    arts_abort(1);
  }
}

/* Single-rank: exercise the arena storage layer directly. */
static void single_rank_arena_checks(void) {
  bool enabled = arts_db_rma_enabled();
  arts_printf("  rma_enabled=%d min_bytes=%lu transport=%s\n", (int)enabled,
              (unsigned long)arts_db_rma_min_bytes(),
              arts_db_rma_enabled() ? "gasnet-arena" : "heap");

  void *p = NULL;
  arts_guid_t db =
      arts_db_create(&p, RMA_N * sizeof(int), ARTS_DB_DEFAULT, NULL);
  if (p == NULL) {
    arts_printf("  FAIL: local arts_db_create returned NULL\n");
    arts_abort(1);
  }
  for (unsigned i = 0; i < RMA_N; i++) {
    ((int *)p)[i] = rma_pattern(i);
  }

  bool owns = arts_db_arena_owns(p);
  if (enabled && !owns) {
    arts_printf("  FAIL: RMA enabled but DB body is not segment-resident\n");
    arts_abort(1);
  }
  if (!enabled && owns) {
    arts_printf("  FAIL: RMA disabled but DB body claims segment residency\n");
    arts_abort(1);
  }
  /* Content survives the chosen allocator. */
  for (unsigned i = 0; i < RMA_N; i++) {
    if (((int *)p)[i] != rma_pattern(i)) {
      arts_printf("  FAIL: DB content corrupted by arena allocation\n");
      arts_abort(1);
    }
  }
  arts_db_destroy(db);

  if (enabled) {
    arts_printf("  PASS: single-rank DB arena live, body segment-resident, "
                "content intact\n");
  } else {
    arts_printf("  PASS: single-rank arena correctly disabled (heap DBs)\n");
  }
  arts_db_rma_stats_dump();
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== rma_dbmove ===\n");
  unsigned int total = arts_get_total_nodes();

  if (total < 2) {
    single_rank_arena_checks();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  /* Owner-resident DB on node 0, initialized to a known pattern. An EW writer
   * on node 1 (remote to the owner) acquires it as a private working copy —
   * served by full_send_now. An owner-local RO reader then confirms the
   * writeback, exercising cross-node ordering. */
  void *dbp = NULL;
  arts_guid_t db = arts_db_create(&dbp, RMA_N * sizeof(int), ARTS_DB_DEFAULT,
                                  &(arts_hint_t){.route = 0});
  if (dbp == NULL) {
    arts_printf("  FAIL: owner arts_db_create returned NULL on node 0\n");
    arts_abort(1);
  }
  for (unsigned i = 0; i < RMA_N; i++) {
    ((int *)dbp)[i] = rma_pattern(i);
  }
  arts_db_release(db);

  arts_guid_t writer = arts_edt_create_with_epoch(
      rma_ew_writer, 0, NULL, 1, epoch, &(arts_hint_t){.route = 1});
  arts_add_dependence(db, writer, 0, DB_MODE_EW);

  arts_guid_t reader = arts_edt_create_with_epoch(rma_reader, 0, NULL, 1, epoch,
                                                  &(arts_hint_t){.route = 0});
  arts_add_dependence(db, reader, 0, DB_MODE_RO);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
