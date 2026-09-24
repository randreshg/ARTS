/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* label_reuse_ring_edt — one reserved EDT GUID reused for GENERATIONS
 * lifetimes, in two lanes run one after the other on two GUIDs.
 *
 * Each GUID is homed on the last rank; a driver on the first rank runs every
 * generation.  Generation k appends its value to the lane's record; its
 * completion retires the GUID and then signals.  The driver of generation k+1
 * is gated on that signal, reads the record to confirm generation k
 * completed, and only then issues generation k+1's satisfy and its dependence
 * on the record.
 *
 *   - late lane: the signal is generation k's finish scope, and generation
 *     k+1 is CREATED by the gated driver, so its create lands on a retired
 *     GUID.
 *   - early lane: the signal is generation k's output event, and generation
 *     k+1 is created while generation k runs, so a create that reaches the
 *     home before the retire parks there and installs at it: from k's own
 *     body on the home (even k), which parks every time, or from a helper k
 *     starts on the first rank (odd k), which parks or finds the GUID retired
 *     depending on when its message lands.
 *
 * The program provides two orderings, because under several progress threads
 * the runtime may dispatch one peer's messages to a GUID out of the order they
 * were sent, so an order the program needs must travel through a chain of
 * messages from the GUID's home:
 *   1. create k before create k+1: create k+1 is issued only by generation k's
 *      body or by a task that body started, so generation k is known to be
 *      installed.  Two creates in flight together could be dispatched in
 *      either order and install k+1 first.
 *   2. retire k before every other message for k+1: generation k+1's satisfy
 *      and its record dependence are issued only by the driver that k's
 *      completion signal gates, and that signal leaves after the retire.
 *
 * Every generation's value must be recorded, in order, in both lanes.  The run
 * ends with the last generations retired and nothing parked, so its shutdown
 * is quiescent.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define GENERATIONS 64u
#define VALUE(lane, k) ((arts_guid_t)(0x7000u + 0x1000u * (lane) + (k)))

enum { LANE_LATE, LANE_EARLY, LANES };

typedef struct {
  uint64_t count;
  uint64_t vals[GENERATIONS];
  uint64_t signal[GENERATIONS]; /* early lane: generation k's output event */
} record_t;

/* P_SIGNAL: the output event of the generation an early helper creates. */
enum { P_LANE, P_RING, P_RECORD, P_GEN, P_SIGNAL, P_OTHER_RING,
       P_OTHER_RECORD, P_COUNT };

static const char *const lane_name[LANES] = {"late", "early"};

static void driver(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]);
static void early_helper(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]);
static arts_guid_t create_member(const uint64_t *pv, arts_guid_t scope,
                                 arts_guid_t signal);

/* depv[0] = generation k's value, depv[1] = the record (RW). */
static void member(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t k = paramv[P_GEN];
  record_t *r = (record_t *)depv[1].ptr;
  if (r->count != k) {
    arts_printf("FAIL: label_reuse_ring_edt %s generation %lu ran after %lu "
                "records\n",
                lane_name[paramv[P_LANE]], (unsigned long)k,
                (unsigned long)r->count);
    arts_test_fail();
  }
  if (k < GENERATIONS) {
    r->vals[k] = (uint64_t)depv[0].guid;
  }
  r->count = k + 1u;
  if (paramv[P_LANE] == LANE_EARLY && k + 1u < GENERATIONS) {
    arts_guid_t signal = arts_event_create(&ARTS_EVENT_HINT_COUNTED(1));
    r->signal[k + 1u] = (uint64_t)signal;
    uint64_t pv[P_COUNT];
    for (unsigned int i = 0; i < P_COUNT; i++) {
      pv[i] = paramv[i];
    }
    pv[P_GEN] = k + 1u;
    pv[P_SIGNAL] = (uint64_t)signal;
    if (k % 2u == 0u) {
      (void)create_member(pv, NULL_GUID, signal);
    } else {
      (void)arts_edt_create(early_helper, P_COUNT, pv, 0,
                            &(arts_edt_hint_t){.rank = 0u});
    }
  }
}

static arts_guid_t create_member(const uint64_t *pv, arts_guid_t scope,
                                 arts_guid_t signal) {
  return arts_edt_create(member, P_COUNT, pv, 2,
                         &(arts_edt_hint_t){.guid = (arts_guid_t)pv[P_RING],
                                            .finish_event = scope,
                                            .output_event = signal});
}

static void early_helper(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  (void)create_member(paramv, NULL_GUID, (arts_guid_t)paramv[P_SIGNAL]);
}

