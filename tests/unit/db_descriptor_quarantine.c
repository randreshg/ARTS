/* SPDX-License-Identifier: Apache-2.0
 *
 * The freed-descriptor quarantine (ARTS_DEBUG_DB_QUARANTINE).  A write through
 * a reference that outlived a coherent descriptor is reported when the
 * descriptor leaves the quarantine and at the teardown drain, wherever in the
 * allocation it lands, a second free of a held descriptor is refused, and
 * frees with no stray write report nothing.  Each case runs in a child whose
 * output is discarded and is judged by how the child ends: a report ends the
 * process, the quarantine's own and, in an AddressSanitizer build, the
 * sanitizer's at the access.
 */
#include "arts/coherence/types.h"
#include "arts/db.h"
#include "arts/utils/malloc.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* More than the quarantine holds, so the first descriptor freed leaves it. */
#define FREES 2048

static struct arts_db_s *fresh(void) {
  struct arts_db_s *db =
      (struct arts_db_s *)arts_malloc_aligned(sizeof(struct arts_db_s), 64);
  memset(db, 0, sizeof(struct arts_db_s));
  return db;
}

static void clean(void) {
  for (int i = 0; i < FREES; i++) {
    arts_db_descriptor_free(fresh());
  }
  arts_db_descriptor_quarantine_drain();
}

static void free_twice(void) {
  struct arts_db_s *db = fresh();
  arts_db_descriptor_free(db);
  arts_db_descriptor_free(db);
}

static void stray_write_at(size_t offset) {
  struct arts_db_s *db = fresh();
  arts_db_descriptor_free(db);
  ((volatile unsigned char *)db)[offset] = 0;
}

static void stray_write(void) { stray_write_at(sizeof(arts_guid_t)); }

static void write_then_evict(void) {
  stray_write();
  for (int i = 0; i < FREES; i++) {
    arts_db_descriptor_free(fresh());
  }
}

static void write_then_drain(void) {
  stray_write();
  arts_db_descriptor_quarantine_drain();
}

/* The last byte of a home descriptor, past the span a cache-only stub
 * allocates. */
static void tail_write_then_drain(void) {
  stray_write_at(sizeof(struct arts_db_s) - 1);
  arts_db_descriptor_quarantine_drain();
}

static struct arts_db_s *g_other;

static void *free_on_other_thread(void *arg) {
  (void)arg;
  g_other = fresh();
  arts_db_descriptor_free(g_other);
  return NULL;
}

/* A descriptor another thread freed sits in that thread's ring, which the
 * drain reaches past the caller's own. */
static void other_thread_write_then_drain(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, free_on_other_thread, NULL) != 0 ||
      pthread_join(t, NULL) != 0) {
    _exit(2);
  }
  arts_db_descriptor_free(fresh());
  ((volatile unsigned char *)g_other)[sizeof(arts_guid_t)] = 0;
  arts_db_descriptor_quarantine_drain();
}

/* The child's exit status, or -1 when it could not be run or waited for or
 * did not exit on its own: a report ends the process with status 1. */
static int exit_status(void (*body)(void)) {
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);
    body();
    _exit(0);
  }
  int status = 0;
  if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  /* A SIGCHLD ignored by whoever started the test would reap the children
   * before waitpid could see how they ended. */
  signal(SIGCHLD, SIG_DFL);
  int fail = 0;
  struct {
    void (*body)(void);
    int want;
    const char *what;
  } cases[] = {
      {clean, 0, "frees with no stray write were reported"},
      {free_twice, 1, "a second free of a held descriptor went unreported"},
      {write_then_evict, 1,
       "a stray write went unreported when its descriptor left"},
      {write_then_drain, 1, "a stray write went unreported at the drain"},
      {tail_write_then_drain, 1,
       "a stray write past a stub's span went unreported at the drain"},
      {other_thread_write_then_drain, 1,
       "a stray write into another thread's held descriptor went unreported "
       "at the drain"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int got = exit_status(cases[i].body);
    if (got != cases[i].want) {
      printf("FAIL: %s (exit status %d)\n", cases[i].what, got);
      fail = 1;
    }
  }
  printf(fail ? "FAIL db_descriptor_quarantine\n"
              : "PASS db_descriptor_quarantine\n");
  return fail;
}
