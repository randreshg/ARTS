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
#include "arts/gas/OutOfOrderList.h"

#include "arts/arts.h"
#include "arts/system/ArtsPrint.h"
#include "arts/system/Debug.h"
#include "arts/utils/Atomics.h"
#include <stdlib.h>
#include <time.h>

#define fireLock 1U
#define resetLock 2U

static inline unsigned int ooWriterLockLoad(
    const struct artsOutOfOrderList *list) {
  return artsAtomicLoadU32Relaxed(&list->writerLock);
}

static inline unsigned int ooReaderLockLoad(
    const struct artsOutOfOrderList *list) {
  return artsAtomicLoadU32Relaxed(&list->readerLock);
}

static inline struct artsOutOfOrderElement *
ooNextLoad(volatile struct artsOutOfOrderElement *elem) {
  return (struct artsOutOfOrderElement *)artsAtomicLoadPtrAcquire(
      (void *const volatile *)&elem->next);
}

static inline void ooNextStore(volatile struct artsOutOfOrderElement *elem,
                               struct artsOutOfOrderElement *next) {
  artsAtomicStorePtrRelease((void *volatile *)&elem->next, next);
}

typedef struct {
  unsigned int elementCount;
  unsigned int occupiedSlots;
  void *firstPtr;
  unsigned int firstElement;
  unsigned int firstSlot;
  unsigned int sampleCount;
  void *samplePtr[4];
  unsigned int sampleElement[4];
  unsigned int sampleSlot[4];
} artsOODeleteStats_t;

bool readerOOTryLock(struct artsOutOfOrderList *list) {
  while (1) {
    if (ooWriterLockLoad(list) == fireLock)
      return false;
    while (ooWriterLockLoad(list) == resetLock)
      ;
    artsAtomicFetchAdd(&list->readerLock, 1U);
    if (ooWriterLockLoad(list) == 0)
      break;
    artsAtomicSub(&list->readerLock, 1U);
  }
  return true;
}

inline void readerOOLock(struct artsOutOfOrderList *list) {
  while (1) {
    while (ooWriterLockLoad(list))
      ;
    artsAtomicFetchAdd(&list->readerLock, 1U);
    if (ooWriterLockLoad(list) == 0)
      break;
    artsAtomicSub(&list->readerLock, 1U);
  }
}

void readerOOUnlock(struct artsOutOfOrderList *list) {
  artsAtomicSub(&list->readerLock, 1U);
}

void writerOOLock(struct artsOutOfOrderList *list, unsigned int lockType) {
  while (artsAtomicCswap(&list->writerLock, 0U, lockType) != 0U)
    ;
  while (ooReaderLockLoad(list))
    ;
  return;
}

void writerOOUnlock(struct artsOutOfOrderList *list) {
  artsAtomicSwap(&list->writerLock, 0U);
}

bool writerTryOOLock(struct artsOutOfOrderList *list, unsigned int lockType) {
  // Attempt to acquire the writer lock atomically
  unsigned int temp = artsAtomicCswap(&list->writerLock, 0U, lockType);

  if (temp == 0U) {
    // We got the writer lock - now check for readers
    unsigned int readerCount = ooReaderLockLoad(list);
    if (readerCount) {
      // Readers are present - release lock and fail immediately
      writerOOUnlock(list);
      return false;
    }
    // No readers - we have exclusive access
    return true;
  }

  if (temp == lockType) {
    // We already hold this lock type - prevent re-entry
    return false;
  }

  // Lock is held by someone else - fail immediately
  return false;
}

bool artsOOisFired(struct artsOutOfOrderList *list) {
  return artsAtomicLoadU32Relaxed(&list->isFired) != 0U;
}

