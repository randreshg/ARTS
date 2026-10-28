/* SPDX-License-Identifier: Apache-2.0 */
/// @file papi_scope.c
/// @brief The PAPI scope bookkeeping behind the PAPI_* counters.
///
/// Drives counter/papi.c directly on this thread, with no runtime started, and
/// reads the slots out of arts_thread_local_counters:
///   - a scope charges its slots only at its end, by the span's difference;
///   - a re-entered scope is measured once, by its outermost span;
///   - the arts_event_wait pattern (close the EDT scope, run inner EDTs, reopen
///     it) charges both outer halves and the inner EDT exactly once each, so
///     the runtime work between them stays out of the EDT scope;
///   - the TOTAL scope closed by thread_stop covers every EDT span inside it;
///   - a thread that never started PAPI passes through the hooks untouched;
///   - an unknown event name is a hard error;
///   - arts.cfg's papi_events reaches the config.
/// The counts are whatever papi_events[0] is (cycles by default), so the
/// checks are ordering and proportionality, never absolute values.
///
/// A host that cannot count (perf_event_paranoid above 2, no PMU in a VM)
/// SKIPs: that is the host, not the runtime.

#include "arts.h"
#include "arts/counter/counter.h"
#include "arts/counter/papi.h"
#include "arts/system/config.h"

#include <papi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOT(scope) (PAPI_TOTAL_WORKER_E0 + (scope) * ARTS_PAPI_MAX_EVENTS)

static int fails;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL papi_scope: " __VA_ARGS__);                                 \
      printf("\n");                                                            \
      fails++;                                                                 \
    }                                                                          \
  } while (0)

static uint64_t slot0(arts_papi_scope_t scope) {
  return arts_thread_local_counters[SLOT(scope)].count;
}

static void reset(void) {
  memset(arts_thread_local_counters, 0, sizeof(arts_thread_local_counters));
}

static volatile uint64_t sink;
static void work(uint64_t iters) {
  for (uint64_t i = 0; i < iters; i++) {
    sink += i * 2654435761u;
  }
}

/* Can this host count papi_events[0] at all?  Probed with PAPI directly, in
 * a child, so a refusal never touches the library state the test then uses. */
