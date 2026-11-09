// SPDX-License-Identifier: Apache-2.0
/*
 * mesh_line — pure_unit
 *
 * TARGET: the startup failure line and the startup budget of
 * libs/src/core/transport/socket.c.
 *
 * PROPERTIES UNDER TEST:
 *   1. arts_mesh_format_line writes exactly
 *        ARTS_MESH_FAILED rank=<r|-> host=<h> phase=<p> missing=<r,...|->
 *        missing_hosts=<h,...|-> more=<k>
 *      and a newline: the missing ranks ascending, each listed rank's
 *      routing-table name in the same place of the second list, at most 16
 *      ranks listed and the rest counted in more.
 *   2. A line never passes 2048 bytes: an entry that would push it past is
 *      dropped from both lists and counted in more, so the cut falls on an
 *      entry boundary and the lists stay parallel; a buffer smaller than the
 *      line is never written past.
 *   3. Every phase prints its name.
 *   4. After arts_startup_begin, the backstop's line names no rank, this
 *      host's node name and the config step; the budget reads 20 s with
 *      ARTS_STARTUP_TIMEOUT_S unset and the value it sets otherwise (argv[1]
 *      names what it must read), and the deadline lies the budget after the
 *      call, within a second.
 *   5. From the first step at or past listen the backstop's line names this
 *      rank and its routing-table name, whichever such step comes first
 *      (fabric here), and keeps them at the steps after it.
 *   6. A give-up before the process is a rank writes its line (no rank, the
 *      node name) and ends through arts_exit with status 75; when the
 *      process's end already began it writes nothing and still enters
 *      arts_exit, whose late entry leaves that end to its thread.
 *
 * The real socket.c is compiled into this TU; externs are stubbed locally.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/transport/protocol.h"

#include "arts/system/shutdown.h"
#include "counter_stubs.h"

/* ----- extern stubs the included socket.c needs ----- */
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;

static int g_exit_rc;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(70 + rc);
}
bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)origin;
  g_exit_rc = rc;
  return true;
}
_Noreturn void arts_exit_resume(void) { _exit(70 + g_exit_rc); }
void arts_exit_startup_arm(uint64_t at_ns) { (void)at_ns; }
void arts_exit_startup_disarm(void) {}
unsigned int arts_signal_handlers_rank(void) { return 0; }
static bool g_exit_began;
static bool g_rank_known = true;
bool arts_exit_began(void) { return g_exit_began; }
bool arts_exit_rank_known(void) { return g_rank_known; }
bool arts_shutdown_taken(void) { return false; }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_realloc(void *p, size_t s) { return realloc(p, s); }
void arts_free(void *p) { free(p); }
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* pull in the real implementation */
#include "../../libs/src/core/transport/socket.c"

#define CHECK(cond, what)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL mesh_line: %s (line %d)\n", what, __LINE__);       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The text of `name` in line (up to the next space or newline), copied. */
static const char *field(const char *line, const char *name) {
  static char out[4][4096];
  static int next;
  char *o = out[next++ % 4];
  const char *at = strstr(line, name);
  size_t n = 0;
  if (at != NULL) {
    at += strlen(name);
    while (at[n] != ' ' && at[n] != '\n' && at[n] != '\0') {
      n++;
    }
    memcpy(o, at, n);
  }
  o[n] = '\0';
  return o;
}

/* The entries of a comma list; "-" holds none. */
static unsigned int listed(const char *list) {
  if (strcmp(list, "-") == 0 || list[0] == '\0') {
    return 0;
  }
  unsigned int n = 1;
  for (const char *c = list; *c != '\0'; c++) {
    n += *c == ',';
  }
  return n;
}

static unsigned int count_commas(const char *list) {
  unsigned int n = 0;
  for (const char *c = list; *c != '\0'; c++) {
    n += *c == ',';
  }
  return n;
}

static unsigned long more_of(const char *line) {
  return strtoul(field(line, " more="), NULL, 10);
}

/* The i-th entry of a comma list, copied. */
static const char *entry(const char *list, unsigned int i) {
  static char out[2][512];
  static int next;
  char *o = out[next++ % 2];
  const char *c = list;
  for (unsigned int k = 0; k < i && c != NULL; k++) {
    c = strchr(c, ',');
    c = c != NULL ? c + 1 : NULL;
  }
  size_t n = 0;
  while (c != NULL && c[n] != ',' && c[n] != '\0' && n < sizeof(out[0]) - 1) {
    n++;
  }
  if (c != NULL) {
    memcpy(o, c, n);
  }
  o[n] = '\0';
  return o;
}

static char g_names[200][256];
static struct arts_config_table_s g_rows[200];

