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
/// @file spawner_rank_probe.c
/// @brief The ranks the spawner tests drive: each rank names its pid, then
///        the run ends as its arguments say.
///
/// Once every rank is up, an EDT on each rank r prints `RANK_PID <r> <pid>`,
/// and once all have printed, rank 0 prints `ALL_RANKS_READY`.  Then:
///   - argv[1] = <ms> (0 by default): rank 0 calls arts_shutdown <ms>
///     milliseconds later;
///   - argv[2] = <rank>:<status>, if given: that rank calls arts_abort(status)
///     at once instead.
/// With SPAWNER_PROBE_NO_RANK set in the environment, a process started as a
/// rank prints SPAWNER_PROBE_RANK_STARTED and exits 1 before arts_rt: the
/// program of a run that must start no rank.

#include "arts.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(uint64_t ms) {
  struct timespec left = {.tv_sec = (time_t)(ms / 1000),
                          .tv_nsec = (long)(ms % 1000) * 1000000L};
  while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
  }
}

void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  sleep_ms(paramv[0]);
  arts_shutdown();
}

void abort_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("SPAWNER_PROBE_ABORT %u\n", (unsigned int)paramv[0]);
  arts_abort((uint8_t)paramv[0]);
}

/// paramv = {delay ms, abort rank (total ranks for none), abort status}.
void ready_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("ALL_RANKS_READY\n");
  unsigned int abort_rank = (unsigned int)paramv[1];
  if (abort_rank < arts_get_total_ranks()) {
    uint64_t status = paramv[2];
    arts_edt_create(abort_edt, 1, &status, 0,
                    &(arts_edt_hint_t){.rank = abort_rank});
    return;
  }
  uint64_t delay = paramv[0];
  arts_edt_create(end_edt, 1, &delay, 0, &(arts_edt_hint_t){.rank = 0});
}

void pid_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
             arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_printf("RANK_PID %u %d\n", arts_get_current_rank(), (int)getpid());
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  unsigned int total = arts_get_total_ranks();
  uint64_t params[3] = {(argc > 1) ? strtoull(argv[1], NULL, 10) : 0u, total,
                        0u};
  if (argc > 2) {
    char *colon = NULL;
    params[1] = strtoull(argv[2], &colon, 10);
    params[2] = (colon && *colon == ':') ? strtoull(colon + 1, NULL, 10) : 1u;
  }
  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_LATCH(total));
  arts_guid_t ready =
      arts_edt_create(ready_edt, 3, params, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(all, ready, 0, DB_MODE_NULL);
  for (unsigned int r = 0; r < total; r++) {
    uint64_t latch = (uint64_t)all;
    arts_edt_create(pid_edt, 1, &latch, 0, &(arts_edt_hint_t){.rank = r});
  }
}

int main(int argc, char **argv) {
  if (getenv("SPAWNER_PROBE_NO_RANK") != NULL && getenv("ARTS_RANK") != NULL) {
    printf("SPAWNER_PROBE_RANK_STARTED %s\n", getenv("ARTS_RANK"));
    fflush(stdout);
    _exit(1);
  }
  return arts_rt(argc, argv);
}
