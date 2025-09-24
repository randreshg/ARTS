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
#include "arts/runtime/sync/TerminationDetection.h"
#include "arts/arts.h"
#include "arts/gas/Guid.h"
#include "arts/gas/OutOfOrder.h"
#include "arts/gas/RouteTable.h"
#include "arts/introspection/Counter.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/RT.h"
#include "arts/runtime/compute/EdtFunctions.h"
#include "arts/runtime/network/RemoteFunctions.h"
#include "arts/system/ArtsPrint.h"
#include "arts/utils/Atomics.h"

#define EpochMask 0x7FFFFFFFFFFFFFFF
#define EpochBit 0x8000000000000000

#define DEFAULT_EPOCH_POOL_SIZE 4096
__thread artsEpochPool_t *epochThreadPool;

void globalShutdownGuidIncActive() {
  if (artsNodeInfo.shutdownEpoch)
    incrementActiveEpoch(artsNodeInfo.shutdownEpoch);
}

void globalShutdownGuidIncQueue() {
  if (artsNodeInfo.shutdownEpoch)
    incrementQueueEpoch(artsNodeInfo.shutdownEpoch);
}

void globalShutdownGuidIncFinished() {
  if (artsNodeInfo.shutdownEpoch)
    incrementFinishedEpoch(artsNodeInfo.shutdownEpoch);
}

void globalGuidShutdown(artsGuid_t guid) {
  if (artsNodeInfo.shutdownEpoch == guid) {
    ARTS_DEBUG("TERMINATION GUID SHUTDOWN %lu", guid);
    artsShutdown();
  }
}

bool decrementQueueEpoch(artsEpoch_t *epoch) {
  ARTS_DEBUG("Dec queue Epoch: %lu", epoch->guid);
  uint64_t local;
  while (1) {
    local = epoch->queued;
    if (local == 1) {
      if (1 == artsAtomicCswapU64(&epoch->queued, 1, EpochBit))
        return true;
    } else {
      if (local == artsAtomicCswapU64(&epoch->queued, local, local - 1))
        return false;
    }
  }
}

void incrementQueueEpoch(artsGuid_t epochGuid) {
  ARTS_DEBUG("Inc queue Epoch: %lu", epochGuid);
  if (epochGuid != NULL_GUID) {
    artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
    if (epoch) {
      artsAtomicAddU64(&epoch->queued, 1);
    } else {
      artsOutOfOrderIncQueueEpoch(epochGuid);
      ARTS_DEBUG("OOIncQueueEpoch %lu", epochGuid);
    }
  }
}

void incrementActiveEpoch(artsGuid_t epochGuid) {
  ARTS_DEBUG("Inc active Epoch: %lu", epochGuid);
  artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
  if (epoch) {
    artsAtomicAdd(&epoch->activeCount, 1);
  } else {
    artsOutOfOrderIncActiveEpoch(epochGuid);
    ARTS_DEBUG("OOIncActiveEpoch %lu", epochGuid);
  }
}

void incrementFinishedEpoch(artsGuid_t epochGuid) {
  if (epochGuid != NULL_GUID) {
    artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
    if (epoch) {
      artsAtomicAdd(&epoch->finishedCount, 1);
      if (artsGlobalRankCount == 1) {
        if (!checkEpoch(epoch, epoch->activeCount, epoch->finishedCount)) {
          if (epoch->phase == PHASE_3)
            deleteEpoch(epochGuid, epoch);
        }
      } else {
        unsigned int rank = artsGuidGetRank(epochGuid);
        if (rank == artsGlobalRankId) {
          if (!artsAtomicSubU64(&epoch->queued, 1)) {
            if (!artsAtomicCswapU64(&epoch->outstanding, 0,
                                    artsGlobalRankCount)) {
              broadcastEpochRequest(epochGuid);
              ARTS_DEBUG("%lu Broadcasting req... ", epochGuid);
            } else {
              ARTS_DEBUG("OUTSTANDING: %lu %lu", epoch->guid,
                         epoch->outstanding);
            }
          } else {
            ARTS_DEBUG("QUEUED: %lu %lu", epoch->guid, epoch->outstanding);
          }
        } else {
          if (decrementQueueEpoch(epoch)) {
            artsRemoteEpochSend(rank, epochGuid, epoch->activeCount,
                                epoch->finishedCount);
            ARTS_DEBUG("%lu Now responding... ", epochGuid);
          }
        }
      }
    } else {
      artsOutOfOrderIncFinishedEpoch(epochGuid);
      ARTS_DEBUG("%lu ooFinish", epochGuid);
    }
  }
}

