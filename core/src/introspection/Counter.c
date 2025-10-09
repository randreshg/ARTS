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

#include "arts/introspection/Introspection.h"
#include "arts/runtime/Globals.h"
#include "arts/system/ArtsPrint.h"
#include "arts/utils/ArrayList.h"
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

static int counterDefaultEnabled = 1;
static int counterEnabledOverride[lastCounter];
static bool counterOverrideInitialized = false;

// Event-based counter interface implementation
void artsCounterTriggerEvent(artsCounterType counterType, uint64_t value) {
  if (!ARTS_COUNTERS_IS_ACTIVE())
    return;

  artsCounter *counter = artsCounterGet(counterType);
  if (counter) {
    if (value == 1)
      artsCounterIncrement(counter);
    else
      artsCounterIncrementBy(counter, value);
  }
}

void artsCounterTriggerTimerEvent(artsCounterType counterType, bool start) {
  if (!ARTS_COUNTERS_IS_ACTIVE())
    return;

  artsCounter *counter = artsCounterGet(counterType);
  if (counter) {
    if (start)
      artsCounterTimerStart(counter);
    else
      artsCounterTimerEndIncrement(counter);
  }
}

// Private methods
static void ensureCounterOverrides(void) {
  if (!counterOverrideInitialized) {
    for (int i = 0; i < lastCounter; i++)
      counterEnabledOverride[i] = -1;
    counterOverrideInitialized = true;
  }
}

static int counterIndexFromName(const char *name) {
  if (!name)
    return -1;
  for (int i = 0; i < lastCounter; i++) {
    const char *candidate = artsCounterNames[i];
    if (candidate && !strcasecmp(candidate, name))
      return i;
  }
  return -1;
}

void artsCounterConfigSetDefaultEnabled(bool enabled) {
  ensureCounterOverrides();
  counterDefaultEnabled = enabled ? 1 : 0;
}

void artsCounterConfigSetEnabled(const char *name, bool enabled) {
  ensureCounterOverrides();
  int index = counterIndexFromName(name);
  if (index >= 0)
    counterEnabledOverride[index] = enabled ? 1 : 0;
}

static inline bool counterIsActive(const artsCounter *counter) {
  return counter && counter->enabled && countersOn &&
         artsThreadInfo.localCounting;
}

static inline void counterRecordTiming(artsCounter *counter, uint64_t increment,
                                       bool overwrite) {
  if (!counterIsActive(counter))
    return;

  counter->endTime = COUNTERTIMESTAMP;
  uint64_t elapsed = counter->endTime - counter->startTime;
  if (overwrite)
    counter->totalTime = elapsed;
  else
    counter->totalTime += elapsed;
  counter->count += increment;
}

char *counterPrefix;
uint64_t countersOn = 0;
unsigned int printCounters = 0;
unsigned int counterStartPoint = 0;

void artsCounterInitList(unsigned int threadId, unsigned int nodeId) {
  counterPrefix = globalIntrospectionConfig.introspectionFolder;
  counterStartPoint = globalIntrospectionConfig.introspectionStartPoint;
  artsThreadInfo.counterList =
      artsNewArrayList(sizeof(artsCounter), COUNTERARRAYBLOCKSIZE);
  for (int i = FIRSTCOUNTER; i < LASTCOUNTER; i++) {
    artsPushToArrayList(artsThreadInfo.counterList,
                        artsCounterCreate(threadId, nodeId, GETCOUNTERNAME(i)));
  }
  ensureCounterOverrides();
  for (int i = FIRSTCOUNTER; i < LASTCOUNTER; i++) {
    artsCounter *counter = artsGetFromArrayList(artsThreadInfo.counterList, i);
    int override = counterEnabledOverride[i];
    if (override != -1)
      counter->enabled = override;
  }
  if (counterStartPoint == 1) {
    countersOn = COUNTERTIMESTAMP;
    printCounters = 1;
  }
}

void artsCounterStart(unsigned int startPoint) {
  if (counterStartPoint == startPoint) {
    uint64_t temp = COUNTERTIMESTAMP;
    if (!artsAtomicCswapU64(&countersOn, 0, temp)) {
      printCounters = 1;
      artsCounterTriggerTimerEvent(edtCounter, true);
    }
  }
}

/// Private methods
void artsCounterStop() {
  uint64_t temp = countersOn;
  countersOn = 0;
  ARTS_INFO("COUNTERS TIME: %lu countersOn: %lu", COUNTERTIMESTAMP - temp,
            countersOn);
}

artsCounter *artsCounterCreate(unsigned int threadId, unsigned int nodeId,
                               const char *counterName) {
  artsCounter *counter = (artsCounter *)artsCalloc(1, sizeof(artsCounter));
  counter->threadId = threadId;
  counter->nodeId = nodeId;
  counter->name = counterName;
  counter->enabled = counterDefaultEnabled ? true : false;
  artsCounterReset(counter);
  return counter;
}

artsCounter *artsCounterGetUser(unsigned int index, char *name) {
  unsigned int currentSize =
      (unsigned int)artsLengthArrayList(artsThreadInfo.counterList);
  for (unsigned int i = currentSize; i <= index; i++)
    artsPushToArrayList(
        artsThreadInfo.counterList,
        artsCounterCreate(artsThreadInfo.coreId, artsGlobalRankId, NULL));
  artsCounter *counter = artsCounterGet((artsCounterType)index);
  if (counter->name == NULL)
    counter->name = name;
  return counter;
}

artsCounter *artsCounterGet(artsCounterType counter) {
  return (artsCounter *)artsGetFromArrayList(artsThreadInfo.counterList,
                                             counter);
}

void artsCounterReset(artsCounter *counter) {
  counter->count = 0;
  counter->totalTime = 0;
  counter->startTime = 0;
  counter->endTime = 0;
}

void artsCounterIncrement(artsCounter *counter) {
  if (counterIsActive(counter))
    counter->count++;
}

void artsCounterIncrementBy(artsCounter *counter, uint64_t num) {
  if (counterIsActive(counter))
    counter->count += num;
}

void artsCounterTimerStart(artsCounter *counter) {
  if (counterIsActive(counter))
    counter->startTime = COUNTERTIMESTAMP;
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
  if (counterIsActive(counter)) {
    counter->totalTime += time;
    counter->count++;
  }
}

void artsCounterAddEndTime(artsCounter *counter) {
  if (counterIsActive(counter)) {
    if (counter->startTime && counter->endTime) {
      counter->totalTime += counter->endTime - counter->startTime;
      counter->count++;
      counter->startTime = 0;
      counter->endTime = 0;
    }
  }
}

void artsCounterNonEmtpy(artsCounter *counter) {
  if (counterIsActive(counter)) {
    if (!counter->startTime) {
      counter->startTime = counter->endTime;
      counter->endTime = COUNTERTIMESTAMP;
    }
  }
}

void artsCounterSetStartTime(artsCounter *counter, uint64_t start) {
  if (counterIsActive(counter))
    counter->startTime = start;
}

void artsCounterSetEndTime(artsCounter *counter, uint64_t end) {
  if (counterIsActive(counter))
    counter->endTime = end;
}

uint64_t artsCounterGetStartTime(artsCounter *counter) {
  return counterIsActive(counter) ? counter->startTime : 0;
}

uint64_t artsCounterGetEndTime(artsCounter *counter) {
  return counterIsActive(counter) ? counter->endTime : 0;
}
