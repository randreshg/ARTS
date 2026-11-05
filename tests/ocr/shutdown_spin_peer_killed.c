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
/// @file shutdown_spin_peer_killed.c
/// @brief A rank whose every worker streams to a peer that is killed ends.
///
/// Rank 1 makes a sink EDT whose slots no sender fills, prints
/// `RANK_PID 1 <pid>`, and has rank 0 start one sender per worker; rank 0
/// prints FLOODING, and each sender satisfies PER_SENDER of the sink's slots,
/// a fixed count, so a sender returns once its sends end.  The test's driver
/// kills rank 1 when FLOODING arrives: rank 0's workers are then sending to
/// a peer gone for good, the ring-full spin included, and rank 0 must learn
/// of the end from rank 1's closed connection and end.  A rank 0 that never
/// learns is killed by the spawner's end grace, whose line fails the test.

#include "arts.h"

#include <stdint.h>
#include <unistd.h>

#include "../test_failure_status.h"

#define PER_SENDER 20000u

void sender(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t sink = (arts_guid_t)paramv[0];
  uint32_t base = (uint32_t)paramv[1] * PER_SENDER;
  for (uint32_t i = 0; i < PER_SENDER; i++) {
    arts_edt_satisfy_slot(sink, base + i, NULL_GUID, DB_MODE_NULL);
  }
}

void sink(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
          arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("  FAIL: the sink ran with a slot never satisfied\n");
  arts_test_fail();
}

/// Runs on rank 0: one sender per worker.  paramv = {sink}.
void relay(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("FLOODING\n");
  uint32_t senders = arts_get_workers_per_rank();
  for (uint32_t k = 0; k < senders; k++) {
    uint64_t p[2] = {paramv[0], k};
    arts_edt_create(sender, 2, p, 0, &(arts_edt_hint_t){.rank = 0});
  }
}

/// Runs on rank 1: the sink there, then the relay on rank 0.
void setup(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  uint32_t senders = arts_get_workers_per_rank();
  arts_guid_t s = arts_edt_create(sink, 0, NULL, senders * PER_SENDER + 1u,
                                  &(arts_edt_hint_t){.rank = 1});
  arts_printf("RANK_PID 1 %d\n", (int)getpid());
  uint64_t p[1] = {(uint64_t)s};
  arts_edt_create(relay, 1, p, 0, &(arts_edt_hint_t){.rank = 0});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() != 2) {
    arts_printf("  FAIL: needs exactly two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_printf("RANK_PID 0 %d\n", (int)getpid());
  arts_edt_create(setup, 0, NULL, 0, &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
