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

#include "arts/runtime/memory/DbFunctions.h"
#include "arts/arts.h"
#include "arts/gas/Guid.h"
#include "arts/gas/OutOfOrder.h"
#include "arts/gas/RouteTable.h"
#include "arts/introspection/Counter.h"
#include "arts/introspection/Introspection.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/RT.h"
#include "arts/runtime/Runtime.h"
#include "arts/runtime/compute/EdtFunctions.h"
#include "arts/runtime/memory/DbList.h"
#include "arts/runtime/network/RemoteFunctions.h"
#include "arts/runtime/sync/TerminationDetection.h"
#include "arts/system/Debug.h"
#include "arts/utils/Atomics.h"
#include <assert.h>

#ifdef USE_GPU
#include "arts/gpu/GpuRuntime.cuh"
#endif

#define DPRINTF(...)

artsTypeName;

// inline void * artsDbMalloc(artsType_t mode, unsigned int size)
void *artsDbMalloc(artsType_t mode, unsigned int size) {
  void *ptr = NULL;
#ifdef USE_GPU
  if (artsNodeInfo.gpu) {
    if (mode == ARTS_DB_LC)
      ptr = artsCudaMallocHost(size * 2);
    else if (mode == ARTS_DB_GPU_READ || mode == ARTS_DB_GPU_WRITE)
      ptr = artsCudaMallocHost(size);
  }
#endif
  if (!ptr)
    ptr = artsMallocAlign(size, 16);
  return ptr;
}

void artsDbFree(void *ptr) {
  struct artsDb *db = (struct artsDb *)ptr;
#ifdef USE_GPU
  if (artsNodeInfo.gpu &&
      (db->header.type == ARTS_DB_GPU_READ ||
       db->header.type == ARTS_DB_GPU_WRITE || db->header.type == ARTS_DB_LC)) {
    artsCudaFreeHost(ptr);
    ptr = NULL;
  }
#endif
  if (ptr)
    artsFreeAlign(ptr);
}

void artsDbCreateInternal(artsGuid_t guid, void *addr, uint64_t size,
                          uint64_t packetSize, artsType_t mode) {
  struct artsHeader *header = (struct artsHeader *)addr;
  header->type = mode;
  header->size = packetSize;

  struct artsDb *dbRes = (struct artsDb *)header;
  dbRes->guid = guid;
  dbRes->version = 0;
  dbRes->reader = 0;
  dbRes->writer = 0;
  dbRes->copyCount = 0;
  if (mode != ARTS_DB_PIN) {
    dbRes->dbList = artsNewDbList();
  }
  if (mode == ARTS_DB_LC) {
    void *shadowCopy = (void *)(((char *)addr) + packetSize);
    memcpy(shadowCopy, addr, sizeof(struct artsDb));
  }
#ifdef USE_SMART_DB
  dbRes->eventGuid = artsPersistentEventCreate(artsGuidGetRank(guid), 0, guid);
#endif
}

artsGuid_t artsDbCreateRemote(unsigned int route, uint64_t size,
                              artsType_t mode) {
  ARTSEDTCOUNTERTIMERSTART(dbCreateCounter);
  artsGuid_t guid = artsGuidCreateForRank(route, mode);
  void *ptr = artsDbMalloc(mode, sizeof(struct artsDb));
  struct artsDb *db = (struct artsDb *)ptr;
  db->header.size = size + sizeof(struct artsDb);
  db->dbList = (mode == ARTS_DB_PIN) ? (void *)0 : (void *)1;

  artsRemoteMemoryMove(route, guid, ptr, sizeof(struct artsDb),
                       ARTS_REMOTE_DB_SEND_MSG, artsDbFree);
  ARTSEDTCOUNTERTIMERENDINCREMENT(dbCreateCounter);
  return guid;
}

