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
#include "arts/introspection/Counter.h"
#include "arts/introspection/Introspection.h"
#include "arts/runtime/Globals.h"
#include "arts/runtime/RT.h"
#include "arts/system/Debug.h"

#define IS_POWER_OF_TWO(x) (!((x) & ((x)-1)))

typedef struct header {
  size_t size;
  size_t align; // 0 if not aligned
  void *base;
} header_t;

static inline void *alignPointer(void *ptr, size_t align) {
  return (void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

void *artsMalloc(size_t size) {
  ARTSEDTCOUNTERTIMERSTART(mallocMemory);

  if (!size)
    return NULL;

  header_t *base = malloc(size + sizeof(header_t));
  if (!base)
    return NULL;

  base->size = size;
  base->align = 0;
  base->base = base;

  if (artsThreadInfo.mallocTrace)
    artsUpdatePerformanceMetric(artsMallocBW, artsThread, size, false);
  ARTSEDTCOUNTERTIMERENDINCREMENT(mallocMemory);

  return base + 1;
}

void *artsMallocAlign(size_t size, size_t align) {
  ARTSEDTCOUNTERTIMERSTART(mallocMemory);

  if (!size || align < ALIGNMENT || !IS_POWER_OF_TWO(align))
    return NULL;

  void *base = malloc(size + align - 1 + sizeof(header_t));
  if (!base)
    return NULL;

  void *aligned = alignPointer((char *)base + sizeof(header_t), align);
  header_t *hdr = (header_t *)aligned - 1;

  hdr->size = size;
  hdr->align = align;
  hdr->base = base;

  if (artsThreadInfo.mallocTrace)
    artsUpdatePerformanceMetric(artsMallocBW, artsThread, size, false);
  ARTSEDTCOUNTERTIMERENDINCREMENT(mallocMemory);

  return aligned;
}

void *artsCalloc(size_t nmemb, size_t size) {
  ARTSEDTCOUNTERTIMERSTART(callocMemory);

  if (!nmemb || !size || size > SIZE_MAX / nmemb)
    return NULL;

  size_t totalSize = nmemb * size;
  void *ptr = artsMalloc(totalSize);
  if (!ptr)
    return NULL;

  memset(ptr, 0, totalSize);

  ARTSEDTCOUNTERTIMERENDINCREMENT(callocMemory);

  return ptr;
}

void *artsCallocAlign(size_t nmemb, size_t size, size_t align) {
  ARTSEDTCOUNTERTIMERSTART(callocMemory);

  if (!nmemb || !size || size > SIZE_MAX / nmemb || align < ALIGNMENT ||
      !IS_POWER_OF_TWO(align))
    return NULL;

  size_t totalSize = nmemb * size;
  void *ptr = artsMallocAlign(totalSize, align);
  if (!ptr)
    return NULL;

  memset(ptr, 0, totalSize);

  ARTSEDTCOUNTERTIMERENDINCREMENT(callocMemory);

  return ptr;
}

void *artsRealloc(void *ptr, size_t size) {
  if (!ptr)
    return artsMalloc(size);
  if (!size) {
    artsFree(ptr);
    return NULL;
  }

  header_t *old_hdr = (header_t *)ptr - 1;
  size_t old_size = old_hdr->size;
  if (size <= old_size) {
    old_hdr->size = size;
    return ptr;
  }

  size_t align = old_hdr->align;

  void *new_ptr = align ? artsMallocAlign(size, align) : artsMalloc(size);
  if (!new_ptr)
    return NULL;
  memcpy(new_ptr, ptr, old_size);
  artsFree(ptr);
  return new_ptr;
}

void artsFree(void *ptr) {
  ARTSEDTCOUNTERTIMERSTART(freeMemory);

  if (!ptr)
    return;
  header_t *hdr = (header_t *)ptr - 1;
  free(hdr->base);

  ARTSEDTCOUNTERTIMERENDINCREMENT(freeMemory);
}