static int host_can_count(void) {
  pid_t pid = fork();
  if (pid == 0) {
    int set = PAPI_NULL;
    int code = 0;
    if (PAPI_library_init(PAPI_VER_CURRENT) != PAPI_VER_CURRENT ||
        PAPI_create_eventset(&set) != PAPI_OK ||
        PAPI_event_name_to_code("PAPI_TOT_CYC", &code) != PAPI_OK ||
        PAPI_add_event(set, code) != PAPI_OK || PAPI_start(set) != PAPI_OK) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* 1 when arts_papi_node_init(events) dies (ARTS_ERROR), probed in a child. */
static int node_init_rejected(const char *events) {
  pid_t pid = fork();
  if (pid == 0) {
    arts_papi_node_init(events);
    _exit(0);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0);
  return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void *unstarted_thread(void *arg) {
  (void)arg;
  arts_papi_scope_begin(ARTS_PAPI_EDT);
  work(100000);
  arts_papi_scope_end(ARTS_PAPI_EDT);
  return (void *)(uintptr_t)slot0(ARTS_PAPI_EDT);
}

static void check_config_key(void) {
  char path[128];
  snprintf(path, sizeof(path), "papi_scope_%ld.cfg", (long)getpid());
  FILE *f = fopen(path, "w");
  if (!f) {
    CHECK(0, "cannot write %s", path);
    return;
  }
  fputs("[ARTS]\nworker_threads=1\nlauncher=local\nnode_count=1\n"
        "papi_events=PAPI_TOT_CYC,PAPI_L3_TCM\n",
        f);
  (void)fclose(f);
  setenv("ARTS_CONFIG", path, 1);
  struct arts_config_s config;
  memset(&config, 0, sizeof(config));
  arts_config_load(&config);
  CHECK(config.papi_events &&
            strcmp(config.papi_events, "PAPI_TOT_CYC,PAPI_L3_TCM") == 0,
        "papi_events read back as '%s'",
        config.papi_events ? config.papi_events : "(null)");
  (void)remove(path);
}

int main(void) {
  check_config_key();

  if (!host_can_count()) {
    /* ctest matches PASS/SKIP and ignores the exit code, so a failure above
     * must never be followed by a SKIP line. */
    if (fails) {
      printf("FAIL papi_scope: %d check(s) failed\n", fails);
      return 1;
    }
    printf("SKIP papi_scope: this host cannot count PAPI_TOT_CYC "
           "(perf_event_paranoid > 2, or no PMU)\n");
    return 0;
  }

  CHECK(node_init_rejected("PAPI_TOT_CYC,ARTS_BOGUS_EVENT"),
        "an unknown event name was accepted");
  CHECK(node_init_rejected("A,B,C,D,E,F,G"),
        "seven events were accepted for six slots");

  arts_papi_node_init(NULL); /* the default list */
  CHECK(arts_papi_event_count() == 6 &&
            strcmp(arts_papi_event_name(0), "PAPI_TOT_CYC") == 0,
        "default list is %u events starting '%s'", arts_papi_event_count(),
        arts_papi_event_name(0) ? arts_papi_event_name(0) : "(null)");

  /* arts_thread_info is zeroed: role WORKER, so TOTAL_WORKER is opened. */
  arts_papi_thread_start();
  reset();

  /* A scope charges nothing until it ends. */
  arts_papi_scope_begin(ARTS_PAPI_EDT);
  work(2000000);
  CHECK(slot0(ARTS_PAPI_EDT) == 0, "EDT charged before its end");
  arts_papi_scope_end(ARTS_PAPI_EDT);
  uint64_t one = slot0(ARTS_PAPI_EDT);
  CHECK(one > 0, "EDT span charged nothing");

  /* Twice the work, roughly twice the count. */
  reset();
  arts_papi_scope_begin(ARTS_PAPI_EDT);
  work(4000000);
  arts_papi_scope_end(ARTS_PAPI_EDT);
  uint64_t two = slot0(ARTS_PAPI_EDT);
  CHECK(two > one * 13 / 10 && two < one * 3,
        "2x work counted %llu vs %llu", (unsigned long long)two,
        (unsigned long long)one);

  /* Re-entry: the inner begin/end must neither charge nor reset the span. */
  reset();
  arts_papi_scope_begin(ARTS_PAPI_DB_RELEASE);
  work(1000000);
  arts_papi_scope_begin(ARTS_PAPI_DB_RELEASE);
  work(1000000);
  arts_papi_scope_end(ARTS_PAPI_DB_RELEASE);
  uint64_t inner_end = slot0(ARTS_PAPI_DB_RELEASE);
  work(1000000);
  arts_papi_scope_end(ARTS_PAPI_DB_RELEASE);
  uint64_t nested = slot0(ARTS_PAPI_DB_RELEASE);
  CHECK(inner_end == 0, "inner end of a re-entered scope charged %llu",
        (unsigned long long)inner_end);
  CHECK(nested > one, "re-entered scope lost its outer span (%llu <= %llu)",
        (unsigned long long)nested, (unsigned long long)one);

  /* The arts_event_wait pattern.  Runtime work between the halves (here
   * `runtime`) must land outside the EDT scope. */
  reset();
  arts_papi_scope_begin(ARTS_PAPI_EDT); /* outer body          */
  work(1000000);
  arts_papi_scope_end(ARTS_PAPI_EDT); /* wait: close          */
  work(8000000);                      /* runtime: scheduling  */
  arts_papi_scope_begin(ARTS_PAPI_EDT); /* inner EDT            */
  work(1000000);
  arts_papi_scope_end(ARTS_PAPI_EDT);
  arts_papi_scope_begin(ARTS_PAPI_EDT); /* wait: reopen         */
  work(1000000);
  arts_papi_scope_end(ARTS_PAPI_EDT); /* outer body ends      */
  uint64_t parked = slot0(ARTS_PAPI_EDT);
  CHECK(parked < one * 3, "a parked EDT absorbed the runtime between its "
                          "halves (%llu vs ~%llu)",
        (unsigned long long)parked, (unsigned long long)(one * 3 / 2));

  /* A thread that never ran thread_start passes through. */
  pthread_t th;
  void *other = NULL;
  pthread_create(&th, NULL, unstarted_thread, NULL);
  pthread_join(th, &other);
  CHECK((uintptr_t)other == 0, "an unstarted thread charged %llu",
        (unsigned long long)(uintptr_t)other);

  /* TOTAL spans everything since thread_start, so it covers every EDT span. */
  uint64_t edt = slot0(ARTS_PAPI_EDT);
  arts_papi_thread_stop();
  uint64_t total = slot0(ARTS_PAPI_TOTAL_WORKER);
  CHECK(total > edt, "TOTAL_WORKER %llu does not cover EDT %llu",
        (unsigned long long)total, (unsigned long long)edt);
  CHECK(slot0(ARTS_PAPI_TOTAL_PROGRESS) == 0,
        "a worker charged TOTAL_PROGRESS");

  /* Hooks after stop are inert. */
  arts_papi_scope_begin(ARTS_PAPI_EDT);
  arts_papi_scope_end(ARTS_PAPI_EDT);
  CHECK(slot0(ARTS_PAPI_EDT) == edt, "a scope charged after thread_stop");

  if (fails) {
    printf("FAIL papi_scope: %d check(s) failed\n", fails);
    return 1;
  }
  printf("PASS papi_scope\n");
  return 0;
}
