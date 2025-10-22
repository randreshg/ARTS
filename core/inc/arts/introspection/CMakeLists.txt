###############################################################################
## This material was prepared as an account of work sponsored by an agency 
## of the United States Government.  Neither the United States Government 
## nor the United States Department of Energy, nor Battelle, nor any of 
## their employees, nor any jurisdiction or organization that has cooperated 
## in the development of these materials, makes any warranty, express or 
## implied, or assumes any legal liability or responsibility for the accuracy, 
## completeness, or usefulness or any information, apparatus, product, 
## software, or process disclosed, or represents that its use would not 
## infringe privately owned rights.
##
## Reference herein to any specific commercial product, process, or service 
## by trade name, trademark, manufacturer, or otherwise does not necessarily 
## constitute or imply its endorsement, recommendation, or favoring by the 
## United States Government or any agency thereof, or Battelle Memorial 
## Institute. The views and opinions of authors expressed herein do not 
## necessarily state or reflect those of the United States Government or 
## any agency thereof.
##
##                      PACIFIC NORTHWEST NATIONAL LABORATORY
##                                  operated by
##                                    BATTELLE
##                                    for the
##                      UNITED STATES DEPARTMENT OF ENERGY
##                         under Contract DE-AC05-76RL01830
##
## Copyright 2019 Battelle Memorial Institute
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##    https://www.apache.org/licenses/LICENSE-2.0 
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
## WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
## License for the specific language governing permissions and limitations
###############################################################################

# =============================================================================
# Define All Counters and Metrics
# =============================================================================

# List all your counters
set(COUNTER_TYPES
    edtCounter
    sleepCounter
    totalCounter
    signalEventCounter
    signalPersistentEventCounter
    signalEdtCounter
    edtCreateCounter
    eventCreateCounter
    persistentEventCreateCounter
    dbCreateCounter
    smartDbCreateCounter
    mallocMemory
    callocMemory
    freeMemory
    guidAllocCounter
    guidLookupCounter
    getDbCounter
    putDbCounter
    contextSwitch
    yield
    remoteMemoryMove
)

# List all your metrics
set(METRIC_TYPES
    artsEdtThroughput
    artsEdtQueue
    artsEdtStealAttempt
    artsEdtSteal
    artsEdtLastLocalHit
    artsEdtSignalThroughput
    artsEventSignalThroughput
    artsPersistentEventSignalThroughput
    artsGetBW
    artsPutBW
    artsNetworkSendBW
    artsNetworkRecieveBW
    artsNetworkQueuePush
    artsNetworkQueuePop
    artsYieldBW
    artsGpuEdt
    artsGpuGC
    artsGpuGCBW
    artsGpuBWPush
    artsGpuBWPull
    artsGpuBufferFlush
    artsGpuSync
    artsGpuSyncDelete
    artsMallocBW
    artsFreeBW
    artsRemoteShutdownMsg
    artsRemoteEdtSignalMsg
    artsRemoteDbRequestMsg
    artsRemoteDbSendMsg
    artsDbCount
)