bool artsOutOfOrderListAddItem(struct artsOutOfOrderList *addToMe, void *item) {
  if (!readerOOTryLock(addToMe)) {
    return false;
  }

  if (artsOOisFired(addToMe)) {
    readerOOUnlock(addToMe);
    return false;
  }
  unsigned int pos = artsAtomicFetchAdd(&addToMe->count, 1U);
  unsigned int numElements = pos / OOPERELEMENT;
  unsigned int elementPos = pos % OOPERELEMENT;

  volatile struct artsOutOfOrderElement *current = &addToMe->head;
  for (unsigned int i = 0; i < numElements; i++) {
    struct artsOutOfOrderElement *next = ooNextLoad(current);
    if (!next) {
      if (i + 1 == numElements && elementPos == 0) {
        struct artsOutOfOrderElement *created =
            (struct artsOutOfOrderElement *)artsCalloc(
                1, sizeof(struct artsOutOfOrderElement));
        ooNextStore(current, created);
        next = created;
      } else {
        while (!(next = ooNextLoad(current)))
          ;
      }
    }
    current = next;
  }

  // Always insert and always release lock
  // The CAS is used to wait for slot availability, but we should still unlock
  while (artsAtomicCswapPtr((volatile void **)&current->array[elementPos],
                            (void *)0, item)) {
    // Slot was occupied - this shouldn't happen in normal operation
    // but we need to wait for it to become available
  }

  readerOOUnlock(addToMe);
  return true;
}

void artsOutOfOrderListReset(struct artsOutOfOrderList *list) {
  if (writerTryOOLock(list, resetLock)) {
    artsAtomicSwap(&list->isFired, 0U);
    writerOOUnlock(list);
  }
}

static unsigned int deleteOOElements(struct artsOutOfOrderElement *current,
                                     bool waitForClear,
                                     artsOODeleteStats_t *stats) {
  struct artsOutOfOrderElement *trail = NULL;
  unsigned int dropped = 0;
  unsigned int elementIndex = 0;
  if (stats) {
    stats->elementCount = 0U;
    stats->occupiedSlots = 0U;
    stats->firstPtr = NULL;
    stats->firstElement = 0U;
    stats->firstSlot = 0U;
    stats->sampleCount = 0U;
  }
  while (current) {
    if (stats)
      stats->elementCount++;
    for (unsigned int i = 0; i < OOPERELEMENT; i++) {
      if (waitForClear) {
        while (current->array[i])
          ;
      } else if (current->array[i]) {
        void *entry = (void *)current->array[i];
        if (stats) {
          stats->occupiedSlots++;
          if (!stats->firstPtr) {
            stats->firstPtr = entry;
            stats->firstElement = elementIndex;
            stats->firstSlot = i;
          }
          if (stats->sampleCount < 4U) {
            unsigned int idx = stats->sampleCount++;
            stats->samplePtr[idx] = entry;
            stats->sampleElement[idx] = elementIndex;
            stats->sampleSlot[idx] = i;
          }
        }
        // We are tearing down the OO list under exclusive writer ownership and
        // the list is detached, so pending entries cannot be processed. Drop
        // them instead of blocking indefinitely.
        current->array[i] = NULL;
        dropped++;
      }
    }
    trail = current;
    current = ooNextLoad(current);
    artsFree(trail);
    elementIndex++;
  }
  return dropped;
}

