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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "arts/gas/route_table.h"

#define CACHELINE ARTS_ROUTE_TABLE_CACHELINE_SIZE
#define FIELD_LINE(field) (offsetof(arts_route_table_t, field) / CACHELINE)

_Static_assert(_Alignof(arts_route_table_t) >= CACHELINE,
               "route table must be cacheline aligned");
_Static_assert(sizeof(arts_route_table_t) % CACHELINE == 0,
               "route table size must be cacheline rounded");
_Static_assert(offsetof(arts_route_table_t, readerLock) % CACHELINE == 0,
               "readerLock must start on a cacheline");
_Static_assert(offsetof(arts_route_table_t, writerLock) % CACHELINE == 0,
               "writerLock must start on a cacheline");
_Static_assert(FIELD_LINE(readerLock) != FIELD_LINE(writerLock),
               "route table locks must not share a cacheline");
_Static_assert(FIELD_LINE(newFunc) != FIELD_LINE(readerLock),
               "readerLock must not share a cacheline with metadata");
_Static_assert(FIELD_LINE(newFunc) != FIELD_LINE(writerLock),
               "writerLock must not share a cacheline with metadata");

static bool require_true(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return false;
  }
  return true;
}

static uint64_t legacy_hash_multiplier(unsigned int shift) {
  uint64_t hash = 14695981039346656037U;
  switch (shift) {
  case 10:
    hash *= 1021;
  case 11:
    hash *= 2039;
  case 12:
    hash *= 4093;
  case 13:
    hash *= 8191;
  case 14:
    hash *= 16381;
  case 15:
    hash *= 32749;
  case 16:
    hash *= 65521;
  case 17:
    hash *= 131071;
  case 18:
    hash *= 262139;
  case 19:
    hash *= 524287;
  case 20:
    hash *= 1048573;
  case 21:
    hash *= 2097143;
  case 22:
    hash *= 4194301;
  case 31:
    hash *= 2147483647;
  case 32:
    hash *= 4294967291;
  default:
    break;
  }
  return hash;
}

static uint64_t legacy_route_table_key(arts_guid_t key, unsigned int shift) {
  return (((uint64_t)key * legacy_hash_multiplier(shift)) >> (64 - shift)) *
         COLLISION_RESOLVES;
}

int main(void) {
  arts_route_table_t *table = arts_new_route_table(16, 4);
  arts_route_table_t *cascade_table = arts_new_route_table(65536, 16);
  int ok = 1;

  ok &= require_true(((uintptr_t)table % CACHELINE) == 0,
                     "route table allocation is not cacheline aligned");
  ok &= require_true(((uintptr_t)&table->readerLock % CACHELINE) == 0,
                     "readerLock allocation address is not cacheline aligned");
  ok &= require_true(((uintptr_t)&table->writerLock % CACHELINE) == 0,
                     "writerLock allocation address is not cacheline aligned");
  ok &= require_true(table->hash_multiplier == legacy_hash_multiplier(4),
                     "route table did not precompute hash multiplier");
  ok &= require_true(cascade_table->hash_multiplier ==
                         legacy_hash_multiplier(16),
                     "route table did not precompute cascade hash multiplier");

  bool added = false;
  arts_guid_t key = (arts_guid_t)0x0100000000000001ULL;
  arts_route_item_t *item = internal_route_table_add_item_race(
      &added, table, (void *)(uintptr_t)key, key, 0, false, false, 0);
  uint64_t expected_slot = legacy_route_table_key(key, table->shift);
  ok &= require_true(added, "route table insert did not report an added item");
  ok &= require_true(item != NULL, "route table insert returned NULL");
  ok &= require_true(item == &table->data[expected_slot],
                     "route table insert did not use legacy hash slot");
  ok &= require_true(arts_route_table_search_for_key(table, key,
                                                     AVAILABLE_KEY) == item,
                     "route table lookup did not find inserted item");

  added = false;
  key = (arts_guid_t)0x0100000012345678ULL;
  item = internal_route_table_add_item_race(
      &added, cascade_table, (void *)(uintptr_t)key, key, 0, false, false, 0);
  expected_slot = legacy_route_table_key(key, cascade_table->shift);
  ok &= require_true(added, "cascade route table insert did not add item");
  ok &= require_true(item == &cascade_table->data[expected_slot],
                     "cascade route table insert did not use legacy hash slot");
  ok &= require_true(arts_route_table_search_for_key(cascade_table, key,
                                                     AVAILABLE_KEY) == item,
                     "cascade route table lookup did not find inserted item");

  arts_delete_route_table(cascade_table);
  arts_delete_route_table(table);
  return ok ? 0 : 1;
}
