/* SPDX-License-Identifier: Apache-2.0
 *
 * guid_from_index_overflow — pins which kinds arts_guid_from_index and
 * arts_guid_index_from take, and the overflow behavior of the flat kinds.
 *
 * A range is a range of EDTs or events: a data-block kind names none, so
 * arts_guid_from_index and arts_guid_index_from refuse it, and this test
 * forks each call and asserts the refusal (status 1, never a GUID).
 *
 * The flat kinds keep the flat 48-bit key and the historical plain-add
 * contract with NO bound check: a key near 2^48 plus a carrying idx still
 * silently bumps the rank field.  That surprising behavior stays pinned
 * here (as before) so any future guard on those kinds is a deliberate,
 * reviewed change.
 *
 * No runtime: guid.c is #include'd with libc-backed shims.
 */

#include "arts/system/shutdown.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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
    (void)fprintf(stderr, "FAIL guid_from_index_overflow: " __VA_ARGS__);      \
    return 1;                                                                  \
  } while (0)

/* Whether the call ends the process with the abort stub's status 1, run in
 * a child so the stub's _exit does not end the test; 1 refused, 0 returned. */
static int refused(int index_from, arts_guid_t range_guid, arts_guid_t other) {
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
    if (index_from) {
      (void)arts_guid_index_from(range_guid, other);
    } else {
      (void)arts_guid_from_index(range_guid, 1);
    }
    _exit(0); /* reaching here means the refusal did NOT fire */
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
    return -1;
  }
  return WEXITSTATUS(st) == 1 ? 1 : (WEXITSTATUS(st) == 0 ? 0 : -1);
}

int main(void) {
  /* ---- Sanity: a small idx on an EDT range stays in the key. ---- */
  arts_guid_t base = ARTS_GUID_MAKE(ARTS_GUID_EDT, 2, 1000);
  arts_guid_t g5 = arts_guid_from_index(base, 5);
  if (arts_guid_get_rank(g5) != 2) {
    FAIL("small idx changed rank: %u\n", arts_guid_get_rank(g5));
  }
  if (arts_guid_get_key(g5) != 1005) {
    FAIL("small idx key wrong: %lu\n", (unsigned long)arts_guid_get_key(g5));
  }
  if (arts_guid_index_from(base, g5) != 5) {
    FAIL("small idx round-trip broke\n");
  }

  /* ---- A data-block kind names no range: both entries refuse it, for a
   * rank-homed and a distributed one. ---- */
  const unsigned int homes[] = {2u, ARTS_DISTRIBUTED_RANK};
  for (size_t i = 0; i < sizeof(homes) / sizeof(homes[0]); i++) {
    arts_guid_t db_range =
        ARTS_GUID_MAKE(ARTS_GUID_DB, homes[i], ARTS_GUID_DB_KEY(0, 1000));
    if (refused(0, db_range, NULL_GUID) != 1) {
      FAIL("arts_guid_from_index of a data-block range (home %u) was not "
           "refused\n",
           homes[i]);
    }
    if (refused(1, db_range, db_range) != 1) {
      FAIL("arts_guid_index_from of a data-block range (home %u) was not "
           "refused\n",
           homes[i]);
    }
  }

  /* ---- Flat kinds: the historical unguarded carry stays pinned. ---- */
  unsigned int home = 2;
  arts_guid_t edge = ARTS_GUID_MAKE(ARTS_GUID_EDT, home, ARTS_GUID_KEY_MASK);
  arts_guid_t carried = arts_guid_from_index(edge, 1);
  if (arts_guid_get_key(carried) != 0) {
    FAIL("expected key field to wrap to 0 on carry, got %lu\n",
         (unsigned long)arts_guid_get_key(carried));
  }
  if (arts_guid_get_rank(carried) == home) {
    FAIL("expected rank to be corrupted by carry (no bound guard); "
         "rank stayed %u — has from_index gained a guard for flat kinds?\n",
         home);
  }
  if (arts_guid_get_rank(carried) != home + 1) {
    FAIL("carry produced unexpected rank %u (want %u)\n",
         arts_guid_get_rank(carried), home + 1);
  }

  printf("PASS guid_from_index_overflow: a data-block range is refused by both "
         "entries; flat-kind "
         "unguarded carry stays pinned (key 2^48-1 + 1 -> rank %u, key 0)\n",
         arts_guid_get_rank(carried));
  return 0;
}
