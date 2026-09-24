/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* label_reuse_ring_db — one labeled data block reused for GENERATIONS
 * lifetimes.
 *
 * The label is homed on the first rank.  Generation g is made by a creator
 * task on rank creator(g) (the last rank; or, where RING_ROTATE is defined,
 * the ranks in turn, the home included).  Each creator task:
 *   1. runs only once generation g-1's readers have finished, which proves
 *      generation g-1 installed;
 *   2. on even generations creates generation g at once, while generation g-1
 *      is still live, so the create parks behind it (the early lane);
 *   3. has generation g-1 destroyed at the home and waits for that;
 *   4. on odd generations creates generation g only now (the late lane);
 *   5. writes generation g's value through its pointer, releases it, binds
 *      generation g's readers and the next creator task.
 * Only the create of generation g is ever issued before generation g-1's
 * destroy, and only once generation g-1 is known installed; every other
 * message for generation g follows the destroy.
 *
 * A reader on the home checks every generation's value, in order.  Readers
 * on other ranks (rank 1 with three or more ranks; with RING_ROTATE, the
 * next generation's creator rank, so that rank holds the previous
 * generation's reader cache when it creates) may still see an earlier
 * generation, or be woken with no payload by that generation's late
 * teardown: the runtime's recorded weakness, printed as KNOWN-WEAKNESS, not a
 * failure.  The run ends with the last generation destroyed and no create
 * parked, so its shutdown is quiescent.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#ifndef RING_NAME
#define RING_NAME "label_reuse_ring_db"
#endif

#define GENERATIONS 64u
#define VALUE(k) (0xD8000000u + (uint32_t)(k))

enum { P_LABEL, P_GEN, P_DONE, P_COUNT };

/* Home readers run one generation after another, so a plain counter on the
 * home records the order. */
static uint64_t g_next_home;

static unsigned int creator_rank(uint64_t g) {
  unsigned int n = arts_get_total_ranks();
#ifdef RING_ROTATE
  return (unsigned int)(g % n);
#else
  (void)g;
  return n - 1u;
#endif
}

/* The rank of generation g's second reader, or 0 when it has none. */
static unsigned int second_reader_rank(uint64_t g) {
#ifdef RING_ROTATE
  return creator_rank(g + 1u);
#else
  (void)g;
  return arts_get_total_ranks() >= 3u ? 1u : 0u;
#endif
}

static void home_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t k = paramv[P_GEN];
  const uint32_t *v = (const uint32_t *)depv[0].ptr;
  if (k != g_next_home) {
    arts_printf("FAIL: " RING_NAME " generation %lu read after %lu\n",
                (unsigned long)k, (unsigned long)g_next_home);
    arts_test_fail();
  }
  g_next_home = k + 1u;
  if (v == NULL || *v != VALUE(k)) {
    arts_printf("FAIL: " RING_NAME " generation %lu read 0x%x at the home, "
                "want 0x%x\n",
                (unsigned long)k, v ? *v : 0u, VALUE(k));
    arts_test_fail();
  }
}

static void other_reader(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t k = paramv[P_GEN];
  const uint32_t *v = (const uint32_t *)depv[0].ptr;
  if (v != NULL && *v == VALUE(k)) {
    return;
  }
  if (v == NULL || (*v >= VALUE(0) && *v < VALUE(k))) {
    arts_printf("KNOWN-WEAKNESS: " RING_NAME " rank %u read %s while "
                "generation %lu was live\n",
                arts_get_current_rank(),
                v == NULL ? "no payload" : "an earlier generation",
                (unsigned long)k);
    return;
  }
  arts_printf("FAIL: " RING_NAME " generation %lu read 0x%x on rank %u, "
              "want 0x%x\n",
              (unsigned long)k, *v, arts_get_current_rank(), VALUE(k));
  arts_test_fail();
}

static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[P_LABEL]);
}

static void creator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint64_t g = paramv[P_GEN];
  arts_guid_t done = (arts_guid_t)paramv[P_DONE];
  bool early = (g % 2u) == 0u;
  uint32_t *p = NULL;

  if (g < GENERATIONS && (early || g == 0u)) {
    p = (uint32_t *)arts_db_create_with_guid(label, sizeof(uint32_t), ARTS_DB,
                                             ARTS_DB_PROP_NONE, NULL);
  }
  if (g > 0u) {
    /* Generation g-1's readers have finished: destroy it at the home. */
    arts_guid_t destroyed = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    uint64_t dpv[P_COUNT] = {(uint64_t)label, g - 1u, (uint64_t)done};
    (void)arts_edt_create(
        destroyer, P_COUNT, dpv, 0,
        &(arts_edt_hint_t){.rank = 0u, .finish_event = destroyed});
    (void)arts_event_wait(destroyed);
  }
  if (g == GENERATIONS) {
    arts_event_satisfy(done, NULL_GUID);
    return;
  }
  if (p == NULL) {
    p = (uint32_t *)arts_db_create_with_guid(label, sizeof(uint32_t), ARTS_DB,
                                             ARTS_DB_PROP_NONE, NULL);
  }
  if (p == NULL) {
    arts_printf("FAIL: " RING_NAME " generation %lu create handed back no "
                "pointer\n",
                (unsigned long)g);
    arts_test_fail();
    return;
  }
  *p = VALUE(g);
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t read = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t pv[P_COUNT] = {(uint64_t)label, g, (uint64_t)done};
  arts_guid_t r = arts_edt_create(
      home_reader, P_COUNT, pv, 1,
      &(arts_edt_hint_t){.rank = 0u, .finish_event = read});
  arts_add_dependence(label, r, 0, DB_MODE_RO);
  unsigned int second = second_reader_rank(g);
  if (second != 0u) {
    arts_guid_t r2 = arts_edt_create(
        other_reader, P_COUNT, pv, 1,
        &(arts_edt_hint_t){.rank = second, .finish_event = read});
    arts_add_dependence(label, r2, 0, DB_MODE_RO);
  }
  uint64_t npv[P_COUNT] = {(uint64_t)label, g + 1u, (uint64_t)done};
  arts_guid_t next = arts_edt_create(
      creator, P_COUNT, npv, 1,
      &(arts_edt_hint_t){.rank = creator_rank(g + 1u)});
  arts_add_dependence(read, next, 0, DB_MODE_NULL);
}

static void tail(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* The creator chain belongs to no finish scope: its tasks run on other
 * ranks, and the last one signals the home, where the only scope main waits
 * on lives with its one member. */
void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, 0u);
  arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_STICKY);
  arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t t = arts_edt_create(
      tail, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u, .finish_event = finish});
  arts_add_dependence(done, t, 0, DB_MODE_NULL);
  uint64_t pv[P_COUNT] = {(uint64_t)label, 0u, (uint64_t)done};
  (void)arts_edt_create(creator, P_COUNT, pv, 0,
                        &(arts_edt_hint_t){.rank = creator_rank(0u)});
  (void)arts_event_wait(finish);
  arts_event_destroy(done);
  if (g_next_home != GENERATIONS) {
    arts_printf("FAIL: " RING_NAME " %lu of %u generations read\n",
                (unsigned long)g_next_home, GENERATIONS);
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: " RING_NAME " %u generations in order, late and "
                "early creates\n",
                GENERATIONS);
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
