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

// #include "artsDb.h"x
#include "arts/arts.h"
#include "arts/gas/Guid.h"
#include "arts/gas/RouteTable.h"
#include "arts/introspection/Introspection.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/compute/EdtFunctions.h"
#include "arts/runtime/memory/DbFunctions.h"
#include "arts/runtime/sync/TerminationDetection.h"
#include "artsSmartDb.h"
#include <assert.h>
#include <math.h>
#include <string.h>

// Constants for memory management
#define HOT_ACCESS_THRESHOLD 1000 // Number of accesses to consider data "hot"
#define CONTENTION_THRESHOLD 0.7f // Threshold for considering data contended
#define MIGRATION_COST_THRESHOLD 0.5f // Cost threshold for migration
#define REPLICATION_THRESHOLD 0.8f    // Threshold for replication

// Helper function to calculate access cost based on metrics
static float calculateAccessCost(const artsMemMetrics_t *metrics,
                                 artsMemPlacement_t placement) {
  float baseCost = 0.0f;

  // Base cost based on placement
  switch (placement) {
  case ARTS_MEM_PLACE_DRAM:
    baseCost = 100.0f; // ns
    break;
  case ARTS_MEM_PLACE_HBM:
    baseCost = 50.0f; // ns
    break;
  case ARTS_MEM_PLACE_GPU:
    baseCost = 200.0f; // ns
    break;
  case ARTS_MEM_PLACE_REMOTE:
    baseCost = 1000.0f; // ns
    break;
  case ARTS_MEM_PLACE_NUMA_LOCAL:
    baseCost = 80.0f; // ns
    break;
  default:
    baseCost = 150.0f; // ns
  }

  // Adjust cost based on contention
  baseCost *= (1.0f + metrics->contentionScore);

  // Adjust for access pattern
  if (metrics->pattern & ARTS_ACCESS_PATTERN_SEQUENTIAL) {
    baseCost *= 0.8f; // Sequential access is more efficient
  } else if (metrics->pattern & ARTS_ACCESS_PATTERN_RANDOM) {
    baseCost *= 1.2f; // Random access is less efficient
  }

  return baseCost;
}

// Create a new SmartDB with the given size and type
artsDb_t *artsDbCreate(uint64_t size, artsType_t type,
                                 artsDbFlags_t flags) {
  artsDb_t *smartDb = (artsDb_t *)artsMalloc(sizeof(artsDb_t));
  if (!smartDb)
    return NULL;

  // Initialize core components
  smartDb->size = size;
  smartDb->type = type;
  smartDb->flags = flags;

  // Initialize readiness sensor
  smartDb->numProducers = 0;
  smartDb->numConsumers = 0;
  smartDb->version = 0;
  smartDb->latchCount = 0;
  smartDb->isReady = false;

  // Initialize memory sensor
  smartDb->placement = ARTS_MEM_PLACE_DEFAULT;
  memset(&smartDb->metrics, 0, sizeof(artsMemMetrics_t));
  smartDb->numaNode = artsGetCurrentCluster();
  smartDb->gpuDevice = 0;
  smartDb->accessCost = 0.0f;
  smartDb->memRef = NULL;
  smartDb->memRefSize = 0;
  smartDb->isMigrating = false;
  smartDb->homeNode = artsGetCurrentNode();
  memset(smartDb->accessOffsets, 0, sizeof(smartDb->accessOffsets));
  smartDb->accessHistoryIdx = 0;
  smartDb->accessHistoryCount = 0;

  // Create the underlying DataBlock
  void *data = NULL;
  smartDb->dbGuid = artsDbCreate(&data, size, type);
  if (smartDb->dbGuid == NULL_GUID) {
    artsFree(smartDb);
    return NULL;
  }

  // Create a persistent event to track readiness
  smartDb->eventGuid =
      artsPersistentEventCreate(artsGlobalRankId, 0, smartDb->dbGuid);
  if (smartDb->eventGuid == NULL_GUID) {
    artsDbDestroy(smartDb->dbGuid);
    artsFree(smartDb);
    return NULL;
  }

  smartDb->dataGuid = smartDb->dbGuid;
  smartDb->memRef = data;
  smartDb->memRefSize = size;

  return smartDb;
}

