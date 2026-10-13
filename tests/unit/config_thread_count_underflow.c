/* SPDX-License-Identifier: Apache-2.0
 *
 * config_compute_derived worker-count derivation.  With a pre-set
 * thread_count the worker count is
 *     worker = thread_count - progress_thread_count,
 * an unsigned subtraction that must never wrap.  A progress_thread_count
 * above a pre-set thread_count is refused at load, so the boundary the
 * derivation itself must survive is progress == thread_count.
 *
 * This test drives the multi-node branch (table_length>1, table==NULL so the
 * port loop is skipped) with
 *   1. the boundary (thread=2, progress=2): worker is a small in-range value
 *      (0 here) and thread_count stays consistent, NOT an astronomical wrap;
 *   2. progress above the pre-set count (thread=2, progress=4): the load is
 *      refused with the configuration loader's message and ARTS_ERROR's exit
 *      status 1.  The refusal leaves the derivation half done and its message
 *      on stderr, so this case runs in a forked child whose stderr the parent
 *      reads; the stubbed arts_abort turns ARTS_ERROR's code into the child's
 *      exit status.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#define REFUSAL "progress_threads=4 exceeds the 2 thread(s) this rank was given"

static void config_with_threads(struct arts_config_s *c,
                                unsigned int progress) {
  memset(c, 0, sizeof(*c));
  c->table_length = 2; /* multi-node branch */
  c->table = NULL;     /* port-population loop is guarded on table != NULL */
  /* The multi-node branch also resolves the port block, which needs a launcher
     to know who is allowed to choose it. */
  c->launcher = arts_config_make_new_var("local");
  c->route_table_size = 16;
  c->gpu_route_table_size = 12;

  c->thread_count = 2; /* pre-set (as if from SLURM_CPUS_PER_TASK) */
  c->progress_thread_count = progress;
  c->worker_thread_count = 0;
}

/* Derive the counts for progress=4 in a forked child.  Answers 1 when the
 * child ended with exit status 1 and printed the refusal, 0 otherwise. */
static int refused_above_thread_count(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    perror("pipe");
    return 0;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(fds[0]);
    close(fds[1]);
    return 0;
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    struct arts_config_s c;
    config_with_threads(&c, 4);
    /* arts_abort(code) lands here as code + 1; ARTS_ERROR passes 1. */
    g_abort_armed = 1;
    switch (setjmp(g_abort_jmp)) {
    case 0:
      config_compute_derived(&c);
      _exit(0);
    case 2:
      _exit(1);
    default:
      _exit(3);
    }
  }
  close(fds[1]);
  char out[4096];
  size_t len = 0;
  ssize_t n;
  while ((n = read(fds[0], out + len, sizeof(out) - 1 - len)) > 0) {
    len += (size_t)n;
    if (len == sizeof(out) - 1) {
      break;
    }
  }
  out[len] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    perror("waitpid");
    return 0;
  }
  int exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  bool said = strstr(out, REFUSAL) != NULL;
  if (exit_status != 1 || !said) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: thread=2 progress=4 ended "
            "with status %d and %s the refusal (child stderr: %s)\n",
            exit_status, said ? "printed" : "did not print", out);
    return 0;
  }
  return 1;
}

int main(void) {
  struct arts_config_s c;
  config_with_threads(&c, 2); /* boundary: progress == thread_count */

  config_compute_derived(&c);

  /* worker = thread - progress = 2 - 2 = 0 (in range, not a ~4e9 wrap). */
  if (c.worker_thread_count > c.thread_count) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: worker=%u underflowed "
            "(thread_count=%u)\n",
            c.worker_thread_count, c.thread_count);
    return 1;
  }
  if (c.worker_thread_count != 0) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: worker=%u, expected 0 "
            "(thread=2, progress=2)\n",
            c.worker_thread_count);
    return 1;
  }
  /* compute_derived allocated the port block (multi-node, none named). */
  arts_config_destroy(&c);

  if (!refused_above_thread_count()) {
    return 1;
  }

  printf("PASS config_thread_count_underflow: worker=0 at progress == "
         "thread_count, progress above it refused\n");
  return 0;
}
