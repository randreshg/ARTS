/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/// @file route_budget.c
/// @brief A program of one shape and n units of it, for the differential
///        oracle on the route table's per-unit probe and pin budget.
///
/// Usage: route_budget <shape> <n>
///
/// The program only runs the shape; the route table's counters are read back
/// from the per-rank counter files by route_budget_diff.cmake, which runs it
/// at n and at 2n.  A process-local counter on the judged rank starts at n + 1
/// (the main EDT decrements it last); the last EDT of each unit decrements
/// it, and whoever takes it to zero shuts the run down.  That termination is
/// harness mechanics, not a pattern for a program.
///
///   none    Nothing per unit.
///   create  A sticky event, a block created with ARTS_DB_PROP_NO_ACQUIRE and
///           an EDT with no dependence, whose body ends the unit.

#include "arts.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_failure_status.h"

/* The oracle runs n and 2n units with n <= 256. */
#define N_MAX 512u

/* Units still to finish on the judged rank.  Protocol: initialised before
 * arts_rt; relaxed decrements by the units' last EDTs; the decrement that
 * reaches zero is the only one that acts, so no other access needs an order. */
static _Atomic(int64_t) g_remaining;

static void unit_done(void) {
  if (atomic_fetch_sub_explicit(&g_remaining, 1, memory_order_relaxed) == 1) {
    arts_shutdown();
  }
}

typedef void (*shape_fn)(uint64_t n);

static void shape_none(uint64_t n) {
  for (uint64_t i = 0; i < n; i++) {
    unit_done();
  }
}

static void create_unit_end(uint32_t paramc, const uint64_t *paramv,
                            uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unit_done();
}

static void shape_create(uint64_t n) {
  for (uint64_t i = 0; i < n; i++) {
    if (arts_event_create(NULL) == NULL_GUID) {
      printf("FAIL: route_budget create: an event create failed\n");
      arts_test_fail();
    }
    void *addr = NULL;
    if (arts_db_create(&addr, 64, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, NULL) ==
        NULL_GUID) {
      printf("FAIL: route_budget create: a block create failed\n");
      arts_test_fail();
    }
    if (arts_edt_create(create_unit_end, 0, NULL, 0, NULL) == NULL_GUID) {
      printf("FAIL: route_budget create: an EDT create failed\n");
      arts_test_fail();
    }
  }
}

static const struct {
  const char *name;
  shape_fn run;
} g_shapes[] = {
    {"none", shape_none},
    {"create", shape_create},
};

static shape_fn g_run;
static uint64_t g_n;

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  g_run(g_n);
  unit_done();
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("FAIL: route_budget needs <shape> <n>\n");
    return 1;
  }
  g_run = NULL;
  for (size_t i = 0; i < sizeof(g_shapes) / sizeof(g_shapes[0]); i++) {
    if (strcmp(argv[1], g_shapes[i].name) == 0) {
      g_run = g_shapes[i].run;
    }
  }
  char *end = NULL;
  g_n = strtoull(argv[2], &end, 10);
  if (g_run == NULL || end == argv[2] || *end != '\0' || g_n == 0 ||
      g_n > N_MAX) {
    printf("FAIL: route_budget: bad shape or n (%s %s)\n", argv[1], argv[2]);
    return 1;
  }
  atomic_store_explicit(&g_remaining, (int64_t)g_n + 1, memory_order_relaxed);

  int rc = arts_rt(argc, argv);
  if (rc != 0 || arts_test_status() != 0) {
    printf("FAIL: route_budget %s %s\n", argv[1], argv[2]);
    return 1;
  }
  printf("PASS: route_budget %s %s\n", argv[1], argv[2]);
  return 0;
}