// Create a SmartDB with a specific GUID
artsDb_t *artsDbCreateWithGuid(artsGuid_t guid, uint64_t size,
                                         artsDbFlags_t flags) {
  artsDb_t *smartDb = (artsDb_t *)artsMalloc(sizeof(artsDb_t));
  if (!smartDb)
    return NULL;

  // Initialize core components
  smartDb->dbGuid = guid;
  smartDb->size = size;
  smartDb->type = artsGuidGetType(guid);
  smartDb->flags = flags;

  // Initialize readiness sensor
  smartDb->numProducers = 0;
  smartDb->numConsumers = 0;
  smartDb->version = 0;
  smartDb->latchCount = 0;
  smartDb->isReady = false;

  // Initialize memory sensor
  smartDb->placement = ARTS_MEM_PLACE_DEFAULT;
  memset(&smartDb->metrics, 0, sizeof(artsMemMetrics_t));
  smartDb->numaNode = artsGetCurrentCluster();
  smartDb->gpuDevice = 0;
  smartDb->accessCost = 0.0f;
  smartDb->memRef = NULL;
  smartDb->memRefSize = 0;
  smartDb->isMigrating = false;
  smartDb->homeNode = artsGetCurrentNode();
  memset(smartDb->accessOffsets, 0, sizeof(smartDb->accessOffsets));
  smartDb->accessHistoryIdx = 0;
  smartDb->accessHistoryCount = 0;

  // Create the underlying DataBlock with the given GUID
  void *data = artsDbCreateWithGuid(guid, size);
  if (!data) {
    artsFree(smartDb);
    return NULL;
  }

  // Create a persistent event to track readiness
  smartDb->eventGuid = artsPersistentEventCreate(artsGlobalRankId, 0, guid);
  if (smartDb->eventGuid == NULL_GUID) {
    artsDbDestroy(guid);
    artsFree(smartDb);
    return NULL;
  }

  smartDb->dataGuid = guid;
  smartDb->memRef = data;
  smartDb->memRefSize = size;

  return smartDb;
}

// Destroy a SmartDB and its associated resources
void artsDbDestroy(artsDb_t *smartDb) {
  if (!smartDb)
    return;

  // Wait for any ongoing migration to complete
  while (smartDb->isMigrating) {
    artsYield();
  }

  // Destroy the persistent event
  artsEventDestroy(smartDb->eventGuid);

  // Destroy the underlying DataBlock
  artsDbDestroy(smartDb->dbGuid);

  // Free the SmartDB structure
  artsFree(smartDb);
}

// Readiness sensor operations
void artsDbAddProducer(artsDb_t *smartDb) {
  if (!smartDb)
    return;
  smartDb->numProducers++;
  smartDb->latchCount++;
  artsPersistentEventIncrementLatch(smartDb->eventGuid, smartDb->dataGuid);
}

void artsDbAddConsumer(artsDb_t *smartDb) {
  if (!smartDb)
    return;
  smartDb->numConsumers++;
}

void artsDbProducerComplete(artsDb_t *smartDb) {
  if (!smartDb)
    return;
  artsPersistentEventDecrementLatch(smartDb->eventGuid, smartDb->dataGuid);
  smartDb->latchCount--;
  smartDb->version++;

  // Update readiness state
  smartDb->isReady = (smartDb->latchCount == 0);
}

void artsDbConsumerComplete(artsDb_t *smartDb) {
  if (!smartDb)
    return;
  // Update metrics for consumer completion
  artsDbUpdateMetrics(smartDb, smartDb->size, 0);
}

bool artsDbIsReady(artsDb_t *smartDb) {
  if (!smartDb)
    return false;
  return smartDb->isReady && !artsIsEventFired(smartDb->eventGuid);
}

// Memory sensor operations
void artsDbUpdateMetrics(artsDb_t *smartDb, uint64_t accessSize,
                              uint64_t latency) {
  if (!smartDb)
    return;

  artsMemMetrics_t *metrics = &smartDb->metrics;
  uint64_t currentTime = artsGetTimeStamp();

  // Update access statistics
  metrics->accessCount++;
  metrics->lastAccessTime = currentTime;
  metrics->totalAccessLatency += latency;
  metrics->totalAccessBytes += accessSize;

  // Update contention score based on access frequency
  float timeSinceLastAccess =
      (currentTime - metrics->lastAccessTime) / 1e9f; // Convert to seconds
  if (timeSinceLastAccess < 0.001f) {                 // High frequency access
    metrics->contentionScore = fminf(1.0f, metrics->contentionScore + 0.1f);
  } else {
    metrics->contentionScore = fmaxf(0.0f, metrics->contentionScore - 0.05f);
  }

  // Update hot data status
  metrics->isHot = (metrics->accessCount > HOT_ACCESS_THRESHOLD);

  // Update access cost
  smartDb->accessCost = calculateAccessCost(metrics, smartDb->placement);
}