// Creates a local DB only
artsGuid_t artsDbCreate(void **addr, uint64_t size, artsType_t mode) {
  ARTSEDTCOUNTERTIMERSTART(dbCreateCounter);
  artsGuid_t guid = NULL_GUID;
  unsigned int dbSize = size + sizeof(struct artsDb);

  ARTSSETMEMSHOTTYPE(artsDbMemorySize);
  void *ptr = artsDbMalloc(mode, dbSize);
  ARTSSETMEMSHOTTYPE(artsDefaultMemorySize);
  if (ptr) {
    guid = artsGuidCreateForRank(artsGlobalRankId, mode);
    artsDbCreateInternal(guid, ptr, size, dbSize, mode);
    // change false to true to force a manual DB delete
    artsRouteTableAddItem(ptr, guid, artsGlobalRankId, false);
    *addr = (void *)((struct artsDb *)ptr + 1);
  }
  ARTSEDTCOUNTERTIMERENDINCREMENT(dbCreateCounter);
  return guid;
}

artsGuid_t artsDbCreatePtr(artsPtr_t *addr, uint64_t size, artsType_t mode) {
  ARTSEDTCOUNTERTIMERSTART(dbCreateCounter);
  artsGuid_t guid = NULL_GUID;
  unsigned int dbSize = size + sizeof(struct artsDb);

  ARTSSETMEMSHOTTYPE(artsDbMemorySize);
  void *ptr = artsDbMalloc(mode, dbSize);
  ARTSSETMEMSHOTTYPE(artsDefaultMemorySize);
  if (ptr) {
    guid = artsGuidCreateForRank(artsGlobalRankId, mode);
    artsDbCreateInternal(guid, ptr, size, dbSize, mode);
    // change false to true to force a manual DB delete
    artsRouteTableAddItem(ptr, guid, artsGlobalRankId, false);
    *addr = (artsPtr_t)((struct artsDb *)ptr + 1);
  }
  ARTSEDTCOUNTERTIMERENDINCREMENT(dbCreateCounter);
  return guid;
}

// Guid must be for a local DB only
void *artsDbCreateWithGuid(artsGuid_t guid, uint64_t size) {
  ARTSEDTCOUNTERTIMERSTART(dbCreateCounter);
  artsType_t mode = artsGuidGetType(guid);

  PRINTF("Creating DB with guid: %lu and mode %s\n", guid, getTypeName(mode));
  void *ptr = NULL;
  if (artsIsGuidLocal(guid)) {
    unsigned int dbSize = size + sizeof(struct artsDb);

    ARTSSETMEMSHOTTYPE(artsDbMemorySize);
    ptr = artsDbMalloc(mode, dbSize);
    ARTSSETMEMSHOTTYPE(artsDefaultMemorySize);
    if (ptr) {
      artsDbCreateInternal(guid, ptr, size, dbSize, mode);
      if (artsRouteTableAddItemRace(ptr, guid, artsGlobalRankId, false)) {
        DPRINTF("RUNNING OO\n");
        artsRouteTableFireOO(guid, artsOutOfOrderHandler);
      } else {
        DPRINTF("NOT RUNNING OO\n");
      }
      ptr = (void *)((struct artsDb *)ptr + 1);
    }
  }
  ARTSEDTCOUNTERTIMERENDINCREMENT(dbCreateCounter);
  return ptr;
}

void *artsDbCreateWithGuidAndData(artsGuid_t guid, void *data, uint64_t size) {
  ARTSEDTCOUNTERTIMERSTART(dbCreateCounter);
  artsType_t mode = artsGuidGetType(guid);
  void *ptr = NULL;
  if (artsIsGuidLocal(guid)) {
    unsigned int dbSize = size + sizeof(struct artsDb);

    ARTSSETMEMSHOTTYPE(artsDbMemorySize);
    ptr = artsDbMalloc(mode, dbSize);
    ARTSSETMEMSHOTTYPE(artsDefaultMemorySize);

    if (ptr) {
      artsDbCreateInternal(guid, ptr, size, dbSize, mode);
      void *dbData = (void *)((struct artsDb *)ptr + 1);
      memcpy(dbData, data, size);
      if (artsRouteTableAddItemRace(ptr, guid, artsGlobalRankId, false))
        artsRouteTableFireOO(guid, artsOutOfOrderHandler);
      ptr = dbData;
    }
  }
  ARTSEDTCOUNTERTIMERENDINCREMENT(dbCreateCounter);
  return ptr;
}

