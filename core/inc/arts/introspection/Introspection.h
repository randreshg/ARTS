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
#ifndef ARTS_INTROSPECTION_INTROSPECTION_H
#define ARTS_INTROSPECTION_INTROSPECTION_H

#include <stdbool.h>

#include "arts/introspection/Counter.h"
#include "arts/introspection/Metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(USE_COUNTERS) || defined(USE_METRICS)

typedef struct {
  char *introspectionFolder;
  unsigned int introspectionStartPoint;
  unsigned int introspectionTraceLevel;
  bool counterDefault;
  bool enableCounters;
  bool enableMetrics;
} artsIntrospectionConfig;

extern artsIntrospectionConfig globalIntrospectionConfig;

void artsIntrospectionInit(const char *configFile);
void artsIntrospectionStart(unsigned int startPoint);
void artsIntrospectionStop();
void artsIntrospectionWriteOutput(const char *outputFolder, unsigned int nodeId,
                                  unsigned int threadId, bool writeCounters,
                                  bool writeMetrics);

/// Counter Functions
void artsCounterTriggerEvent(artsCounterType counterType, uint64_t value);
void artsCounterTriggerTimerEvent(artsCounterType counterType, bool start);
void artsCounterConfigSetDefaultEnabled(bool enabled);
void artsCounterConfigSetEnabled(const char *name, bool enabled);

/// Metrics Functions
void artsMetricsTriggerEvent(artsMetricType metricType, artsMetricLevel level,
                             uint64_t value);
void artsMetricsTriggerTimerEvent(artsMetricType metricType,
                                  artsMetricLevel level, bool start);
void artsMetricsConfigSetDefaultEnabled(bool enabled);
void artsMetricsConfigSetEnabled(const char *name, bool enabled);

#else

#define artsIntrospectionInit(configFile) ((void)0)
#define artsIntrospectionStart(startPoint) ((void)0)
#define artsIntrospectionStop() ((void)0)
#define artsIntrospectionWriteOutput(outputFolder, nodeId, threadId,           \
                                     writeCounters, writeMetrics)              \
  ((void)0)

#define artsCounterTriggerEvent(counterType, value) ((void)0)
#define artsCounterTriggerTimerEvent(counterType, start) ((void)0)
#define artsCounterConfigSetDefaultEnabled(enabled) ((void)0)
#define artsCounterConfigSetEnabled(name, enabled) ((void)0)

#define artsMetricsTriggerEvent(metricType, level, value) ((void)0)
#define artsMetricsTriggerTimerEvent(metricType, level, start) ((void)0)
#define artsMetricsConfigSetDefaultEnabled(enabled) ((void)0)
#define artsMetricsConfigSetEnabled(name, enabled) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* ARTSINTROSPECTION_H */
