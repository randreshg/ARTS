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
#include "arts/runtime/network/RemoteFunctions.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "arts/arts.h"
#include "arts/gas/OutOfOrder.h"
#include "arts/gas/RouteTable.h"
#include "arts/introspection/Metrics.h"
#include "arts/network/RemoteProtocol.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/Runtime.h"
#include "arts/runtime/compute/EdtFunctions.h"
#include "arts/runtime/memory/ArrayDb.h"
#include "arts/runtime/memory/DbFunctions.h"
#include "arts/runtime/memory/DbList.h"
#include "arts/runtime/sync/TerminationDetection.h"
#include "arts/system/ArtsPrint.h"
#include "arts/system/Debug.h"
#include "arts/utils/Atomics.h"

static inline void artsFillPacketHeader(struct artsRemotePacket *header,
                                        uint64_t size,
                                        unsigned int messageType) {
  header->size = size;
  header->messageType = messageType;
  header->rank = artsGlobalRankId;
}

static void artsClearExclusiveRequest(struct artsDb *db, int rank,
                                      artsGuid_t edtGuid) {
  if (!db || !db->dbList)
    return;

  struct artsDbList *dbList = (struct artsDbList *)db->dbList;
  artsWriterLock(&dbList->reader, &dbList->writer);
  for (struct artsDbFrontier *frontier = dbList->head; frontier;
       frontier = frontier->next) {
    if (frontier->exNode == (unsigned int)rank &&
        frontier->exEdtGuid == edtGuid) {
      frontier->exEdtGuid = NULL_GUID;
      frontier->exEdt = NULL;
      frontier->exSlot = 0;
      frontier->exMode = ARTS_NULL;
      break;
    }
  }
  artsWriterUnlock(&dbList->writer);
}

