/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_destroy_untouched_rank — a destroy issued on a rank that holds no copy
 * of the block reaches its home.
 *
 * A labeled block is homed on the first rank.  Generation 1 is created there
 * and written.  A task on the last rank, which receives the GUID only as a
 * parameter and never acquires it, destroys it.  Once that task has finished,
 * a task on the home creates generation 2 of the same GUID and writes it, and
 * a reader on the last rank, then one on the home, check generation 2's
 * value.  A destroy that did not leave the last rank would leave generation 1
 * live at the home: the second create would park behind it for good, both
 * readers would see generation 1, and the run would end with that create
 * still parked.
 *
 * The last rank's acquire leaves it after its destroy did, so under one
 * progress thread the home handles the destroy first.  With two progress
 * threads the home may dispatch the destroy after the second create, which
 * then parks until it lands — still a pass; the readers' acquires trail the
 * destroy by a chain of messages through both ranks, an order the runtime
 * does not guarantee there (the recorded weakness of labeled-GUID reuse) and
 * this shape leaves no practical window against.  The last generation is destroyed at the home
 * before the end, so the shutdown is quiescent.  On one rank the destroying
 * task runs on the home, which holds the block, so the run passes without
 * exercising the path.
 */

#include <stdbool.h>
#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define VALUE(k) (0xDE570000u + (uint32_t)(k))

enum { P_LABEL, P_DONE, P_COUNT };

static int g_closed;

static unsigned int last_rank(void) { return arts_get_total_ranks() - 1u; }

static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[P_LABEL]);
}

static bool check(const arts_edt_dep_t *dep) {
  const uint32_t *v = (const uint32_t *)dep->ptr;
  if (v == NULL || *v != VALUE(2)) {
    arts_printf("FAIL: db_destroy_untouched_rank rank %u read 0x%x, want "
                "0x%x\n",
                arts_get_current_rank(), v ? *v : 0u, VALUE(2));
    arts_test_fail();
    return false;
  }
  return true;
}

static void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)check(&depv[0]);
}

/* A block still holding the first generation is left live, so the second
 * create stays parked into the shutdown and the teardown walk reports it. */
static void closer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (check(&depv[1])) {
    arts_db_destroy((arts_guid_t)paramv[P_LABEL]);
  }
  g_closed = 1;
  arts_event_satisfy((arts_guid_t)paramv[P_DONE], NULL_GUID);
}

static uint32_t *create_generation(arts_guid_t label, uint32_t k) {
  uint32_t *p = (uint32_t *)arts_db_create_with_guid(
      label, sizeof(uint32_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_destroy_untouched_rank generation %u create handed "
                "back no pointer\n",
                k);
    arts_test_fail();
  }
  return p;
}

static void second_creator(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint32_t *p = create_generation(label, 2u);
  if (p == NULL) {
    return;
  }
  *p = VALUE(2);
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t read = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t r = arts_edt_create(
      reader, P_COUNT, paramv, 1,
      &(arts_edt_hint_t){.rank = last_rank(), .finish_event = read});
  arts_add_dependence(label, r, 0, DB_MODE_RO);
  arts_guid_t c =
      arts_edt_create(closer, P_COUNT, paramv, 2, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(read, c, 0, DB_MODE_NULL);
  arts_add_dependence(label, c, 1, DB_MODE_RO);
}

static void first_creator(uint32_t paramc, const uint64_t *paramv,
                          uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[P_LABEL];
  uint32_t *p = create_generation(label, 1u);
  if (p == NULL) {
    return;
  }
  *p = VALUE(1);
  arts_db_release(label, DB_MODE_RW);

  arts_guid_t destroyed = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  (void)arts_edt_create(
      destroyer, P_COUNT, paramv, 0,
      &(arts_edt_hint_t){.rank = last_rank(), .finish_event = destroyed});
  arts_guid_t s = arts_edt_create(second_creator, P_COUNT, paramv, 1,
                                  &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(destroyed, s, 0, DB_MODE_NULL);
}

static void tail(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* The chain belongs to no finish scope: its last task signals the home, where
 * the only scope main waits on lives with its one member. */
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
  uint64_t pv[P_COUNT] = {(uint64_t)label, (uint64_t)done};
  (void)arts_edt_create(first_creator, P_COUNT, pv, 0,
                        &(arts_edt_hint_t){.rank = 0u});
  (void)arts_event_wait(finish);
  arts_event_destroy(done);
  if (!g_closed) {
    arts_printf("FAIL: db_destroy_untouched_rank chain did not close\n");
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: db_destroy_untouched_rank destroyed from rank %u, "
                "generation 2 read there and at the home\n",
                last_rank());
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