void *artsDbResizePtr(struct artsDb *dbRes, unsigned int size, bool copy) {
  if (dbRes) {
    unsigned int oldSize = dbRes->header.size;
    unsigned int newSize = size + sizeof(struct artsDb);
    ARTSSETMEMSHOTTYPE(artsDbMemorySize);
    struct artsDb *ptr = artsCallocAlign(size + sizeof(struct artsDb), 16);
    ARTSSETMEMSHOTTYPE(artsDefaultMemorySize);
    if (ptr) {
      if (copy)
        memcpy(ptr, dbRes, oldSize);
      else
        memcpy(ptr, dbRes, sizeof(struct artsDb));
      artsFreeAlign(dbRes);
      ptr->header.size = size + sizeof(struct artsDb);
      return (void *)(ptr + 1);
    }
  }
  return NULL;
}

// Must be in write mode (or only copy) to update and alloced (no NO_ACQUIRE
// nonsense), otherwise will be racy...
void *artsDbResize(artsGuid_t guid, unsigned int size, bool copy) {
  struct artsDb *dbRes = artsRouteTableLookupItem(guid);
  void *ptr = artsDbResizePtr(dbRes, size, copy);
  if (ptr) {
    dbRes = ((struct artsDb *)ptr) - 1;
    //        artsRouteTableUpdateItem(dbRes, guid, artsGlobalRankId);
  }
  return ptr;
}

void artsDbMove(artsGuid_t dbGuid, unsigned int rank) {
  unsigned int guidRank = artsGuidGetRank(dbGuid);
  if (guidRank != rank) {
    if (guidRank != artsGlobalRankId)
      artsDbMoveRequest(dbGuid, rank);
    else {
      struct artsDb *dbRes = artsRouteTableLookupItem(dbGuid);
      if (dbRes)
        artsRemoteMemoryMove(rank, dbGuid, dbRes, dbRes->header.size,
                             ARTS_REMOTE_DB_MOVE_MSG, artsDbFree);
      //                artsRemoteMemoryMove(rank, dbGuid, dbRes,
      //                dbRes->header.size, ARTS_REMOTE_DB_MOVE_MSG, artsFree);
      else
        artsOutOfOrderDbMove(dbGuid, rank);
    }
  }
}

void artsDbDestroy(artsGuid_t guid) {
  artsType_t mode = artsGuidGetType(guid);
  struct artsDb *dbRes = artsRouteTableLookupItem(guid);
  if (dbRes != NULL) {
    artsRemoteDbDestroy(guid, artsGlobalRankId, 0);
    artsDbFree(dbRes);
    artsRouteTableRemoveItem(guid);
  } else
    artsRemoteDbDestroy(guid, artsGlobalRankId, 0);
}

bool artsDbRenameWithGuid(artsGuid_t newGuid, artsGuid_t oldGuid) {
  bool ret = false;
  unsigned int rank = artsGuidGetRank(oldGuid);
  if (rank == artsGlobalRankId) {
    struct artsDb *dbRes = artsRouteTableLookupItem(oldGuid);
    if (dbRes != NULL) {
      dbRes->guid = newGuid;
      // This is only being done by the owner...
      artsRouteTableHideItem(oldGuid);
      if (artsRouteTableAddItemRace(dbRes, newGuid, artsGlobalRankId, false)) {
        DPRINTF("RUNNING OO %lu\n");
        artsRouteTableFireOO(newGuid, artsOutOfOrderHandler);
      } else {
        DPRINTF("NOT RUNNING OO %lu\n");
      }
      ret = true;
    }
  } else
    artsRemoteDbRename(newGuid, oldGuid);
  return true;
}

artsGuid_t artsDbCopyToNewType(artsGuid_t oldGuid, artsType_t newType) {
  artsGuid_t ret = NULL_GUID;
  unsigned int rank = artsGuidGetRank(oldGuid);
  if (rank == artsGlobalRankId) {
    artsGuid_t newGuid = artsGuidCreateForRank(rank, artsGuidGetType(newType));
    struct artsDb *dbRes = artsRouteTableLookupItem(oldGuid);
    if (dbRes != NULL) {
      artsAtomicAdd(&dbRes->copyCount, 1);
      dbRes->guid = newGuid;
      if (artsRouteTableAddItemRace(dbRes, newGuid, artsGlobalRankId, false)) {
        DPRINTF("RUNNING OO %lu\n");
        artsRouteTableFireOO(newGuid, artsOutOfOrderHandler);
      } else {
        DPRINTF("NOT RUNNING OO %lu\n");
      }
      ret = newGuid;
    }
  }
  return ret;
}