/* Gate the driver of generation `pv[P_GEN]` on `after` and the record. */
static void start_driver(const uint64_t *pv, arts_guid_t after) {
  arts_guid_t d =
      arts_edt_create(driver, P_COUNT, pv, 2, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(after, d, 0, DB_MODE_NULL);
  arts_add_dependence((arts_guid_t)pv[P_RECORD], d, 1, DB_MODE_RO);
}

/* depv[0] = the previous generation's completion signal (its finish scope in
 * the late lane, its output event in the early lane), depv[1] = the record
 * (RO). */
static void driver(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t lane = paramv[P_LANE];
  arts_guid_t ring = (arts_guid_t)paramv[P_RING];
  arts_guid_t record = (arts_guid_t)paramv[P_RECORD];
  uint64_t k = paramv[P_GEN];
  const record_t *r = (const record_t *)depv[1].ptr;
  if (r->count != k) {
    arts_printf("FAIL: label_reuse_ring_edt %s driver %lu sees %lu records\n",
                lane_name[lane], (unsigned long)k, (unsigned long)r->count);
    arts_test_fail();
    return;
  }
  if (k == GENERATIONS) {
    for (uint64_t i = 0; i < GENERATIONS; i++) {
      if (r->vals[i] != (uint64_t)VALUE(lane, i)) {
        arts_printf("FAIL: label_reuse_ring_edt %s generation %lu recorded "
                    "0x%lx, want 0x%lx\n",
                    lane_name[lane], (unsigned long)i,
                    (unsigned long)r->vals[i],
                    (unsigned long)VALUE(lane, i));
        arts_test_fail();
      }
    }
    if (lane == LANE_LATE) {
      uint64_t pv[P_COUNT] = {LANE_EARLY, paramv[P_OTHER_RING],
                              paramv[P_OTHER_RECORD], 0u, NULL_GUID,
                              NULL_GUID, NULL_GUID};
      arts_guid_t first = arts_edt_create(driver, P_COUNT, pv, 2,
                                          &(arts_edt_hint_t){.rank = 0u});
      arts_edt_satisfy_slot(first, 0, NULL_GUID, DB_MODE_NULL);
      arts_add_dependence(pv[P_RECORD], first, 1, DB_MODE_RO);
    } else if (arts_test_status() == 0) {
      arts_printf("PASS: label_reuse_ring_edt %u generations in order, late "
                  "and early creates\n",
                  GENERATIONS);
    }
    return;
  }

  uint64_t pv[P_COUNT];
  for (unsigned int i = 0; i < P_COUNT; i++) {
    pv[i] = paramv[i];
  }
  arts_guid_t signal;
  if (lane == LANE_LATE) {
    signal = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    (void)create_member(pv, signal, NULL_GUID);
  } else if (k == 0u) {
    signal = arts_event_create(&ARTS_EVENT_HINT_COUNTED(1));
    (void)create_member(pv, NULL_GUID, signal);
  } else {
    signal = (arts_guid_t)r->signal[k];
  }
  arts_edt_satisfy_slot(ring, 0, VALUE(lane, k), DB_MODE_NULL);
  /* The next driver binds to the signal before generation k can run: a
   * finish scope is reclaimed when it fires. */
  pv[P_GEN] = k + 1u;
  start_driver(pv, signal);
  arts_add_dependence(record, ring, 1, DB_MODE_RW);
}

static arts_guid_t make_record(void) {
  void *p = NULL;
  arts_guid_t record = arts_db_create(&p, sizeof(record_t), ARTS_DB,
                                      ARTS_DB_PROP_NONE,
                                      &(arts_db_hint_t){.rank = 0u});
  record_t *r = (record_t *)p;
  r->count = 0u;
  for (uint64_t i = 0; i < GENERATIONS; i++) {
    r->vals[i] = 0u;
    r->signal[i] = 0u;
  }
  arts_db_release(record, DB_MODE_RW);
  return record;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_guid_t late_ring = arts_guid_reserve(ARTS_GUID_EDT, last);
  arts_guid_t early_ring = arts_guid_reserve(ARTS_GUID_EDT, last);
  arts_guid_t late_record = make_record();
  arts_guid_t early_record = make_record();

  arts_guid_t finish = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  uint64_t pv[P_COUNT] = {LANE_LATE,           (uint64_t)late_ring,
                          (uint64_t)late_record, 0u,
                          NULL_GUID,           (uint64_t)early_ring,
                          (uint64_t)early_record};
  arts_guid_t first = arts_edt_create(
      driver, P_COUNT, pv, 2,
      &(arts_edt_hint_t){.rank = 0u, .finish_event = finish});
  arts_edt_satisfy_slot(first, 0, NULL_GUID, DB_MODE_NULL);
  arts_add_dependence(late_record, first, 1, DB_MODE_RO);
  arts_event_wait(finish);
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
