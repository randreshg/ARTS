/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_reserve_range — pure-unit coverage of arts_guid_reserve and
 * arts_guid_reserve_range: which kinds and ranks they take, and the
 * round-robin stride.
 *
 * Properties:
 *
 *   0. Only EDT and event GUIDs are reserved.  A data-block kind is refused
 *      by both entries for a live rank, the current-rank sentinel and the
 *      round-robin sentinel alike, and arts_guid_reserve refuses every rank
 *      value that is neither a live rank nor the current-rank sentinel.
 *
 *   1. The round-robin stride is ceil(size / nrank) for every size: at
 *      nrank = 3 a range of UINT_MAX events advances every home's counter by
 *      1,431,655,765, and a later reserve on every home lies above the
 *      range's last member there.
 *
 * No runtime: guid.c is #include'd with libc-backed shims; node_init runs
 * the runtime's node init for one simulated rank.
 */

#include "arts/system/shutdown.h"
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 4;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(rc ? rc : 70);
}
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_calloc_aligned(size_t n, size_t s, size_t a) {
  void *p = NULL;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n * s)) {
    return NULL;
  }
  memset(p, 0, n * s);
  return p;
}
void arts_free(void *p) { free(p); }

#include "../../libs/src/core/gas/guid.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL guid_reserve_range: " __VA_ARGS__);            \
    return 1;                                                                  \
  } while (0)

static void node_fini(void) {
  if (arts_node_info.keys != NULL) {
    for (unsigned t = 0; t < arts_node_info.total_thread_count; t++) {
      free(arts_node_info.keys[t]);
    }
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
  arts_guid_generator_cleanup();
}

/* The runtime's node init for rank `rank` of `nrank` with one thread: the
 * geometry, then the thread's key counters. */
static void node_init(unsigned int rank, unsigned int nrank) {
  node_fini();
  arts_global_rank_id = rank;
  arts_global_rank_count = nrank;
  arts_node_info.total_thread_count = 1;
  arts_node_info.keys = (uint64_t **)calloc(1, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  arts_thread_info.thread_id = 0;
  arts_guid_key_generator_init();
}

/* Home h's counter of the flat arm that serves `kind`. */
static uint64_t home_counter(arts_guid_kind_t kind, unsigned int h) {
  return *arts_guid_generator_get_key(h, (unsigned int)kind);
}

/* Property 1 for `kind`; 0 when it holds. */
static int check_stride(arts_guid_kind_t kind) {
  const unsigned int nrank = 3;
  const uint64_t stride = 1431655765u; /* ceil(UINT_MAX / 3) */
  node_init(0, nrank);
  uint64_t before[3];
  for (unsigned int h = 0; h < nrank; h++) {
    before[h] = home_counter(kind, h);
  }
  arts_guid_t range =
      arts_guid_reserve_range(kind, UINT_MAX, ARTS_HINT_ROUND_ROBIN);
  if (range == NULL_GUID) {
    FAIL("kind %u: a round-robin range of UINT_MAX returned NULL_GUID\n",
         (unsigned)kind);
  }
  for (unsigned int h = 0; h < nrank; h++) {
    uint64_t advance = home_counter(kind, h) - before[h];
    if (advance != stride) {
      FAIL("kind %u: a range of UINT_MAX advanced home %u's counter by "
           "%lu, not %lu\n",
           (unsigned)kind, h, (unsigned long)advance, (unsigned long)stride);
    }
  }
  arts_guid_t next =
      arts_guid_reserve_range(kind, nrank, ARTS_HINT_ROUND_ROBIN);
  for (unsigned int h = 0; h < nrank; h++) {
    /* Home h's last member is the one of ordinal stride - 1. */
    arts_guid_t last =
        arts_guid_from_index(range, (unsigned int)((stride - 1) * nrank + h));
    arts_guid_t single = arts_guid_reserve(kind, h);
    arts_guid_t later = arts_guid_from_index(next, h);
    if (arts_guid_get_rank(last) != h ||
        arts_guid_index_from(range, last) !=
            (int64_t)((stride - 1) * nrank + h)) {
      FAIL("kind %u: home %u's last member does not round-trip\n",
           (unsigned)kind, h);
    }
    if (ARTS_GUID_GET_KEY(single) <= ARTS_GUID_GET_KEY(last) ||
        ARTS_GUID_GET_KEY(later) <= ARTS_GUID_GET_KEY(last)) {
      FAIL("kind %u: a later reserve on home %u lies inside the range\n",
           (unsigned)kind, h);
    }
  }
  return 0;
}

enum entry_e { ENTRY_ONE, ENTRY_RANGE };

/* Whether the entry called with `kind` and `rank` ends the process with the
 * abort stub's status 1 (the refusal), run in a child so the stub's _exit
 * does not end the test. */
static int reserve_refused(enum entry_e entry, arts_guid_kind_t kind,
                           unsigned int rank) {
  (void)fflush(stdout);
  (void)fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDOUT_FILENO);
      (void)dup2(null_fd, STDERR_FILENO);
    }
    if (entry == ENTRY_ONE) {
      (void)arts_guid_reserve(kind, rank);
    } else {
      (void)arts_guid_reserve_range(kind, 4u, rank);
    }
    _exit(0);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status) == 1 ? 1 : (WEXITSTATUS(status) == 0 ? 0 : -1);
}

