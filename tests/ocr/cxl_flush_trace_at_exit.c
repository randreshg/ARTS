/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file cxl_flush_trace_at_exit.c
/// @brief A run's end replays the fake CXL library's finalizers, which write
///        its flush trace and release its region.
///
/// main_edt writes a block, a reader acquires it after the writer released
/// it, and the reader ends the run.  The block's turns move through the
/// store, which flushes; the process ends with exit_group, which runs no
/// finalizer, so the trace at $ARTS_FLUSH_LOG and the region's release come
/// from the end's own replay of the library's finalizers.

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc != 1 || depv[0].ptr == NULL ||
      *(const uint64_t *)depv[0].ptr != 0x1234u) {
    arts_printf("  FAIL: the reader did not see the writer's bytes\n");
    arts_test_fail();
  }
  arts_printf("CXL_FLUSH_TRACE: the reader read the block\n");
  arts_shutdown();
}

void writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  *(uint64_t *)depv[0].ptr = 0x1234u;
  arts_guid_t r =
      arts_edt_create(reader, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence((arts_guid_t)paramv[0], r, 0, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *p = NULL;
  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  arts_guid_t db = arts_db_create(&p, sizeof(uint64_t), ARTS_DB,
                                  ARTS_DB_PROP_NO_ACQUIRE, &hint);
  uint64_t g = (uint64_t)db;
  arts_guid_t w =
      arts_edt_create(writer, 1, &g, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(db, w, 0, DB_MODE_RW);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