artsGuid_t artsDbRename(artsGuid_t guid) {
  artsGuid_t newGuid =
      artsGuidCreateForRank(artsGuidGetRank(guid), artsGuidGetType(guid));
  return (artsDbRenameWithGuid(newGuid, guid)) ? newGuid : NULL_GUID;
}

void artsDbDestroySafe(artsGuid_t guid, bool remote) {
  struct artsDb *dbRes = artsRouteTableLookupItem(guid);
  if (dbRes != NULL) {
    if (remote)
      artsRemoteDbDestroy(guid, artsGlobalRankId, 0);
    artsDbFree(dbRes);
    //        artsFree(dbRes);
    artsRouteTableRemoveItem(guid);
  } else if (remote)
    artsRemoteDbDestroy(guid, artsGlobalRankId, 0);
}

#ifdef USE_SMART_DB
void artsDbIncrementLatch(artsGuid_t guid) {
  struct artsDb *dbRes = artsRouteTableLookupItem(guid);
  if (dbRes != NULL)
    artsPersistentEventIncrementLatch(dbRes->eventGuid);
  else
    artsRemoteDbIncrementLatch(guid);
}

void artsDbDecrementLatch(artsGuid_t guid) {
  struct artsDb *dbRes = artsRouteTableLookupItem(guid);
  if (dbRes != NULL)
    artsPersistentEventDecrementLatch(dbRes->eventGuid);
  else
    artsRemoteDbDecrementLatch(guid);
}

void artsDbAddDependence(artsGuid_t dbSrc, artsGuid_t edtDest,
                         uint32_t edtSlot) {
  struct artsDb *dbRes = artsRouteTableLookupItem(dbSrc);
  if (dbRes != NULL)
    artsAddDependenceToPersistentEvent(dbRes->eventGuid, edtDest, edtSlot);
  else
    artsRemoteDbAddDependence(dbSrc, edtDest, edtSlot);
}
#endif