void sendEpoch(artsGuid_t epochGuid, unsigned int source, unsigned int dest) {
  artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
  if (epoch) {
    artsAtomicFetchAndU64(&epoch->queued, EpochMask);
    if (!artsAtomicCswapU64(&epoch->queued, 0, EpochBit)) {
      artsRemoteEpochSend(dest, epochGuid, epoch->activeCount,
                          epoch->finishedCount);
      //            ARTS_INFO("%lu Sending Now...", epochGuid);
    }
    //        else
    //            ARTS_INFO("Buffer Send...");
  } else
    artsOutOfOrderSendEpoch(epochGuid, source, dest);
}

artsEpoch_t *createEpoch(artsGuid_t *guid, artsGuid_t edtGuid,
                         unsigned int slot) {
  if (*guid == NULL_GUID)
    *guid = artsGuidCreateForRank(artsGlobalRankId, ARTS_EDT);

  artsEpoch_t *epoch = (artsEpoch_t *)artsCalloc(1, sizeof(artsEpoch_t));
  epoch->phase = PHASE_1;
  epoch->terminationExitGuid = edtGuid;
  epoch->terminationExitSlot = slot;
  epoch->guid = *guid;
  epoch->poolGuid = NULL_GUID;
  epoch->queued = (artsIsGuidLocal(*guid)) ? 0 : EpochBit;
  artsRouteTableAddItemRace(epoch, *guid, artsGlobalRankId, false);
  artsRouteTableFireOO(*guid, artsOutOfOrderHandler);
  //    ARTS_INFO("Create %lu %p", *guid, epoch);
  return epoch;
}

bool createShutdownEpoch() {
  if (artsNodeInfo.shutdownEpoch) {
    artsNodeInfo.shutdownEpoch = artsGuidCreateForRank(0, ARTS_EDT);
    artsEpoch_t *epoch = createEpoch(&artsNodeInfo.shutdownEpoch, NULL_GUID, 0);
    artsAtomicAdd(&epoch->activeCount, artsGetTotalWorkers());
    artsAtomicAddU64(&epoch->queued, artsGetTotalWorkers());
    ARTS_DEBUG("Shutdown guy %u : %lu --------> %lu %p", epoch->activeCount,
               epoch->queued, epoch->guid, epoch);
    return true;
  }
  return false;
}

void artsAddEdtToEpoch(artsGuid_t edtGuid, artsGuid_t epochGuid) {
  struct artsEdt *edt = (struct artsEdt *)artsRouteTableLookupItem(edtGuid);
  if (edt) {
    edt->epochGuid = epochGuid;
    incrementActiveEpoch(epochGuid);
    return;
  }
  ARTS_DEBUG("Out-of-order add to epoch not supported...");
  return;
}

void broadcastEpochRequest(artsGuid_t epochGuid) {
  unsigned int originRank = artsGuidGetRank(epochGuid);
  for (unsigned int i = 0; i < artsGlobalRankCount; i++) {
    if (i != originRank) {
      artsRemoteEpochReq(i, epochGuid);
    }
  }
}

artsGuid_t artsInitializeAndStartEpoch(artsGuid_t finishEdtGuid,
                                       unsigned int slot) {
  //    artsGuid_t guid = NULL_GUID;
  //    artsEpoch_t * epoch = createEpoch(&guid, finishEdtGuid, slot);
  artsEpoch_t *epoch = getPoolEpoch(finishEdtGuid, slot);

  artsSetCurrentEpochGuid(epoch->guid);
  artsAtomicAdd(&epoch->activeCount, 1);
  artsAtomicAddU64(&epoch->queued, 1);

  //    for(unsigned int i=0; i<artsGlobalRankCount; i++)
  //    {
  //        if(i != artsGlobalRankId)
  //            artsRemoteEpochInitSend(i, guid, finishEdtGuid, slot);
  //    }

  ARTS_DEBUG("%u : %lu --------> %lu %p", epoch->activeCount, epoch->queued,
             epoch->guid, epoch);
  return epoch->guid;
}

