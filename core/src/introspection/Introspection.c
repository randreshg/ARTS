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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "arts/introspection/Counter.h"
#include "arts/introspection/JsonWriter.h"
#include "arts/introspection/Metrics.h"
#include "arts/runtime/Globals.h"

artsIntrospectionConfig globalIntrospectionConfig = {0};

static void trimWhitespace(char *str) {
  char *start = str;
  char *end;

  while (isspace((unsigned char)*start))
    start++;

  if (*start == 0)
    return;

  end = start + strlen(start) - 1;
  while (end > start && isspace((unsigned char)*end))
    end--;

  end[1] = '\0';

  memmove(str, start, end - start + 2);
}

static bool parseDirective(const char *line, artsIntrospectionConfig *config) {
  if (line[0] != '@')
    return false;

  char directive[256];
  char value[256];

  if (sscanf(line, "@%s %[^\n]", directive, value) != 2)
    return false;

  trimWhitespace(value);

  if (strcmp(directive, "introspectionFolder") == 0) {
    config->introspectionFolder = strdup(value);
  } else if (strcmp(directive, "introspectionStartPoint") == 0) {
    config->introspectionStartPoint = atoi(value);
  } else if (strcmp(directive, "introspectionTraceLevel") == 0) {
    config->introspectionTraceLevel = atoi(value);
  } else if (strcmp(directive, "counterDefault") == 0) {
    config->counterDefault =
        (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0 ||
         strcmp(value, "1") == 0);
  } else if (strcmp(directive, "enableCounters") == 0) {
    config->enableCounters =
        (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0 ||
         strcmp(value, "1") == 0);
  } else if (strcmp(directive, "enableMetrics") == 0) {
    config->enableMetrics =
        (strcasecmp(value, "on") == 0 || strcasecmp(value, "true") == 0 ||
         strcmp(value, "1") == 0);
  }

  return true;
}

static bool matchPattern(const char *name, const char *pattern) {
  while (*pattern) {
    if (*pattern == '*') {
      pattern++;
      if (!*pattern)
        return true;
      while (*name) {
        if (matchPattern(name, pattern))
          return true;
        name++;
      }
      return false;
    }
    if (*pattern == *name) {
      pattern++;
      name++;
    } else {
      return false;
    }
  }
  return !*name;
}

static void applyMetricConfig(const char *name, const char *keyValue,
                              bool isPattern) {
  if (!keyValue)
    return;

  if (isPattern) {
    for (int i = 0; i < artsLastMetricType; i++) {
      if (matchPattern(artsMetricName[i], name)) {
        if (strstr(keyValue, "disabled") != NULL) {
          artsMetricsConfigSetEnabled(artsMetricName[i], false);
        }
      }
    }
  } else {
    if (strstr(keyValue, "disabled") != NULL) {
      artsMetricsConfigSetEnabled(name, false);
    }
  }
}