/**********************DB MEMORY MODEL*************************************/
// Side Effects: edt depcNeeded will be incremented, ptr will be updated,
//   and launches out of order handleReadyEdt
// Returns false on out of order and true otherwise
void acquireDbs(struct artsEdt *edt) {
  artsEdtDep_t *depv = artsGetDepv(edt);
  edt->depcNeeded = edt->depc + 1;
  for (int i = 0; i < edt->depc; i++) {
    // PRINTF("MODE: %s -> %p\n", getTypeName(depv[i].mode), depv[i].ptr);
    if (depv[i].guid && depv[i].ptr == NULL) {
      struct artsDb *dbFound = NULL;
      int owner = artsGuidGetRank(depv[i].guid);
      // PRINTF("Owner: %u\n", owner);
      switch (depv[i].mode) {
      // This case assumes that the guid exists only on the owner
      case ARTS_DB_ONCE: {
        if (owner != artsGlobalRankId) {
          artsOutOfOrderHandleDbRequest(depv[i].guid, edt, i, false);
          artsDbMove(depv[i].guid, artsGlobalRankId);
          break;
        }
        // else fall through to the local case :-p
      }
      case ARTS_DB_ONCE_LOCAL: {
        struct artsDb *dbTemp = artsRouteTableLookupItem(depv[i].guid);
        if (dbTemp) {
          dbFound = dbTemp;
          artsAtomicSub(&edt->depcNeeded, 1U);
        } else
          artsOutOfOrderHandleDbRequest(depv[i].guid, edt, i, false);
        break;
      }
      case ARTS_DB_PIN: {
        int validRank = -1;
        struct artsDb *dbTemp =
            artsRouteTableLookupDb(depv[i].guid, &validRank, true);
        if (dbTemp) {
          dbFound = dbTemp;
          artsAtomicSub(&edt->depcNeeded, 1U);
        } else {
          artsOutOfOrderHandleDbRequest(depv[i].guid, edt, i, true);
        }
        break;
      }
      case ARTS_DB_LC_SYNC: {
        // Owner Rank
        if (owner == artsGlobalRankId) {
          int validRank = -1;
          struct artsDb *dbTemp =
              artsRouteTableLookupDb(depv[i].guid, &validRank, false);
          // We have found an entry
          if (dbTemp) {
            DPRINTF("MODE: %s -> %p\n", getTypeName(depv[i].mode), dbTemp);
            dbFound = dbTemp;
            artsAtomicSub(&edt->depcNeeded, 1U);
          }
          // The Db hasn't been created yet
          else {
            // TODO: Create an out-of-order sync
            DPRINTF("%lu out of order request for LC_SYNC not supported yet\n",
                    depv[i].guid);
            // artsOutOfOrderHandleDbRequest(depv[i].guid, edt, i, true);
          }
        }
        break;
      }
      case ARTS_DB_LC_NO_COPY:
      case ARTS_DB_GPU_MEMSET:
      case ARTS_DB_GPU_READ:
      case ARTS_DB_GPU_WRITE:
      case ARTS_DB_LC:
      case ARTS_DB_READ:
      case ARTS_DB_WRITE: {
        // Owner Rank
        if (owner == artsGlobalRankId) {
          int validRank = -1;
          struct artsDb *dbTemp =
              artsRouteTableLookupDb(depv[i].guid, &validRank, true);
          // We have found an entry
          if (dbTemp) {
            if (artsAddDbDuplicate(dbTemp, artsGlobalRankId, edt, i,
                                   depv[i].mode)) {
              DPRINTF("Adding duplicate %u\n", depv[i].guid);
              // Owner rank and we have the valid copy
              if (validRank == artsGlobalRankId) {
                dbFound = dbTemp;
                artsAtomicSub(&edt->depcNeeded, 1U);
              }
              // Owner rank but someone else has valid copy
              else {
                if (depv[i].mode == ARTS_DB_READ ||
                    depv[i].mode == ARTS_DB_GPU_READ ||
                    depv[i].mode == ARTS_DB_GPU_WRITE ||
                    depv[i].mode == ARTS_DB_LC ||
                    depv[i].mode == ARTS_DB_LC_NO_COPY ||
                    depv[i].mode == ARTS_DB_GPU_MEMSET)
                  artsRemoteDbRequest(depv[i].guid, validRank, edt, i,
                                      depv[i].mode, true);
                else
                  artsRemoteDbFullRequest(depv[i].guid, validRank, edt, i,
                                          depv[i].mode);
              }
            } else {
              DPRINTF("Duplicate not added %lu\n", depv[i].guid);
            }
          }
          // The Db hasn't been created yet
          else {
            PRINTF("%u out of order request slot %u\n", depv[i].guid, i);
            artsOutOfOrderHandleDbRequest(depv[i].guid, edt, i, true);
          }
        } else {
          int validRank = -1;
          struct artsDb *dbTemp =
              artsRouteTableLookupDb(depv[i].guid, &validRank, true);
          // We have found an entry
          if (dbTemp) {
            dbFound = dbTemp;
            artsAtomicSub(&edt->depcNeeded, 1U);
          }

          if (depv[i].mode == ARTS_DB_WRITE) {
            // We can't aggregate read requests for cdag write
            artsRemoteDbFullRequest(depv[i].guid, owner, edt, i, depv[i].mode);
          } else if (!dbTemp) {
            // We can aggregate read requests for reads
            artsRemoteDbRequest(depv[i].guid, owner, edt, i, depv[i].mode,
                                true);
          }
        }
      } break;

      case ARTS_NULL:
      default:
        artsAtomicSub(&edt->depcNeeded, 1U);
        break;
      }

      if (dbFound)
        depv[i].ptr = dbFound + 1;
      // Shouldn't there be an else return here...
    } else {
      artsAtomicSub(&edt->depcNeeded, 1U);
    }
  }
  PRINTF("[acquireDbs] EDT %u has finished acquiring DBs\n", edt->currentEdt);
}

