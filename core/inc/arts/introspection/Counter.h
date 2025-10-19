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
#ifndef ARTS_INTROSPECTION_COUNTER_H
#define ARTS_INTROSPECTION_COUNTER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

extern const char *const artsCounterNames[];
extern uint64_t countersOn;

void artsCounterConfigSetDefaultEnabled(bool enabled);
void artsCounterConfigSetEnabled(const char *name, bool enabled);

#define ARTS_COUNTERS_IS_ACTIVE() (countersOn && artsThreadInfo.localCounting)
#define COUNTERTIMESTAMP artsGetTimeStamp()
#define GETCOUNTERNAME(x) artsCounterNames[x]
#define COUNTERARRAYBLOCKSIZE 128
#define COUNTERPREFIXSIZE 1024
#define FIRSTCOUNTER edtCounter
#define LASTCOUNTER lastCounter

enum artsCounterType {
  edtCounter = 0,
  sleepCounter,
  totalCounter,
  signalEventCounter,
  signalPersistentEventCounter,
  signalEdtCounter,
  edtCreateCounter,
  eventCreateCounter,
  persistentEventCreateCounter,
  dbCreateCounter,
  smartDbCreateCounter,
  mallocMemory,
  callocMemory,
  freeMemory,
  guidAllocCounter,
  guidLookupCounter,
  getDbCounter,
  putDbCounter,
  contextSwitch,
  yield,
  remoteMemoryMove,
  lastCounter
};
typedef enum artsCounterType artsCounterType;

typedef struct {
  unsigned int threadId;
  unsigned int nodeId;
  const char *name;
  bool enabled;
  uint64_t count;
  uint64_t totalTime;
  uint64_t startTime;
  uint64_t endTime;
} artsCounter;

#if USE_COUNTER
void artsCounterTriggerEvent(artsCounterType counterType, uint64_t value);
void artsCounterTriggerTimerEvent(artsCounterType counterType, bool start);
void artsCounterConfigSetDefaultEnabled(bool enabled);
void artsCounterConfigSetEnabled(const char *name, bool enabled);

#else

#define artsCounterTriggerEvent(counterType, value) ((void)0)
#define artsCounterTriggerTimerEvent(counterType, start) ((void)0)
#define artsCounterConfigSetDefaultEnabled(enabled) ((void)0)
#define artsCounterConfigSetEnabled(name, enabled) ((void)0)

#endif // USE_COUNTER

/// Private methods
void artsCounterInitList(unsigned int threadId, unsigned int nodeId);
void artsCounterStart(unsigned int startPoint);
void artsCounterStop();
inline unsigned int artsCounterIsActive() { return countersOn; }
artsCounter *artsCounterCreate(unsigned int threadId, unsigned int nodeId,
                               const char *counterName);
artsCounter *artsCounterGet(artsCounterType counter);
artsCounter *artsCounterGetUser(unsigned int index, char *name);
void artsCounterReset(artsCounter *counter);
void artsCounterIncrement(artsCounter *counter);
void artsCounterIncrementBy(artsCounter *counter, uint64_t num);
void artsCounterTimerStart(artsCounter *counter);
void artsCounterTimerEndIncrement(artsCounter *counter);
void artsCounterTimerEndIncrementBy(artsCounter *counter, uint64_t num);
void artsCounterTimerEndOverwrite(artsCounter *counter);
void artsCounterSetStartTime(artsCounter *counter, uint64_t start);
void artsCounterSetEndTime(artsCounter *counter, uint64_t end);
void artsCounterAddEndTime(artsCounter *counter);
void artsCounterNonEmtpy(artsCounter *counter);
uint64_t artsCounterGetStartTime(artsCounter *counter);
uint64_t artsCounterGetEndTime(artsCounter *counter);
void artsCounterAddTime(artsCounter *counter, uint64_t time);

#ifdef __cplusplus
}
#endif
#endif /* ARTSCOUNTER_H */
