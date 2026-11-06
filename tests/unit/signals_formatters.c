/* SPDX-License-Identifier: Apache-2.0
 *
 * Async-signal-safe formatters in signals.c (write_signal_line, signal_name)
 * plus arts_print_line truncation (print.h).  write_signal_line and
 * signal_name are file-static in signals.c, so this TU #includes signals.c to
 * pull them in; the runtime symbols signals.c references (arts_global_rank_id,
 * arts_exit) are stubbed so the unit links standalone.
 *
 * Properties under test
 * -------------------
 * write_signal_line(pre, name, rank, post): composes the whole crash/term line
 *   ("<pre><name> (rank <N>)<post>") into one stack buffer and emits it with a
 *   SINGLE write() — so concurrent worker logging cannot interleave between the
 *   prefix, the signal name, the rank digits, and the suffix (a split line made
 *   ctest's PASS_REGEX miss intermittently before the single-write rewrite).
 *   Invariants:
 *     - rank 0 prints "0"; single- and multi-digit ranks print every digit;
 *     - the emitted bytes equal exactly <pre><name> (rank <N>)<post>;
 *     - nothing overflows the fixed buffer.
 *   We capture STDERR via a pipe and compare to a reference snprintf.
 *
 * signal_name(sig): async-signal-safe switch; MUST enumerate exactly the
 *   signals the handlers install (crash {SEGV,BUS,FPE} + term {TERM,INT,ALRM,
 *   HUP}); any other -> "UNKNOWN".  We assert each installed signal maps to its
 *   literal and a sampling of un-installed signals map to "UNKNOWN".
 *
 * arts_print_line(fmt,...): vsnprintf into a 4096 stack buffer + one
 *   write(STDERR) when stderr takes it.  A formatted length >= 4096 must
 *   TRUNCATE to size-1 bytes (no overflow, no NUL written).  We feed an
 *   oversized format and assert the captured output length is exactly 4095 and
 *   the tail is the truncated content (no trailing garbage).  A stderr pipe
 *   that is full and non-blocking (as an ssh client sharing the descriptor
 *   leaves it) must delay the line, never drop it: a child prints into such a
 *   pipe, stays blocked while the pipe is full, and its whole line follows
 *   the filler once the pipe is drained.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arts/system/shutdown.h"

/* ---- standalone stubs for symbols signals.c references ------------------- */
/* arts_global_rank_id is declared extern in identity.h (pulled in via the
 * include below); define storage here. */
unsigned int arts_global_rank_id = 0;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(rc);
}

/* Pull in the real signals.c (static write_signal_line + signal_name). */
#include "../../libs/src/core/system/signals.c"

/* signals.c also references the thread-local arts_thread_info (declared extern
 * in runtime_state.h, now visible via signals.c's include chain). Provide
 * standalone storage so the TU links without the full runtime. */
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

/* print.h (arts_print_line) is reachable via signals.c's includes. */
#include "arts/system/print.h"

/* ------------------------------------------------------------------------- */

static void fail(const char *msg) {
  /* write directly: stderr may be redirected mid-test. */
  char line[256];
  int n = snprintf(line, sizeof(line), "FAIL signals_formatters: %s\n", msg);
  (void)write(STDOUT_FILENO, line, (size_t)n);
  exit(1);
}

/* Capture whatever the callback writes to STDERR_FILENO into out (NUL-term). */
static size_t capture_stderr(void (*body)(void *), void *arg, char *out,
                             size_t out_cap) {
  int saved = dup(STDERR_FILENO);
  if (saved < 0) {
    fail("dup(stderr)");
  }
  int pfd[2];
  if (pipe(pfd) != 0) {
    fail("pipe");
  }
  /* Make read end non-blocking so we never hang if body writes nothing. */
  fflush(stderr);
  if (dup2(pfd[1], STDERR_FILENO) < 0) {
    fail("dup2");
  }
  close(pfd[1]);

  body(arg);

  fflush(stderr);
  /* Restore stderr BEFORE reading, so the write end is fully closed. */
  if (dup2(saved, STDERR_FILENO) < 0) {
    fail("dup2 restore");
  }
  close(saved);

  /* Drain the pipe. */
  size_t total = 0;
  for (;;) {
    if (total >= out_cap - 1) {
      break;
    }
    ssize_t r = read(pfd[0], out + total, out_cap - 1 - total);
    if (r > 0) {
      total += (size_t)r;
      continue;
    }
    break; /* r==0 (EOF, write end closed) or r<0 */
  }
  close(pfd[0]);
  out[total] = '\0';
  return total;
}

