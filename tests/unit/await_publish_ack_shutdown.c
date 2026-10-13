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

/// @file await_publish_ack_shutdown.c
/// @brief A release blocked on the home's answer escapes when a shutdown
///        overtakes it.
///
/// A non-home RW release under a write-through publish (VAL, INV) or under
/// FLUSH's write-back does not return until the home has answered: it parks
/// on a heap waiter in `arts_db_await_ack` that the home's reply posts.  Once
/// teardown starts the progress threads stop draining, so that reply may
/// never come.  `arts_db_await_ack` must escape on its coarse shutdown
/// re-check (sem_timedwait + shutdown_state poll), so the blocked releaser is
/// not stranded and the shutdown does not hang.  The waiter an escape
/// abandons stays allocated, so a reply that still arrives posts into live
/// memory.
///
/// Black-box driver: a steady stream of cross-rank RW handoffs (each release
/// waiting for the home's answer, so releasers are frequently mid-await),
/// after which an EDT triggers `arts_shutdown()` concurrently with RW releases
/// still draining.  Correct behavior = the process tears down cleanly (every
/// blocked releaser escapes); a regressed escape hangs → ctest TIMEOUT FAIL.
///
/// Config gate: compiled out under the WB write policy and under EXCL, where
/// no release in this driver waits for the home's answer to its bytes (VAL WB
/// publishes nothing and INV WB's publish carries none; an EXCL release waits
/// only for the CTS of a landing-less announce).

#include "arts.h"

#include <stdint.h>
#include <stdio.h>

#if defined(ARTS_PROTOCOL_EXCL) || defined(ARTS_WRITE_POLICY_WB)
int main(void) {
  printf("SKIP await_publish_ack_shutdown: no release here waits for the "
         "home's answer to its bytes\n");
  return 0;
}
#else

#define WARMUP 100u
#define BURST 64u

static void rw_bump_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d != NULL) {
    d[0] = d[0] + 1u;
  }
}

static void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  /* Trigger teardown.  RW releases from the concurrent burst below may still be
   * mid-await on their waiters; each must escape via the shutdown re-check. */
  arts_printf("PASS: await_publish_ack_shutdown reached shutdown\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== await_publish_ack_shutdown ===\n");

  unsigned int nranks = arts_get_total_ranks();

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(unsigned int), ARTS_DB, ARTS_DB_PROP_NONE,
                     &(arts_db_hint_t){.rank = 0});
  ((unsigned int *)ptr)[0] = 0u;
  arts_db_release(db, DB_MODE_RW);

  /* Warmup: many completed RW handoffs.  Each non-home release waits for the
   * home's answer, exercising the normal await/post path before shutdown. */
  for (unsigned int it = 0; it < WARMUP; it++) {
    arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    unsigned int hops = (nranks > 1) ? (2u * nranks) : 2u;
    for (unsigned int h = 0; h < hops; h++) {
      unsigned int target = (nranks > 1) ? (h % nranks) : 0u;
      arts_guid_t e = arts_edt_create(
          rw_bump_edt, 0, NULL, 1,
          &(arts_edt_hint_t){.rank = target, .finish_event = fe});
      arts_add_dependence(db, e, 0, DB_MODE_RW);
    }
    arts_event_wait(fe);
  }

  /* Final burst: a stream of RW handoffs whose releases are deliberately NOT
   * waited on, then a shutdown_edt that triggers teardown concurrently with the
   * in-flight releases.  Releasers caught mid-await must escape on the shutdown
   * re-check; if any is stranded the runtime never finishes draining and ctest
   * TIMEOUT fails.  No member finish-event here: shutdown_edt is its own scope.
   */
  for (unsigned int h = 0; h < BURST; h++) {
    unsigned int target = (nranks > 1) ? (h % nranks) : 0u;
    arts_guid_t e = arts_edt_create(rw_bump_edt, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = target});
    arts_add_dependence(db, e, 0, DB_MODE_RW);
  }

  /* The shutdown EDT runs on the home; it executes after being scheduled, by
   * which point burst releases are in flight. */
  arts_edt_create(shutdown_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly: their exit status
     reaches nobody else, and a run with a dead rank did not succeed. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}

#endif /* skip */
