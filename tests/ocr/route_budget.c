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
/// on a one-rank shape (the main EDT decrements it last) and at n on rank
/// 1 of a two-rank shape; the last EDT of each unit decrements it, and whoever
/// takes it to zero shuts the run down.  That termination is harness
/// mechanics, not a pattern for a program.
///
///   none        Nothing per unit.
///   create      A sticky event, a block created with ARTS_DB_PROP_NO_ACQUIRE
///               and an EDT with no dependence, whose body ends the unit.
///   rw          A block created with the creator's hold, written through the
///               pointer the create hands out, and destroyed by the same run.
///   group       Once per run, an EDT joined to a group; per unit, a
///               child it creates inside that group and a grandchild the child
///               creates, both members by inheritance; the grandchild ends the
///               unit.
///   firsttouch  Two ranks: rank 0 creates a block, writes and releases it,
///               creates an EDT on rank 1 with one dependence and satisfies it
///               with the block (RO); the EDT checks the bytes and ends the
///               unit.
///   rwremote    As firsttouch, RW: the EDT checks the bytes, writes them,
///               releases the block and ends the unit.

#include "arts.h"

#include <stdatomic.h>
#include <stdbool.h>
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

/* The two-rank units: rank 1's EDT, the block rank 0 wrote for unit i. */
static void remote_unit_check(const arts_edt_dep_t *dep, uint64_t i) {
  const uint64_t *addr = (const uint64_t *)dep->ptr;
  if (addr == NULL) {
    printf("FAIL: route_budget: unit %llu found no bytes\n",
           (unsigned long long)i);
    arts_test_fail();
    return;
  }
  for (uint64_t k = 0; k < 64 / sizeof(uint64_t); k++) {
    if (addr[k] != i + k) {
      printf("FAIL: route_budget: unit %llu read word %llu = %llu\n",
             (unsigned long long)i, (unsigned long long)k,
             (unsigned long long)addr[k]);
      arts_test_fail();
      return;
    }
  }
}

static void firsttouch_unit_end(uint32_t paramc, const uint64_t *paramv,
                                uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  remote_unit_check(&depv[0], paramv[0]);
  unit_done();
}

static void rwremote_unit_end(uint32_t paramc, const uint64_t *paramv,
                              uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  remote_unit_check(&depv[0], paramv[0]);
  uint64_t *addr = (uint64_t *)depv[0].ptr;
  if (addr != NULL) {
    addr[0] = ~paramv[0];
  }
  /* Released before the unit ends, so the shutdown cuts no release. */
  arts_db_release(depv[0].guid, DB_MODE_RW);
  unit_done();
}

static void shape_remote(uint64_t n, arts_edt_t body,
                         arts_db_access_mode_t mode) {
  for (uint64_t i = 0; i < n; i++) {
    uint64_t *addr = NULL;
    arts_db_hint_t dh = ARTS_DB_HINT_DEFAULTS;
    arts_guid_t db =
        arts_db_create((void **)&addr, 64, ARTS_DB, ARTS_DB_PROP_NONE, &dh);
    if (db == NULL_GUID || addr == NULL) {
      printf("FAIL: route_budget: a block create failed\n");
      arts_test_fail();
      return;
    }
    for (uint64_t k = 0; k < 64 / sizeof(uint64_t); k++) {
      addr[k] = i + k;
    }
    arts_db_release(db, DB_MODE_RW);
    arts_edt_hint_t eh = ARTS_EDT_HINT_DEFAULTS;
    eh.rank = 1u;
    uint64_t param = i;
    arts_guid_t edt = arts_edt_create(body, 1, &param, 1, &eh);
    if (edt == NULL_GUID) {
      printf("FAIL: route_budget: an EDT create failed\n");
      arts_test_fail();
      return;
    }
    arts_edt_satisfy_slot(edt, 0, db, mode);
  }
}

static void shape_firsttouch(uint64_t n) {
  shape_remote(n, firsttouch_unit_end, DB_MODE_RO);
}