struct line_arg {
  const char *pre;
  const char *name;
  unsigned int rank;
  const char *post;
};
static void call_write_signal_line(void *a) {
  struct line_arg *p = (struct line_arg *)a;
  write_signal_line(p->pre, p->name, p->rank, p->post);
}

static void check_write_signal_line(const char *pre, const char *name,
                                    unsigned int rank, const char *post) {
  char got[256];
  struct line_arg a = {pre, name, rank, post};
  size_t n = capture_stderr(call_write_signal_line, &a, got, sizeof(got));
  char want[256];
  /* signals.c emits exactly: <pre><name> (rank <N>)<post>. */
  int wn =
      snprintf(want, sizeof(want), "%s%s (rank %u%s", pre, name, rank, post);
  if (n != (size_t)wn || memcmp(got, want, n) != 0) {
    char msg[600];
    snprintf(msg, sizeof(msg),
             "write_signal_line: got \"%s\" (%zu bytes) want \"%s\" (%d bytes)",
             got, n, want, wn);
    fail(msg);
  }
}

struct print_arg {
  const char *fmt;
  const char *blob;
};
static void call_print_line(void *a) {
  struct print_arg *p = (struct print_arg *)a;
  arts_print_line(p->fmt, p->blob);
}

int main(void) {
  /* ---- write_signal_line: rank 0, single/multi-digit, real crash/term forms
   */
  check_write_signal_line("\n[ARTS] Crashed: ", "SIGFPE", 0,
                          ") — stack trace:\n");
  check_write_signal_line("\n[ARTS] Killed by ", "SIGINT", 5, ")\n");
  check_write_signal_line("", "SIGSEGV", 9, ")\n");     /* single-digit rank */
  check_write_signal_line("p:", "SIGTERM", 123, ")\n"); /* multi-digit rank */
  check_write_signal_line("\n[ARTS] Crashed: ", "SIGBUS", 1000000u,
                          ") — stack trace:\n");

  /* ---- signal_name: every installed signal -> literal; others -> UNKNOWN ---
   */
  struct {
    int sig;
    const char *name;
  } installed[] = {
      {SIGSEGV, "SIGSEGV"}, {SIGBUS, "SIGBUS"}, {SIGFPE, "SIGFPE"},
      {SIGTERM, "SIGTERM"}, {SIGINT, "SIGINT"}, {SIGALRM, "SIGALRM"},
      {SIGHUP, "SIGHUP"},
  };
  for (size_t i = 0; i < sizeof(installed) / sizeof(installed[0]); i++) {
    const char *got = signal_name(installed[i].sig);
    if (strcmp(got, installed[i].name) != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "signal_name(%d) = \"%s\" want \"%s\"",
               installed[i].sig, got, installed[i].name);
      fail(msg);
    }
  }
  /* Signals NOT in the table must map to UNKNOWN. */
  int unknowns[] = {SIGUSR1, SIGUSR2, SIGCHLD, SIGQUIT, SIGKILL, 0, 9999};
  for (size_t i = 0; i < sizeof(unknowns) / sizeof(unknowns[0]); i++) {
    if (strcmp(signal_name(unknowns[i]), "UNKNOWN") != 0) {
      char msg[128];
      snprintf(msg, sizeof(msg), "signal_name(%d) != UNKNOWN", unknowns[i]);
      fail(msg);
    }
  }
  /* Consistency with the actual install set: the watcher's arts_term_sigs[]
   * must each have a non-UNKNOWN name (kept in sync invariant). */
  for (unsigned i = 0; i < arts_term_sig_count; i++) {
    if (strcmp(signal_name(arts_term_sigs[i]), "UNKNOWN") == 0) {
      fail("an installed term signal maps to UNKNOWN (name table drift)");
    }
  }

  /* ---- arts_print_line truncation: >=4096 formatted -> single 4095 write -
   */
  {
    /* Build a blob longer than the 4096 buffer so vsnprintf truncates. */
    size_t blobsz = 8000;
    char *blob = malloc(blobsz + 1);
    if (!blob) {
      fail("malloc blob");
    }
    memset(blob, 'X', blobsz);
    blob[blobsz] = '\0';

    struct print_arg pa = {"%s", blob};
    char *got = malloc(8192);
    if (!got) {
      fail("malloc got");
    }
    size_t n = capture_stderr(call_print_line, &pa, got, 8192);
    /* Single write of exactly size-1 == 4095 bytes (the truncated content),
     * no embedded NUL written. */
    if (n != 4095) {
      char msg[128];
      snprintf(msg, sizeof(msg),
               "arts_print_line truncation wrote %zu bytes, want 4095", n);
      fail(msg);
    }
    for (size_t i = 0; i < n; i++) {
      if (got[i] != 'X') {
        fail("arts_print_line truncated output corrupted");
      }
    }
    free(blob);
    free(got);
  }

  /* A short print must pass through unchanged (no spurious truncation). */
  {
    struct print_arg pa = {"%s", "hello-short"};
    char got[64];
    size_t n = capture_stderr(call_print_line, &pa, got, sizeof(got));
    if (n != strlen("hello-short") || memcmp(got, "hello-short", n) != 0) {
      fail("arts_print_line short string altered");
    }
  }

  /* A full non-blocking stderr pipe delays the line; it never drops it. */
  {
    int pfd[2];
    if (pipe(pfd) != 0 || fcntl(pfd[1], F_SETFL, O_NONBLOCK) != 0) {
      fail("non-blocking pipe");
    }
    char fill[4096];
    memset(fill, 'F', sizeof(fill));
    size_t filled = 0;
    ssize_t w;
    while ((w = write(pfd[1], fill, sizeof(fill))) > 0) {
      filled += (size_t)w;
    }
    while (write(pfd[1], fill, 1) == 1) {
      filled++;
    }
    int started[2];
    if (pipe(started) != 0) {
      fail("pipe");
    }
    pid_t child = fork();
    if (child == 0) {
      (void)dup2(pfd[1], STDERR_FILENO);
      (void)!write(started[1], "s", 1);
      arts_print_line("NONBLOCKING-LINE %d\n", 7);
      _exit(0);
    }
    (void)close(pfd[1]);
    (void)close(started[1]);
    char mark;
    if (read(started[0], &mark, 1) != 1) {
      fail("child did not start");
    }
    (void)close(started[0]);
    struct timespec pause = {0, 200 * 1000 * 1000};
    (void)nanosleep(&pause, NULL);
    if (waitpid(child, NULL, WNOHANG) != 0) {
      fail("arts_print_line returned while stderr was full and non-blocking "
           "(the line was dropped)");
    }
    const char *line = "NONBLOCKING-LINE 7\n";
    const size_t want = filled + strlen(line);
    char *got = malloc(want + 64);
    if (!got) {
      fail("malloc drained");
    }
    size_t total = 0;
    ssize_t r;
    while ((r = read(pfd[0], got + total, want + 64 - total)) > 0) {
      total += (size_t)r;
    }
    (void)close(pfd[0]);
    int status = 0;
    (void)waitpid(child, &status, 0);
    if (total != want || memcmp(got + filled, line, strlen(line)) != 0) {
      fail("arts_print_line into a full non-blocking pipe: the line did not "
           "follow the filler whole");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      fail("arts_print_line child did not exit 0");
    }
    free(got);
  }

  printf("PASS signals_formatters: write_signal_line + signal_name table + "
         "arts_print_line truncation and non-blocking stderr verified\n");
  return 0;
}