artsGuid_t artsInitializeEpoch(unsigned int rank, artsGuid_t finishEdtGuid,
                               unsigned int slot) {
  artsGuid_t guid = NULL_GUID;
  // I think the idea is this is that during parallel start
  // (artsNodeInfo.readyToExecute > 0) This means that the epoch will be created
  // on all nodes assuming that each node goes through the initializeEpoch code
  // path. Pool assume the current host...
  if (!artsNodeInfo.readyToExecute || rank != artsGlobalRankId) {
    guid = artsGuidCreateForRank(rank, ARTS_EDT);
    createEpoch(&guid, finishEdtGuid, slot);
    if (!artsNodeInfo.readyToExecute) {
      for (unsigned int i = 0; i < artsGlobalRankCount; i++) {
        if (i != artsGlobalRankId)
          artsRemoteEpochInitSend(i, guid, finishEdtGuid, slot);
      }
    }
  } else // Lets get it from the pool...
  {
    artsEpoch_t *epoch = getPoolEpoch(finishEdtGuid, slot);
    guid = epoch->guid;
  }
  return guid;
}

void artsStartEpoch(artsGuid_t epochGuid) {
  artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
  if (epoch) {
    artsSetCurrentEpochGuid(epoch->guid);
    artsAtomicAdd(&epoch->activeCount, 1);
    artsAtomicAddU64(&epoch->queued, 1);
  } else
    ARTS_INFO("Out-of-Order epoch start not supported %lu", epochGuid);
}

bool checkEpoch(artsEpoch_t *epoch, unsigned int totalActive,
                unsigned int totalFinish) {
  unsigned int diff = totalActive - totalFinish;
  ARTS_DEBUG("%lu : %u - %u = %u", epoch->guid, totalActive, totalFinish, diff);
  // We have a zero
  if (totalFinish && !diff) {
    // Lets check the phase and if we have the same counts as before
    if (epoch->phase == PHASE_2 && epoch->lastActiveCount == totalActive &&
        epoch->lastFinishedCount == totalFinish) {
      epoch->phase = PHASE_3;
      ARTS_DEBUG("%lu epoch done!!!!!!!", epoch->guid);
      if (epoch->waitPtr)
        *epoch->waitPtr = 0;
      if (epoch->ticket) {
        artsSignalContext(epoch->ticket);
        ARTS_DEBUG("%lu Signaling context %u", epoch->guid, epoch->ticket);
      }
      if (epoch->terminationExitGuid) {
        ARTS_DEBUG(
            "%lu Calling finalization continuation provided by the user %u",
            epoch->guid, totalFinish);
        artsSignalEdtValue(epoch->terminationExitGuid,
                           epoch->terminationExitSlot, totalFinish);
      } else {
        globalGuidShutdown(epoch->guid);
      }
      return false;
    }
    // We didn't match the last one so lets try again
    epoch->lastActiveCount = totalActive;
    epoch->lastFinishedCount = totalFinish;
    epoch->phase = PHASE_2;
    ARTS_DEBUG("%lu Starting phase 2 %u", epoch->guid,
               epoch->lastFinishedCount);
    if (artsGlobalRankCount == 1) {
      epoch->phase = PHASE_3;
      ARTS_DEBUG("%lu epoch done!!!!!!!", epoch->guid);
      if (epoch->waitPtr)
        *epoch->waitPtr = 0;
      if (epoch->ticket)
        artsSignalContext(epoch->ticket);
      if (epoch->terminationExitGuid) {
        ARTS_DEBUG("%lu Calling finalization continuation provided by the user "
                   "%u !",
                   epoch->guid, totalFinish);
        artsSignalEdtValue(epoch->terminationExitGuid,
                           epoch->terminationExitSlot, totalFinish);
      } else {
        globalGuidShutdown(epoch->guid);
      }
      return false;
    }
    return true;
  }
  epoch->phase = PHASE_1;
  return (epoch->queued == 0);
}