# Set introspection configuration
set(INTROSPECTION_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/introsp.cfg" CACHE FILEPATH "Path to introspection configuration file")
if(NOT EXISTS "${INTROSPECTION_CONFIG_FILE}")
    message(FATAL_ERROR "Introspection configuration file not found: ${INTROSPECTION_CONFIG_FILE}")
endif()
message(STATUS "Using introspection configuration file: ${INTROSPECTION_CONFIG_FILE}")

# Parse configuration file
file(STRINGS "${INTROSPECTION_CONFIG_FILE}" INTROSPECTION_CONFIG_LINES)
set(ENABLED_COUNT 0)
set(DISABLED_COUNT 0)

foreach(LINE ${INTROSPECTION_CONFIG_LINES})
    if(LINE MATCHES "^[ \\t]*#" OR LINE MATCHES "^[ \\t]*$")
        continue()
    endif()
    if(LINE MATCHES "^([A-Za-z0-9_]+)[ \\t]*=[ \\t]*(ON|OFF)")
        set(KEY ${CMAKE_MATCH_1})
        set(VALUE ${CMAKE_MATCH_2})

        # Set CMake variable for this metric/counter
        if(VALUE STREQUAL "ON")
            set(ENABLE_${KEY} 1)
            math(EXPR ENABLED_COUNT "${ENABLED_COUNT} + 1")
        else()
            set(ENABLE_${KEY} 0)
            math(EXPR DISABLED_COUNT "${DISABLED_COUNT} + 1")
        endif()
    endif()
endforeach()

message(STATUS "Introspection configuration: ${ENABLED_COUNT} enabled, ${DISABLED_COUNT} disabled")

# Generate introspection header file
set(GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/Introspection.h")

# Start writing the header
file(WRITE ${GENERATED_HEADER}
    "/* Introspection.h - AUTOMATICALLY GENERATED - DO NOT EDIT */

#ifndef ARTS_INTROSPECTION_INTROSPECTION_H
#define ARTS_INTROSPECTION_INTROSPECTION_H

#ifdef __cplusplus
extern \"C\" {
#endif

/*
* This file is automatically generated by CMake
* Source: ${INTROSPECTION_CONFIG_FILE}
*
* To modify: Edit CMakeLists.txt and re-run cmake
*/

#include <stdbool.h>

#include \"arts/introspection/Counter.h\"
#include \"arts/introspection/Metrics.h\"

/* ============================================================================
* Enable/Disable Flags
* ============================================================================ */

    ")

# Generate enable flags for counters
file(APPEND ${GENERATED_HEADER} "/* Counter Flags */\n")
foreach(COUNTER ${COUNTER_TYPES})
    if(NOT DEFINED ENABLE_${COUNTER})
        set(ENABLE_${COUNTER} 0)
    endif()
    file(APPEND ${GENERATED_HEADER} "#define ENABLE_${COUNTER} ${ENABLE_${COUNTER}}\n")
endforeach()

file(APPEND ${GENERATED_HEADER} "\n/* Metric Flags */\n")
foreach(METRIC ${METRIC_TYPES})
    if(NOT DEFINED ENABLE_${METRIC})
        set(ENABLE_${METRIC} 0)
    endif()
    file(APPEND ${GENERATED_HEADER} "#define ENABLE_${METRIC} ${ENABLE_${METRIC}}\n")
endforeach()

# Generate counter macros automatically
file(APPEND ${GENERATED_HEADER} "
/* ============================================================================
 * Counter Macros - Automatically Generated
 * ============================================================================ */

")

set(COUNTER_INDEX 0)
foreach(COUNTER ${COUNTER_TYPES})
    # Convert counter name to macro name (e.g., edtCounter -> ARTS_COUNT_EDT_COUNTER)
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" MACRO_NAME ${COUNTER})
    string(TOUPPER ${MACRO_NAME} MACRO_NAME)
    set(TIMER_START "${MACRO_NAME}_START")
    set(TIMER_STOP "${MACRO_NAME}_STOP")
    set(EVENT_NAME "INCREMENT_${MACRO_NAME}_BY")

    file(APPEND ${GENERATED_HEADER}
        "#if ENABLE_${COUNTER}
    #define ${TIMER_START}() artsCounterTimerStart(&artsThreadInfo.counterList[${COUNTER_INDEX}])
    #define ${TIMER_STOP}() artsCounterTimerEndIncrement(&artsThreadInfo.counterList[${COUNTER_INDEX}])
    #define ${EVENT_NAME}(value) artsCounterIncrementBy(&artsThreadInfo.counterList[${COUNTER_INDEX}], value)
#else
    #define ${TIMER_START}() ((void)0)
    #define ${TIMER_STOP}() ((void)0)
    #define ${EVENT_NAME}(value) ((void)0)
#endif

")
    math(EXPR COUNTER_INDEX "${COUNTER_INDEX} + 1")
endforeach()

# Generate metric macros automatically
file(APPEND ${GENERATED_HEADER} "
/* ============================================================================
 * Metric Macros - Automatically Generated
 * ============================================================================ */

")

foreach(METRIC ${METRIC_TYPES})
    # Convert metric name to macro name
    string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" MACRO_NAME ${METRIC})
    string(TOUPPER ${MACRO_NAME} MACRO_NAME)
    set(MACRO_NAME "ARTS_METRIC_${MACRO_NAME}")

    file(APPEND ${GENERATED_HEADER}
        "#if ENABLE_${METRIC}
    #define ${MACRO_NAME}(value, time) artsMetricRecord(${METRIC}, value, time)
#else
    #define ${MACRO_NAME}(value, time) ((void)0)
#endif

")
endforeach()

# Generate counter enabled array
file(APPEND ${GENERATED_HEADER} "
/* ============================================================================
 * Counter Enabled Array - Automatically Generated
 * ============================================================================ */

/* Static array mapping counter index to enabled status */
static const bool artsCounterEnabled[] = {
")

foreach(COUNTER ${COUNTER_TYPES})
    if(NOT DEFINED ENABLE_${COUNTER})
        set(ENABLE_${COUNTER} 0)
    endif()
    file(APPEND ${GENERATED_HEADER} "    ${ENABLE_${COUNTER}},  /* ${COUNTER} */\n")
endforeach()

file(APPEND ${GENERATED_HEADER} "};\n\n
void artsIntrospectionStart(unsigned int startPoint);
void artsIntrospectionStop();
void artsIntrospectionWriteOutput(const char *outputFolder, unsigned int nodeId,
                                unsigned int threadId);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* ARTS_INTROSPECTION_INTROSPECTION_H */
")

message(STATUS "Generated: ${GENERATED_HEADER}")
message(STATUS "  Counters: ${CMAKE_CURRENT_LIST_LENGTH} types")