static void table_of(unsigned int n, unsigned int width) {
  for (unsigned int i = 0; i < n; i++) {
    if (width == 0) {
      snprintf(g_names[i], sizeof(g_names[i]), "n%02u", i);
    } else {
      memset(g_names[i], 'a' + (int)(i % 26), width);
      snprintf(g_names[i], 8, "r%03u-", i);
      g_names[i][5] = 'x';
      g_names[i][width] = '\0';
    }
    g_rows[i].rank = i;
    g_rows[i].ip_address = g_names[i];
  }
}

int main(int argc, char **argv) {
  char buf[4096];
  size_t n;

  /* CONNECT: rank 1 of 4 lacks ranks 2 and 3. */
  table_of(4, 0);
  bool miss4[4] = {false, false, true, true};
  n = arts_mesh_format_line(buf, sizeof buf, 1, "n01", ARTS_MESH_PHASE_CONNECT,
                            miss4, 4, g_rows);
  CHECK(strcmp(buf, "ARTS_MESH_FAILED rank=1 host=n01 phase=connect "
                    "missing=2,3 missing_hosts=n02,n03 more=0\n") == 0,
        "CONNECT");
  CHECK(n == strlen(buf), "CONNECT: the length returned");

  /* UNKNOWN: before the rank, no missing set. */
  n = arts_mesh_format_line(buf, sizeof buf, -1, "nodeA",
                            ARTS_MESH_PHASE_CONFIG, NULL, 0, NULL);
  CHECK(strcmp(buf, "ARTS_MESH_FAILED rank=- host=nodeA phase=config "
                    "missing=- missing_hosts=- more=0\n") == 0,
        "UNKNOWN");
  CHECK(n == strlen(buf), "UNKNOWN: the length returned");

  /* EMPTY: a missing set that names no rank. */
  bool none4[4] = {false, false, false, false};
  arts_mesh_format_line(buf, sizeof buf, 0, "n00", ARTS_MESH_PHASE_HANGUP,
                        none4, 4, g_rows);
  CHECK(strcmp(buf, "ARTS_MESH_FAILED rank=0 host=n00 phase=hangup "
                    "missing=- missing_hosts=- more=0\n") == 0,
        "EMPTY");

  /* LIST16: 40 missing of 41 short names: 16 listed, more=24. */
  table_of(41, 0);
  bool miss41[41];
  for (unsigned int i = 0; i < 41; i++) {
    miss41[i] = i != 0;
  }
  n = arts_mesh_format_line(buf, sizeof buf, 0, "n00", ARTS_MESH_PHASE_CONNECT,
                            miss41, 41, g_rows);
  CHECK(count_commas(field(buf, "missing=")) == 15, "LIST16: ranks listed");
  CHECK(listed(field(buf, "missing_hosts=")) == 16, "LIST16: hosts listed");
  CHECK(strstr(buf, " more=24\n") != NULL, "LIST16: more");
  CHECK(strcmp(field(buf, "missing="), "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,"
                                       "16") == 0,
        "LIST16: the lowest ranks, ascending");

  /* CUT: 200 missing, 200-byte host names. */
  table_of(200, 200);
  bool miss200[200];
  for (unsigned int i = 0; i < 200; i++) {
    miss200[i] = true;
  }
  memset(buf, 0x5a, sizeof buf);
  n = arts_mesh_format_line(buf, sizeof buf, 7, g_names[7],
                            ARTS_MESH_PHASE_EXCHANGE, miss200, 200, g_rows);
  CHECK(n <= ARTS_MESH_LINE_MAX && n == strlen(buf) && buf[n - 1] == '\n',
        "CUT: at most 2048 bytes, newline-terminated");
  unsigned int k = listed(field(buf, "missing="));
  CHECK(k >= 1 && k < 16, "CUT: the cut drops entries, not the whole list");
  CHECK(k == listed(field(buf, "missing_hosts=")),
        "CUT: the two lists hold the same count");
  CHECK(more_of(buf) == 200u - k, "CUT: more counts every dropped entry");

  /* PARALLEL: each listed rank's host is its own table row's. */
  for (unsigned int i = 0; i < k; i++) {
    unsigned long r = strtoul(entry(field(buf, "missing="), i), NULL, 10);
    CHECK(r < 200 &&
              strcmp(entry(field(buf, "missing_hosts="), i), g_names[r]) == 0,
          "PARALLEL: a listed host is not its rank's row");
  }

  /* SMALL: a buffer smaller than the line is never written past. */
  char small[40];
  memset(small, 0x5a, sizeof small);
  n = arts_mesh_format_line(small, 32, 1, "n01", ARTS_MESH_PHASE_CONNECT, miss4,
                            4, g_rows);
  CHECK(n <= 31 && n == strlen(small) && small[n - 1] == '\n',
        "SMALL: cut to the buffer, newline-terminated");
  for (size_t i = 32; i < sizeof small; i++) {
    CHECK(small[i] == 0x5a, "SMALL: written past the buffer");
  }

  /* PHASES: every phase prints its name. */
  static const char *const names[] = {"config", "resolve",  "listen", "connect",
                                      "fabric", "exchange", "hangup"};
  for (int p = ARTS_MESH_PHASE_CONFIG; p <= ARTS_MESH_PHASE_HANGUP; p++) {
    arts_mesh_format_line(buf, sizeof buf, 2, "h", (enum arts_mesh_phase)p,
                          NULL, 0, NULL);
    CHECK(strcmp(field(buf, " phase="), names[p]) == 0, "PHASES");
  }

  /* BUDGET and EXPIRED: the process's startup. */
  unsigned long want = (argc > 1) ? strtoul(argv[1], NULL, 10) : 20ul;
  uint64_t before = now_ns();
  arts_startup_begin();
  CHECK(arts_startup_budget_s() == want, "BUDGET: the budget read");
  uint64_t d = arts_startup_deadline_ns();
  CHECK(d >= before + (uint64_t)want * 1000000000ull &&
            d <= before + ((uint64_t)want + 1u) * 1000000000ull,
        "BUDGET: the deadline");

  struct utsname u;
  CHECK(uname(&u) == 0, "uname");
  char expect[512];
  snprintf(expect, sizeof expect,
           "ARTS_MESH_FAILED rank=- host=%s phase=config missing=- "
           "missing_hosts=- more=0\n",
           u.nodename);
  n = arts_startup_expired_line(buf, sizeof buf);
  CHECK(strcmp(buf, expect) == 0 && n == strlen(buf), "EXPIRED");

  /* PAST_LISTEN: rank 1 of 4 reaches fabric, then exchange, entering no
   * listen step; the stubbed handler rank reads 0. */
  table_of(4, 0);
  static struct arts_config_s config4;
  config4.table_length = 4;
  config4.my_rank = 1;
  config4.table = g_rows;
  arts_global_message_table = &config4;
  arts_startup_step(ARTS_MESH_PHASE_FABRIC);
  arts_startup_expired_line(buf, sizeof buf);
  CHECK(strcmp(buf, "ARTS_MESH_FAILED rank=0 host=n01 phase=fabric "
                    "missing=- missing_hosts=- more=0\n") == 0,
        "PAST_LISTEN: fabric");
  arts_startup_step(ARTS_MESH_PHASE_EXCHANGE);
  arts_startup_expired_line(buf, sizeof buf);
  CHECK(strcmp(buf, "ARTS_MESH_FAILED rank=0 host=n01 phase=exchange "
                    "missing=- missing_hosts=- more=0\n") == 0,
        "PAST_LISTEN: exchange");

  /* PRE_RANK_GIVE_UP and PRE_RANK_END_BEGAN: arts_mesh_fail in a child
   * that is no rank yet, its stderr a pipe; the stubbed arts_exit ends it
   * with 70 + rc, so either child shows that the give-up entered it. */
  g_rank_known = false;
  for (int began = 0; began < 2; began++) {
    int err[2];
    CHECK(pipe(err) == 0, "pipe");
    fflush(NULL);
    pid_t child = fork();
    CHECK(child >= 0, "fork");
    if (child == 0) {
      dup2(err[1], STDERR_FILENO);
      g_exit_began = began != 0;
      arts_mesh_fail(ARTS_MESH_PHASE_RESOLVE, NULL);
    }
    close(err[1]);
    int status = 0;
    pid_t ended = waitpid(child, &status, 0);
    char got[4096];
    ssize_t len = read(err[0], got, sizeof(got) - 1);
    got[len > 0 ? len : 0] = '\0';
    close(err[0]);
    if (!began) {
      snprintf(expect, sizeof expect,
               "ARTS_MESH_FAILED rank=- host=%s phase=resolve missing=- "
               "missing_hosts=- more=0\n",
               u.nodename);
      CHECK(ended == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 70 + ARTS_MESH_EXIT_STATUS,
            "PRE_RANK_GIVE_UP: the process's end");
      CHECK(strcmp(got, expect) == 0, "PRE_RANK_GIVE_UP: the line");
    } else {
      CHECK(ended == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 70 + ARTS_MESH_EXIT_STATUS,
            "PRE_RANK_END_BEGAN: the give-up did not enter arts_exit");
      CHECK(got[0] == '\0', "PRE_RANK_END_BEGAN: a line was written");
    }
  }

  printf("PASS mesh_line\n");
  return 0;
}