void prepDbs(unsigned int depc, artsEdtDep_t *depv, bool gpu) {
  for (unsigned int i = 0; i < depc; i++) {
    if (depv[i].guid != NULL_GUID && depv[i].mode == ARTS_DB_WRITE) {
      artsRemoteUpdateRouteTable(depv[i].guid, -1);
    }
#ifdef USE_GPU
    if (!gpu && depv[i].mode == ARTS_DB_LC) {
      struct artsDb *db = ((struct artsDb *)depv[i].ptr) - 1;
      artsReaderLock(&db->reader, &db->writer);
      internalIncDbVersion(&db->version);
    }

    if (!gpu && depv[i].mode == ARTS_DB_LC_SYNC) {
      struct artsDb *db = ((struct artsDb *)depv[i].ptr) - 1;
      DPRINTF("internalLCSync %lu %p\n", depv[i].guid, db);
      internalLCSyncGPU(depv[i].guid, db);
    }
#endif
  }
}

void releaseDbs(unsigned int depc, artsEdtDep_t *depv, bool gpu) {
  for (int i = 0; i < depc; i++) {
    DPRINTF("Releasing %u", depv[i].guid);
    unsigned int owner = artsGuidGetRank(depv[i].guid);
#ifdef USE_SMART_DB
    /// Smart DBs automatically decrement the latch count when the db is
    /// released.
    if (depv[i].mode == ARTS_DB_PIN || depv[i].mode == ARTS_DB_WRITE) {
      artsDbDecrementLatch(depv[i].guid);
    }
#endif
    if (depv[i].guid != NULL_GUID && depv[i].mode == ARTS_DB_WRITE) {
      // Signal we finished and progress frontier
      if (owner == artsGlobalRankId) {
        struct artsDb *db = ((struct artsDb *)depv[i].ptr - 1);
        artsProgressFrontier(db, artsGlobalRankId);
      } else {
        artsRemoteUpdateDb(depv[i].guid, false);
      }
    } else if (depv[i].mode == ARTS_DB_ONCE_LOCAL ||
               depv[i].mode == ARTS_DB_ONCE) {
      artsRouteTableInvalidateItem(depv[i].guid);
    } else if (depv[i].mode == ARTS_PTR) {
      artsFree(depv[i].ptr);
    } else if (!gpu && depv[i].mode == ARTS_DB_LC) {
      struct artsDb *db = ((struct artsDb *)depv[i].ptr) - 1;
      artsReaderUnlock(&db->reader);
    } else {
      if (artsRouteTableReturnDb(depv[i].guid, depv[i].mode != ARTS_DB_PIN)) {
        DPRINTF("FREED A COPY!\n");
      }
    }
  }
}

bool artsAddDbDuplicate(struct artsDb *db, unsigned int rank,
                        struct artsEdt *edt, unsigned int slot,
                        artsType_t mode) {
  bool write = (mode == ARTS_DB_WRITE);
  bool exclusive = false;
  return artsPushDbToList(db->dbList, rank, write, exclusive,
                          artsGuidGetRank(db->guid) == rank, false, edt, slot,
                          mode);
}

void internalGetFromDb(artsGuid_t edtGuid, artsGuid_t dbGuid, unsigned int slot,
                       unsigned int offset, unsigned int size,
                       unsigned int rank) {
  if (rank == artsGlobalRankId) {
    struct artsDb *db = artsRouteTableLookupItem(dbGuid);
    if (db) {
      void *data = (void *)(((char *)(db + 1)) + offset);
      void *ptr = artsMalloc(size);
      memcpy(ptr, data, size);
      PRINTF("GETTING: %u From: %p\n", *(unsigned int *)ptr, data);
      if (edtGuid != NULL_GUID)
        artsSignalEdtPtr(edtGuid, slot, ptr, size);
      artsUpdatePerformanceMetric(artsGetBW, artsThread, size, false);
    } else {
      assert(edtGuid != NULL_GUID && "DB not found and no EDT to signal");
      PRINTF("GETTING OO: %u From: %p\n", 0, NULL);
      artsOutOfOrderGetFromDb(edtGuid, dbGuid, slot, offset, size);
    }
  } else {
    DPRINTF("Sending to %u\n", rank);
    assert(edtGuid != NULL_GUID && "DB not found and no EDT to signal");
    artsRemoteGetFromDb(edtGuid, dbGuid, slot, offset, size, rank);
  }
}

