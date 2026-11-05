/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* group_nested_chain — a group opened inside another and chained into it
 * keeps the outer group open until the inner group's members are done.
 *
 * main_edt opens the outer group P and creates M members of it on round-robin
 * ranks.  Each member opens an inner group g, chains g into its current group
 * (an INCR of it, and g's fire wired to its DECR slot), creates J members of g
 * on round-robin ranks and DECRs g.  The current group of a member on another
 * rank than P's is its local proxy of P, so the chain is the same two calls on
 * every rank.  Each member of g creates a marker on rank 0, which inherits
 * g and counts itself there.  P's successor, on rank 0, must count M*J
 * markers: groups do not nest by themselves, so without the chain P would
 * fire once its own members returned.
 */

#include <stdatomic.h>
#include <stdint.h>

#include "../test_failure_status.h"
#include "arts.h"

#define M 8u
#define J 16u

static _Atomic(uint64_t) g_marks;

static void marker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_fetch_add_explicit(&g_marks, 1u, memory_order_release);
}

static void leaf(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)arts_edt_create(marker, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0u});
}

static void inner_opener(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  arts_guid_t cur = arts_current_group();
  if (cur == NULL_GUID) {
    arts_printf("FAIL: group_nested_chain: a member of the outer group has "
                "no current group\n");
    arts_test_fail();
    return;
  }
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t g = arts_event_create(&gh);
  arts_event_satisfy_slot(cur, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
  arts_add_dependence(g, cur, ARTS_EVENT_LATCH_DECR_SLOT, DB_MODE_NULL);
  for (unsigned int j = 0; j < J; j++) {
    unsigned int rank = (unsigned int)(paramv[0] + j) % n;
    (void)arts_edt_create(leaf, 0, NULL, 0,
                          &(arts_edt_hint_t){.rank = rank, .group = g});
  }
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

static void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t marks = atomic_load_explicit(&g_marks, memory_order_acquire);
  if (marks != (uint64_t)M * J) {
    arts_printf("FAIL: group_nested_chain: %llu markers ran before the outer "
                "group fired, want %u\n",
                (unsigned long long)marks, M * J);
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: group_nested_chain %u markers\n", M * J);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  arts_event_hint_t ph = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t p = arts_event_create(&ph);
  for (unsigned int i = 0; i < M; i++) {
    uint64_t base = i;
    (void)arts_edt_create(inner_opener, 1, &base, 0,
                          &(arts_edt_hint_t){.rank = i % n, .group = p});
  }
  arts_guid_t succ =
      arts_edt_create(successor, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(p, succ, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(p, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
