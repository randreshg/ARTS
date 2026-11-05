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
#ifndef ARTS_SYSTEM_SHUTDOWN_H
#define ARTS_SYSTEM_SHUTDOWN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* What made this rank begin its end.  An origin fits in three bits, and 0
 * names none. */
enum arts_exit_origin {
  ARTS_EXIT_APP = 1,       /* the program ends the run */
  ARTS_EXIT_SIGNAL,        /* a terminating signal reached the process */
  ARTS_EXIT_FAULT,         /* a thread of the process faulted */
  ARTS_EXIT_RUNTIME_ERROR, /* the runtime met a fatal error */
  ARTS_EXIT_WIRE,          /* a peer's SHUTDOWN, taken in by a progress loop */
  ARTS_EXIT_SENTINEL,      /* a peer's closed connection, seen by its probe */
  ARTS_EXIT_BACKPRESSURE,  /* either, seen where nothing may be dispatched */
};

/* Whether the end begins on this rank, so it posts SHUTDOWN to its peers.  A
 * rank that learned of another rank's end posts nothing: every rank that
 * originates one posts, and every other rank learns of it from a post or
 * from a closed connection. */
static inline bool arts_exit_origin_originates(enum arts_exit_origin o) {
  switch (o) {
  case ARTS_EXIT_APP:
  case ARTS_EXIT_SIGNAL:
  case ARTS_EXIT_FAULT:
  case ARTS_EXIT_RUNTIME_ERROR:
    return true;
  case ARTS_EXIT_WIRE:
  case ARTS_EXIT_SENTINEL:
  case ARTS_EXIT_BACKPRESSURE:
    return false;
  }
  return false;
}

static inline const char *arts_exit_origin_name(enum arts_exit_origin o) {
  switch (o) {
  case ARTS_EXIT_APP:
    return "APP";
  case ARTS_EXIT_SIGNAL:
    return "SIGNAL";
  case ARTS_EXIT_FAULT:
    return "FAULT";
  case ARTS_EXIT_RUNTIME_ERROR:
    return "RUNTIME_ERROR";
  case ARTS_EXIT_WIRE:
    return "WIRE";
  case ARTS_EXIT_SENTINEL:
    return "SENTINEL";
  case ARTS_EXIT_BACKPRESSURE:
    return "BACKPRESSURE";
  }
  return "NONE";
}

#ifdef __cplusplus
}
#endif
#endif
