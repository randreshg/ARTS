/// @file fam_strict_backend_rule.c
/// @brief Which backend may run the strict oracle, and where its ranks may
/// run.
///
///   - over the vendored fake library (SHM): fam_strict defaults to on, both
///     values are accepted, and a run of more than one rank is refused under
///     any launcher but local, since the store is one host's memory
///   - over a device library (DEVICE): fam_strict defaults to off and
///     fam_strict=1 is refused, naming the rule, because that memory already
///     has the second coherency domain the oracle emulates
///
/// One source, compiled once per backend: the SHM target takes the tree's own
/// definitions and the DEVICE target replaces them, so both rules are checked
/// in a tree that can link the adapter.  The refusals are ARTS_ERROR death
/// paths, so every check runs in a forked child.

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/fam/pool.h"
#include "arts/system/config.h"

#if defined(ARTS_FAM_BACKEND_SHM) == defined(ARTS_FAM_BACKEND_DEVICE)
#error "this probe is compiled for exactly one of the two backends"
#endif

static int fails;

/* 1 when the check killed the child, and its output in out. */
static int check_dies(bool strict, const char *launcher, unsigned nranks,
                      char *out, size_t cap) {
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
    memset(&config, 0, sizeof(config));
    config.fam_pool_mb = 8;
    config.fam_strict = strict;
    config.table_length = nranks;
    config.worker_thread_count = 1;
    config.launcher = (char *)launcher;
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

static void expect_accepted(const char *tag, bool strict, const char *launcher,
                            unsigned nranks) {
  char out[4096];
  if (check_dies(strict, launcher, nranks, out, sizeof(out)) != 0) {
    printf("FAIL fam_strict_backend_rule: %s was refused: %s\n", tag, out);
    fails++;
  }
}

static void expect_refused(const char *tag, bool strict, const char *launcher,
                           unsigned nranks, const char *must_say) {
  char out[4096];
  if (check_dies(strict, launcher, nranks, out, sizeof(out)) != 1) {
    printf("FAIL fam_strict_backend_rule: %s was accepted\n", tag);
    fails++;
  } else if (!strstr(out, must_say)) {
    printf("FAIL fam_strict_backend_rule: %s was refused without saying "
           "'%s': %s\n",
           tag, must_say, out);
    fails++;
  }
}

int main(void) {
#ifdef ARTS_FAM_BACKEND_SHM
  const char *backend = "SHM";
  if (strcmp(ARTS_FAM_STRICT_DEFAULT, "1") != 0) {
    printf("FAIL fam_strict_backend_rule: fam_strict defaults to %s over the "
           "vendored fake library\n",
           ARTS_FAM_STRICT_DEFAULT);
    fails++;
  }
  expect_accepted("strict", true, "local", 1u);
  expect_accepted("plain", false, "local", 1u);
  expect_accepted("local, two ranks", true, "local", 2u);
  expect_accepted("ssh, one rank", true, "ssh", 1u);
  expect_refused("ssh, two ranks", true, "ssh", 2u, "launcher=local");
#else
  const char *backend = "DEVICE";
  if (strcmp(ARTS_FAM_STRICT_DEFAULT, "0") != 0) {
    printf("FAIL fam_strict_backend_rule: fam_strict defaults to %s over a "
           "device library\n",
           ARTS_FAM_STRICT_DEFAULT);
    fails++;
  }
  expect_refused("strict", true, "local", 1u, "ARTS_FAM_BACKEND=SHM");
  expect_refused("strict (names the key)", true, "local", 1u, "fam_strict");
  expect_accepted("plain", false, "local", 1u);
  expect_accepted("ssh, two ranks", false, "ssh", 2u);
#endif
  if (fails) {
    return 1;
  }
  printf("PASS fam_strict_backend_rule %s\n", backend);
  return 0;
}
