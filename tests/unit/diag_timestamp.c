/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_get_time_stamp and arts_printf's line integrity, from
 * libs/src/core/utils/diag.c.
 *
 * arts_get_time_stamp() returns clock_gettime(CLOCK_MONOTONIC) folded to
 * nanoseconds (tv_sec*1e9 + tv_nsec).  Properties:
 *   A. Per-thread non-decreasing: within a single thread, consecutive samples
 *      never decrease (the clock is monotonic, so any backward step fails).
 *   B. Source and fold: every sample lies between two CLOCK_MONOTONIC reads
 *      folded the same way, one taken before it and one after, so the call
 *      reads that clock and the tv_sec*NANOSECS multiply did not garble it.
 *   C. Resolution: across many threads sampling concurrently the call is
 *      thread-safe (no shared mutable state) — ASan/TSan must stay clean.
 *
 *   D. Line integrity: arts_printf writes the " [rank] " prefix and the body
 *      as one piece of output.  Many threads print fixed tokens concurrently
 *      into a captured stdout; every captured line must be exactly
 *      " [0] TOKEN_<id>_<nnn>" and every line must arrive.
 *
 * STANDALONE STRATEGY: diag.c reaches arts_global_rank_id via arts.h +
 * arts/system/threads.h.  We pre-define those include guards and provide a
 * stub arts_global_rank_id, then #include the diag.c TU directly.
 */

#include "arts/utils/stdatomic_compat.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Block the heavy headers diag.c pulls in and stub the one symbol it needs. */
#define ARTS_H 1                /* skip public arts.h body */
#define ARTS_SYSTEM_THREADS_H 1 /* skip arts/system/threads.h body */
unsigned int arts_global_rank_id = 0;

#include "../../libs/src/core/utils/diag.c"

/* ---- Part A/B/C: monotonicity and source under concurrent sampling ---- */
#define MONO_THREADS 8
#define MONO_SAMPLES 200000
#define BRACKET_EVERY 1000 /* every this many samples is bracketed */

static _Atomic(int) g_gate;
static _Atomic(uint_least64_t) g_backward_steps; /* samples below the last */
static _Atomic(uint_least64_t) g_outside;        /* samples off the bracket */

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void *mono_worker(void *arg) {
  (void)arg;
  while (atomic_load_explicit(&g_gate, memory_order_acquire) == 0) {
  }
  uint64_t prev = arts_get_time_stamp();
  for (int i = 0; i < MONO_SAMPLES; i++) {
    uint64_t before = 0, after = 0;
    bool bracket = (i % BRACKET_EVERY) == 0;
    if (bracket) {
      before = monotonic_ns();
    }
    uint64_t now = arts_get_time_stamp();
    if (bracket) {
      after = monotonic_ns();
      if (now < before || now > after) {
        atomic_fetch_add_explicit(&g_outside, 1, memory_order_relaxed);
      }
    }
    if (now < prev) {
      atomic_fetch_add_explicit(&g_backward_steps, 1, memory_order_relaxed);
    }
    prev = now;
  }
  return NULL;
}

/* ---- Part D: arts_printf line integrity ---- */
#define PRINTF_THREADS 6
#define PRINTF_LINES 300

static _Atomic(int) g_pgate;

static void *printf_worker(void *arg) {
  long id = (long)arg;
  while (atomic_load_explicit(&g_pgate, memory_order_acquire) == 0) {
  }
  for (int i = 0; i < PRINTF_LINES; i++) {
    arts_printf("TOKEN_%ld_%03d\n", id, i);
  }
  return NULL;
}

int main(void) {
  int rc = 0;

  /* ===== Part A/B/C: monotonicity and source under concurrency. ===== */
  atomic_init(&g_gate, 0);
  atomic_init(&g_backward_steps, 0);
  atomic_init(&g_outside, 0);
  {
    pthread_t th[MONO_THREADS];
    for (int i = 0; i < MONO_THREADS; i++) {
      pthread_create(&th[i], NULL, mono_worker, NULL);
    }
    atomic_store_explicit(&g_gate, 1, memory_order_release);
    for (int i = 0; i < MONO_THREADS; i++) {
      pthread_join(th[i], NULL);
    }
  }
  uint64_t back = atomic_load_explicit(&g_backward_steps, memory_order_relaxed);
  if (back != 0) {
    (void)fprintf(stderr,
                  "FAIL diag_timestamp: %" PRIu64 " sample(s) below the "
                  "previous one on the same thread\n",
                  back);
    rc = 1;
  }
  uint64_t outside = atomic_load_explicit(&g_outside, memory_order_relaxed);
  if (outside != 0) {
    (void)fprintf(stderr,
                  "FAIL diag_timestamp: %" PRIu64 " sample(s) outside the "
                  "CLOCK_MONOTONIC reads around them\n",
                  outside);
    rc = 1;
  }

  /* ===== Part D: arts_printf line integrity. ===== */
  /* Redirect stdout to a temp file so the lines can be scanned. */
  {
    char tmpl[] = "diag_printf_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
      (void)fprintf(stderr, "FAIL diag_timestamp: mkstemp failed\n");
      rc = 1;
    } else {
      fflush(stdout);
      int saved = dup(STDOUT_FILENO);
      dup2(fd, STDOUT_FILENO);

      atomic_init(&g_pgate, 0);
      pthread_t th[PRINTF_THREADS];
      for (long i = 0; i < PRINTF_THREADS; i++) {
        pthread_create(&th[i], NULL, printf_worker, (void *)i);
      }
      atomic_store_explicit(&g_pgate, 1, memory_order_release);
      for (int i = 0; i < PRINTF_THREADS; i++) {
        pthread_join(th[i], NULL);
      }
      fflush(stdout);

      /* restore stdout */
      dup2(saved, STDOUT_FILENO);
      close(saved);
      close(fd);

      /* A well-formed line is exactly " [0] TOKEN_<id>_<nnn>\n"; another
       * thread's output between a prefix and its body breaks two lines. */
      FILE *f = fopen(tmpl, "r");
      uint64_t torn = 0, total = 0;
      if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
          total++;
          long id = -1;
          int seq = -1;
          int end = 0;
          if (sscanf(line, " [0] TOKEN_%ld_%d\n%n", &id, &seq, &end) != 2 ||
              strncmp(line, " [0] TOKEN_", 11) != 0 || line[end] != '\0' ||
              line[end - 1] != '\n' || strstr(line + 11, "TOKEN_") != NULL) {
            torn++;
          }
        }
        fclose(f);
      }
      (void)remove(tmpl);
      if (torn != 0 ||
          total != (uint64_t)PRINTF_THREADS * (uint64_t)PRINTF_LINES) {
        (void)fprintf(stderr,
                      "FAIL diag_timestamp: arts_printf produced %" PRIu64
                      " broken line(s), %" PRIu64 " line(s) in all (want %d) "
                      "under %d concurrent writers\n",
                      torn, total, PRINTF_THREADS * PRINTF_LINES,
                      PRINTF_THREADS);
        rc = 1;
      }
    }
  }

  if (rc) {
    return 1;
  }
  printf("PASS diag_timestamp: per-thread non-decreasing, CLOCK_MONOTONIC "
         "source, thread-safe concurrent sampling; arts_printf lines whole\n");
  return 0;
}
