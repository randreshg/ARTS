/* SPDX-License-Identifier: Apache-2.0
 *
 * Link-time floor for the standalone fam tests: the runtime symbols the
 * module's diagnostics and its DRAM allocation reach, with libc behind them.
 * Not a test. */
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/utils/malloc.h"

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

void arts_abort(uint8_t code) { _exit(code ? code : 70); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }
