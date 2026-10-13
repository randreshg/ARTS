/* SPDX-License-Identifier: Apache-2.0
 *
 * Removed cfg-key hard errors (config_reject_removed_keys, config.c).
 *
 * Property under test
 * -------------------
 * A key that no longer names anything in the config surface is a HARD ERROR:
 * arts_config_load() calls ARTS_ERROR (== arts_abort(1) == exit) as soon as
 * the key is present in the parsed variable list, before any default it used
 * to carry could silently apply.  A stale cfg must fail loudly, not run with
 * an unintended setting.  The keys: sender_threads (no sender role exists),
 * receiver_threads (renamed progress_threads), default_ports (renamed ports),
 * deque_type (there is one deque), auto_shutdown (the runtime never ends a
 * run because its work drained), fam_pool_mb and fam_strict (the CXL store's
 * arenas are sized at build time).
 *
 * ARTS_ERROR aborts the process, so each rejection case is driven in a forked
 * child (same discipline as config_ports_contract.c) and the parent asserts
 * the child exited with ARTS_ERROR's status 1.  A clean exit is an acceptance;
 * any other end (another status, a signal) is neither a load nor a refusal,
 * and fails the test too.  A positive control loads a cfg naming the live
 * `progress_threads` key and asserts it loads normally (in the parent
 * process, since nothing should abort) — so the refusal is shown to follow
 * the removed names, not every key the loader reads.
 *
 * This is a config-parser test: it calls arts_config_load() directly against
 * crafted temp cfgs (no runtime started, no ports bound).  Self-contained: it
 * sets its own ARTS_CONFIG and clears the inherited `ports`, `port_count` and
 * `progress_threads` env overrides.  Orthogonal to the coherence protocol
 * axis.
 */

#include "arts.h"
#include "arts/system/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_cfg(char *path_out, size_t path_cap, const char *tag,
                     const char *extra_line) {
  snprintf(path_out, path_cap, "config_removed_keys_reject_%s_%ld.cfg", tag,
           (long)getpid());
  FILE *f = fopen(path_out, "w");
  if (!f) {
    return -1;
  }
  fputs("[ARTS]\n"
        "launcher=local\n"
        "node_count=2\n"
        "worker_threads=2\n",
        f);
  fputs(extra_line, f);
  fputs("route_table_size=14\n", f);
  (void)fclose(f);
  return 0;
}

/* Fork and load the given cfg in the child: 1 when the load was refused (exit
 * status 1, ARTS_ERROR's), 0 when it loaded (exit 0), -1 for any other end. */
static int child_load_refused(const char *cfg_path) {
  /* The child's exit through ARTS_ERROR flushes what it inherited. */
  (void)fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }
  if (pid == 0) {
    setenv("ARTS_CONFIG", cfg_path, 1);
    unsetenv("ports");
    unsetenv("port_count");
    struct arts_config_s config;
    arts_config_load(&config);
    /* Reached only if the removed-key check did NOT fire. */
    _exit(0);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return -1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
    return 1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return 0;
  }
  if (WIFSIGNALED(status)) {
    printf("config_removed_keys_reject: child died on signal %d\n",
           WTERMSIG(status));
  } else {
    printf("config_removed_keys_reject: child exited with status %d\n",
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  }
  return -1;
}

int main(void) {
  int fails = 0;
  char cfg_path[256];

  static const struct {
    const char *key;
    const char *line;
  } removed[] = {
      {"sender_threads", "sender_threads=1\n"},
      {"receiver_threads", "receiver_threads=1\n"},
      {"default_ports", "default_ports=25000\n"},
      {"deque_type", "deque_type=0\n"},
      {"auto_shutdown", "auto_shutdown=1\n"},
      {"fam_pool_mb", "fam_pool_mb=1\n"},
      {"fam_strict", "fam_strict=1\n"},
  };
  for (size_t k = 0; k < sizeof(removed) / sizeof(removed[0]); k++) {
    if (write_cfg(cfg_path, sizeof(cfg_path), removed[k].key,
                  removed[k].line) != 0) {
      printf("FAIL config_removed_keys_reject: cannot write %s cfg\n",
             removed[k].key);
      return 1;
    }
    int refused = child_load_refused(cfg_path);
    (void)remove(cfg_path);
    if (refused == 0) {
      printf("FAIL config_removed_keys_reject: %s was ACCEPTED "
             "(expected hard error)\n",
             removed[k].key);
      fails++;
    } else if (refused < 0) {
      printf("FAIL config_removed_keys_reject: %s was neither loaded nor "
             "refused\n",
             removed[k].key);
      fails++;
    }
  }

  /* Positive control: the live progress_threads key loads normally, so
   * the refusal follows the removed NAMES, not every key the loader reads.
   * Run directly (not forked): this must NOT abort. */
  if (write_cfg(cfg_path, sizeof(cfg_path), "progress",
                "progress_threads=1\n") != 0) {
    printf("FAIL config_removed_keys_reject: cannot write progress_threads "
           "cfg\n");
    return 1;
  }
  setenv("ARTS_CONFIG", cfg_path, 1);
  unsetenv("ports");
  unsetenv("port_count");
  unsetenv("progress_threads");
  struct arts_config_s config;
  arts_config_load(&config);
  if (config.progress_thread_count != 1) {
    printf("FAIL config_removed_keys_reject: progress_threads=1 not honored "
           "(progress=%u)\n",
           config.progress_thread_count);
    fails++;
  }
  arts_config_destroy(&config);
  (void)remove(cfg_path);

  if (fails == 0) {
    printf("PASS config_removed_keys_reject: sender_threads, "
           "receiver_threads, default_ports, deque_type, auto_shutdown, "
           "fam_pool_mb and fam_strict hard-error; progress_threads loads "
           "normally\n");
    return 0;
  }
  return 1;
}
