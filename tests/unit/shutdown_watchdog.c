/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/* shutdown_watchdog -- an end that cannot finish is ended by its watchdog.
 *
 * One rank.  An observer EDT starts first and starts the rest, then waits for
 * the rank's shutdown point to be taken: it prints that it still runs
 * ARTS_EXIT_WATCHDOG_S - 1 seconds after it saw the take, and fails the run
 * if the process is still alive ARTS_EXIT_WATCHDOG_S + 5 seconds after it.
 * The watchdog must therefore end the process between the two.  Modes:
 *
 *   fires   a holder EDT locks stdout and starts the EDT that ends the run,
 *           then holds the lock for good: the end's last flush waits for it,
 *           so the watchdog names phase flush and ends the rank with 71.
 *   late    as fires, but once the point is taken the holder calls
 *           arts_abort(9), a late entry that parks holding the lock: the
 *           watchdog ends the rank with the late entry's 9.
 *   fault   as fires, but the EDT the holder starts raises SIGSEGV: the end
 *           runs in the crash handler, whose signal stays blocked there, and
 *           the watchdog ends the rank by SIGSEGV again (139).
 *   fault_spin
 *           an EDT pins itself to its CPU and raises SIGSEGV; the program's
 *           status function, which the end calls in the crash handler right
 *           after arming the watchdog, then keeps that CPU for good.  The
 *           timer expires on that CPU with the owner running; on a kernel
 *           that hands a process timer's signal to the thread it interrupts
 *           (Linux 6.4 on) the signal reaches the owner, whose SIGSEGV is
 *           blocked, and the watchdog must unblock it to end the rank by it
 *           (phase post, no crash report).  Elsewhere the signal may reach
 *           another thread, and the mode checks what fault does.
 *   blocked as fires, from a process whose main thread blocks the
 *           watchdog's signal before the runtime starts, as a launcher may
 *           hand it a mask that blocks it: the runtime unblocks it.
 *   stderr  as fires, with the process's stderr a pipe nobody reads, which
 *           the EDT that ends the run fills before it does: every write to
 *           stderr now blocks, so the watchdog skips its line and still ends
 *           the rank with 71.  The test's own lines go to the stderr the
 *           process was started with.
 *   record  main makes the counter folder and a FIFO under the name of the
 *           node record's temporary file before the runtime starts, so the
 *           record's open waits for a reader that never comes: the watchdog
 *           names phase record, says the record is lost and keeps status 0.
 *           A run whose watchdog never fires removes the FIFO itself.
 *
 * Every line of the test is one write(2) to the stderr the process was
 * started with: stdout stays locked but in record.
 * Whitebox: it reads arts_shutdown_taken() and the tree's bound.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/system/shutdown.h" /* arts_shutdown_taken, the bound */

enum watchdog_mode {
  MODE_FIRES,
  MODE_LATE,
  MODE_FAULT,
  MODE_FAULT_SPIN,
  MODE_BLOCKED,
  MODE_STDERR,
  MODE_RECORD
};

/* Written by main before the runtime starts any thread. */
static enum watchdog_mode g_mode;
static char g_fifo[4096];
static int g_report_fd = STDERR_FILENO;

static void say(const char *line) {
  (void)!write(g_report_fd, line, strlen(line));
}

static uint64_t now_ns(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return ((uint64_t)t.tv_sec * 1000000000u) + (uint64_t)t.tv_nsec;
}

static void sleep_until(uint64_t when_ns) {
  for (uint64_t t = now_ns(); t < when_ns; t = now_ns()) {
    uint64_t left = when_ns - t;
    if (left > 10000000u) {
      left = 10000000u;
    }
    struct timespec d = {.tv_sec = 0, .tv_nsec = (long)left};
    (void)nanosleep(&d, NULL);
  }
}

static void wait_for_take(void) {
  while (!arts_shutdown_taken()) {
    sleep_until(now_ns() + 1000000u);
  }
}

static void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

/* Fills stderr, a pipe nobody reads, to the last byte, so that no write to it
 * can complete, then ends the run. */
static void fill_end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  static char chunk[4096];
  const int flags = fcntl(STDERR_FILENO, F_GETFL);
  (void)fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK);
  while (write(STDERR_FILENO, chunk, sizeof(chunk)) > 0) {
  }
  while (write(STDERR_FILENO, chunk, 1) > 0) {
  }
  (void)fcntl(STDERR_FILENO, F_SETFL, flags);
  arts_shutdown();
}

static void fault_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)raise(SIGSEGV);
}

static void fault_spin_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  cpu_set_t here;
  CPU_ZERO(&here);
  CPU_SET(sched_getcpu(), &here);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(here), &here);
  (void)raise(SIGSEGV);
}

