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

// Copyright (c) 2013 Amanieu d'Antras
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#include "arts/utils/deque.h"

#include <stdint.h>

#include "arts.h"
#include "arts/defs.h"
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

/*
 * Chase-Lev work-stealing deque in the C11 memory model (Le, Pop, Cohen and
 * Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory
 * Models", PPoPP 2013).  An element is published by a release store of
 * `bottom` that a thief's acquire load reads, so everything a pusher wrote
 * before the push happens-before whatever the thief does with the element.
 *
 * One owner thread pushes and pops at the bottom; any thread steals at the
 * top.  An array the owner outgrew stays linked until the deque is deleted,
 * so a thief that loaded an older array still reads valid memory, and its CAS
 * on `top` decides whether the element it read is its own.  Fields are
 * cache-line padded so the owner's `bottom` and the thieves' `top` do not
 * share a line.
 */
struct circular_array_s {
  struct circular_array_s *next; /* owner-only: the chain freed at delete */
  unsigned int size;
  /* Written by the owner, read by thieves; a slot carries no order of its own:
   * the release store of `bottom` (or of `active` for a grown array) that
   * follows its write publishes it. */
  _Atomic(void *) segment[];
} ARTS_ALIGNED(64);

struct arts_deque_s {
  /* Advanced by CAS only, by thieves and by the owner on the last element.
   * An acquire load orders the reader after the slot reads of the thief whose
   * CAS it reads, so the owner never overwrites a slot still being read. */
  _Atomic(uint64_t) top;
  char pad1[56];
  /* Written by the owner only, always with a release store; a thief's acquire
   * load of it is the edge that publishes the slots below it. */
  _Atomic(uint64_t) bottom;
  char pad2[56];
  /* Written by the owner only, with a release store after the new array is
   * filled; a thief's acquire load sees the copied slots. */
  _Atomic(struct circular_array_s *) active;
  char pad3[56];
  struct circular_array_s *head; /* owner-only: the chain freed at delete */
} ARTS_ALIGNED(64);

static inline struct circular_array_s *new_circular_array(unsigned int size) {
  /* calloc's zero fill is NULL in every slot. */
  struct circular_array_s *array = (struct circular_array_s *)arts_calloc_aligned(
      1, sizeof(struct circular_array_s) + (sizeof(_Atomic(void *)) * size),
      64);
  array->size = size;
  array->next = NULL;
  return array;
}

static inline void *get_circular_array(struct circular_array_s *array,
                                       uint64_t i) {
  return atomic_load_explicit(&array->segment[i % array->size],
                              memory_order_relaxed);
}

static inline void put_circular_array(struct circular_array_s *array,
                                      uint64_t i, void *object) {
  atomic_store_explicit(&array->segment[i % array->size], object,
                        memory_order_relaxed);
}

static inline struct circular_array_s *
grow_circular_array(struct circular_array_s *array, uint64_t b, uint64_t t) {
  struct circular_array_s *a = new_circular_array(array->size * 2);
  array->next = a;
  for (uint64_t i = t; i < b; i++) {
    put_circular_array(a, i, get_circular_array(array, i));
  }
  return a;
}

struct arts_deque_s *arts_deque_new(unsigned int size) {
  struct arts_deque_s *deque = (struct arts_deque_s *)arts_calloc_aligned(
      1, sizeof(struct arts_deque_s), 64);
  atomic_init(&deque->top, 1);
  atomic_init(&deque->bottom, 1);
  deque->head = new_circular_array(size);
  atomic_init(&deque->active, deque->head);
  return deque;
}

void arts_deque_delete(struct arts_deque_s *deque) {
  struct circular_array_s *trail;
  struct circular_array_s *current = deque->head;
  while (current) {
    trail = current;
    current = current->next;
    arts_free(trail);
  }
  arts_free(deque);
}

bool arts_deque_push_front(struct arts_deque_s *deque, void *item,
                           unsigned int priority) {
  (void)priority;
  uint64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
  /* Acquire: a thief's read of a slot happens before this push reuses it. */
  uint64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);
  struct circular_array_s *a =
      atomic_load_explicit(&deque->active, memory_order_relaxed);
  if (b >= a->size - 1 + t) {
    a = grow_circular_array(a, b, t);
    /* Release: a thief that loads the new array sees what was copied in. */
    atomic_store_explicit(&deque->active, a, memory_order_release);
  }
  put_circular_array(a, b, item);
  atomic_store_explicit(&deque->bottom, b + 1, memory_order_release);
  return true;
}

void *arts_deque_pop_front(struct arts_deque_s *deque) {
  uint64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed) - 1;
  struct circular_array_s *a =
      atomic_load_explicit(&deque->active, memory_order_relaxed);
  /* Every store of bottom releases: a thief synchronises with whichever one
   * it reads, not only through the release sequence of an earlier push. */
  atomic_store_explicit(&deque->bottom, b, memory_order_release);
  /* Orders the store of bottom before the load of top; pairs with the fence
   * in arts_deque_pop_back between its load of top and its load of bottom.
   * Of an owner and a thief racing for the last element, at least one sees
   * the other's claim. */
  atomic_thread_fence(memory_order_seq_cst);
  uint64_t t = atomic_load_explicit(&deque->top, memory_order_relaxed);
  if (t > b) {
    atomic_store_explicit(&deque->bottom, b + 1, memory_order_release);
    return NULL;
  }
  void *o = get_circular_array(a, b);
  if (b > t) {
    return o;
  }
  /* seq_cst: the owner's side of the last-element race; the thief's CAS on
   * top in arts_deque_pop_back is the other. */
  if (!atomic_compare_exchange_strong_explicit(&deque->top, &t, t + 1,
                                               memory_order_seq_cst,
                                               memory_order_relaxed)) {
    o = NULL;
  }
  atomic_store_explicit(&deque->bottom, b + 1, memory_order_release);
  return o;
}

void *arts_deque_pop_back(struct arts_deque_s *deque) {
  uint64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);
  /* Orders the load of top before the load of bottom; pairs with the fence
   * in arts_deque_pop_front between its store of bottom and its load of top. */
  atomic_thread_fence(memory_order_seq_cst);
  uint64_t b = atomic_load_explicit(&deque->bottom, memory_order_acquire);
  if (t < b) {
    struct circular_array_s *a =
        atomic_load_explicit(&deque->active, memory_order_acquire);
    void *o = get_circular_array(a, t);
    /* seq_cst: the thief's side of the last-element race; the owner's CAS on
     * top in arts_deque_pop_front is the other. */
    if (atomic_compare_exchange_strong_explicit(&deque->top, &t, t + 1,
                                                memory_order_seq_cst,
                                                memory_order_relaxed)) {
      return o;
    }
  }
  return NULL;
}