static void sendRemoteAddDependencePacket(unsigned int messageType,
                                          artsGuid_t source,
                                          artsGuid_t destination, uint32_t slot,
                                          unsigned int rank,
                                          artsType_t acquireMode) {
  struct artsRemoteAddDependencePacket packet;
  packet.source = source;
  packet.destination = destination;
  packet.slot = slot;
  packet.acquireMode = acquireMode;
  artsFillPacketHeader(&packet.header, sizeof(packet), messageType);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteAddDependence(artsGuid_t source, artsGuid_t destination,
                             uint32_t slot, unsigned int rank) {
  ARTS_DEBUG("Remote Add dependence sent %d", rank);
  sendRemoteAddDependencePacket(ARTS_REMOTE_ADD_DEPENDENCE_MSG, source,
                                destination, slot, rank, ARTS_NULL);
}

void artsRemoteAddDependenceWithHints(artsGuid_t source, artsGuid_t destination,
                                      uint32_t slot, unsigned int rank,
                                      artsType_t acquireMode) {
  ARTS_DEBUG("Remote Add dependence (acquire=%u) sent %d", acquireMode, rank);
  sendRemoteAddDependencePacket(ARTS_REMOTE_ADD_DEPENDENCE_MSG, source,
                                destination, slot, rank, acquireMode);
}

void artsRemoteAddDependenceToPersistentEvent(artsGuid_t source,
                                              artsGuid_t destination,
                                              uint32_t slot,
                                              unsigned int rank) {
  ARTS_DEBUG("Remote Add dependence to persistent event sent %d", rank);
  sendRemoteAddDependencePacket(
      ARTS_REMOTE_ADD_DEPENDENCE_TO_PERSISTENT_EVENT_MSG, source, destination,
      slot, rank, ARTS_NULL);
}

void artsRemoteAddDependenceToPersistentEventWithHints(
    artsGuid_t source, artsGuid_t destination, uint32_t slot, unsigned int rank,
    artsType_t acquireMode) {
  ARTS_DEBUG("Remote Add dependence to persistent event (acquire=%u) sent %d",
             acquireMode, rank);
  sendRemoteAddDependencePacket(
      ARTS_REMOTE_ADD_DEPENDENCE_TO_PERSISTENT_EVENT_MSG, source, destination,
      slot, rank, acquireMode);
}

void artsRemoteAddDependenceToPersistentEventWithByteOffset(
    artsGuid_t source, artsGuid_t destination, uint32_t slot, unsigned int rank,
    artsType_t acquireMode, uint64_t byteOffset, uint64_t size) {
  ARTS_DEBUG("Remote Add dep to persistent event with byte offset "
             "(acquire=%u, offset=%lu, size=%lu) sent to rank %d",
             acquireMode, byteOffset, size, rank);
  struct artsRemoteAddDependenceWithByteOffsetPacket packet;
  packet.source = source;
  packet.destination = destination;
  packet.slot = slot;
  packet.acquireMode = acquireMode;
  packet.byteOffset = byteOffset;
  packet.size = size;
  artsFillPacketHeader(
      &packet.header, sizeof(packet),
      ARTS_REMOTE_ADD_DEPENDENCE_TO_PERSISTENT_EVENT_WITH_BYTE_OFFSET_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteUpdateRouteTable(artsGuid_t guid, unsigned int rank) {
  unsigned int owner = artsGuidGetRank(guid);
  if (owner == artsGlobalRankId) {
    struct artsDbFrontierIterator *iter =
        artsRouteTableGetRankDuplicates(guid, rank);
    if (iter) {
      unsigned int node;
      while (artsDbFrontierIterNext(iter, &node)) {
        if (node != artsGlobalRankId && node != rank) {
          struct artsRemoteGuidOnlyPacket outPacket;
          outPacket.guid = guid;
          artsFillPacketHeader(&outPacket.header, sizeof(outPacket),
                               ARTS_REMOTE_INVALIDATE_DB_MSG);
          artsRemoteSendRequestAsync(node, (char *)&outPacket,
                                     sizeof(outPacket));
        }
      }
      artsFree(iter);
    }
  } else {
    struct artsRemoteGuidOnlyPacket packet;
    artsFillPacketHeader(&packet.header, sizeof(packet),
                         ARTS_REMOTE_DB_UPDATE_GUID_MSG);
    packet.guid = guid;
    artsRemoteSendRequestAsync(owner, (char *)&packet, sizeof(packet));
  }
}

void artsRemoteHandleUpdateDbGuid(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  ARTS_DEBUG("Updated %ld to %d", packet->guid, packet->header.rank);
  artsRemoteUpdateRouteTable(packet->guid, packet->header.rank);
}

void artsRemoteHandleInvalidateDb(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  void **data = NULL;
  itemState_t state =
      artsRouteTableLookupItemWithState(packet->guid, &data, anyKey, false);

  // If a request for this DB is in-flight, invalidating now can drop the OO
  // waiters attached to the route-table entry and strand pending dep slots.
  // Defer invalidation until the in-flight response installs the fresh copy.
  if (state == requestedKey || state == reservedKey) {
    ARTS_DEBUG("Deferring invalidate for DB[Guid:%lu] in state=%u",
               packet->guid, state);
    return;
  }

  artsRouteTableInvalidateItem(packet->guid);
}

void artsRemoteDbDestroy(artsGuid_t guid, unsigned int originRank, bool clean) {
  unsigned int ownerRank = artsGuidGetRank(guid);

  // Non-owner forwards destroy/clean to owner.
  if (ownerRank != artsGlobalRankId) {
    struct artsRemoteGuidOnlyPacket packet;
    packet.guid = guid;
    artsFillPacketHeader(
        &packet.header, sizeof(packet),
        clean ? ARTS_REMOTE_DB_CLEAN_FORWARD_MSG
              : ARTS_REMOTE_DB_DESTROY_FORWARD_MSG);
    artsRemoteSendRequestAsync(ownerRank, (char *)&packet, sizeof(packet));
    return;
  }

  // Owner notifies cached duplicate holders to destroy local copies.
  struct artsDbFrontierIterator *iter =
      artsRouteTableGetRankDuplicates(guid, UINT_MAX);
  if (!iter)
    return;

  bool *sentRank = (bool *)artsCalloc(artsGlobalRankCount, sizeof(bool));
  if (!sentRank) {
    artsFree(iter);
    return;
  }

  struct artsRemoteGuidOnlyPacket outPacket;
  outPacket.guid = guid;
  artsFillPacketHeader(&outPacket.header, sizeof(outPacket),
                       ARTS_REMOTE_DB_DESTROY_MSG);

  unsigned int rank = 0;
  while (artsDbFrontierIterNext(iter, &rank)) {
    if (rank == artsGlobalRankId || rank == originRank)
      continue;
    if (rank < artsGlobalRankCount && sentRank[rank])
      continue;
    if (rank < artsGlobalRankCount)
      sentRank[rank] = true;

    artsRemoteSendRequestAsync(rank, (char *)&outPacket, sizeof(outPacket));
  }

  artsFree(sentRank);
  artsFree(iter);
}

void artsRemoteHandleDbDestroyForward(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  artsRemoteDbDestroy(packet->guid, packet->header.rank, 0);
  artsDbDestroySafe(packet->guid, false);
}

void artsRemoteHandleDbCleanForward(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  artsRemoteDbDestroy(packet->guid, packet->header.rank, 1);
}

void artsRemoteHandleDbDestroy(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  artsDbDestroySafe(packet->guid, false);
}

void artsRemoteUpdateDb(artsGuid_t guid, bool sendDb, artsGuid_t epochGuid) {
  unsigned int rank = artsGuidGetRank(guid);
  if (rank != artsGlobalRankId) {
    struct artsRemoteDbUpdatePacket packet;
    packet.guid = guid;
    packet.epochGuid = epochGuid;
    struct artsDb *db = NULL;
    if (sendDb && (db = (struct artsDb *)artsRouteTableLookupItem(guid))) {
      uint64_t size = sizeof(struct artsRemoteDbUpdatePacket) + db->header.size;
      artsFillPacketHeader(&packet.header, size, ARTS_REMOTE_DB_UPDATE_MSG);
      artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
                                        (char *)db, db->header.size);
    } else {
      artsFillPacketHeader(&packet.header, sizeof(struct artsRemoteDbUpdatePacket),
                           ARTS_REMOTE_DB_UPDATE_MSG);
      artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
    }
  }
}

void artsRemoteHandleUpdateDb(void *ptr) {
  struct artsRemoteDbUpdatePacket *packet = (struct artsRemoteDbUpdatePacket *)ptr;
  struct artsDb *packetDb = (struct artsDb *)(packet + 1);
  unsigned int rank = artsGuidGetRank(packet->guid);
  if (rank == artsGlobalRankId) {
    if (packet->epochGuid != NULL_GUID) {
      incrementQueueEpoch(packet->epochGuid);
      globalShutdownGuidIncQueue();
    }
    struct artsDb **dataPtr = NULL;
    bool write = packet->header.size > sizeof(struct artsRemoteDbUpdatePacket);
    artsRouteTableLookupItemWithState(packet->guid, (void ***)&dataPtr,
                                      allocatedKey, write);
    struct artsDb *db = (dataPtr) ? *dataPtr : NULL;
    if (!db) {
      artsDbDecrementLatch(packet->guid);
      if (packet->epochGuid != NULL_GUID) {
        incrementFinishedEpoch(packet->epochGuid);
        globalShutdownGuidIncFinished();
      }
      return;
    }
    if (write) {
      uint64_t packetDbBytes =
          packet->header.size - sizeof(struct artsRemoteDbUpdatePacket);
      if (packetDbBytes < sizeof(struct artsDb) ||
          db->header.size < sizeof(struct artsDb)) {
        artsDbDecrementLatch(packet->guid);
        if (packet->epochGuid != NULL_GUID) {
          incrementFinishedEpoch(packet->epochGuid);
          globalShutdownGuidIncFinished();
        }
        return;
      }

      uint64_t remotePayloadBytes = packetDbBytes - sizeof(struct artsDb);
      uint64_t localPayloadBytes = db->header.size - sizeof(struct artsDb);
      uint64_t copyBytes = (remotePayloadBytes < localPayloadBytes)
                               ? remotePayloadBytes
                               : localPayloadBytes;

      void *ptr = (void *)(db + 1);
      memcpy(ptr, packetDb + 1, copyBytes);
      artsRouteTableSetRank(packet->guid, artsGlobalRankId);
      artsProgressFrontier(db, artsGlobalRankId);
    } else {
      artsProgressFrontier(db, packet->header.rank);
    }
    artsDbDecrementLatch(packet->guid);
    if (packet->epochGuid != NULL_GUID) {
      incrementFinishedEpoch(packet->epochGuid);
      globalShutdownGuidIncFinished();
    }
  }
}

void artsRemotePartialUpdateDb(artsGuid_t guid, struct artsDiffList *diffs,
                               void *working) {
  (void)guid;
  (void)diffs;
  (void)working;
  ARTS_DEBUG("artsRemotePartialUpdateDb: partial updates disabled");
}

void artsRemoteHandlePartialUpdate(void *ptr) {
  (void)ptr;
  ARTS_DEBUG("artsRemoteHandlePartialUpdate: partial updates disabled");
}

void artsRemoteMemoryMove(unsigned int route, artsGuid_t guid, void *ptr,
                          unsigned int memSize, unsigned messageType,
                          void (*freeMethod)(void *)) {
  REMOTE_MEMORY_MOVE_START();
  struct artsRemoteGuidOnlyPacket packet;
  artsFillPacketHeader(&packet.header, sizeof(packet) + memSize, messageType);
  packet.guid = guid;
  artsRemoteSendRequestPayloadAsyncFree(route, (char *)&packet, sizeof(packet),
                                        (char *)ptr, 0, memSize, freeMethod);
  artsRouteTableRemoveItem(guid);
  REMOTE_MEMORY_MOVE_STOP();
}

void artsRemoteMemoryMoveNoFree(unsigned int route, artsGuid_t guid, void *ptr,
                                unsigned int memSize, unsigned messageType) {
  struct artsRemoteGuidOnlyPacket packet;
  artsFillPacketHeader(&packet.header, sizeof(packet) + memSize, messageType);
  packet.guid = guid;
  artsRemoteSendRequestPayloadAsync(route, (char *)&packet, sizeof(packet),
                                    (char *)ptr, memSize);
}

void artsRemoteHandleEdtMove(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct artsRemoteGuidOnlyPacket);
  struct artsEdt *edt =
      (struct artsEdt *)artsMallocAlignWithType(size, 16, artsEdtMemorySize);

  memcpy(edt, packet + 1, size);
  artsRouteTableAddItemRace(edt, (artsGuid_t)packet->guid, artsGlobalRankId,
                            false);
  ARTS_DEBUG("EDT[Guid:%lu] Moved to Rank: %d", packet->guid,
             artsGlobalRankId);
  if (edt->depcNeeded == 0)
    artsHandleReadyEdt(edt);
  else
    artsRouteTableFireOO(packet->guid, artsOutOfOrderHandler);
}

void artsRemoteHandleDbMove(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct artsRemoteGuidOnlyPacket);

  struct artsDb *dbHeader = (struct artsDb *)(packet + 1);
  uint64_t dbSize = dbHeader->header.size;

  struct artsHeader *memPacket = (struct artsHeader *)artsMallocAlignWithType(
      dbSize, 16, artsDbMemorySize);

  if (size == dbSize)
    memcpy(memPacket, packet + 1, size);
  else {
    memPacket->type = (unsigned int)artsGuidGetType(packet->guid);
    memPacket->size = dbSize;
  }
  // We need a local pointer for this node
  if (dbHeader->dbList) {
    struct artsDb *newDb = (struct artsDb *)memPacket;
    newDb->dbList = artsNewDbList();
  }

  ARTS_DEBUG("DB[Guid:%lu] Moved to Rank: %d", packet->guid, artsGlobalRankId);
  if (artsRouteTableAddItemRace(memPacket, (artsGuid_t)packet->guid,
                                artsGlobalRankId, false))
    artsRouteTableFireOO(packet->guid, artsOutOfOrderHandler);
}

