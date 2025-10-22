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

#include "arts/introspection/Introspection.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "arts/introspection/Counter.h"
#include "arts/introspection/JsonWriter.h"
#include "arts/introspection/Metrics.h"
#include "arts/runtime/Globals.h"

void artsIntrospectionStart(unsigned int startPoint) {
  artsCounterStart(startPoint);
  artsMetricsStart(startPoint);
}

void artsIntrospectionStop() {
  artsCounterStop();
  artsMetricsStop();

  if (artsNodeInfo.introspectionFolder) {
    for (unsigned int threadId = 0; threadId < artsNodeInfo.totalThreadCount;
         threadId++) {
      artsIntrospectionWriteOutput(artsNodeInfo.introspectionFolder,
                                   artsGlobalRankId, threadId);
    }
  }
}

void artsIntrospectionWriteOutput(const char *outputFolder, unsigned int nodeId,
                                  unsigned int threadId) {
  if (!outputFolder)
    return;

  struct stat st = {0};
  if (stat(outputFolder, &st) == -1)
    mkdir(outputFolder, 0755);

  char filename[1024];
  snprintf(filename, sizeof(filename), "%s/introspection_n%u_t%u.json",
           outputFolder, nodeId, threadId);

  FILE *fp = fopen(filename, "w");
  if (!fp)
    return;

  artsJsonWriter writer;
  artsJsonWriterInit(&writer, fp, 2);

  artsJsonWriterBeginObject(&writer, NULL);

  artsJsonWriterBeginObject(&writer, "metadata");
  artsJsonWriterWriteUInt64(&writer, "nodeId", nodeId);
  artsJsonWriterWriteUInt64(&writer, "threadId", threadId);
  artsJsonWriterWriteUInt64(&writer, "timestamp", (uint64_t)time(NULL));
  artsJsonWriterWriteString(&writer, "version", "1.0");
  artsJsonWriterWriteUInt64(&writer, "startPoint",
                            artsNodeInfo.introspectionStartPoint);
  if (artsNodeInfo.introspectionFolder) {
    artsJsonWriterWriteString(&writer, "introspectionFolder",
                              artsNodeInfo.introspectionFolder);
  }
  artsJsonWriterEndObject(&writer);

  artsJsonWriterBeginObject(&writer, "counters");
  for (uint64_t i = 0; i < lastCounter; i++) {
    artsCounter *counter = &artsThreadInfo.counterList[i];
    if (artsCounterEnabled[i]) {
      artsJsonWriterBeginObject(&writer, artsCounterNames[i]);
      artsJsonWriterWriteUInt64(&writer, "count", counter->count);
      artsJsonWriterWriteUInt64(&writer, "totalTime", counter->totalTime);
      if (counter->count > 0) {
        artsJsonWriterWriteUInt64(&writer, "avgTime",
                                  counter->totalTime / counter->count);
      } else {
        artsJsonWriterWriteUInt64(&writer, "avgTime", 0);
      }
      artsJsonWriterEndObject(&writer);
    }
  }
  artsJsonWriterEndObject(&writer);

  artsJsonWriterBeginObject(&writer, "metrics");
  for (unsigned int i = 0; i < artsLastMetricType; i++) {
    artsJsonWriterBeginObject(&writer, artsMetricName[i]);

    uint64_t threadTotal = artsMetricsGetTotal(i, artsThread);
    uint64_t nodeTotal = artsMetricsGetTotal(i, artsNode);
    uint64_t systemTotal = artsMetricsGetTotal(i, artsSystem);

    artsJsonWriterBeginObject(&writer, "thread");
    artsJsonWriterWriteUInt64(&writer, "totalCount", threadTotal);
    artsJsonWriterEndObject(&writer);

    artsJsonWriterBeginObject(&writer, "node");
    artsJsonWriterWriteUInt64(&writer, "totalCount", nodeTotal);
    artsJsonWriterEndObject(&writer);

    artsJsonWriterBeginObject(&writer, "system");
    artsJsonWriterWriteUInt64(&writer, "totalCount", systemTotal);
    artsJsonWriterEndObject(&writer);

    artsJsonWriterEndObject(&writer);
  }
  artsJsonWriterEndObject(&writer);

  artsJsonWriterEndObject(&writer);
  artsJsonWriterFinish(&writer);
  fputc('\n', fp);
  fclose(fp);
}
