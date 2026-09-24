/// @file fam_strict_device_rule.c
/// @brief Over a real device library, fam_strict is refused and its default
/// is off.
///
/// The device backend compiled WITHOUT ARTS_FAM_DEVICE_VENDORED is the backend
/// a real device library gets; this probe is that compilation (its target
/// undefines the flag), so the rule is checked in a tree that links the
/// vendored fake library for its symbols.  The refusal is an ARTS_ERROR death
/// path, so it is probed in a forked child.

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/fam/pool.h"
#include "arts/system/config.h"

#ifdef ARTS_FAM_DEVICE_VENDORED
#error "this probe is the real-device compilation of the backend"
#endif

static int fails;

static void base_config(struct arts_config_s *config, bool strict) {
  memset(config, 0, sizeof(*config));
  config->fam_pool_mb = 8;
  config->fam_strict = strict;
  config->table_length = 1;
  config->worker_thread_count = 1;
  config->launcher = (char *)"local";
}

/* 1 when the check killed the child, and its output in out. */
static int check_dies(bool strict, char *out, size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) {
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    struct arts_config_s config;
    base_config(&config, strict);
    arts_fam_config_check(&config);
    _exit(0);
  }
  close(fds[1]);
  size_t got = 0;
  for (;;) {
    ssize_t n = read(fds[0], out + got, cap - 1 - got);
    if (n <= 0 || got + 1 >= cap) {
      break;
    }
    got += (size_t)n;
  }
  out[got] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status));
}

int main(void) {
  char out[4096];
  if (strcmp(ARTS_FAM_STRICT_DEFAULT, "0") != 0) {
    printf("FAIL fam_strict_device_rule: fam_strict defaults to %s over a "
           "real device library\n",
           ARTS_FAM_STRICT_DEFAULT);
    fails++;
  }
  if (check_dies(true, out, sizeof(out)) != 1) {
    printf("FAIL fam_strict_device_rule: fam_strict=1 was accepted\n");
    fails++;
  } else if (!strstr(out, "fam_strict") || !strstr(out, "vendored")) {
    printf("FAIL fam_strict_device_rule: fam_strict=1 was refused without "
           "naming the rule: %s\n",
           out);
    fails++;
  }
  if (check_dies(false, out, sizeof(out)) != 0) {
    printf("FAIL fam_strict_device_rule: fam_strict=0 was refused: %s\n", out);
    fails++;
  }
  if (fails) {
    return 1;
  }
  printf("PASS fam_strict_device_rule\n");
  return 0;
}
