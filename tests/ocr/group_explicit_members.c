/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* group_explicit_members — an explicit group fires after every member and
 * every descendant of a member, and a successor on another rank sees what
 * the opener released before its DECR.
 *
 * main_edt creates a block and writes it, opens a group and creates K members
 * of it on round-robin ranks.  Each member creates a marker and a grandchild
 * on the next rank, which creates a marker too.  A marker runs on rank 0 and
 * names no group, so it inherits its creator's and the group cannot fire
 * before it has run; it counts itself on rank 0.  main_edt wires a checker on
 * rank 0 to the group and a reader on the last rank to the group and,
 * read-only, to the block, releases the block and DECRs the group, then stays
 * in its body for a while.  The reader must read the value main_edt wrote,
 * and the checker, which also waits for the reader, must count 2K markers.
 *
 * The opener stays in its body after its DECR, so the run ends while it is
 * still running: without the release before the DECR it still holds the
 * block then.  The reader runs either way; in INV x WB that hold leaves a cut
 * release wait, which fails the test through the quiescence marker.  Other
 * cells are not claimed: VAL x WT passes without the release.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "../test_failure_status.h"
#include "arts.h"

#define K 64u
#define VALUE 0x5EEDF00Dull
#define OPENER_HOLD_NS 2000000000L

static _Atomic(uint64_t) g_marks;

static void marker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_fetch_add_explicit(&g_marks, 1u, memory_order_release);
}

static void grandchild(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)arts_edt_create(marker, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0u});
}

static void member(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int next = (arts_get_current_rank() + 1u) % arts_get_total_ranks();
  (void)arts_edt_create(marker, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0u});
  (void)arts_edt_create(grandchild, 0, NULL, 0,
                        &(arts_edt_hint_t){.rank = next});
}

static void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *v = (const uint64_t *)depv[1].ptr;
  if (v == NULL || *v != VALUE) {
    arts_printf("FAIL: group_explicit_members: the reader read %llx, want the "
                "opener's %llx\n",
                v ? (unsigned long long)*v : 0ull, (unsigned long long)VALUE);
    arts_test_fail();
  }
  arts_event_satisfy((arts_guid_t)paramv[0], NULL_GUID);
}

static void checker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint64_t marks = atomic_load_explicit(&g_marks, memory_order_acquire);
  if (marks != 2u * K) {
    arts_printf("FAIL: group_explicit_members: %llu markers ran before the "
                "group fired, want %u\n",
                (unsigned long long)marks, 2u * K);
    arts_test_fail();
  }
  if (arts_test_status() == 0) {
    arts_printf("PASS: group_explicit_members %u markers\n", 2u * K);
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
  uint64_t *p = NULL;
  arts_guid_t b = arts_db_create((void **)&p, sizeof(uint64_t), ARTS_DB,
                                 ARTS_DB_PROP_NONE, NULL);
  *p = VALUE;

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  arts_guid_t g = arts_event_create(&gh);
  for (unsigned int i = 0; i < K; i++) {
    (void)arts_edt_create(member, 0, NULL, 0,
                          &(arts_edt_hint_t){.rank = i % n, .group = g});
  }
  arts_guid_t read_done = arts_event_create(NULL);
  uint64_t rd = read_done;
  arts_guid_t chk =
      arts_edt_create(checker, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(g, chk, 0, DB_MODE_NULL);
  arts_add_dependence(read_done, chk, 1, DB_MODE_NULL);
  arts_guid_t rdr =
      arts_edt_create(reader, 1, &rd, 2, &(arts_edt_hint_t){.rank = n - 1u});
  arts_add_dependence(g, rdr, 0, DB_MODE_NULL);
  arts_add_dependence(b, rdr, 1, DB_MODE_RO);
  arts_db_release(b);
  arts_event_satisfy_slot(g, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  do {
    clock_gettime(CLOCK_MONOTONIC, &t1);
  } while ((t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec) <
           OPENER_HOLD_NS);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