static void parseCounterLine(const char *line) {
  char name[256];
  char value[256];

  if (strncmp(line, "counter ", 8) == 0) {
    if (sscanf(line, "counter %s %s", name, value) == 2) {
      bool enabled =
          (strcasecmp(value, "on") == 0 || strcasecmp(value, "enabled") == 0 ||
           strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
      artsCounterConfigSetEnabled(name, enabled);
    }
  }
}

static void parseMetricLine(const char *line) {
  char name[256];
  char rest[512];

  if (strncmp(line, "metric-defaults", 15) == 0) {
    if (sscanf(line, "metric-defaults %[^\n]", rest) == 1) {
      artsMetricsConfigSetDefaultEnabled(true);
    }
  } else if (strncmp(line, "metric-group", 12) == 0) {
    if (sscanf(line, "metric-group %s %[^\n]", name, rest) == 2) {
      applyMetricConfig(name, rest, true);
    }
  } else if (strncmp(line, "metric ", 7) == 0) {
    if (sscanf(line, "metric %s %[^\n]", name, rest) == 2) {
      applyMetricConfig(name, rest, false);
    }
  }
}

#if defined(USE_COUNTER) || defined(USE_METRICS)

void artsIntrospectionInit(const char *configFile) {
  globalIntrospectionConfig.introspectionFolder = strdup("./introspection");
  globalIntrospectionConfig.counterDefault = true;
  globalIntrospectionConfig.introspectionStartPoint = 1;
  globalIntrospectionConfig.introspectionTraceLevel = 0;

#ifdef USE_COUNTER
  globalIntrospectionConfig.enableCounters = true;
#else
  globalIntrospectionConfig.enableCounters = false;
#endif // USE_COUNTER

#ifdef USE_METRICS
  globalIntrospectionConfig.enableMetrics = true;
#else
  globalIntrospectionConfig.enableMetrics = false;
#endif // USE_METRICS

  if (!configFile)
    return;

  FILE *fp = fopen(configFile, "r");
  if (!fp)
    return;

  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  while ((read = getline(&line, &len, fp)) != -1) {
    trimWhitespace(line);

    if (line[0] == '#' || line[0] == '\0')
      continue;

    if (!parseDirective(line, &globalIntrospectionConfig)) {
      if (globalIntrospectionConfig.enableMetrics)
        parseMetricLine(line);
      if (globalIntrospectionConfig.enableCounters)
        parseCounterLine(line);
    }
  }

  fclose(fp);

  if (globalIntrospectionConfig.enableCounters) {
    artsCounterConfigSetDefaultEnabled(
        globalIntrospectionConfig.counterDefault);
  }

  if (globalIntrospectionConfig.enableMetrics) {
    artsMetricsConfigSetDefaultEnabled(true);
#ifdef USE_METRICS
    artsMetricsInitIntrospector(
        globalIntrospectionConfig.introspectionStartPoint);
#endif // USE_METRICS
  }

  if (line)
    free(line);
}

void artsIntrospectionStart(unsigned int startPoint) {
#ifdef USE_COUNTER
  artsCounterStart(startPoint);
#endif // USE_COUNTER
#ifdef USE_METRICS
  artsMetricsStart(startPoint);
#endif // USE_METRICS
}

void artsIntrospectionStop() {
#ifdef USE_COUNTER
  if (globalIntrospectionConfig.enableCounters) {
    artsCounterStop();
  }
#endif // USE_COUNTER
#ifdef USE_METRICS
  if (globalIntrospectionConfig.enableMetrics) {
    artsMetricsStop();
  }
#endif // USE_METRICS

  if (globalIntrospectionConfig.introspectionFolder) {
    for (unsigned int threadId = 0; threadId < artsNodeInfo.totalThreadCount;
         threadId++) {
      artsIntrospectionWriteOutput(
          globalIntrospectionConfig.introspectionFolder, artsGlobalRankId,
          threadId, globalIntrospectionConfig.enableCounters,
          globalIntrospectionConfig.enableMetrics);
    }
  }
}

void artsIntrospectionWriteOutput(const char *outputFolder, unsigned int nodeId,
                                  unsigned int threadId, bool writeCounters,
                                  bool writeMetrics) {
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
  artsJsonWriterWriteUInt64(&writer, "traceLevel",
                            globalIntrospectionConfig.introspectionTraceLevel);
  artsJsonWriterWriteUInt64(&writer, "startPoint",
                            globalIntrospectionConfig.introspectionStartPoint);
  if (globalIntrospectionConfig.introspectionFolder) {
    artsJsonWriterWriteString(&writer, "introspectionFolder",
                              globalIntrospectionConfig.introspectionFolder);
  }
  artsJsonWriterEndObject(&writer);

#ifdef USE_COUNTER
  if (writeCounters && artsThreadInfo.counterList) {
    artsJsonWriterBeginObject(&writer, "counters");
    uint64_t length = artsLengthArrayList(artsThreadInfo.counterList);
    for (uint64_t i = 0; i < length; i++) {
      artsCounter *counter =
          (artsCounter *)artsGetFromArrayList(artsThreadInfo.counterList, i);
      if (counter && counter->name && counter->enabled) {
        artsJsonWriterBeginObject(&writer, counter->name);
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
  }
#endif // USE_COUNTER

#ifdef USE_METRICS
  if (writeMetrics) {
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
  }
#endif // USE_METRICS

  artsJsonWriterEndObject(&writer);
  artsJsonWriterFinish(&writer);
  fputc('\n', fp);
  fclose(fp);
}

#endif // USE_COUNTER || USE_METRICS
