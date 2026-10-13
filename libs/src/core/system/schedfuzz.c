/* SPDX-License-Identifier: Apache-2.0 — see schedfuzz.h for the contract. */
#include "arts/system/schedfuzz.h"

#ifdef ARTS_DEBUG_INJECTION

#include <stdlib.h>
#include <time.h>

#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/utils/random.h"

/* -1 = unread, 0 = disabled, 1-100 = per-point probability (percent). */
static int g_fuzz_pct = -1;

void arts_sched_fuzz_init(void) {
  const char *env = getenv("ARTS_SCHED_FUZZ");
  int pct = 0;
  if (env != NULL && env[0] != '\0') {
    char *end = NULL;
    long v = strtol(env, &end, 10);
    if (end == env || *end != '\0' || v < 0 || v > 100) {
      ARTS_ERROR("ARTS_SCHED_FUZZ=\"%s\" is not a percentage (0-100)", env);
    }
    pct = (int)v;
  }
  __atomic_store_n(&g_fuzz_pct, pct, __ATOMIC_RELAXED);
}

void arts_sched_fuzz_point(void) {
  int pct = __atomic_load_n(&g_fuzz_pct, __ATOMIC_RELAXED);
  if (pct < 0) {
    /* A program that reaches a fuzz point without a rank's initialization. */
    arts_sched_fuzz_init();
    pct = __atomic_load_n(&g_fuzz_pct, __ATOMIC_RELAXED);
  }
  if (pct == 0) {
    return;
  }
  if ((arts_thread_safe_random() % 100u) >= (uint64_t)pct) {
    return;
  }
  /* 0.5us to 64us in powers of two: the short end swallows a
   * cache-miss-scale decision window, the long end a message's delivery to
   * this rank.  The wait spins: a sleep may overrun its request by the
   * thread's timer slack (50us by default), which would swamp the draw, and
   * a runtime thread has its processing unit to itself, so giving it up buys
   * nothing. */
  uint64_t delay_ns = 500ull << (arts_thread_safe_random() % 8u);
  struct timespec start;
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  do {
    arts_runtime_idle_pause();
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
  } while ((uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull +
               (uint64_t)now.tv_nsec - (uint64_t)start.tv_nsec <
           delay_ns);
}

#endif /* ARTS_DEBUG_INJECTION */
