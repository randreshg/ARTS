/// @file config_fam_contract.c
/// @brief What the fabric-attached-memory cfg keys accept, and what they
/// refuse.
///
///   - the two keys parse in EVERY tree, FAM-enabled or not: the cfg files are
///     shared by all of them, and a tree that cannot use a key ignores it
///   - fam_pool_mb defaults to 64 and must name at least 1 MB
///   - a rank with no worker thread is refused: nobody would bring a block's
///     bytes in
///   - a pool this host cannot afford is refused at load, in both modes
///   - under the inherited-mapping backend, any launcher but local is refused
///   - fam_strict parses wherever the store is one host's memory (that backend,
///     and the device backend over the vendored fake library, where it is ON
///     by default) and is refused over a real device library
///
/// Config-parser test: arts_config_load() against crafted temp cfgs, no
/// runtime started. The rejections are ARTS_ERROR death paths, so they are
/// probed in a forked child.

#include "arts.h"
#include "arts/system/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

/* What a case that says nothing about the run's shape is written on top of.
 * The parser keeps the FIRST value it sees for a key, so a case ABOUT one of
 * these keys must be written without it -- see write_cfg_on. */
#define CFG_PRELUDE                                                            \
  "worker_threads=2\n"                                                         \
  "progress_threads=1\n"                                                       \
  "route_table_size=14\n"

static int write_cfg_on(char *path_out, size_t path_cap, const char *tag,
                        const char *prelude, const char *body) {
  snprintf(path_out, path_cap, "config_fam_contract_%s_%ld.cfg", tag,
           (long)getpid());
  FILE *f = fopen(path_out, "w");
  if (!f) {
    return -1;
  }
  fputs("[ARTS]\n", f);
  fputs(prelude, f);
  fputs(body, f);
  (void)fclose(f);
  return 0;
}

static int write_cfg(char *path_out, size_t path_cap, const char *tag,
                     const char *body) {
  return write_cfg_on(path_out, path_cap, tag, CFG_PRELUDE, body);
}

/* Load in a forked child; returns the child's output and whether it died. */
static int load_dies(const char *cfg_path, char *out, size_t cap) {
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
    setenv("ARTS_CONFIG", cfg_path, 1);
    struct arts_config_s config;
    arts_config_load(&config);
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

static void expect_refused_on(const char *tag, const char *prelude,
                              const char *body, const char *must_say) {
  char cfg[256];
  char out[4096];
  if (write_cfg_on(cfg, sizeof(cfg), tag, prelude, body) != 0) {
    printf("FAIL config_fam_contract: cannot write %s cfg\n", tag);
    fails++;
    return;
  }
  int died = load_dies(cfg, out, sizeof(out));
  (void)unlink(cfg);
  if (died != 1) {
    printf("FAIL config_fam_contract: %s was accepted\n", tag);
    fails++;
    return;
  }
  if (!strstr(out, must_say)) {
    printf("FAIL config_fam_contract: %s was refused without saying '%s': %s\n",
           tag, must_say, out);
    fails++;
  }
}

static void expect_refused(const char *tag, const char *body,
                           const char *must_say) {
  expect_refused_on(tag, CFG_PRELUDE, body, must_say);
}

static void expect_parsed(const char *tag, const char *body,
                          unsigned want_pool_mb, int want_strict) {
  char cfg[256];
  if (write_cfg(cfg, sizeof(cfg), tag, body) != 0) {
    printf("FAIL config_fam_contract: cannot write %s cfg\n", tag);
    fails++;
    return;
  }
  setenv("ARTS_CONFIG", cfg, 1);
  struct arts_config_s config;
  arts_config_load(&config);
  if (config.fam_pool_mb != want_pool_mb) {
    printf("FAIL config_fam_contract: %s fam_pool_mb=%u want %u\n", tag,
           config.fam_pool_mb, want_pool_mb);
    fails++;
  }
  if ((config.fam_strict ? 1 : 0) != want_strict) {
    printf("FAIL config_fam_contract: %s fam_strict=%d want %d\n", tag,
           config.fam_strict ? 1 : 0, want_strict);
    fails++;
  }
  arts_config_destroy(&config);
  (void)unlink(cfg);
}

int main(void) {
  /* Parsed in every tree: the keys are inert where they cannot be used. */
  const int strict_default = ARTS_FAM_STRICT_DEFAULT[0] == '1';
#ifdef ARTS_FAM_DEVICE_VENDORED
  if (!strict_default) {
    printf("FAIL config_fam_contract: fam_strict is not on by default over "
           "the vendored fake library\n");
    fails++;
  }
#else
  if (strict_default) {
    printf("FAIL config_fam_contract: fam_strict is on by default in a tree "
           "whose store is not the vendored fake library\n");
    fails++;
  }
#endif
  expect_parsed("named", "launcher=local\nnode_count=1\nfam_pool_mb=8\n", 8u,
                strict_default);
  expect_parsed("default", "launcher=local\nnode_count=1\n", 64u,
                strict_default);

#ifdef ARTS_FAM
  /* A rank with no worker has nobody to bring a block's bytes in.  Written
   * with NO prelude, because the prelude names both thread counts and the
   * first value for a key is the one that stands; both are named here so the
   * single-node reclaim still leaves the derived worker count at zero, which
   * is what the check reads.  The must-say is the key and its value, which no
   * other refusal in this file can produce. */
  expect_refused_on("no_worker", "route_table_size=14\n",
                    "launcher=local\nnode_count=1\nworker_threads=0\n"
                    "progress_threads=0\nfam_pool_mb=8\n",
                    "worker_threads=0");
  expect_refused("zero", "launcher=local\nnode_count=1\nfam_pool_mb=0\n",
                 "fam_pool_mb");
  /* A pool no host of this class can afford, in either mode. */
  expect_refused("unaffordable",
                 "launcher=local\nnode_count=1\nfam_pool_mb=1048576\n",
                 "host allows");
  /* fam_pool_mb=2^31+1000: 2 * fam_pool_mb overflows a 32-bit accumulator
   * and wraps to ~2000 MB, comfortably under the cap it should have failed.
   * Must refuse with the budget message specifically -- a wrapped
   * computation would still be refused, but by the granule-sentinel check
   * instead, with a different message. */
  expect_refused("budget_wrap",
                 "launcher=local\nnode_count=1\nfam_pool_mb=2147484648\n",
                 "host allows");
#endif

#ifdef ARTS_FAM_BACKEND_SHM
  expect_parsed("strict", "launcher=local\nnode_count=1\nfam_strict=1\n", 64u, 1);
  expect_refused("remote",
                 "launcher=ssh\nnodes=localhost,localhost\nnode_count=2\n"
                 "ports=25000\nfam_pool_mb=8\n",
                 "launcher=local");
#endif
#ifdef ARTS_FAM_DEVICE_VENDORED
  expect_parsed("strict", "launcher=local\nnode_count=1\nfam_strict=1\n", 64u,
                1);
  expect_parsed("strict_off", "launcher=local\nnode_count=1\nfam_strict=0\n",
                64u, 0);
#endif
#if defined(ARTS_FAM) && !defined(ARTS_FAM_BACKEND_SHM) &&                     \
    !defined(ARTS_FAM_DEVICE_VENDORED)
  expect_refused("strict_unsupported",
                 "launcher=local\nnode_count=1\nfam_strict=1\n", "fam_strict");
#endif

  if (fails) {
    return 1;
  }
  printf("PASS config_fam_contract\n");
  return 0;
}
