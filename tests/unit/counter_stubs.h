/* SPDX-License-Identifier: Apache-2.0
 *
 * Counter stubs for a test that compiles a runtime translation unit without
 * the counter translation unit: the unit's introspection increments resolve
 * here, so the test links under whatever counter set the tree enables.
 *
 * Including this header DEFINES the two symbols; include it in exactly one
 * translation unit of a program that links no counter translation unit.
 */
#ifndef ARTS_TESTS_COUNTER_STUBS_H
#define ARTS_TESTS_COUNTER_STUBS_H

#include <stdint.h>

#include "arts/counter/counter.h"
#include "arts/defs.h"

ARTS_THREAD_LOCAL arts_counter_t arts_thread_local_counters[NUM_COUNTER_TYPES];

void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  (void)counter;
  (void)num;
}

#endif /* ARTS_TESTS_COUNTER_STUBS_H */