void artsGetFromDb(artsGuid_t edtGuid, artsGuid_t dbGuid, unsigned int slot,
                   unsigned int offset, unsigned int size) {
  ARTSEDTCOUNTERTIMERSTART(getDbCounter);
  unsigned int rank = artsGuidGetRank(dbGuid);
  internalGetFromDb(edtGuid, dbGuid, slot, offset, size, rank);
  ARTSEDTCOUNTERTIMERENDINCREMENT(getDbCounter);
}

void artsGetFromDbAt(artsGuid_t edtGuid, artsGuid_t dbGuid, unsigned int slot,
                     unsigned int offset, unsigned int size,
                     unsigned int rank) {
  ARTSEDTCOUNTERTIMERSTART(getDbCounter);
  internalGetFromDb(edtGuid, dbGuid, slot, offset, size, rank);
  ARTSEDTCOUNTERTIMERENDINCREMENT(getDbCounter);
}

void internalPutInDb(void *ptr, artsGuid_t edtGuid, artsGuid_t dbGuid,
                     unsigned int slot, unsigned int offset, unsigned int size,
                     artsGuid_t epochGuid, unsigned int rank) {
  if (rank == artsGlobalRankId) {
    struct artsDb *db = artsRouteTableLookupItem(dbGuid);
    if (db) {
      // Do this so when we increment finished we can check the term status
      incrementQueueEpoch(epochGuid);
      globalShutdownGuidIncQueue();
      void *data = (void *)(((char *)(db + 1)) + offset);
      memcpy(data, ptr, size);
      if (edtGuid != NULL_GUID)
        artsSignalEdt(edtGuid, slot, dbGuid);
      incrementFinishedEpoch(epochGuid);
      globalShutdownGuidIncFinished();
      artsUpdatePerformanceMetric(artsPutBW, artsThread, size, false);
    } else {
      void *cpyPtr = artsMalloc(size);
      memcpy(cpyPtr, ptr, size);
      artsOutOfOrderPutInDb(cpyPtr, edtGuid, dbGuid, slot, offset, size,
                            epochGuid);
    }
  } else {
    void *cpyPtr = artsMalloc(size);
    memcpy(cpyPtr, ptr, size);
    artsRemotePutInDb(cpyPtr, edtGuid, dbGuid, slot, offset, size, epochGuid,
                      rank);
  }
}

void artsPutInDbAt(void *ptr, artsGuid_t edtGuid, artsGuid_t dbGuid,
                   unsigned int slot, unsigned int offset, unsigned int size,
                   unsigned int rank) {
  ARTSEDTCOUNTERTIMERSTART(putDbCounter);
  artsGuid_t epochGuid = artsGetCurrentEpochGuid();
  DPRINTF("EPOCH %lu\n", epochGuid);
  incrementActiveEpoch(epochGuid);
  globalShutdownGuidIncActive();
  internalPutInDb(ptr, edtGuid, dbGuid, slot, offset, size, epochGuid, rank);
  ARTSEDTCOUNTERTIMERENDINCREMENT(putDbCounter);
}

void artsPutInDb(void *ptr, artsGuid_t edtGuid, artsGuid_t dbGuid,
                 unsigned int slot, unsigned int offset, unsigned int size) {
  ARTSEDTCOUNTERTIMERSTART(putDbCounter);
  unsigned int rank = artsGuidGetRank(dbGuid);
  artsGuid_t epochGuid = artsGetCurrentEpochGuid();
  DPRINTF("EPOCH %lu\n", epochGuid);
  incrementActiveEpoch(epochGuid);
  globalShutdownGuidIncActive();
  internalPutInDb(ptr, edtGuid, dbGuid, slot, offset, size, epochGuid, rank);
  ARTSEDTCOUNTERTIMERENDINCREMENT(putDbCounter);
}

void artsPutInDbEpoch(void *ptr, artsGuid_t epochGuid, artsGuid_t dbGuid,
                      unsigned int offset, unsigned int size) {
  ARTSEDTCOUNTERTIMERSTART(putDbCounter);
  unsigned int rank = artsGuidGetRank(dbGuid);
  incrementActiveEpoch(epochGuid);
  globalShutdownGuidIncActive();
  internalPutInDb(ptr, NULL_GUID, dbGuid, 0, offset, size, epochGuid, rank);
  ARTSEDTCOUNTERTIMERENDINCREMENT(putDbCounter);
}
