/// @file config_fam_contract.c
/// @brief What the fabric-attached-memory cfg keys accept, and what they
/// refuse.
///
///   - the two keys parse in EVERY tree, FAM-enabled or not: the cfg files are
///     shared by all of them, and a tree that cannot use a key ignores it
///   - fam_pool_mb defaults to 64 and must name at least 1 MB
///   - a rank with no worker thread is refused: nobody would bring a block's
///     bytes in
///   - a pool is never refused for its size, only past the allocator's own
///     granule-index bound
///   - over the vendored fake library (SHM), a run of more than one rank is
///     refused under any launcher but local, since the store is one host's
///     memory; over a device library it is not
///   - fam_strict, a key that no longer exists, is refused in every tree
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
                          unsigned want_pool_mb) {
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
  arts_config_destroy(&config);
  (void)unlink(cfg);
}

int main(void) {
  /* Parsed in every tree: the key is inert where it cannot be used. */
  expect_parsed("named", "launcher=local\nnode_count=1\nfam_pool_mb=8\n", 8u);
  expect_parsed("default", "launcher=local\nnode_count=1\n", 64u);
  /* Larger than any laptop has free, and still accepted: the runtime never
   * refuses a pool for its size. */
  expect_parsed("large", "launcher=local\nnode_count=1\nfam_pool_mb=65536\n",
                65536u);
  expect_refused("strict_removed",
                 "launcher=local\nnode_count=1\nfam_strict=0\n",
                 "fam_strict no longer exists");

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
  /* A pool is never refused for its size, only past the allocator's own
   * bound: a 1 TB pool on one rank leaves a slice whose granule count reaches
   * the index sentinel. */
  expect_refused("sentinel",
                 "launcher=local\nnode_count=1\nfam_pool_mb=1048576\n",
                 "index sentinel");
#endif

#ifdef ARTS_FAM_BACKEND_SHM
  expect_refused("remote",
                 "launcher=ssh\nnodes=localhost,localhost\nnode_count=2\n"
                 "ports=25000\nfam_pool_mb=8\n",
                 "launcher=local");
#endif
#ifdef ARTS_FAM_BACKEND_DEVICE
  expect_parsed("remote",
                "launcher=ssh\nnodes=localhost,localhost\nnode_count=2\n"
                "ports=25000\nfam_pool_mb=8\n",
                8u);
#endif

  if (fails) {
    return 1;
  }
  printf("PASS config_fam_contract\n");
  return 0;
}
