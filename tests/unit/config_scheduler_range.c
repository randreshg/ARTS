/* SPDX-License-Identifier: Apache-2.0
 *
 * The `scheduler` key indexes the scheduler-loop dispatch table, so config
 * load must refuse an index the table does not have (the first one past its
 * last entry) and accept its last entry.  The bound is the table this build
 * links, read from the library itself.
 *
 * ARTS_ERROR exits the process, so each case loads its cfg in a forked child
 * and the parent reads the child's exit status.  No runtime starts.
 */

#include "arts.h"
#include "arts/runtime.h"
#include "arts/system/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Load a cfg naming `scheduler=<index>` in a forked child: 1 when the load
 * was refused (ARTS_ERROR's exit status 1), 0 when it completed, -1 when the
 * child ended any other way or the harness failed. */
static int load_refused(unsigned int index) {
  char path[256];
  snprintf(path, sizeof(path), "config_scheduler_range_%u_%ld.cfg", index,
           (long)getpid());
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }
  fprintf(f,
          "[ARTS]\n"
          "launcher=local\n"
          "node_count=1\n"
          "worker_threads=2\n"
          "route_table_size=14\n"
          "scheduler=%u\n",
          index);
  (void)fclose(f);
  pid_t pid = fork();
  if (pid < 0) {
    (void)remove(path);
    return -1;
  }
  if (pid == 0) {
    setenv("ARTS_CONFIG", path, 1);
    unsetenv("scheduler");
    unsetenv("ports");
    unsetenv("port_count");
    struct arts_config_s config;
    arts_config_load(&config);
    _exit(0);
  }
  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  (void)remove(path);
  if (w < 0) {
    return -1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
    return 1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return 0;
  }
  return -1;
}

int main(void) {
  if (scheduler_loop_count < 3u) {
    printf("FAIL config_scheduler_range: the table has %u loops, fewer than "
           "the three every build carries\n",
           scheduler_loop_count);
    return 1;
  }
  int last = load_refused(scheduler_loop_count - 1u);
  int past = load_refused(scheduler_loop_count);
  if (last != 0) {
    printf("FAIL config_scheduler_range: scheduler=%u (the last loop) was "
           "%s\n",
           scheduler_loop_count - 1u,
           last < 0 ? "neither loaded nor refused" : "refused");
    return 1;
  }
  if (past != 1) {
    printf("FAIL config_scheduler_range: scheduler=%u (past the last loop) "
           "was %s\n",
           scheduler_loop_count,
           past < 0 ? "neither loaded nor refused" : "accepted");
    return 1;
  }
  printf("PASS config_scheduler_range: scheduler=%u loads, scheduler=%u is "
         "refused\n",
         scheduler_loop_count - 1u, scheduler_loop_count);
  return 0;
}
