/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file db_move_plan.c
/// @brief Unit + negative checks for the lazy DB-move granularity policy.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts/memory/db_move.h"

static int failures = 0;

static bool require_true(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
  return condition;
}

static void expect_plan(bool read_only, bool owner_is_local,
                        bool local_copy_valid, uint64_t offset, uint64_t size,
                        uint64_t payload, arts_db_move_kind_t want_kind,
                        arts_db_move_reason_t want_reason, const char *tag) {
  arts_db_move_plan_t plan =
      arts_db_move_plan(read_only, owner_is_local, local_copy_valid, offset,
                        size, payload);
  require_true(plan.kind == want_kind, tag);
  require_true(plan.reason == want_reason, tag);
}

int main(void) {
  const uint64_t payload = 4096;

  /// Local owner: no move, regardless of mode or footprint.
  expect_plan(true, true, false, 0, 0, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_OWNER_LOCAL, "RO owner-local -> no move");
  expect_plan(false, true, false, 0, 0, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_OWNER_LOCAL, "writer owner-local -> no move");
  expect_plan(true, true, false, payload, 64, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_OWNER_LOCAL,
              "owner-local wins over footprint");
  expect_plan(true, true, true, 0, 0, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_OWNER_LOCAL, "owner-local wins over cache");

  /// Remote read-only footprint: demand-move the footprint only.
  expect_plan(true, false, false, 0, 1024, payload, ARTS_DB_MOVE_SLICE,
              ARTS_DB_MOVE_REASON_FOOTPRINT, "remote RO footprint -> slice");
  expect_plan(true, false, false, 1024, 1024, payload, ARTS_DB_MOVE_SLICE,
              ARTS_DB_MOVE_REASON_FOOTPRINT, "remote RO mid footprint -> slice");
  expect_plan(true, false, false, 0, payload, payload, ARTS_DB_MOVE_SLICE,
              ARTS_DB_MOVE_REASON_FOOTPRINT, "full-cover footprint -> slice");
  expect_plan(true, false, false, payload - 8, 8, payload, ARTS_DB_MOVE_SLICE,
              ARTS_DB_MOVE_REASON_FOOTPRINT, "tail footprint -> slice");

  /// Resident whole-DB copies do not transfer again.
  expect_plan(true, false, true, 0, 1024, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_CACHE_VALID,
              "resident whole-DB copy -> no duplicate move");
  expect_plan(true, false, true, 0, 0, payload, ARTS_DB_MOVE_LOCAL,
              ARTS_DB_MOVE_REASON_CACHE_VALID,
              "resident whole-DB copy, no footprint -> no move");

  /// A partial footprint must differ from a missing footprint.
  {
    arts_db_move_plan_t with_fp =
        arts_db_move_plan(true, false, false, 0, 64, payload);
    arts_db_move_plan_t without_fp =
        arts_db_move_plan(true, false, false, 0, 0, payload);
    require_true(with_fp.kind == ARTS_DB_MOVE_SLICE, "partial footprint -> slice");
    require_true(without_fp.kind == ARTS_DB_MOVE_WHOLE,
                 "same offset, no footprint -> whole (contrast)");
  }

  /// Missing footprint: conservative whole-DB with a reason.
  expect_plan(true, false, false, 0, 0, payload, ARTS_DB_MOVE_WHOLE,
              ARTS_DB_MOVE_REASON_NO_FOOTPRINT, "RO no footprint -> whole");
  expect_plan(true, false, false, 128, 0, payload, ARTS_DB_MOVE_WHOLE,
              ARTS_DB_MOVE_REASON_NO_FOOTPRINT,
              "RO zero-size footprint -> whole");
  expect_plan(false, false, false, 0, 0, payload, ARTS_DB_MOVE_WHOLE,
              ARTS_DB_MOVE_REASON_WRITE_MODE, "writer no footprint -> whole");

  /// Invalid footprint: fail closed explicitly.
  expect_plan(true, false, false, payload - 8, 16, payload, ARTS_DB_MOVE_REJECT,
              ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS,
              "footprint past end -> reject");
  expect_plan(true, false, false, payload + 1, 8, payload, ARTS_DB_MOVE_REJECT,
              ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS,
              "footprint offset past end -> reject");
  expect_plan(true, false, false, payload, 1, payload, ARTS_DB_MOVE_REJECT,
              ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS,
              "footprint offset == extent -> reject");
  expect_plan(false, false, false, 0, 64, payload, ARTS_DB_MOVE_REJECT,
              ARTS_DB_MOVE_REASON_WRITE_FOOTPRINT,
              "writer footprint -> reject");

  /// Unknown extents are validated by the holder.
  expect_plan(true, false, false, 1u << 20, 4096, 0, ARTS_DB_MOVE_SLICE,
              ARTS_DB_MOVE_REASON_FOOTPRINT,
              "unknown extent RO footprint -> slice (deferred validate)");
  expect_plan(false, false, false, 0, 4096, 0, ARTS_DB_MOVE_REJECT,
              ARTS_DB_MOVE_REASON_WRITE_FOOTPRINT,
              "unknown extent writer footprint -> reject");
  expect_plan(true, false, false, 0, 0, 0, ARTS_DB_MOVE_WHOLE,
              ARTS_DB_MOVE_REASON_NO_FOOTPRINT,
              "unknown extent no footprint -> whole");

  /// Every defined kind/reason maps to a non-default name.
  for (int k = ARTS_DB_MOVE_LOCAL; k <= ARTS_DB_MOVE_REJECT; ++k) {
    const char *name = arts_db_move_kind_str((arts_db_move_kind_t)k);
    require_true(name != NULL, "kind_str non-null");
    require_true(strcmp(name, "unknown kind") != 0, "kind_str maps a defined kind");
  }
  for (int r = ARTS_DB_MOVE_REASON_NONE;
       r <= ARTS_DB_MOVE_REASON_FOOTPRINT_OUT_OF_BOUNDS; ++r) {
    const char *name = arts_db_move_reason_str((arts_db_move_reason_t)r);
    require_true(name != NULL, "reason_str non-null");
    require_true(strcmp(name, "unknown reason") != 0,
                 "reason_str maps a defined reason");
  }
  require_true(strcmp(arts_db_move_kind_str((arts_db_move_kind_t)99),
                      "unknown kind") == 0,
               "kind_str catch-all");
  require_true(strcmp(arts_db_move_reason_str((arts_db_move_reason_t)99),
                      "unknown reason") == 0,
               "reason_str catch-all");

  if (failures == 0)
    printf("db_move_plan: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