int main(void) {
  /* ---- 0. arts_guid_reserve takes a live rank or the current-rank
   * sentinel and refuses every other value (no reduction modulo the rank
   * count); a data-block kind is refused whatever the rank. ---- */
  node_init(2, 4);
  if (arts_guid_get_rank(arts_guid_reserve(ARTS_GUID_EVENT, 3)) != 3 ||
      arts_guid_get_rank(
          arts_guid_reserve(ARTS_GUID_EDT, ARTS_HINT_CURRENT_RANK)) != 2) {
    FAIL("a live rank or ARTS_HINT_CURRENT_RANK did not reserve there\n");
  }
  const unsigned int bad[] = {4u,
                              5u,
                              4u + 3u,
                              16383u,
                              16384u,
                              ARTS_HINT_ROUND_ROBIN,
                              ARTS_HINT_ANY_RANK};
  const arts_guid_kind_t flat[] = {ARTS_GUID_EDT, ARTS_GUID_EVENT};
  for (size_t k = 0; k < sizeof(flat) / sizeof(flat[0]); k++) {
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      if (reserve_refused(ENTRY_ONE, flat[k], bad[i]) != 1) {
        FAIL("arts_guid_reserve(kind %u, rank %u) was not refused\n",
             (unsigned)flat[k], bad[i]);
      }
    }
  }
  const unsigned int ranks[] = {0u, 3u, ARTS_HINT_CURRENT_RANK,
                                ARTS_HINT_ROUND_ROBIN};
  for (size_t i = 0; i < sizeof(ranks) / sizeof(ranks[0]); i++) {
    if (reserve_refused(ENTRY_ONE, ARTS_GUID_DB, ranks[i]) != 1) {
      FAIL("arts_guid_reserve(a data block, rank %u) was not refused\n",
           ranks[i]);
    }
    if (reserve_refused(ENTRY_RANGE, ARTS_GUID_DB, ranks[i]) != 1) {
      FAIL("arts_guid_reserve_range(a data block, rank %u) was not refused\n",
           ranks[i]);
    }
  }

  /* ---- 1. the round-robin stride. ---- */
  if (check_stride(ARTS_GUID_EVENT) != 0) {
    return 1;
  }

  node_fini();
  printf("PASS guid_reserve_range: a data-block kind refused by both entries "
         "for every rank form, rank refusals, round-robin stride of a "
         "UINT_MAX range\n");
  return 0;
}