void artsRemoteHandleEventMove(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct artsRemoteGuidOnlyPacket);

  struct artsHeader *memPacket = (struct artsHeader *)artsMallocAlignWithType(
      size, 16, artsEventMemorySize);

  memcpy(memPacket, packet + 1, size);
  artsRouteTableAddItemRace(memPacket, (artsGuid_t)packet->guid,
                            artsGlobalRankId, false);
  artsRouteTableFireOO(packet->guid, artsOutOfOrderHandler);
}

void artsRemoteHandlePersistentEventMove(void *ptr) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct artsRemoteGuidOnlyPacket);

  struct artsHeader *memPacket = (struct artsHeader *)artsMallocAlignWithType(
      size, 16, artsPersistentEventMemorySize);

  memcpy(memPacket, packet + 1, size);
  ARTS_DEBUG("Persistent Event [Guid:%lu] Moved to Rank: %d", packet->guid,
             artsGlobalRankId);
  artsRouteTableAddItemRace(memPacket, (artsGuid_t)packet->guid,
                            artsGlobalRankId, false);
  artsRouteTableFireOO(packet->guid, artsOutOfOrderHandler);
}

static void sendRemoteEdtSignalPacket(artsGuid_t edt, artsGuid_t db,
                                      uint32_t slot, artsType_t mode,
                                      artsType_t acquireMode) {
  struct artsRemoteEdtSignalPacket packet;
  unsigned int rank = artsGuidGetRank(edt);

  if (rank == artsGlobalRankId)
    rank = artsRouteTableLookupRank(edt);
  if (rank == (unsigned int)-1) {
    ARTS_INFO("Remote EDT signal missing route rank for EDT[Guid:%lu] on rank "
              "%u; defaulting to local delivery path",
              edt, artsGlobalRankId);
    rank = artsGlobalRankId;
  }

  packet.db = db;
  packet.edt = edt;
  packet.slot = slot;
  packet.mode = mode;
  packet.dbRoute = artsGuidGetRank(db);
  packet.acquireMode = acquireMode;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_EDT_SIGNAL_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteSignalEdt(artsGuid_t edt, artsGuid_t db, uint32_t slot,
                         artsType_t mode) {
  ARTS_DEBUG("Remote Signal from DB[Guid:%lu] to EDT[Guid:%lu, Slot:%d, Rank: "
             "%d]",
             db, edt, slot, artsGuidGetRank(edt));
  sendRemoteEdtSignalPacket(edt, db, slot, mode, ARTS_NULL);
}

void artsRemoteSignalEdtWithHints(artsGuid_t edt, artsGuid_t db, uint32_t slot,
                                  artsType_t mode, artsType_t acquireMode) {
  ARTS_DEBUG("Remote Signal from DB[Guid:%lu] to EDT[Guid:%lu, Slot:%d, "
             "Rank: %d, AcquireMode:%u]",
             db, edt, slot, artsGuidGetRank(edt), acquireMode);
  sendRemoteEdtSignalPacket(edt, db, slot, mode, acquireMode);
}

void artsRemoteEventSatisfySlot(artsGuid_t eventGuid, artsGuid_t dataGuid,
                                uint32_t slot) {
  struct artsRemoteEventSatisfySlotPacket packet;
  packet.event = eventGuid;
  packet.db = dataGuid;
  packet.slot = slot;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_EVENT_SATISFY_SLOT_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(eventGuid), (char *)&packet,
                             sizeof(packet));
}

void artsRemotePersistentEventSatisfySlot(artsGuid_t eventGuid, uint32_t action,
                                          bool lock) {
  struct artsRemotePersistentEventSatisfySlotPacket packet;
  packet.event = eventGuid;
  packet.action = action;
  packet.lock = lock;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_PERSISTENT_EVENT_SATISFY_SLOT_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(eventGuid), (char *)&packet,
                             sizeof(packet));
}

static void sendRemoteDbAddDependencePacket(artsGuid_t dbSrc,
                                            artsGuid_t edtDest,
                                            uint32_t edtSlot,
                                            artsType_t acquireMode) {
  struct artsRemoteDbAddDependencePacket packet;
  packet.dbSrc = dbSrc;
  packet.edtDest = edtDest;
  packet.edtSlot = edtSlot;
  packet.acquireMode = acquireMode;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_ADD_DEPENDENCE_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(dbSrc), (char *)&packet,
                             sizeof(packet));
}

void artsRemoteDbAddDependence(artsGuid_t dbSrc, artsGuid_t edtDest,
                               uint32_t edtSlot) {
  sendRemoteDbAddDependencePacket(dbSrc, edtDest, edtSlot, ARTS_NULL);
}

void artsRemoteDbAddDependenceWithHints(artsGuid_t dbSrc, artsGuid_t edtDest,
                                        uint32_t edtSlot,
                                        artsType_t acquireMode) {
  sendRemoteDbAddDependencePacket(dbSrc, edtDest, edtSlot, acquireMode);
}

void artsRemoteDbAddDependenceWithByteOffset(artsGuid_t dbSrc,
                                             artsGuid_t edtDest,
                                             uint32_t edtSlot,
                                             artsType_t acquireMode,
                                             uint64_t byteOffset,
                                             uint64_t size) {
  struct artsRemoteDbAddDependenceWithByteOffsetPacket packet;
  packet.dbSrc = dbSrc;
  packet.edtDest = edtDest;
  packet.edtSlot = edtSlot;
  packet.acquireMode = acquireMode;
  packet.byteOffset = byteOffset;
  packet.size = size;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_ADD_DEPENDENCE_WITH_BYTE_OFFSET_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(dbSrc), (char *)&packet,
                             sizeof(packet));
}

void artsRemoteHandleDbAddDependenceWithByteOffset(void *ptr) {
  struct artsRemoteDbAddDependenceWithByteOffsetPacket *packet =
      (struct artsRemoteDbAddDependenceWithByteOffsetPacket *)ptr;

  /// Look up the local DB
  struct artsDb *dbRes =
      (struct artsDb *)artsRouteTableLookupItem(packet->dbSrc);
  if (dbRes != NULL) {
    /// DB is local - add dependency to its persistent event with byte offset
    artsAddDependenceToPersistentEventWithByteOffset(
        dbRes->eventGuid, packet->edtDest, packet->edtSlot, packet->acquireMode,
        packet->byteOffset, packet->size);
  } else {
    /// DB not found locally - this shouldn't happen as we routed to the owner
    ARTS_DEBUG("ESD: Remote byte-offset dep: DB %lu not found on node %u",
               packet->dbSrc, artsGlobalRankId);
  }
}

void artsRemoteDbIncrementLatch(artsGuid_t db) {
  struct artsRemoteGuidOnlyPacket packet;
  packet.guid = db;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_INCREMENT_LATCH_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(db), (char *)&packet,
                             sizeof(packet));
}

void artsRemoteDbDecrementLatch(artsGuid_t db) {
  struct artsRemoteGuidOnlyPacket packet;
  packet.guid = db;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_DECREMENT_LATCH_MSG);
  artsRemoteSendRequestAsync(artsGuidGetRank(db), (char *)&packet,
                             sizeof(packet));
}

