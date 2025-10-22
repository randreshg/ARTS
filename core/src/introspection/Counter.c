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
#include "arts/introspection/Counter.h"

#include <strings.h>
#include <sys/stat.h>

#include "arts/arts.h"
#include "arts/introspection/Introspection.h"
#include "arts/system/ArtsPrint.h"
#include "arts/utils/Atomics.h"

const char *const artsCounterNames[] = {"edtCounter",
                                        "sleepCounter",
                                        "totalCounter",
                                        "signalEventCounter",
                                        "signalPersistentEventCounter",
                                        "signalEdtCounter",
                                        "edtCreateCounter",
                                        "eventCreateCounter",
                                        "persistentEventCreateCounter",
                                        "dbCreateCounter",
                                        "smartDbCreateCounter",
                                        "mallocMemory",
                                        "callocMemory",
                                        "freeMemory",
                                        "guidAllocCounter",
                                        "guidLookupCounter",
                                        "getDbCounter",
                                        "putDbCounter",
                                        "contextSwitch",
                                        "yield",
                                        "remoteMemoryMove"};
static uint64_t countersOn = 0;

static inline void counterRecordTiming(artsCounter *counter, uint64_t increment,
                                       bool overwrite) {
  counter->endTime = artsGetTimeStamp();
  uint64_t elapsed = counter->endTime - counter->startTime;
  if (overwrite) {
    counter->totalTime = elapsed;
  } else {
    counter->totalTime += elapsed;
  }
  counter->count += increment;
}

void artsCounterListInit(const char *introspectionFolder,
                         unsigned int introspectionStartPoint,
                         unsigned int threadId, unsigned int nodeId) {
  for (unsigned int i = 0; i < lastCounter; i++) {
    artsThreadInfo.counterList[i] = (artsCounter){.threadId = threadId,
                                                  .nodeId = nodeId,
                                                  .count = 0,
                                                  .totalTime = 0,
                                                  .startTime = 0,
                                                  .endTime = 0};
  }
}

void artsCounterStart(unsigned int startPoint) {
  if (artsNodeInfo.introspectionStartPoint == startPoint) {
    uint64_t temp = artsGetTimeStamp();
    if (!artsAtomicCswapU64(&countersOn, 0, temp)) {
      EDT_COUNTER_START();
    }
  }
}

/// Private methods
void artsCounterStop() {
  uint64_t temp = countersOn;
  countersOn = 0;
  ARTS_INFO("COUNTERS TIME: %lu countersOn: %lu", artsGetTimeStamp() - temp,
            countersOn);
}

void artsCounterReset(artsCounter *counter) {
  counter->count = 0;
  counter->totalTime = 0;
  counter->startTime = 0;
  counter->endTime = 0;
}

void artsCounterIncrement(artsCounter *counter) { counter->count++; }

void artsCounterIncrementBy(artsCounter *counter, uint64_t num) {
  counter->count += num;
}

void artsCounterTimerStart(artsCounter *counter) {
  counter->startTime = artsGetTimeStamp();
}

void artsCounterTimerEndIncrement(artsCounter *counter) {
  counterRecordTiming(counter, 1, false);
}

void artsCounterTimerEndIncrementBy(artsCounter *counter, uint64_t num) {
  counterRecordTiming(counter, num, false);
}

void artsCounterTimerEndOverwrite(artsCounter *counter) {
  counterRecordTiming(counter, 1, true);
}

void artsCounterAddTime(artsCounter *counter, uint64_t time) {
  counter->totalTime += time;
  counter->count++;
}

void artsCounterAddEndTime(artsCounter *counter) {
  if (counter->startTime && counter->endTime) {
    counter->totalTime += counter->endTime - counter->startTime;
    counter->count++;
    counter->startTime = 0;
    counter->endTime = 0;
  }
}

void artsCounterNonEmpty(artsCounter *counter) {
  if (!counter->startTime) {
    counter->startTime = counter->endTime;
    counter->endTime = artsGetTimeStamp();
  }
}

void artsCounterSetStartTime(artsCounter *counter, uint64_t start) {
  counter->startTime = start;
}

void artsCounterSetEndTime(artsCounter *counter, uint64_t end) {
  counter->endTime = end;
}

uint64_t artsCounterGetStartTime(artsCounter *counter) {
  return counter->startTime;
}

uint64_t artsCounterGetEndTime(artsCounter *counter) {
  return counter->endTime;
}
