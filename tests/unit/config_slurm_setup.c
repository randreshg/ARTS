/* SPDX-License-Identifier: Apache-2.0
 *
 * config_setup_slurm over the environment a Slurm task may present.
 *
 * The slurm launcher arm takes the node count from SLURM_NNODES (1 when it is
 * unset), the rank's thread count from SLURM_CPUS_PER_TASK when it is set,
 * and the routing table from SLURM_STEP_NODELIST, else SLURM_JOB_NODELIST.
 * An environment it cannot use is refused by name (an ARTS_ERROR, probed
 * through the armed longjmp stub) instead of being read as some other value
 * or leaving a table the master lookup would read past:
 *   - neither node list is set;
 *   - SLURM_NNODES or SLURM_CPUS_PER_TASK is not a positive integer (zero, a
 *     sign, surrounding text, a value past an unsigned int);
 *   - the node list names fewer hosts than SLURM_NNODES.
 * A usable environment builds one row per node in list order, prefers the
 * step's list to the job's, and takes the master from row 0. A count may
 * carry leading zeros and reach the largest unsigned int.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

/* Static: its contents stay defined across the abort stub's longjmp. */
static struct arts_config_s g_cfg;

struct slurm_env {
  const char *nnodes, *cpus, *step, *job;
};

static void put(const char *name, const char *value) {
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
}

static void set_env(struct slurm_env e) {
  put("SLURM_NNODES", e.nnodes);
  put("SLURM_CPUS_PER_TASK", e.cpus);
  put("SLURM_STEP_NODELIST", e.step);
  put("SLURM_JOB_NODELIST", e.job);
}

static void expect_refused(const char *desc, struct slurm_env e) {
  set_env(e);
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_setup_slurm(&g_cfg);
    g_abort_armed = 0;
    fprintf(stderr, "FAIL slurm[%s]: accepted (%u node(s))\n", desc,
            g_cfg.nodes);
    fails++;
    arts_config_destroy(&g_cfg);
    return;
  }
  g_abort_armed = 0;
  if (!g_abort_fired) {
    fprintf(stderr, "FAIL slurm[%s]: longjmp without abort\n", desc);
    fails++;
  }
  arts_config_destroy(&g_cfg);
}

static void expect_table(const char *desc, struct slurm_env e,
                         unsigned int threads, const char **want,
                         unsigned int n) {
  set_env(e);
  memset(&g_cfg, 0, sizeof(g_cfg));
  config_setup_slurm(&g_cfg);
  if (g_cfg.table == NULL || g_cfg.table_length != n || g_cfg.nodes != n) {
    fprintf(stderr, "FAIL slurm[%s]: %u node(s), table_length=%u (want %u)\n",
            desc, g_cfg.nodes, g_cfg.table ? g_cfg.table_length : 0, n);
    fails++;
    arts_config_destroy(&g_cfg);
    return;
  }
  if (g_cfg.thread_count != threads) {
    fprintf(stderr, "FAIL slurm[%s]: thread_count=%u (want %u)\n", desc,
            g_cfg.thread_count, threads);
    fails++;
  }
  for (unsigned int i = 0; i < n; i++) {
    const char *ip = g_cfg.table[i].ip_address;
    if (ip == NULL || strcmp(ip, want[i]) != 0) {
      fprintf(stderr, "FAIL slurm[%s]: row %u is '%s' (want '%s')\n", desc, i,
              ip ? ip : "(null)", want[i]);
      fails++;
    }
  }
  if (g_cfg.master_node == NULL || strcmp(g_cfg.master_node, want[0]) != 0 ||
      g_cfg.master_rank != 0) {
    fprintf(stderr, "FAIL slurm[%s]: master '%s' rank %u (want '%s' rank 0)\n",
            desc, g_cfg.master_node ? g_cfg.master_node : "(null)",
            g_cfg.master_rank, want[0]);
    fails++;
  }
  arts_config_destroy(&g_cfg);
}

int main(void) {
  static const char *const bad_counts[] = {
      "0",  "two", "3x", "-1",         "+3",
      " 3", "3 ",  "",   "4294967296", "99999999999999999999999"};
  for (size_t i = 0; i < sizeof(bad_counts) / sizeof(bad_counts[0]); i++) {
    char desc[64];
    snprintf(desc, sizeof(desc), "SLURM_NNODES='%s'", bad_counts[i]);
    expect_refused(
        desc, (struct slurm_env){.nnodes = bad_counts[i], .job = "n[1-3]"});
    snprintf(desc, sizeof(desc), "SLURM_CPUS_PER_TASK='%s'", bad_counts[i]);
    expect_refused(desc, (struct slurm_env){.nnodes = "3",
                                            .cpus = bad_counts[i],
                                            .job = "n[1-3]"});
  }
  expect_refused("no node list", (struct slurm_env){.nnodes = "2"});
  expect_refused("no node list, no node count", (struct slurm_env){0});
  expect_refused("short job list",
                 (struct slurm_env){.nnodes = "3", .job = "n[1-2]"});
  expect_refused(
      "short step list",
      (struct slurm_env){.nnodes = "3", .step = "n7", .job = "n[1-3]"});

  static const char *three[] = {"n1", "n2", "n5"};
  expect_table("job list", (struct slurm_env){.nnodes = "3", .job = "n[1-2,5]"},
               0, three, 3);
  expect_table(
      "step list over job list",
      (struct slurm_env){.nnodes = "3", .step = "n[1-2,5]", .job = "m[1-3]"}, 0,
      three, 3);
  expect_table(
      "thread count",
      (struct slurm_env){.nnodes = "3", .cpus = "8", .job = "n[1-2,5]"}, 8,
      three, 3);
  expect_table("leading zeros",
               (struct slurm_env){.nnodes = "0003", .job = "n[1-2,5]"}, 0,
               three, 3);
  expect_table("largest thread count",
               (struct slurm_env){
                   .nnodes = "3", .cpus = "4294967295", .job = "n[1-2,5]"},
               4294967295u, three, 3);
  static const char *one[] = {"solo"};
  expect_table("no node count", (struct slurm_env){.job = "solo"}, 0, one, 1);

  set_env((struct slurm_env){0});
  if (fails) {
    fprintf(stderr, "config_slurm_setup: %d failure(s)\n", fails);
    return 1;
  }
  printf("PASS config_slurm_setup: unusable environments refused by name, "
         "usable ones build one row per node\n");
  return 0;
}