static inline const char *remoteRouteStateName(itemState_t state);
static void logRouteGuidState(const char *source, artsGuid_t guid);

static void artsDbCallbackFailFast(const char *source, struct artsEdt *edt,
                                   unsigned int slot, struct artsDb *dbRes,
                                   artsGuid_t edtGuidHint,
                                   artsGuid_t dbGuidHint,
                                   artsType_t modeHint) {
  static volatile unsigned int failFastPrinted = 0U;
  const char *src = (source) ? source : "unknown";
  artsGuid_t edtGuid = edt ? edt->currentEdt : edtGuidHint;
  uint64_t edtId = edt ? edt->arts_id : 0;
  unsigned int depc = edt ? edt->depc : 0;
  unsigned int depcNeeded = edt ? edt->depcNeeded : 0;
  artsGuid_t dbGuid = dbRes ? dbRes->guid : dbGuidHint;

  // ARTS_ERROR is compiled out when ARTS_INFO_ENABLED is disabled.
  // Use ARTS_PRINT so this fatal context is always visible.
  ARTS_PRINT("[FATAL] Invalid DB request callback [%s]: edt=%p edtGuid=%lu "
             "id=%lu slot=%u depc=%u depcNeeded=%u db=%p dbGuid=%lu mode=%u "
             "rank=%u",
             src, edt, edtGuid, edtId, slot, depc, depcNeeded, dbRes, dbGuid,
             modeHint, artsGlobalRankId);
  logRouteGuidState("artsDbCallbackFailFast/db_state", dbGuid);
  logRouteGuidState("artsDbCallbackFailFast/edt_state", edtGuid);
  // Print one representative stack trace to avoid overwhelming logs.
  if (artsAtomicCswap(&failFastPrinted, 0U, 1U) == 0U)
    artsDebugPrintStack();
  artsRuntimeStop();
  abort();
}

void artsDbRequestCallbackWithContext(const char *source, struct artsEdt *edt,
                                      unsigned int slot, struct artsDb *dbRes,
                                      artsGuid_t edtGuidHint,
                                      artsGuid_t dbGuidHint,
                                      artsType_t modeHint) {
  if (!edt || !dbRes) {
    artsDbCallbackFailFast(source, edt, slot, dbRes, edtGuidHint, dbGuidHint,
                           modeHint);
    return;
  }
  if (slot >= edt->depc) {
    artsDbCallbackFailFast(source, edt, slot, dbRes, edtGuidHint, dbGuidHint,
                           modeHint);
    return;
  }
  artsEdtDep_t *depv = (artsEdtDep_t *)artsGetDepv(edt);
  depv[slot].ptr = dbRes + 1;
  unsigned int temp = artsAtomicSub(&edt->depcNeeded, 1U);
  if (temp == 0)
    artsHandleRemoteStolenEdt(edt);
}

void artsDbRequestCallback(struct artsEdt *edt, unsigned int slot,
                           struct artsDb *dbRes) {
  artsDbRequestCallbackWithContext("legacy", edt, slot, dbRes,
                                   edt ? edt->currentEdt : NULL_GUID,
                                   dbRes ? dbRes->guid : NULL_GUID, ARTS_NULL);
}

bool artsRemoteDbRequest(artsGuid_t dataGuid, int rank, struct artsEdt *edt,
                         int pos, artsType_t mode, bool aggRequest,
                         artsType_t acquireMode) {
  if (artsRouteTableAddSent(dataGuid, edt, pos, aggRequest)) {
    // Record the current request target even while the route entry is in
    // requested/reserved state so owner lookups never degrade to -1.
    artsRouteTableSetRank(dataGuid, rank);
    struct artsRemoteDbRequestPacket packet;
    packet.dbGuid = dataGuid;
    packet.mode = mode;
    packet.acquireMode = acquireMode;
    artsFillPacketHeader(&packet.header, sizeof(packet),
                         ARTS_REMOTE_DB_REQUEST_MSG);
    ARTS_DEBUG(
        "Rank %u requesting DB[Guid:%lu] from rank %d (slot=%d)",
        artsGlobalRankId, dataGuid, rank, pos);
    artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
    return true;
  }
  return false;
}

void artsRemoteDbForward(int destRank, int sourceRank, artsGuid_t dataGuid,
                         artsType_t mode) {
  struct artsRemoteDbRequestPacket packet;
  packet.header.size = sizeof(packet);
  packet.header.messageType = ARTS_REMOTE_DB_REQUEST_MSG;
  packet.header.rank = destRank;
  packet.dbGuid = dataGuid;
  packet.mode = mode;
  artsRemoteSendRequestAsync(sourceRank, (char *)&packet, sizeof(packet));
}

void artsRemoteDbSendNow(int rank, struct artsDb *db) {
  struct artsRemoteDbSendPacket packet;
  uint64_t size = sizeof(struct artsRemoteDbSendPacket) + db->header.size;
  artsFillPacketHeader(&packet.header, size, ARTS_REMOTE_DB_SEND_MSG);
  artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
                                    (char *)db, db->header.size);
}

static inline const char *remoteRouteStateName(itemState_t state) {
  switch (state) {
  case noKey:
    return "noKey";
  case anyKey:
    return "anyKey";
  case deletedKey:
    return "deletedKey";
  case allocatedKey:
    return "allocatedKey";
  case availableKey:
    return "availableKey";
  case requestedKey:
    return "requestedKey";
  case reservedKey:
    return "reservedKey";
  default:
    return "unknown";
  }
}

static void logRouteGuidState(const char *source, artsGuid_t guid) {
  if (!guid) {
    ARTS_PRINT("[%s] guid=NULL_GUID", source);
    return;
  }
  void **data = NULL;
  itemState_t state = artsRouteTableLookupItemWithState(guid, &data, anyKey, false);
  int owner = artsRouteTableLookupRank(guid);
  ARTS_PRINT("[%s] guid=%lu state=%s owner=%d data=%p", source, guid,
             remoteRouteStateName(state), owner, data ? *data : NULL);
}

void artsRemoteDbSendCheck(int rank, struct artsDb *db, artsGuid_t dbGuidHint,
                           artsType_t mode) {
  artsGuid_t dbGuid = db ? db->guid : dbGuidHint;
  if (!db) {
    ARTS_PRINT("[FATAL] artsRemoteDbSendCheck received NULL db: rank=%d "
               "dbGuidHint=%lu mode=%u localRank=%u",
               rank, dbGuidHint, mode, artsGlobalRankId);
    logRouteGuidState("artsRemoteDbSendCheck/db_state", dbGuid);
    artsDebugPrintStack();
    artsRuntimeStop();
    abort();
    return;
  }
  if (rank == artsGlobalRankId) {
    // A local requester should be satisfied in-process; never enqueue a remote
    // self-send.
    ARTS_DEBUG("Suppressing self DB send [Guid:%lu, Rank:%d, Mode:%u]",
               dbGuid, rank, mode);
    if (artsIsGuidLocal(dbGuid))
      artsRouteTableFireOO(dbGuid, artsOutOfOrderHandler);
    return;
  }
  if (!artsIsGuidLocal(dbGuid)) {
    artsRouteTableReturnDb(dbGuid, false);
    artsRemoteDbSendNow(rank, db);
  } else {
    // Even if the rank is already tracked as a duplicate holder, an explicit
    // remote request means the requester may have invalidated or discarded its
    // cached copy. Always send a response so waiting OO callbacks can fire.
    if (!artsAddDbDuplicate(db, rank, NULL, NULL_GUID, 0, mode)) {
      ARTS_DEBUG(
          "Remote DB send forced [Guid:%lu] to rank %d despite duplicate "
          "tracking (mode=%u)",
          dbGuid, rank, mode);
    }
    artsRemoteDbSendNow(rank, db);
  }
}