void artsDbSetPlacement(artsDb_t *smartDb,
                             artsMemPlacement_t placement) {
  if (!smartDb || smartDb->isMigrating)
    return;

  if (placement != smartDb->placement) {
    smartDb->isMigrating = true;
    // TODO: Implement actual data migration
    smartDb->placement = placement;
    smartDb->isMigrating = false;
  }
}

artsMemPlacement_t artsDbGetPlacement(artsDb_t *smartDb) {
  if (!smartDb)
    return ARTS_MEM_PLACE_DEFAULT;
  return smartDb->placement;
}

void artsDbSetAccessPattern(artsDb_t *smartDb,
                                 artsAccessPattern_t pattern) {
  if (!smartDb)
    return;
  smartDb->metrics.pattern = pattern;
  smartDb->accessCost =
      calculateAccessCost(&smartDb->metrics, smartDb->placement);
}

artsAccessPattern_t artsDbGetAccessPattern(artsDb_t *smartDb) {
  if (!smartDb)
    return ARTS_ACCESS_PATTERN_UNKNOWN;
  return smartDb->metrics.pattern;
}

float artsDbGetAccessCost(artsDb_t *smartDb) {
  if (!smartDb)
    return 0.0f;
  return smartDb->accessCost;
}

bool artsDbShouldMigrate(artsDb_t *smartDb) {
  if (!smartDb || !(smartDb->flags & ARTS_SMART_DB_AUTO_MIGRATE))
    return false;

  const artsMemMetrics_t *metrics = &smartDb->metrics;

  // Consider migration if:
  // 1. Data is hot and contended
  // 2. Current placement is not optimal
  // 3. Migration cost is justified
  bool shouldMigrate = metrics->isHot &&
                       metrics->contentionScore > CONTENTION_THRESHOLD &&
                       smartDb->accessCost > MIGRATION_COST_THRESHOLD;

  return shouldMigrate;
}

bool artsDbShouldReplicate(artsDb_t *smartDb) {
  if (!smartDb || !(smartDb->flags & ARTS_SMART_DB_REPLICATE))
    return false;

  const artsMemMetrics_t *metrics = &smartDb->metrics;

  // Consider replication if:
  // 1. Data is very hot
  // 2. High contention
  // 3. Multiple consumers
  bool shouldReplicate = metrics->isHot &&
                         metrics->contentionScore > REPLICATION_THRESHOLD &&
                         smartDb->numConsumers > 1;

  return shouldReplicate;
}

// Data operations with memory awareness
void *artsDbGetData(artsDb_t *smartDb) {
  if (!smartDb)
    return NULL;

  // Update metrics for this access
  uint64_t startTime = artsGetTimeStamp();
  void *data = artsDbCreateWithGuid(smartDb->dbGuid, smartDb->size);
  uint64_t latency = artsGetTimeStamp() - startTime;

  if (data) {
    artsDbUpdateMetrics(smartDb, smartDb->size, latency);
    // Record access at offset 0 (whole DB)
    artsDbRecordAccess(smartDb, 0);
    artsDbAnalyzeAccessPattern(smartDb);
  }

  return data;
}

void artsDbSetData(artsDb_t *smartDb, void *data, uint64_t size) {
  if (!smartDb || !data || size > smartDb->size)
    return;

  // Get the current data pointer
  void *currentData = artsDbCreateWithGuid(smartDb->dbGuid, smartDb->size);
  if (!currentData)
    return;

  // Copy the new data
  uint64_t startTime = artsGetTimeStamp();
  memcpy(currentData, data, size);
  uint64_t latency = artsGetTimeStamp() - startTime;

  // Update metrics
  artsDbUpdateMetrics(smartDb, size, latency);
  // Record access at offset 0 (whole DB)
  artsDbRecordAccess(smartDb, 0);
  artsDbAnalyzeAccessPattern(smartDb);

  // Signal that the data has been updated
  artsDbProducerComplete(smartDb);
}