static void holder_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  flockfile(stdout);
  arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
  arts_edt_t ends = g_mode == MODE_FAULT    ? fault_edt
                    : g_mode == MODE_STDERR ? fill_end_edt
                                            : end_edt;
  (void)arts_edt_create(ends, 0, NULL, 0, &hint);
  if (g_mode == MODE_LATE) {
    wait_for_take();
    arts_abort(9);
  }
  for (;;) {
    (void)pause();
  }
}

static void observer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                         arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
  arts_edt_t first = g_mode == MODE_RECORD       ? end_edt
                     : g_mode == MODE_FAULT_SPIN ? fault_spin_edt
                                                 : holder_edt;
  (void)arts_edt_create(first, 0, NULL, 0, &hint);
  wait_for_take();
  const uint64_t taken = now_ns();
  char line[128];
  sleep_until(taken + ((uint64_t)(ARTS_EXIT_WATCHDOG_S - 1) * 1000000000u));
  (void)snprintf(line, sizeof(line),
                 "WATCHDOG_TEST: running %d s after the take\n",
                 ARTS_EXIT_WATCHDOG_S - 1);
  say(line);
  sleep_until(taken + ((uint64_t)(ARTS_EXIT_WATCHDOG_S + 5) * 1000000000u));
  (void)snprintf(line, sizeof(line),
                 "FAIL: the process is alive %d s after the take\n",
                 ARTS_EXIT_WATCHDOG_S + 5);
  say(line);
  if (g_mode == MODE_RECORD) {
    (void)unlink(g_fifo);
  }
  _exit(1);
}

/* The program's status, which the end reads: in mode fault_spin, once the end
 * began it never returns and keeps its CPU, as a stuck status function
 * would; otherwise 0. */
int arts_app_exit_status(void) {
  if (g_mode == MODE_FAULT_SPIN && arts_shutdown_taken()) {
    for (;;) {
      __asm__ __volatile__("" ::: "memory");
    }
  }
  return 0;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
  (void)arts_edt_create(observer_edt, 0, NULL, 0, &hint);
}

/* The node record's temporary file becomes a FIFO with no reader. */
static int block_record(void) {
  const char *folder = getenv("counter_folder");
  if (folder == NULL || folder[0] == '\0') {
    say("FAIL: counter_folder is not set\n");
    return 1;
  }
  if (mkdir(folder, 0755) != 0 && errno != EEXIST) {
    say("FAIL: cannot make the counter folder\n");
    return 1;
  }
  int n = snprintf(g_fifo, sizeof(g_fifo), "%s/n0.json.tmp", folder);
  if (n < 0 || (size_t)n >= sizeof(g_fifo)) {
    say("FAIL: the counter folder's path is too long\n");
    return 1;
  }
  (void)unlink(g_fifo);
  if (mkfifo(g_fifo, 0644) != 0) {
    say("FAIL: cannot make the FIFO\n");
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  static const char *const names[] = {
      "fires", "late", "fault", "fault_spin", "blocked", "stderr", "record"};
  const size_t count = sizeof(names) / sizeof(names[0]);
  size_t m = 0;
  while (argc >= 2 && m < count && strcmp(argv[1], names[m]) != 0) {
    m++;
  }
  if (argc < 2 || m == count) {
    say("FAIL: usage: shutdown_watchdog "
        "fires|late|fault|fault_spin|blocked|stderr|record\n");
    return 2;
  }
  g_mode = (enum watchdog_mode)m;
  if (g_mode == MODE_BLOCKED) {
    sigset_t watchdog;
    sigemptyset(&watchdog);
    sigaddset(&watchdog, SIGRTMIN + 1);
    if (pthread_sigmask(SIG_BLOCK, &watchdog, NULL) != 0) {
      say("FAIL: cannot block the watchdog's signal\n");
      return 1;
    }
  }
  if (g_mode == MODE_STDERR) {
    /* The pipe's read end stays open and unread, so a write to a full pipe
     * blocks instead of failing; it holds the runtime's lines up to the
     * fill. */
    int pipe_fds[2];
    g_report_fd = dup(STDERR_FILENO);
    if (g_report_fd < 0 || pipe(pipe_fds) != 0) {
      say("FAIL: cannot make the stderr pipe\n");
      return 1;
    }
    (void)fcntl(pipe_fds[1], F_SETPIPE_SZ, 1 << 20);
    if (dup2(pipe_fds[1], STDERR_FILENO) < 0) {
      say("FAIL: cannot make the stderr pipe\n");
      return 1;
    }
    (void)close(pipe_fds[1]);
  }
  if (g_mode == MODE_RECORD && block_record() != 0) {
    return 1;
  }
  return arts_rt(argc, argv);
}