void artsRemoteDbSend(struct artsRemoteDbRequestPacket *pack) {
  unsigned int redirected = artsRouteTableLookupRank(pack->dbGuid);
  ARTS_DEBUG(
      "Remote DB Send [Guid:%lu] [Rank: %d] [Mode:%d] [AcquireMode:%d]",
      pack->dbGuid, pack->header.rank, pack->mode, pack->acquireMode);
  if (redirected != artsGlobalRankId && redirected != -1)
    artsRemoteSendRequestAsync(redirected, (char *)pack, pack->header.size);
  else {
    struct artsDb *db = (struct artsDb *)artsRouteTableLookupItem(pack->dbGuid);
    if (db == NULL) {
      artsOutOfOrderHandleRemoteDbSend(pack->header.rank, pack->dbGuid,
                                       pack->mode);
    } else if (!artsIsGuidLocal(db->guid) &&
               pack->header.rank == artsGlobalRankId) {
      // This is when the memory model sends a CDAG write after CDAG write to
      // the same node The artsIsGuidLocal should be an extra check, maybe not
      // required
      artsRouteTableFireOO(pack->dbGuid, artsOutOfOrderHandler);
    } else {
      artsRemoteDbSendCheck(pack->header.rank, db, pack->dbGuid, pack->mode);
    }
  }
}

void artsRemoteHandleDbReceived(struct artsRemoteDbSendPacket *packet) {
  struct artsDb *packetDb = (struct artsDb *)(packet + 1);
  ARTS_DEBUG("Handle DB Received [Guid:%lu] on rank %u", packetDb->guid,
             artsGlobalRankId);
  struct artsDb *dbRes = NULL;
  struct artsDb **dataPtr = NULL;
  itemState_t state = artsRouteTableLookupItemWithState(
      packetDb->guid, (void ***)&dataPtr, allocatedKey, true);

  struct artsDb *tPtr = (dataPtr) ? *dataPtr : NULL;
  struct artsDbList *dbList = NULL;
  if (tPtr && artsIsGuidLocal(packetDb->guid))
    dbList = (struct artsDbList *)tPtr->dbList;
  ARTS_DEBUG("Rec DB State: %u", state);
  switch (state) {
  case requestedKey: {
    if (packetDb->header.size == tPtr->header.size) {
      void *source = (void *)((struct artsDb *)packetDb + 1);
      void *dest = (void *)((struct artsDb *)tPtr + 1);
      memcpy(dest, source, packetDb->header.size - sizeof(struct artsDb));
      tPtr->dbList = dbList;
      dbRes = tPtr;
    } else {
      ARTS_INFO("Did the DB do a remote resize...");
    }
  } break;

  case reservedKey: {
    dbRes = (struct artsDb *)artsMallocAlignWithType(packetDb->header.size, 16,
                                                     artsDbMemorySize);
    memcpy(dbRes, packetDb, packetDb->header.size);
    if (artsIsGuidLocal(packetDb->guid))
      dbRes->dbList = artsNewDbList();
    else
      dbRes->dbList = NULL;
  } break;

  default: {
    itemState_t state = artsRouteTableLookupItemWithState(
        packetDb->guid, (void ***)&tPtr, anyKey, false);
  } break;
  }

  if (dbRes && artsRouteTableUpdateItem(packetDb->guid, (void *)dbRes,
                                        artsGlobalRankId, state)) {
    artsRouteTableFireOO(packetDb->guid, artsOutOfOrderHandler);
  }
}

void artsRemoteDbFullRequest(artsGuid_t dataGuid, int rank, artsGuid_t edtGuid,
                             int pos, artsType_t mode) {
  // Do not try to reduce full requests since they are unique
  struct artsRemoteDbFullRequestPacket packet;
  packet.dbGuid = dataGuid;
  packet.edtGuid = edtGuid;
  packet.slot = pos;
  packet.mode = mode;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_FULL_REQUEST_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
  ARTS_DEBUG("Full DB request sent [DbGuid:%lu, EdtGuid:%lu, Slot:%d, Mode:%u] "
             "from rank %u to rank %u",
             dataGuid, edtGuid, pos, mode, artsGlobalRankId, rank);
  ARTS_DEBUG("Request Full DB[Guid:%lu] from rank %u to rank %u, mode: %u",
             dataGuid, rank, packet.header.rank, mode);
}

void artsRemoteDbForwardFull(int destRank, int sourceRank, artsGuid_t dataGuid,
                             artsGuid_t edtGuid, int pos, artsType_t mode) {
  struct artsRemoteDbFullRequestPacket packet;
  packet.header.size = sizeof(packet);
  packet.header.messageType = ARTS_REMOTE_DB_FULL_REQUEST_MSG;
  packet.header.rank = destRank;
  packet.dbGuid = dataGuid;
  packet.edtGuid = edtGuid;
  packet.slot = pos;
  packet.mode = mode;
  artsRemoteSendRequestAsync(sourceRank, (char *)&packet, sizeof(packet));
}

void artsRemoteDbFullSendNow(int rank, struct artsDb *db, artsGuid_t edtGuid,
                             unsigned int slot, artsType_t mode) {
  struct artsRemoteDbFullSendPacket packet;
  packet.edtGuid = edtGuid;
  packet.slot = slot;
  packet.mode = mode;
  uint64_t size = sizeof(struct artsRemoteDbFullSendPacket) + db->header.size;
  artsFillPacketHeader(&packet.header, size, ARTS_REMOTE_DB_FULL_SEND_MSG);
  artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
                                    (char *)db, db->header.size);
  ARTS_DEBUG("Full DB send [DbGuid:%lu, EdtGuid:%lu, Slot:%u, Mode:%u, "
             "Size:%u] from rank %u to rank %u",
             db->guid, edtGuid, slot, mode, db->header.size, artsGlobalRankId,
             rank);
}

void artsRemoteDbFullSendCheck(int rank, struct artsDb *db,
                               artsGuid_t dbGuidHint, artsGuid_t edtGuid,
                               unsigned int slot, artsType_t mode) {
  artsGuid_t dbGuid = db ? db->guid : dbGuidHint;
  if (!db) {
    ARTS_PRINT("[FATAL] artsRemoteDbFullSendCheck received NULL db: rank=%d "
               "dbGuidHint=%lu edtGuid=%lu slot=%u mode=%u localRank=%u",
               rank, dbGuidHint, edtGuid, slot, mode, artsGlobalRankId);
    logRouteGuidState("artsRemoteDbFullSendCheck/db_state", dbGuid);
    logRouteGuidState("artsRemoteDbFullSendCheck/edt_state", edtGuid);
    artsDebugPrintStack();
    artsRuntimeStop();
    abort();
    return;
  }
  if (rank == artsGlobalRankId) {
    // A local requester should be satisfied directly; remote self-send triggers
    // the self-send check in RemoteProtocol.
    ARTS_DEBUG("Handling self FULL DB send locally [DbGuid:%lu, EdtGuid:%lu, "
               "Slot:%u, Mode:%u, Rank:%d]",
               dbGuid, edtGuid, slot, mode, rank);
    struct artsEdt *edt = (struct artsEdt *)artsRouteTableLookupItem(edtGuid);
    if (!edt) {
      void **edtData = NULL;
      itemState_t edtState = artsRouteTableLookupItemWithState(
          edtGuid, &edtData, anyKey, false);
      ARTS_INFO("Self FULL DB send with missing EDT[Guid:%lu] on rank %u "
                "(state=%u, data=%p) [DbGuid:%lu, Slot:%u, Mode:%u]",
                edtGuid, artsGlobalRankId, edtState, edtData ? *edtData : NULL,
                dbGuid, slot, mode);
    } else {
      artsDbRequestCallbackWithContext("full_send_check/local", edt, slot, db,
                                       edtGuid, dbGuid, mode);
    }
    artsClearExclusiveRequest(db, rank, edtGuid);
    return;
  }
  if (!artsIsGuidLocal(dbGuid)) {
    artsRouteTableReturnDb(dbGuid, false);
    artsRemoteDbFullSendNow(rank, db, edtGuid, slot, mode);
  } else {
    // Symmetric with read-path handling: an explicit full-DB request must
    // always receive a response, even if duplicate tracking already contains
    // the requester. The requester may have invalidated its local copy.
    if (!artsAddDbDuplicate(db, rank, NULL, edtGuid, slot, mode)) {
      ARTS_DEBUG(
          "Remote FULL DB send forced [Guid:%lu] to rank %d (edt=%lu slot=%u "
          "mode=%u) despite duplicate tracking",
          dbGuid, rank, edtGuid, slot, mode);
    }
    artsRemoteDbFullSendNow(rank, db, edtGuid, slot, mode);
    artsClearExclusiveRequest(db, rank, edtGuid);
  }
}