static void shape_rwremote(uint64_t n) {
  shape_remote(n, rwremote_unit_end, DB_MODE_RW);
}

static void shape_rw(uint64_t n) {
  for (uint64_t i = 0; i < n; i++) {
    uint64_t *addr = NULL;
    arts_guid_t db = arts_db_create((void **)&addr, 64, ARTS_DB,
                                    ARTS_DB_PROP_NONE, NULL);
    if (db == NULL_GUID || addr == NULL) {
      printf("FAIL: route_budget rw: a block create failed\n");
      arts_test_fail();
      unit_done();
      continue;
    }
    for (uint64_t k = 0; k < 64 / sizeof(uint64_t); k++) {
      addr[k] = i + k;
    }
    arts_db_destroy(db);
    unit_done();
  }
}

static const arts_edt_hint_t g_here = {.rank = ARTS_HINT_CURRENT_RANK,
                                       .guid = NULL_GUID,
                                       .group = NULL_GUID,
                                       .completion_event = NULL_GUID,
                                       .flags = ARTS_EDT_FLAG_NONE};

static void group_grandchild(uint32_t paramc, const uint64_t *paramv,
                             uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unit_done();
}

static void group_child(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_edt_create(group_grandchild, 0, NULL, 0, &g_here) == NULL_GUID) {
    printf("FAIL: route_budget group: a grandchild create failed\n");
    arts_test_fail();
    unit_done();
  }
}

static void group_parent(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  for (uint64_t i = 0; i < paramv[0]; i++) {
    if (arts_edt_create(group_child, 0, NULL, 0, &g_here) == NULL_GUID) {
      printf("FAIL: route_budget group: a child create failed\n");
      arts_test_fail();
      unit_done();
    }
  }
}

static void shape_group(uint64_t n) {
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
  eh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&eh);
  arts_edt_hint_t join = g_here;
  join.group = group;
  if (group == NULL_GUID ||
      arts_edt_create(group_parent, 1, &n, 0, &join) == NULL_GUID) {
    printf("FAIL: route_budget group: the group or its parent failed\n");
    arts_test_fail();
    for (uint64_t i = 0; i < n; i++) {
      unit_done();
    }
  }
  if (group != NULL_GUID) {
    arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
  }
}

static const struct {
  const char *name;
  shape_fn run;
  bool two_rank; /* rank 1's EDTs end the units */
} g_shapes[] = {
    {"none", shape_none, false},
    {"create", shape_create, false},
    {"rw", shape_rw, false},
    {"group", shape_group, false},
    {"firsttouch", shape_firsttouch, true},
    {"rwremote", shape_rwremote, true},
};

static shape_fn g_run;
static bool g_two_rank;
static uint64_t g_n;

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (g_two_rank && arts_get_total_ranks() != 2u) {
    printf("FAIL: route_budget: a two-rank shape on %u ranks\n",
           arts_get_total_ranks());
    arts_test_fail();
    arts_shutdown();
    return;
  }
  g_run(g_n);
  if (!g_two_rank) {
    unit_done();
  }
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
      g_two_rank = g_shapes[i].two_rank;
    }
  }
  char *end = NULL;
  g_n = strtoull(argv[2], &end, 10);
  if (g_run == NULL || end == argv[2] || *end != '\0' || g_n == 0 ||
      g_n > N_MAX) {
    printf("FAIL: route_budget: bad shape or n (%s %s)\n", argv[1], argv[2]);
    return 1;
  }
  atomic_store_explicit(&g_remaining, (int64_t)g_n + (g_two_rank ? 0 : 1),
                        memory_order_relaxed);

  int rc = arts_rt(argc, argv);
  if (rc != 0 || arts_test_status() != 0) {
    printf("FAIL: route_budget %s %s\n", argv[1], argv[2]);
    return 1;
  }
  printf("PASS: route_budget %s %s\n", argv[1], argv[2]);
  return 0;
}