void artsDbMigrate(artsDb_t *smartDb,
                        artsMemPlacement_t newPlacement) {
  if (!smartDb || smartDb->isMigrating)
    return;

  smartDb->isMigrating = true;

  // TODO: Implement actual data migration based on placement
  // This would involve:
  // 1. Allocating memory in the new location
  // 2. Copying data
  // 3. Updating pointers and metadata
  // 4. Freeing old memory

  smartDb->placement = newPlacement;
  smartDb->isMigrating = false;
}

void artsDbReplicate(artsDb_t *smartDb, unsigned int numCopies) {
  if (!smartDb || !(smartDb->flags & ARTS_SMART_DB_REPLICATE))
    return;

  // TODO: Implement data replication
  // This would involve:
  // 1. Creating copies in appropriate locations
  // 2. Setting up replication metadata
  // 3. Managing consistency between copies
}

// Dependence management
void artsDbAddDependence(artsDb_t *smartDb, artsGuid_t edtGuid,
                              uint32_t slot) {
  if (!smartDb || edtGuid == NULL_GUID)
    return;
  artsAddDependenceToPersistentEvent(smartDb->eventGuid, edtGuid, slot,
                                     smartDb->dataGuid);
}

// Metadata operations
artsDbFlags_t artsDbGetFlags(artsDb_t *smartDb) {
  if (!smartDb)
    return ARTS_SMART_DB_NONE;
  return smartDb->flags;
}

void artsDbSetFlags(artsDb_t *smartDb, artsDbFlags_t flags) {
  if (!smartDb)
    return;
  smartDb->flags = flags;
}

unsigned int artsDbGetNumProducers(artsDb_t *smartDb) {
  if (!smartDb)
    return 0;
  return smartDb->numProducers;
}

unsigned int artsDbGetNumConsumers(artsDb_t *smartDb) {
  if (!smartDb)
    return 0;
  return smartDb->numConsumers;
}

// Migration API: Move SmartDB to a new node
void artsDbMigrateToNode(artsDb_t *smartDb, unsigned int newNode) {
  if (!smartDb || smartDb->isMigrating || smartDb->homeNode == newNode)
    return;
  smartDb->isMigrating = true;

  // Quiescence: Prevent concurrent accesses during migration
  // (isMigrating flag is checked in all SmartDB accessors)

  // Marshall SmartDB metadata and data
  size_t msgSize = sizeof(artsDbMigrationMsg_t) + smartDb->memRefSize;
  char *buffer = (char *)artsMalloc(msgSize);
  artsDbMigrationMsg_t *msg = (artsDbMigrationMsg_t *)buffer;
  msg->size = smartDb->size;
  msg->type = smartDb->type;
  msg->flags = smartDb->flags;
  msg->version = smartDb->version;
  msg->numProducers = smartDb->numProducers;
  msg->numConsumers = smartDb->numConsumers;
  msg->latchCount = smartDb->latchCount;
  msg->isReady = smartDb->isReady;
  msg->placement = smartDb->placement;
  msg->metrics = smartDb->metrics;
  msg->numaNode = smartDb->numaNode;
  msg->gpuDevice = smartDb->gpuDevice;
  msg->accessCost = smartDb->accessCost;
  msg->memRefSize = smartDb->memRefSize;
  if (smartDb->memRef && smartDb->memRefSize > 0)
    memcpy(buffer + sizeof(artsDbMigrationMsg_t), smartDb->memRef,
           smartDb->memRefSize);

  // Send to new node
  artsRemoteSend(newNode, (sendHandler_t)artsDbMigrationHandler, buffer,
                 msgSize, true);

  // Update homeNode
  smartDb->homeNode = newNode;

  // Update routing table to remove local entry (simulate move semantics)
  artsRouteTableRemoveItem(smartDb->dbGuid);

  // Destroy local SmartDB
  artsDbDestroy(smartDb);
  // Note: buffer is freed by remote handler (artsRemoteSend with free=true)

  // Only actionable TODOs remain:
  // TODO: Implement persistent event migration and update dependents
  // TODO: Implement notification to dependents
}