void artsRemoteDbFullSend(struct artsRemoteDbFullRequestPacket *pack) {
  unsigned int redirected = artsRouteTableLookupRank(pack->dbGuid);
  if (redirected != artsGlobalRankId && redirected != -1) {
    artsRemoteSendRequestAsync(redirected, (char *)pack, pack->header.size);
  } else {
    struct artsDb *db = (struct artsDb *)artsRouteTableLookupItem(pack->dbGuid);
    if (db == NULL) {
      artsOutOfOrderHandleRemoteDbFullSend(pack->dbGuid, pack->header.rank,
                                           pack->edtGuid, pack->slot,
                                           pack->mode);
    } else {
      artsRemoteDbFullSendCheck(pack->header.rank, db, pack->dbGuid,
                                pack->edtGuid, pack->slot, pack->mode);
    }
  }
}

void artsRemoteHandleDbFullRecieved(struct artsRemoteDbFullSendPacket *packet) {
  bool dec;
  itemState_t state;
  struct artsDb *packetDb = (struct artsDb *)(packet + 1);
  ARTS_DEBUG("Handle Full DB Received [Guid:%lu, Slot:%u, Mode:%u]",
             packetDb->guid, packet->slot, packet->mode);
  void **dataPtr = artsRouteTableReserve(packetDb->guid, &dec, &state);
  struct artsDb *dbRes = (dataPtr) ? (struct artsDb *)*dataPtr : NULL;
  if (dbRes) {
    if (packetDb->header.size == dbRes->header.size) {
      struct artsDbList *dbList = (struct artsDbList *)dbRes->dbList;
      void *source = (void *)((struct artsDb *)packetDb + 1);
      void *dest = (void *)((struct artsDb *)dbRes + 1);
      memcpy(dest, source, packetDb->header.size - sizeof(struct artsDb));
      dbRes->dbList = dbList;
    } else {
      ARTS_INFO("Did the DB do a remote resize...");
    }
  } else {
    dbRes = (struct artsDb *)artsMallocAlignWithType(packetDb->header.size, 16,
                                                     artsDbMemorySize);
    memcpy(dbRes, packetDb, packetDb->header.size);
    if (artsIsGuidLocal(packetDb->guid))
      dbRes->dbList = artsNewDbList();
    else
      dbRes->dbList = NULL;
  }
  if (artsRouteTableUpdateItem(packetDb->guid, (void *)dbRes, artsGlobalRankId,
                               state))
    artsRouteTableFireOO(packetDb->guid, artsOutOfOrderHandler);
  struct artsEdt *edt =
      (struct artsEdt *)artsRouteTableLookupItem(packet->edtGuid);
  if (!edt) {
    void **edtData = NULL;
    itemState_t edtState = artsRouteTableLookupItemWithState(
        packet->edtGuid, &edtData, anyKey, false);
    ARTS_INFO("Full DB received for missing EDT[Guid:%lu] on rank %u "
              "(state=%u, data=%p) [DbGuid:%lu, Slot:%u, Mode:%u]",
              packet->edtGuid, artsGlobalRankId, edtState,
              edtData ? *edtData : NULL, packetDb->guid, packet->slot,
              packet->mode);
    return;
  }
  artsDbRequestCallbackWithContext("db_full_received", edt, packet->slot, dbRes,
                                   packet->edtGuid, packetDb->guid,
                                   packet->mode);
}

