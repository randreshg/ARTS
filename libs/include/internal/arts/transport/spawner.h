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
#ifndef ARTS_TRANSPORT_SPAWNER_H
#define ARTS_TRANSPORT_SPAWNER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/system/config.h"

/* The grace, in seconds, the spawner leaves the other ranks once a first rank
 * of the run has ended: ARTS_END_GRACE_S, a whole number in 1-86400 no lower
 * than this run's startup budget plus the shutdown bound, or by default 30
 * raised to that sum.  Refuses (ARTS_ERROR) any other value; a pure function
 * of the environment, so every process of a run reads the same answer. */
unsigned int arts_end_grace_s(void);

/* Starts the run's ranks 0..table_length-1 as children of this process, which
 * becomes no rank: a local run execs this program again, an ssh run starts
 * each rank through an ssh client.  It forwards SIGINT, SIGTERM, SIGHUP and
 * SIGALRM to every rank still running, reaps every rank, kills those still
 * running the end grace after the first one ended, and ends the process with
 * the highest status a rank ended with (128 + the signal for a rank a signal
 * ended; 127 for a rank whose exec failed), or 75 when a fork failed.  Called
 * before any thread of the process exists, and only by the process that no
 * launcher gave a rank. */
_Noreturn void arts_spawn(struct arts_config_s *config, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