void reduceEpoch(artsGuid_t epochGuid, unsigned int active,
                 unsigned int finish) {
  artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);
  if (epoch) {
    ARTS_DEBUG("%lu A: %u F: %u", epochGuid, active, finish);
    unsigned int totalActive = artsAtomicAdd(&epoch->globalActiveCount, active);
    unsigned int totalFinish =
        artsAtomicAdd(&epoch->globalFinishedCount, finish);
    if (artsAtomicSubU64(&epoch->outstanding, 1) == 1) {
      ARTS_DEBUG("%lu A: %u F: %u", epochGuid, epoch->activeCount,
                 epoch->finishedCount);
      totalActive += epoch->activeCount;
      totalFinish += epoch->finishedCount;

      // Reset for the next round
      epoch->globalActiveCount = 0;
      epoch->globalFinishedCount = 0;

      if (checkEpoch(epoch, totalActive, totalFinish)) {
        ARTS_DEBUG("%lu REDUCE SEND", epochGuid);
        artsAtomicAddU64(&epoch->outstanding, artsGlobalRankCount - 1);
        broadcastEpochRequest(epochGuid);
        // A better idea will be to know when to kick off a new round
        // the checkinCount == 0 indicates there is a new round can be kicked
        // off
        //                artsAtomicSub(&epoch->checkinCount, 1);
      } else
        artsAtomicSubU64(&epoch->outstanding, 1);

      if (epoch->phase == PHASE_3)
        deleteEpoch(epochGuid, epoch);

      ARTS_DEBUG("%lu EPOCH QUEUEU: %u", epochGuid, epoch->queued);
    }
    ARTS_DEBUG("###### %lu -> %lu", epoch->guid, epoch->outstanding);
  } else
    ARTS_INFO("%lu ERROR: NO EPOCH", epochGuid);
}

artsEpochPool_t *createEpochPool(artsGuid_t *epochPoolGuid,
                                 unsigned int poolSize, artsGuid_t *startGuid) {
  if (*epochPoolGuid == NULL_GUID)
    *epochPoolGuid = artsGuidCreateForRank(artsGlobalRankId, ARTS_EDT);

  bool newRange = (*startGuid == NULL_GUID);
  artsGuidRange temp;
  artsGuidRange *range;
  if (newRange) {
    range = artsNewGuidRangeNode(ARTS_EDT, poolSize, artsGlobalRankId);
    *startGuid = artsGetGuid(range, 0);
  } else {
    temp.size = poolSize;
    temp.index = 0;
    temp.startGuid = *startGuid;
    range = &temp;
  }

  artsEpochPool_t *epochPool = (artsEpochPool_t *)artsCalloc(
      1, sizeof(artsEpochPool_t) + sizeof(artsEpoch_t) * poolSize);
  epochPool->index = 0;
  epochPool->outstanding = poolSize;
  epochPool->size = poolSize;

  artsRouteTableAddItem(epochPool, *epochPoolGuid, artsGlobalRankId, false);
  for (unsigned int i = 0; i < poolSize; i++) {
    epochPool->pool[i].phase = PHASE_1;
    epochPool->pool[i].poolGuid = *epochPoolGuid;
    epochPool->pool[i].guid = artsGetGuid(range, i);
    epochPool->pool[i].queued =
        (artsIsGuidLocal(*epochPoolGuid)) ? 0 : EpochBit;
    if (!artsIsGuidLocal(*epochPoolGuid)) {
      artsRouteTableAddItemRace(&epochPool->pool[i], epochPool->pool[i].guid,
                                artsGlobalRankId, false);
      artsRouteTableFireOO(epochPool->pool[i].guid, artsOutOfOrderHandler);
    }
  }

  ARTS_DEBUG("Creating pool %lu starting %lu %p", *epochPoolGuid,
             artsGetGuid(range, 0), epochPool);

  if (newRange)
    artsFree(range);

  return epochPool;
}

void deleteEpoch(artsGuid_t epochGuid, artsEpoch_t *epoch) {
  // Can't call delete unless we already hit two barriers thus it must exit
  if (!epoch)
    epoch = (artsEpoch_t *)artsRouteTableLookupItem(epochGuid);

  if (epoch->poolGuid) {
    artsEpochPool_t *pool =
        (artsEpochPool_t *)artsRouteTableLookupItem(epoch->poolGuid);
    if (artsIsGuidLocal(epoch->poolGuid)) {
      artsRouteTableRemoveItem(epochGuid);
      if (!artsAtomicSub(&pool->outstanding, 1)) {
        artsRouteTableRemoveItem(epoch->poolGuid);
        //                artsFree(pool);  //Free in the next getPoolEpoch
        for (unsigned int i = 0; i < artsGlobalRankCount; i++) {
          if (i != artsGlobalRankId)
            artsRemoteEpochDelete(i, epochGuid);
        }
      }
    } else {
      for (unsigned int i = 0; i < pool->size; i++)
        artsRouteTableRemoveItem(pool->pool[i].guid);
      artsRouteTableRemoveItem(epoch->poolGuid);
      artsFree(pool);
    }
  } else {
    artsRouteTableRemoveItem(epochGuid);
    artsFree(epoch);

    if (artsIsGuidLocal(epochGuid)) {
      for (unsigned int i = 0; i < artsGlobalRankCount; i++) {
        if (i != artsGlobalRankId)
          artsRemoteEpochDelete(i, epochGuid);
      }
    }
  }
}

