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
#ifndef ARTS_MEMORY_REGPOOL_CLASS_H
#define ARTS_MEMORY_REGPOOL_CLASS_H

#include <stddef.h>
#include <stdint.h>

#include <mimalloc.h>
#include <mimalloc/types.h> /* the block-size geometry the key mirrors */

/* The class of an aligned request on the registered pool's arena path
 * (alignment at most 64 KiB).  The allocator draws such a request from at
 * most two of its page queues: its size's (a block there that happens to be
 * aligned, or any block when that block size is naturally aligned for the
 * alignment) and its over-allocated size's.  Which one decides is a function
 * of the size against the alignment, the size's block size and the
 * alignment, so these, with the over-allocated size's block size, name the
 * class: an arena serves or refuses two requests of one class alike, except
 * that a request may also take an aligned block that happens to head its own
 * size's queue.  A block above the largest binned size takes a page of its
 * own, carved from the arena's shared contiguous slices, and one class per
 * alignment covers those: a run that holds a larger one holds a smaller.
 * Guarded sampling, compiled only into a debug allocator and off unless its
 * sample rate is set, sends a sampled request to another queue.  This is the
 * allocator's behaviour, to re-audit whenever the allocator changes; the
 * regpool_request_classes test pins it. */
#define ARTS_REGPOOL_CLASS_SINGLETON ((uint64_t)0xFFFFF)
_Static_assert(MI_LARGE_MAX_OBJ_SIZE < ARTS_REGPOOL_CLASS_SINGLETON,
               "a binned block size must fit a class key's 20-bit field");

static inline uint64_t arts_regpool_class_block(size_t size) {
  size_t g = mi_good_size(size);
  return g > MI_LARGE_MAX_OBJ_SIZE ? ARTS_REGPOOL_CLASS_SINGLETON : (uint64_t)g;
}

/* Never 0.  `align` is a power of two. */
static inline uint64_t arts_regpool_class_key(size_t size, size_t align) {
  size_t over =
      (size < MI_MAX_ALIGN_SIZE ? MI_MAX_ALIGN_SIZE : size) + align - 1;
  return arts_regpool_class_block(size) << 26 |
         arts_regpool_class_block(over) << 6 |
         (uint64_t)__builtin_ctzll(align) << 1 | (uint64_t)(size >= align);
}

#endif /* ARTS_MEMORY_REGPOOL_CLASS_H */