// Handler to reconstruct SmartDB on the destination node
void artsDbMigrationHandler(void *args, unsigned int size) {
  if (!args || size < sizeof(artsDbMigrationMsg_t))
    return;
  artsDbMigrationMsg_t *msg = (artsDbMigrationMsg_t *)args;
  void *dataPtr = (void *)(msg + 1);

  // Create new SmartDB and DataBlock
  artsDb_t *smartDb = artsDbCreate(msg->size, msg->type, msg->flags);
  if (!smartDb)
    return;

  // Copy metadata
  smartDb->version = msg->version;
  smartDb->numProducers = msg->numProducers;
  smartDb->numConsumers = msg->numConsumers;
  smartDb->latchCount = msg->latchCount;
  smartDb->isReady = msg->isReady;
  smartDb->placement = msg->placement;
  smartDb->metrics = msg->metrics;
  smartDb->numaNode = msg->numaNode;
  smartDb->gpuDevice = msg->gpuDevice;
  smartDb->accessCost = msg->accessCost;
  smartDb->memRefSize = msg->memRefSize;
  smartDb->homeNode = artsGetCurrentNode();

  // Copy data
  void *dbData = artsDbGetData(smartDb);
  if (dbData && dataPtr && msg->memRefSize > 0) {
    memcpy(dbData, dataPtr, msg->memRefSize);
  }

  // Update routing table so the SmartDB's GUID points to this node
  // (Assume dbGuid is the SmartDB's GUID for now)
  artsRouteTableAddItem(smartDb, smartDb->dbGuid, smartDb->homeNode, false);

  // Placeholder: Migrate persistent event and update dependents
  // TODO: Implement persistent event migration and update dependents

  // Placeholder: Notify dependents of new location
  // TODO: Implement notification to dependents
}

// Sophisticated access pattern detection
void artsDbRecordAccess(artsDb_t *smartDb, uint64_t offset) {
  if (!smartDb)
    return;
  smartDb->accessOffsets[smartDb->accessHistoryIdx] = offset;
  smartDb->accessHistoryIdx =
      (smartDb->accessHistoryIdx + 1) % ARTS_SMART_DB_ACCESS_HISTORY;
  if (smartDb->accessHistoryCount < ARTS_SMART_DB_ACCESS_HISTORY)
    smartDb->accessHistoryCount++;
}

void artsDbAnalyzeAccessPattern(artsDb_t *smartDb) {
  if (!smartDb || smartDb->accessHistoryCount < 2)
    return;
  int sequential = 0, random = 0, streaming = 0, reuse = 0;
  uint64_t last = smartDb->accessOffsets[(smartDb->accessHistoryIdx +
                                          ARTS_SMART_DB_ACCESS_HISTORY - 1) %
                                         ARTS_SMART_DB_ACCESS_HISTORY];
  for (unsigned int i = 1; i < smartDb->accessHistoryCount; ++i) {
    unsigned int idx =
        (smartDb->accessHistoryIdx + ARTS_SMART_DB_ACCESS_HISTORY - 1 - i) %
        ARTS_SMART_DB_ACCESS_HISTORY;
    uint64_t curr = smartDb->accessOffsets[idx];
    int64_t diff = (int64_t)last - (int64_t)curr;
    if (diff == (int64_t)smartDb->memRefSize) {
      sequential++;
    } else if (diff == 0) {
      reuse++;
    } else if (llabs(diff) < (int64_t)smartDb->memRefSize / 4) {
      streaming++;
    } else {
      random++;
    }
    last = curr;
  }
  // Pick the dominant pattern
  if (sequential > random && sequential > streaming && sequential > reuse)
    smartDb->metrics.pattern = ARTS_ACCESS_PATTERN_SEQUENTIAL;
  else if (streaming > sequential && streaming > random && streaming > reuse)
    smartDb->metrics.pattern = ARTS_ACCESS_PATTERN_STREAMING;
  else if (reuse > sequential && reuse > streaming && reuse > random)
    smartDb->metrics.pattern = ARTS_ACCESS_PATTERN_REUSE;
  else
    smartDb->metrics.pattern = ARTS_ACCESS_PATTERN_RANDOM;
  // Update access cost
  smartDb->accessCost =
      calculateAccessCost(&smartDb->metrics, smartDb->placement);
}