void artsRemoteSendAlreadyLocal(int rank, artsGuid_t guid, artsGuid_t edtGuid,
                                unsigned int slot, artsType_t mode) {
  struct artsRemoteDbFullRequestPacket packet;
  packet.dbGuid = guid;
  packet.edtGuid = edtGuid;
  packet.slot = slot;
  packet.mode = mode;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_DB_FULL_SEND_ALREADY_LOCAL_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleSendAlreadyLocal(void *pack) {
  struct artsRemoteDbFullRequestPacket *packet =
      (struct artsRemoteDbFullRequestPacket *)pack;
  int rank;
  struct artsDb *dbRes =
      (struct artsDb *)artsRouteTableLookupDb(packet->dbGuid, &rank, true);
  struct artsEdt *edt =
      (struct artsEdt *)artsRouteTableLookupItem(packet->edtGuid);
  if (!edt) {
    void **edtData = NULL;
    itemState_t edtState = artsRouteTableLookupItemWithState(
        packet->edtGuid, &edtData, anyKey, false);
    ARTS_INFO("Already-local DB received for missing EDT[Guid:%lu] on rank %u "
              "(state=%u, data=%p) [DbGuid:%lu, Slot:%u, Mode:%u]",
              packet->edtGuid, artsGlobalRankId, edtState,
              edtData ? *edtData : NULL, packet->dbGuid, packet->slot,
              packet->mode);
    return;
  }
  if (!dbRes) {
    unsigned int owner = artsRouteTableLookupRank(packet->dbGuid);
    ARTS_INFO("Already-local DB missing for EDT[Guid:%lu] on rank %u "
              "[DbGuid:%lu, Slot:%u, Mode:%u, lookupRank:%d, owner:%u] "
              "- falling back to full request",
              packet->edtGuid, artsGlobalRankId, packet->dbGuid, packet->slot,
              packet->mode, rank, owner);
    if (owner != (unsigned int)-1 && owner != artsGlobalRankId) {
      artsRemoteDbFullRequest(packet->dbGuid, owner, packet->edtGuid,
                              packet->slot, packet->mode);
    }
    return;
  }
  artsDbRequestCallbackWithContext("send_already_local", edt, packet->slot,
                                   dbRes, packet->edtGuid, packet->dbGuid,
                                   packet->mode);
}

void artsRemoteGetFromDb(artsGuid_t edtGuid, artsGuid_t dbGuid,
                         unsigned int slot, unsigned int offset,
                         unsigned int size, unsigned int rank) {
  struct artsRemoteGetPutPacket packet;
  packet.edtGuid = edtGuid;
  packet.dbGuid = dbGuid;
  packet.slot = slot;
  packet.offset = offset;
  packet.size = size;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_GET_FROM_DB_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleGetFromDb(void *pack) {
  struct artsRemoteGetPutPacket *packet = (struct artsRemoteGetPutPacket *)pack;
  artsGetFromDbAt(packet->edtGuid, packet->dbGuid, packet->slot, packet->offset,
                  packet->size, artsGlobalRankId);
}

void artsRemotePutInDb(void *ptr, artsGuid_t edtGuid, artsGuid_t dbGuid,
                       unsigned int slot, unsigned int offset,
                       unsigned int size, artsGuid_t epochGuid,
                       unsigned int rank) {
  struct artsRemoteGetPutPacket packet;
  packet.edtGuid = edtGuid;
  packet.dbGuid = dbGuid;
  packet.epochGuid = epochGuid;
  packet.slot = slot;
  packet.offset = offset;
  packet.size = size;
  uint64_t totalSize = sizeof(struct artsRemoteGetPutPacket) + size;
  artsFillPacketHeader(&packet.header, totalSize, ARTS_REMOTE_PUT_IN_DB_MSG);
  //    artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
  //    (char *)ptr, size);
  artsRemoteSendRequestPayloadAsyncFree(rank, (char *)&packet, sizeof(packet),
                                        (char *)ptr, 0, size, artsFree);
}

void artsRemoteHandlePutInDb(void *pack) {
  struct artsRemoteGetPutPacket *packet = (struct artsRemoteGetPutPacket *)pack;
  void *data = (void *)(packet + 1);
  internalPutInDb(data, packet->edtGuid, packet->dbGuid, packet->slot,
                  packet->offset, packet->size, packet->epochGuid,
                  artsGlobalRankId);
}

void artsRemoteSignalEdtWithPtr(artsGuid_t edtGuid, artsGuid_t dbGuid,
                                void *ptr, unsigned int size,
                                unsigned int slot) {
  unsigned int rank = artsGuidGetRank(edtGuid);
  ARTS_DEBUG("SEND NOW: %u -> %u", artsGlobalRankId, rank);
  struct artsRemoteSignalEdtWithPtrPacket packet;
  packet.edtGuid = edtGuid;
  packet.dbGuid = dbGuid;
  packet.size = size;
  packet.slot = slot;
  uint64_t totalSize = sizeof(struct artsRemoteSignalEdtWithPtrPacket) + size;
  artsFillPacketHeader(&packet.header, totalSize,
                       ARTS_REMOTE_SIGNAL_EDT_WITH_PTR_MSG);
  artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
                                    (char *)ptr, size);
}

void artsRemoteHandleSignalEdtWithPtr(void *pack) {
  struct artsRemoteSignalEdtWithPtrPacket *packet =
      (struct artsRemoteSignalEdtWithPtrPacket *)pack;
  void *source = (void *)(packet + 1);
  void *dest = artsMalloc(packet->size);
  memcpy(dest, source, packet->size);
  artsSignalEdtPtrWithGuid(packet->edtGuid, packet->slot, packet->dbGuid, dest,
                           packet->size);
}

void artsRemoteMetricUpdate(int rank, int type, int level, uint64_t timeStamp,
                            uint64_t toAdd, bool sub) {
  ARTS_DEBUG("Remote Metric Update");
  struct artsRemoteMetricUpdate packet;
  packet.type = type;
  packet.timeStamp = timeStamp;
  packet.toAdd = toAdd;
  packet.sub = sub;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_METRIC_UPDATE_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteSend(unsigned int rank, sendHandler_t funPtr, void *args,
                    unsigned int size, bool free) {
  if (rank == artsGlobalRankId) {
    funPtr(args);
    if (free)
      artsFree(args);
    return;
  }
  struct artsRemoteSend packet;
  packet.funPtr = funPtr;
  int totalSize = sizeof(struct artsRemoteSend) + size;
  artsFillPacketHeader(&packet.header, totalSize, ARTS_REMOTE_SEND_MSG);

  if (free)
    artsRemoteSendRequestPayloadAsyncFree(rank, (char *)&packet, sizeof(packet),
                                          (char *)args, 0, size, artsFree);
  else
    artsRemoteSendRequestPayloadAsync(rank, (char *)&packet, sizeof(packet),
                                      (char *)args, size);
}

void artsRemoteHandleSend(void *pack) {
  struct artsRemoteSend *packet = (struct artsRemoteSend *)pack;
  void *args = (void *)(packet + 1);
  packet->funPtr(args);
}

void artsRemoteEpochInitSend(unsigned int rank, artsGuid_t epochGuid,
                             artsGuid_t edtGuid, unsigned int slot) {
  struct artsRemoteEpochInitPacket packet;
  packet.epochGuid = epochGuid;
  packet.edtGuid = edtGuid;
  packet.slot = slot;
  artsFillPacketHeader(&packet.header, sizeof(packet), ARTS_EPOCH_INIT_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleEpochInitSend(void *pack) {
  ARTS_DEBUG("Net Epoch Init Rec");
  struct artsRemoteEpochInitPacket *packet =
      (struct artsRemoteEpochInitPacket *)pack;
  artsGuid_t localEpochGuid = packet->epochGuid;
  createEpoch(&localEpochGuid, packet->edtGuid, packet->slot);
  packet->epochGuid = localEpochGuid;
}

void artsRemoteEpochInitPoolSend(unsigned int rank, unsigned int poolSize,
                                 artsGuid_t startGuid, artsGuid_t poolGuid) {
  //    ARTS_INFO("Net Epoch Init Pool Send: %u %lu %lu", rank, startGuid,
  //    poolGuid);
  struct artsRemoteEpochInitPoolPacket packet;
  packet.poolSize = poolSize;
  packet.startGuid = startGuid;
  packet.poolGuid = poolGuid;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_EPOCH_INIT_POOL_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleEpochInitPoolSend(void *pack) {
  //    ARTS_INFO("Net Epoch Init Pool Rec");
  struct artsRemoteEpochInitPoolPacket *packet =
      (struct artsRemoteEpochInitPoolPacket *)pack;
  //    ARTS_INFO("Net Epoch Init Pool Rec %lu %lu", packet->startGuid,
  //    packet->poolGuid);
  artsGuid_t local_poolGuid = packet->poolGuid;
  artsGuid_t local_startGuid = packet->startGuid;
  createEpochPool(&local_poolGuid, packet->poolSize, &local_startGuid);
  packet->poolGuid = local_poolGuid;
  packet->startGuid = local_startGuid;
}

void artsRemoteEpochReq(unsigned int rank, artsGuid_t guid) {
  struct artsRemoteGuidOnlyPacket packet;
  packet.guid = guid;
  artsFillPacketHeader(&packet.header, sizeof(packet), ARTS_EPOCH_REQ_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleEpochReq(void *pack) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)pack;
  // For now the source and dest are the same...
  sendEpoch(packet->guid, packet->header.rank, packet->header.rank);
}

void artsRemoteEpochSend(unsigned int rank, artsGuid_t guid,
                         unsigned int active, unsigned int finish) {
  struct artsRemoteEpochSendPacket packet;
  packet.epochGuid = guid;
  packet.active = active;
  packet.finish = finish;
  artsFillPacketHeader(&packet.header, sizeof(packet), ARTS_EPOCH_SEND_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleEpochSend(void *pack) {
  struct artsRemoteEpochSendPacket *packet =
      (struct artsRemoteEpochSendPacket *)pack;
  reduceEpoch(packet->epochGuid, packet->active, packet->finish);
}

void artsRemoteAtomicAddInArrayDb(unsigned int rank, artsGuid_t dbGuid,
                                  unsigned int index, unsigned int toAdd,
                                  artsGuid_t edtGuid, unsigned int slot,
                                  artsGuid_t epochGuid) {
  struct artsRemoteAtomicAddInArrayDbPacket packet;
  packet.dbGuid = dbGuid;
  packet.edtGuid = edtGuid;
  packet.epochGuid = epochGuid;
  packet.slot = slot;
  packet.index = index;
  packet.toAdd = toAdd;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_ATOMIC_ADD_ARRAYDB_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleAtomicAddInArrayDb(void *pack) {
  struct artsRemoteAtomicAddInArrayDbPacket *packet =
      (struct artsRemoteAtomicAddInArrayDbPacket *)pack;
  struct artsDb *db = (struct artsDb *)artsRouteTableLookupItem(packet->dbGuid);
  internalAtomicAddInArrayDb(packet->dbGuid, packet->index, packet->toAdd,
                             packet->edtGuid, packet->slot, packet->epochGuid);
}

void artsRemoteAtomicCompareAndSwapInArrayDb(
    unsigned int rank, artsGuid_t dbGuid, unsigned int index,
    unsigned int oldValue, unsigned int newValue, artsGuid_t edtGuid,
    unsigned int slot, artsGuid_t epochGuid) {
  struct artsRemoteAtomicCompareAndSwapInArrayDbPacket packet;
  packet.dbGuid = dbGuid;
  packet.edtGuid = edtGuid;
  packet.epochGuid = epochGuid;
  packet.slot = slot;
  packet.index = index;
  packet.oldValue = oldValue;
  packet.newValue = newValue;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_ATOMIC_CAS_ARRAYDB_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleAtomicCompareAndSwapInArrayDb(void *pack) {
  struct artsRemoteAtomicCompareAndSwapInArrayDbPacket *packet =
      (struct artsRemoteAtomicCompareAndSwapInArrayDbPacket *)pack;
  struct artsDb *db = (struct artsDb *)artsRouteTableLookupItem(packet->dbGuid);
  internalAtomicCompareAndSwapInArrayDb(
      packet->dbGuid, packet->index, packet->oldValue, packet->newValue,
      packet->edtGuid, packet->slot, packet->epochGuid);
}

void artsRemoteEpochDelete(unsigned int rank, artsGuid_t epochGuid) {
  struct artsRemoteGuidOnlyPacket packet;
  packet.guid = epochGuid;
  artsFillPacketHeader(&packet.header, sizeof(packet), ARTS_EPOCH_DELETE_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleEpochDelete(void *pack) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)pack;
  deleteEpoch(packet->guid, NULL);
}

void artsDbMoveRequest(artsGuid_t dbGuid, unsigned int destRank) {
  struct artsRemoteDbRequestPacket packet;
  packet.dbGuid = dbGuid;
  packet.mode = ARTS_DB_ONCE;
  packet.header.size = sizeof(packet);
  packet.header.messageType = ARTS_REMOTE_DB_MOVE_REQ_MSG;
  packet.header.rank = destRank;
  artsRemoteSendRequestAsync(artsGuidGetRank(dbGuid), (char *)&packet,
                             sizeof(packet));
}

void artsDbMoveRequestHandle(void *pack) {
  struct artsRemoteDbRequestPacket *packet =
      (struct artsRemoteDbRequestPacket *)pack;
  artsDbMove(packet->dbGuid, packet->header.rank);
}

void artsRemoteHandleBufferSend(void *pack) {
  struct artsRemoteGuidOnlyPacket *packet =
      (struct artsRemoteGuidOnlyPacket *)pack;
  uint64_t size =
      packet->header.size - sizeof(struct artsRemoteGuidOnlyPacket);
  void *buffer = (void *)(packet + 1);
  artsSetBuffer(packet->guid, buffer, size);
}

void artsRemoteSignalContext(unsigned int rank, uint64_t ticket) {
  struct artsRemoteSignalContextPacket packet;
  packet.ticket = ticket;
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_ATOMIC_ADD_ARRAYDB_MSG);
  artsRemoteSendRequestAsync(rank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleSignalContext(void *pack) {
  struct artsRemoteSignalContextPacket *packet =
      (struct artsRemoteSignalContextPacket *)pack;
  artsSignalContext(packet->ticket);
}

void artsRemoteDbRename(artsGuid_t newGuid, artsGuid_t oldGuid) {
  unsigned int destRank = artsGuidGetRank(oldGuid);
  struct artsRemoteDbRename packet;
  packet.oldGuid = oldGuid;
  packet.newGuid = newGuid;
  packet.header.size = sizeof(packet);
  packet.header.messageType = ARTS_REMOTE_DB_RENAME_MSG;
  packet.header.rank = destRank;
  artsRemoteSendRequestAsync(destRank, (char *)&packet, sizeof(packet));
}

void artsRemoteHandleDbRename(void *pack) {
  struct artsRemoteDbRename *packet = (struct artsRemoteDbRename *)pack;
  artsDbRenameWithGuid(packet->newGuid, packet->oldGuid);
}

// RTT-based time synchronization for counter capture alignment
// External declarations for time sync state (defined in Counter.c)
extern volatile int64_t artsCounterTimeOffset;
extern volatile bool artsCounterTimeSyncReceived;

// Worker sends sync request to master with its current timestamp (T1)
void artsRemoteTimeSyncRequest(void) {
  struct artsRemoteTimeSyncReqPacket packet;
  packet.workerSendTime = artsGetTimeStamp(); // T1
  artsFillPacketHeader(&packet.header, sizeof(packet),
                       ARTS_REMOTE_TIME_SYNC_REQ_MSG);

  // Send to master
  artsRemoteSendRequestAsync(artsGlobalMasterRankId, (char *)&packet,
                             sizeof(packet));
  ARTS_INFO("Time sync: Worker %u sent request to master %u at T1=%lu",
            artsGlobalRankId, artsGlobalMasterRankId, packet.workerSendTime);
}

// Master handles sync request: records T2 and sends response with T1, T2
void artsRemoteHandleTimeSyncReq(void *pack) {
  struct artsRemoteTimeSyncReqPacket *req =
      (struct artsRemoteTimeSyncReqPacket *)pack;
  uint64_t masterRecvTime = artsGetTimeStamp(); // T2

  struct artsRemoteTimeSyncRespPacket resp;
  resp.workerSendTime = req->workerSendTime; // Echo T1
  resp.masterRecvTime = masterRecvTime;      // T2
  artsFillPacketHeader(&resp.header, sizeof(resp),
                       ARTS_REMOTE_TIME_SYNC_RESP_MSG);

  // Send response back to the requesting worker
  artsRemoteSendRequestAsync(req->header.rank, (char *)&resp, sizeof(resp));
  ARTS_INFO("Time sync: Master received request from rank %u, T1=%lu, T2=%lu",
            req->header.rank, req->workerSendTime, masterRecvTime);
}

// Worker handles sync response: calculates offset using RTT
void artsRemoteHandleTimeSyncResp(void *pack) {
  struct artsRemoteTimeSyncRespPacket *resp =
      (struct artsRemoteTimeSyncRespPacket *)pack;
  uint64_t workerRecvTime = artsGetTimeStamp(); // T3

  uint64_t T1 = resp->workerSendTime;
  uint64_t T2 = resp->masterRecvTime;
  uint64_t T3 = workerRecvTime;

  // RTT = T3 - T1 (round-trip time in worker's clock)
  // One-way delay estimate = RTT / 2 (assuming symmetric network)
  // At T2 (master clock), worker clock was approximately T1 + RTT/2
  // offset = workerTime - masterTime = (T1 + RTT/2) - T2 = (T1 + T3)/2 - T2
  int64_t offset = (int64_t)((T1 + T3) / 2) - (int64_t)T2;

  artsAtomicStoreI64Relaxed((volatile int64_t *)&artsCounterTimeOffset, offset);
  artsAtomicStoreBoolRelease((volatile bool *)&artsCounterTimeSyncReceived,
                             true);

  uint64_t rtt = T3 - T1;
  ARTS_INFO("Time sync: Worker %u received response, T1=%lu, T2=%lu, T3=%lu, "
            "RTT=%lu ns (%.3f ms), offset=%ld ns (%.3f ms)",
            artsGlobalRankId, T1, T2, T3, rtt, (double)rtt / 1000000.0, offset,
            (double)offset / 1000000.0);
}