void artsOutOfOrderListDelete(struct artsOutOfOrderList *list,
                              uint64_t contextKey, unsigned int contextRank,
                              uint64_t contextLock, void *contextData) {
  unsigned int preReader = ooReaderLockLoad(list);
  unsigned int preWriter = ooWriterLockLoad(list);
  unsigned int preCount = artsAtomicLoadU32Relaxed(&list->count);
  unsigned int preFired = artsAtomicLoadU32Relaxed(&list->isFired);
  struct artsOutOfOrderElement *preHeadNext = ooNextLoad(&list->head);

  writerOOLock(list, resetLock);
  unsigned int heldReader = ooReaderLockLoad(list);
  unsigned int heldWriter = ooWriterLockLoad(list);
  struct artsOutOfOrderElement *detached = ooNextLoad(&list->head);
  ooNextStore(&list->head, NULL);
  unsigned int pending = artsAtomicLoadU32Relaxed(&list->count);
  artsAtomicSwap(&list->isFired, 0U);
  artsAtomicStoreU32Relaxed(&list->count, 0U);
  writerOOUnlock(list);

  artsOODeleteStats_t stats;
  unsigned int dropped = deleteOOElements(detached, false, &stats);
  if (pending || dropped) {
    // TODO(debug-cleanup): remove extended oo-delete diagnostics after the
    // distributed-db teardown issue is fully root-caused and stable.
    ARTS_ERROR("artsOutOfOrderListDelete dropped pending entries [pending=%u "
               "dropped=%u]",
               pending, dropped);
    ARTS_PRINT("[FATAL] artsOutOfOrderListDelete dropped pending entries "
               "[pending=%u dropped=%u]",
               pending, dropped);
    ARTS_PRINT("[FATAL] oo-delete context [key=%lu rank=%u lock=0x%lx data=%p "
               "list=%p]",
               contextKey, contextRank, contextLock, contextData, list);
    ARTS_PRINT("[FATAL] oo-delete snapshot pre [reader=%u writer=%u count=%u "
               "fired=%u headNext=%p]",
               preReader, preWriter, preCount, preFired, preHeadNext);
    ARTS_PRINT("[FATAL] oo-delete snapshot held [reader=%u writer=%u detached=%p "
               "pending=%u]",
               heldReader, heldWriter, detached, pending);
    ARTS_PRINT("[FATAL] oo-delete detached summary [elements=%u occupiedSlots=%u "
               "firstPtr=%p firstPos=%u:%u sampleCount=%u]",
               stats.elementCount, stats.occupiedSlots, stats.firstPtr,
               stats.firstElement, stats.firstSlot, stats.sampleCount);
    for (unsigned int i = 0; i < stats.sampleCount; i++) {
      ARTS_PRINT("[FATAL] oo-delete sample[%u] ptr=%p pos=%u:%u", i,
                 stats.samplePtr[i], stats.sampleElement[i],
                 stats.sampleSlot[i]);
    }
    artsDebugPrintStack();
    abort();
  }
}

void artsOutOfOrderListFireCallback(struct artsOutOfOrderList *fireMe,
                                    void *localGuidAddress,
                                    void (*callback)(void *, void *)) {
  // Retry mechanism: Try multiple times with brief delays
  // This allows readers to complete and release locks
  // 1000 attempts × 10μs ≈ 10ms total retry window
  const int MAX_RETRIES = 1000;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (writerTryOOLock(fireMe, fireLock)) {
      artsAtomicSwap(&fireMe->isFired, 1U);
      unsigned int pos = artsAtomicLoadU32Relaxed(&fireMe->count);
      unsigned int j = 0;
      for (volatile struct artsOutOfOrderElement *current = &fireMe->head;
           current; current = ooNextLoad(current)) {
        for (unsigned int i = 0; i < OOPERELEMENT; i++) {
          if (j < pos) {
            volatile void *item = NULL;
            while (!item) {
              item = artsAtomicSwapPtr((volatile void **)&current->array[i],
                                       (void *)0);
            }
            callback((void *)item, localGuidAddress);
            j++;
          }
        }
        if (j == pos)
          break;
        while (!ooNextLoad(current))
          ;
      }
      artsAtomicStoreU32Relaxed(&fireMe->count, 0U);
      struct artsOutOfOrderElement *p = ooNextLoad(&fireMe->head);
      ooNextStore(&fireMe->head, NULL);
      writerOOUnlock(fireMe);
      deleteOOElements(p, true, NULL);
      return;
    }

    // Failed to get lock - yield CPU briefly to let readers finish
    // Only yield on attempts after the first few quick tries
    if (attempt > 5) {
      // Use nanosleep for 10 microseconds
      struct timespec ts = {0, 10000};
      nanosleep(&ts, NULL);
    }
  }

  // If we get here, we failed after MAX_RETRIES attempts
  // This should be very rare, but log it for debugging
  ARTS_ERROR("artsOutOfOrderListFireCallback: failed to acquire lock after %d "
             "attempts",
             MAX_RETRIES);
}
