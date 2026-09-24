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

/// @file db_create_claims_first_touch_stub.c
/// @brief A dependence that reaches a labeled block's home before the block's
///        create leaves a first-touch stub there, and that stub is not an
///        occupant: the create installs the block and the parked request is
///        served from it.
///
/// Per iteration, a fresh label homed on rank 0:
///  - an UNORDERED reader on rank 1 carries only its read dependence, so its
///    request can reach the home before the create.  It is ordered after
///    nothing the creator does, so it may observe the block before or after
///    the creator's write; it asserts only that it got storage;
///  - the creator on rank 0 creates the block, writes a per-iteration
///    sentinel, releases it, and fires its output event;
///  - a gate on the last rank waits on that output event and only then adds
///    an ORDERED reader's read dependence: that reader is ordered after the
///    creator's release and must observe the sentinel.
/// A stub taken for an occupant parks the create forever (the run times out);
/// a lost or regressed write fails the ordered reader.
///
/// Needs 2+ ranks (SKIP on one).  Config-agnostic.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#include "../test_failure_status.h"

#define ITERS 200u
#define SENTINEL_BASE 0x50000000u

/// paramv: [label, sentinel].
static void creator_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t label = (arts_guid_t)paramv[0];
  unsigned int *ptr = (unsigned int *)arts_db_create_with_guid(
      label, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (ptr == NULL) {
    arts_printf(
        "FAIL: db_create_claims_first_touch_stub creator got no storage\n");
    arts_test_fail();
    return;
  }
  ptr[0] = (unsigned int)paramv[1];
  arts_db_release(label, DB_MODE_RW);
}

static void unordered_reader_edt(uint32_t paramc, const uint64_t *paramv,
                                 uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf(
        "FAIL: db_create_claims_first_touch_stub unordered reader got no "
        "storage\n");
    arts_test_fail();
  }
}

/// paramv: [sentinel].
static void ordered_reader_edt(uint32_t paramc, const uint64_t *paramv,
                               uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const unsigned int *data = (const unsigned int *)depv[0].ptr;
  unsigned int expect = (unsigned int)paramv[0];
  if (data == NULL || data[0] != expect) {
    arts_printf(
        "FAIL: db_create_claims_first_touch_stub ordered reader expected "
        "0x%x got 0x%x\n",
        expect, data ? data[0] : 0u);
    arts_test_fail();
  }
}

/// Runs once the creator's output event fired.  paramv: [label, sentinel].
static void gate_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t sentinel = paramv[1];
  arts_guid_t reader =
      arts_edt_create(ordered_reader_edt, 1, &sentinel, 1,
                      &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence((arts_guid_t)paramv[0], reader, 0, DB_MODE_RO);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int ranks = arts_get_total_ranks();
  if (ranks < 2) {
    arts_printf(
        "SKIP: db_create_claims_first_touch_stub requires node_count >= "
        "2\n");
    arts_shutdown();
    return;
  }

  arts_guid_t outer = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  for (unsigned int it = 0; it < ITERS; it++) {
    uint64_t sentinel = SENTINEL_BASE + it;
    arts_guid_t label = arts_guid_reserve(ARTS_GUID_DB, 0);

    arts_guid_t reader =
        arts_edt_create(unordered_reader_edt, 0, NULL, 1,
                        &(arts_edt_hint_t){.rank = 1, .finish_event = outer});
    arts_add_dependence(label, reader, 0, DB_MODE_RO);

    arts_guid_t done = arts_event_create(NULL);
    uint64_t gparams[2] = {(uint64_t)label, sentinel};
    arts_guid_t gate = arts_edt_create(
        gate_edt, 2, gparams, 1,
        &(arts_edt_hint_t){.rank = ranks - 1, .finish_event = outer});
    arts_add_dependence(done, gate, 0, DB_MODE_NULL);

    uint64_t cparams[2] = {(uint64_t)label, sentinel};
    arts_edt_create(creator_edt, 2, cparams, 0,
                    &(arts_edt_hint_t){.rank = 0,
                                       .finish_event = outer,
                                       .output_event = done});
  }

  arts_event_wait(outer);
  if (arts_test_status() == 0) {
    arts_printf(
        "PASS: db_create_claims_first_touch_stub %u iters x %u ranks\n",
        ITERS, ranks);
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