void cleanEpochPool() {
  ARTS_DEBUG("EPOCHTHREADPOOL -------- %p", epochThreadPool);

  artsEpochPool_t *trailPool = NULL;
  artsEpochPool_t *pool = epochThreadPool;

  while (pool) {
    ARTS_DEBUG("###### POOL %p", pool);
    if (pool->index == epochThreadPool->size && !pool->outstanding) {
      artsEpochPool_t *toFree = pool;
      ARTS_DEBUG("Deleting %p", toFree);

      pool = pool->next;

      if (trailPool)
        trailPool->next = pool;
      else
        epochThreadPool = pool;

      artsFree(toFree);
      ARTS_DEBUG("JUST FREED A POOL");
    } else {
      ARTS_DEBUG("Next...");
      trailPool = pool;
      pool = pool->next;
    }
  }
}

artsEpoch_t *getPoolEpoch(artsGuid_t edtGuid, unsigned int slot) {
  ARTS_DEBUG("EpochThreadPool %p", epochThreadPool);

  //    cleanEpochPool();
  artsEpochPool_t *trailPool = NULL;
  artsEpochPool_t *pool = epochThreadPool;
  artsEpoch_t *epoch = NULL;
  while (!epoch) {
    if (!pool) {
      artsGuid_t poolGuid = NULL_GUID;
      artsGuid_t startGuid = NULL_GUID;
      pool = createEpochPool(&poolGuid, DEFAULT_EPOCH_POOL_SIZE, &startGuid);

      if (trailPool)
        trailPool->next = pool;
      else
        epochThreadPool = pool;

      for (unsigned int i = 0; i < artsGlobalRankCount; i++) {
        if (i != artsGlobalRankId)
          artsRemoteEpochInitPoolSend(i, DEFAULT_EPOCH_POOL_SIZE, startGuid,
                                      poolGuid);
      }
    }

    ARTS_DEBUG("Pool index: %u", pool->index);
    if (pool->index < pool->size)
      epoch = &pool->pool[pool->index++];
    else {
      trailPool = pool;
      pool = pool->next;
    }
  }
  ARTS_DEBUG("GetPoolEpoch %lu", epoch->guid);

  epoch->terminationExitGuid = edtGuid;
  epoch->terminationExitSlot = slot;
  artsRouteTableAddItemRace(epoch, epoch->guid, artsGlobalRankId, false);
  artsRouteTableFireOO(epoch->guid, artsOutOfOrderHandler);
  return epoch;
}

void artsYield() {
  ARTSCOUNTERINCREMENT(yield);
  threadLocal_t tl;
  artsSaveThreadLocal(&tl);
  artsNodeInfo.scheduler();
  artsRestoreThreadLocal(&tl);
}

bool artsWaitOnHandle(artsGuid_t epochGuid) {
  artsGuid_t *guid = artsCheckEpochIsRoot(epochGuid);
  // For now lets leave this rule here
  if (guid) {
    artsGuid_t local = *guid;
    *guid = NULL_GUID; // Unset
    unsigned int flag = 1;
    artsEpoch_t *epoch = (artsEpoch_t *)artsRouteTableLookupItem(local);
    epoch->ticket = artsGetContextTicket();
    if (artsNodeInfo.tMT && epoch->ticket) {
      incrementFinishedEpoch(local);
      artsContextSwitch(1);
      cleanEpochPool();
      return true;
    }
    if (!epoch->ticket) {
      epoch->waitPtr = &flag;
      incrementFinishedEpoch(local);
      //        globalShutdownGuidIncFinished();

      ARTSCOUNTERINCREMENT(yield);
      threadLocal_t tl;
      artsSaveThreadLocal(&tl);
      while (flag)
        artsNodeInfo.scheduler();
      artsRestoreThreadLocal(&tl);

      cleanEpochPool();

      return true;
    }
  }
  return false;
}
