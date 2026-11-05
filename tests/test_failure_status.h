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

#ifndef ARTS_TEST_FAILURE_STATUS_H
#define ARTS_TEST_FAILURE_STATUS_H

/* Carries a failure detected inside an EDT out to the process exit status.
 *
 * A test that only PRINTS its verdict cannot fail: the checks run on worker
 * threads, and main() regains control only after arts_rt() with nothing left
 * to look at.  The counter below is the channel — bumped where the failure is
 * found, returned by arts_rt() through arts_app_exit_status() below.
 *
 * Atomic because the checks run on whatever worker the scheduler picked, and
 * several may find a failure at once.
 *
 * SCOPE: one process, so this carries a rank's OWN verdict.  Every rank's
 * main() returns arts_rt()'s result, which carries this status; the local
 * launcher judges each spawned rank's exit status, so a failing peer makes
 * the master's arts_rt() return non-zero.
 */

#include "arts.h"

#include <stdatomic.h>

static _Atomic(unsigned int) arts_test_failures;

static inline void arts_test_fail(void) {
  atomic_fetch_add_explicit(&arts_test_failures, 1u, memory_order_relaxed);
}

/* Exit status for a test's main(): non-zero once anything failed. */
static inline int arts_test_status(void) {
  return atomic_load_explicit(&arts_test_failures, memory_order_relaxed) ? 1 : 0;
}

/* The program's verdict as arts_rt's result.  Defined strongly here, so a
 * program includes this header in exactly one translation unit. */
int arts_app_exit_status(void) { return arts_test_status(); }

#endif /* ARTS_TEST_FAILURE_STATUS_H */
