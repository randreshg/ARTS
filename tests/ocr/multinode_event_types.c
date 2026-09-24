/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/// @file multinode_event_types.c
/// @brief Event kinds across ranks: ONCE, STICKY, IDEM and a LATCH fan-in,
///        each satisfied from another rank than the dependent's, plus late
///        dependences on persistent events added after the fire.
///
/// Every dependent checks it runs on the rank it was placed on; the finish
/// scope the shutdown waits on counts them all, so a lost delivery never
/// reaches the shutdown (the run times out).  Requires 2+ ranks (SKIP on one).

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

static void check_rank(const char *what, uint64_t expected) {
  unsigned int me = arts_get_current_rank();
  if (me != (unsigned int)expected) {
    arts_printf("  FAIL: %s ran on rank %u, placed on %u\n", what, me,
                (unsigned int)expected);
    arts_test_fail();
    return;
  }
  arts_printf("  PASS: %s (rank %u)\n", what, me);
}

/// Satisfier: paramv[0] = event.
void remote_satisfy(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

/// Dependent: paramv[0] = the rank it was placed on.
void once_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("cross-rank ONCE event fired its dependent", paramv[0]);
}

void once_remote_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("ONCE dependence added before a remote satisfy fired", paramv[0]);
}

void sticky_late_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("cross-rank STICKY late dependence fired", paramv[0]);
}

/// Runs once the STICKY event fired; adds a late dependence, then destroys it.
/// paramv: [event].  The late dependent inherits this EDT's finish scope.
void sticky_trampoline(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_guid_t event = (arts_guid_t)paramv[0];
  uint64_t r = 0;
  arts_guid_t late =
      arts_edt_create(sticky_late_dep, 1, &r, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(event, late, 0, DB_MODE_RW);
  arts_event_destroy(event);
}

void idem_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("cross-rank IDEM event fired its dependent", paramv[0]);
}

/// Runs once the IDEM event fired: a second satisfy is absorbed silently.
void idem_re_satisfy(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_guid_t event = (arts_guid_t)paramv[0];
  arts_event_satisfy_slot(event, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
  arts_event_destroy(event);
}

void idem_late_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("IDEM late dependence off the event's home fired", paramv[0]);
}

/// Runs on rank 1 once the IDEM event fired; adds a late dependence there.
/// paramv: [event].  The late dependent inherits this EDT's finish scope.
void idem_setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t r = 1;
  arts_guid_t late =
      arts_edt_create(idem_late_dep, 1, &r, 1, &(arts_edt_hint_t){.rank = 1});
  arts_add_dependence((arts_guid_t)paramv[0], late, 0, DB_MODE_RW);
}

void fan_in_dep(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  check_rank("LATCH fan-in from every rank fired", paramv[0]);
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: multinode_event_types\n");
  }
  arts_shutdown();
}

static void satisfy_from(arts_guid_t ev, unsigned int rank, arts_guid_t fe) {
  uint64_t p = (uint64_t)ev;
  arts_edt_create(remote_satisfy, 1, &p, 0,
                  &(arts_edt_hint_t){.rank = rank, .finish_event = fe});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int total = arts_get_total_ranks();
  if (total < 2) {
    arts_printf("SKIP: multinode_event_types requires 2+ ranks\n");
    arts_shutdown();
    return;
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(fe, shut, 0, DB_MODE_NULL);
  uint64_t r0 = 0;
  uint64_t r1 = 1;

  // ONCE (defaults) homed on rank 0, dependent on rank 0, satisfied from 1.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_DEFAULTS;
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    arts_guid_t dep = arts_edt_create(
        once_dep, 1, &r0, 1, &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ev, dep, 0, DB_MODE_RW);
    satisfy_from(ev, 1, fe);
  }

  // ONCE homed on rank 0 with its dependent on rank 1, the dependence added
  // before the satisfier is created.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_ONCE;
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    arts_guid_t dep =
        arts_edt_create(once_remote_dep, 1, &r1, 1,
                        &(arts_edt_hint_t){.rank = 1, .finish_event = fe});
    arts_add_dependence(ev, dep, 0, DB_MODE_RW);
    satisfy_from(ev, 0, fe);
  }

  // STICKY: the trampoline runs once the event fired and adds a late
  // dependence on the same event.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_STICKY;
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    uint64_t tp[1] = {(uint64_t)ev};
    arts_guid_t tramp =
        arts_edt_create(sticky_trampoline, 1, tp, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ev, tramp, 0, DB_MODE_RW);
    satisfy_from(ev, 1, fe);
  }

  // IDEM satisfied from rank 1; a second satisfy after the fire is absorbed.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_IDEMPOTENT;
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    arts_guid_t dep = arts_edt_create(
        idem_dep, 1, &r0, 1, &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ev, dep, 0, DB_MODE_RW);
    satisfy_from(ev, 1, fe);
    uint64_t rp[1] = {(uint64_t)ev};
    arts_guid_t re =
        arts_edt_create(idem_re_satisfy, 1, rp, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ev, re, 0, DB_MODE_RW);
  }

  // IDEM satisfied on its home; once it fired, rank 1 adds a late dependence.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_IDEMPOTENT;
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    uint64_t sp[1] = {(uint64_t)ev};
    arts_guid_t setup =
        arts_edt_create(idem_setup, 1, sp, 1,
                        &(arts_edt_hint_t){.rank = 1, .finish_event = fe});
    arts_add_dependence(ev, setup, 0, DB_MODE_RW);
    satisfy_from(ev, 0, fe);
  }

  // LATCH fan-in: latch = rank count, one decrement from every rank.
  {
    arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(total);
    h.rank = 0;
    arts_guid_t ev = arts_event_create(&h);
    arts_guid_t dep =
        arts_edt_create(fan_in_dep, 1, &r0, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(ev, dep, 0, DB_MODE_RW);
    for (unsigned int r = 0; r < total; r++) {
      satisfy_from(ev, r, fe);
    }
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
