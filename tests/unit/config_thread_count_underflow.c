/* SPDX-License-Identifier: Apache-2.0
 *
 * config_compute_derived's worker count.  With a pre-set thread_count (the
 * thread count SLURM_CPUS_PER_TASK gives a rank) the worker count is
 *     worker = thread_count - progress_thread_count,
 * an unsigned subtraction that must never wrap, and a rank needs at least
 * one worker to run EDTs.  Cases:
 *   1. thread=3, progress=2 on two nodes: accepted with one worker;
 *   2. thread=2, progress=2: refused, naming SLURM_CPUS_PER_TASK and
 *      progress_threads (no worker left);
 *   3. thread=2, progress=4: refused with the loader's message (the
 *      subtraction would wrap);
 *   4. worker_threads=0 on one node with no progress thread: refused, naming
 *      worker_threads and not SLURM_CPUS_PER_TASK;
 *   5. worker_threads=0 on one node with two progress threads: accepted, the
 *      progress threads reclaimed as two workers;
 *   6. worker_threads=0 on two nodes: refused, naming worker_threads and not
 *      SLURM_CPUS_PER_TASK, though the progress default makes thread_count 1.
 * A refusal leaves the derivation half done and its message on stderr, so
 * each refused case runs in a forked child whose stderr the parent reads; the
 * stubbed arts_abort turns ARTS_ERROR's code into the child's exit status.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

static void config_for(struct arts_config_s *c, unsigned int nodes,
                       unsigned int threads, unsigned int workers,
                       unsigned int progress) {
  memset(c, 0, sizeof(*c));
  c->table_length = nodes;
  c->table = NULL; /* port-population loop is guarded on table != NULL */
  /* The multi-node branch also resolves the port block, which needs a launcher
     to know who is allowed to choose it. */
  c->launcher = arts_config_make_new_var("local");
  c->route_table_size = 16;
  c->gpu_route_table_size = 12;
  c->deque_size = 4096;
  c->thread_count = threads; /* 0 = not given by the launcher */
  c->worker_thread_count = workers;
  c->progress_thread_count = progress;
}

/* Derives the counts in a forked child.  Answers 1 when the child ended with
 * ARTS_ERROR's exit status 1, printed `said` and did not print `unsaid`. */
static int refused(const char *name, unsigned int nodes, unsigned int threads,
                   unsigned int workers, unsigned int progress,
                   const char *said, const char *unsaid) {
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
    config_for(&c, nodes, threads, workers, progress);
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
  bool has = strstr(out, said) != NULL;
  bool has_not = unsaid == NULL || strstr(out, unsaid) == NULL;
  if (exit_status != 1 || !has || !has_not) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: %s ended with status %d; "
            "wanted \"%s\"%s%s%s (child stderr: %s)\n",
            name, exit_status, said, unsaid ? " and not \"" : "",
            unsaid ? unsaid : "", unsaid ? "\"" : "", out);
    return 0;
  }
  return 1;
}

/* Derives the counts in this process; 1 when they are as wanted. */
static int accepted(const char *name, unsigned int nodes, unsigned int threads,
                    unsigned int workers, unsigned int progress,
                    unsigned int want_workers, unsigned int want_threads) {
  struct arts_config_s c;
  config_for(&c, nodes, threads, workers, progress);
  config_compute_derived(&c);
  int ok =
      c.worker_thread_count == want_workers && c.thread_count == want_threads;
  if (!ok) {
    fprintf(stderr,
            "FAIL config_thread_count_underflow: %s gave worker=%u "
            "thread_count=%u, want %u and %u\n",
            name, c.worker_thread_count, c.thread_count, want_workers,
            want_threads);
  }
  arts_config_destroy(&c);
  return ok;
}

int main(void) {
  if (!accepted("thread=3 progress=2", 2, 3, 0, 2, 1, 3) ||
      !refused("thread=2 progress=2", 2, 2, 0, 2,
               "progress_threads=2 takes every one of the 2 thread(s) this "
               "rank was given (SLURM_CPUS_PER_TASK)",
               NULL) ||
      !refused("thread=2 progress=4", 2, 2, 0, 4,
               "progress_threads=4 exceeds the 2 thread(s) this rank was "
               "given",
               NULL) ||
      !refused("worker_threads=0 on one node", 1, 0, 0, 0,
               "worker_threads=0 leaves this rank no worker thread",
               "SLURM_CPUS_PER_TASK") ||
      !accepted("worker_threads=0 progress_threads=2 on one node", 1, 0, 0, 2,
                2, 2) ||
      !refused("worker_threads=0 on two nodes", 2, 0, 0, 0,
               "worker_threads=0 leaves this rank no worker thread",
               "SLURM_CPUS_PER_TASK")) {
    return 1;
  }
  printf("PASS config_thread_count_underflow: one worker left of 3 threads, "
         "no worker refused by its source, progress above the thread count "
         "refused, progress reclaimed on one node\n");
  return 0;
}
