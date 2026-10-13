/* SPDX-License-Identifier: Apache-2.0
 *
 * extract_nodelist_lsf on the two edge inputs of its token walk, the launcher
 * setup over the second, and the scheduler setups leaving the environment as
 * they found it.
 *
 * The walk skips the list's first host, appends each later distinct host
 * followed by a ',', and then cuts the trailing ','.
 *   - variable unset: NULL, with the count out-parameter set to 0;
 *   - a single host: nothing is appended, so the result is the empty string
 *     with a count of 0, and the cut must not write the byte before the
 *     buffer (a sanitizer build reports that write; every build checks the
 *     result).
 * A list that leaves no node is refused by the lsf launcher setup (an
 * ARTS_ERROR, probed through the armed longjmp stub) instead of building a
 * table with no row to take the master from.
 *
 * The string getenv returns must not be modified, and every rank, child and
 * later getenv of the process reads it: the lsf setup over LSB_HOSTS and over
 * LSB_MCPU_HOSTS, and the slurm setup over a compressed SLURM_JOB_NODELIST,
 * must each leave its variable byte for byte as it was.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  /* Case 1: variable unset -> NULL and a count of 0.  The sentinel shows
     whether the count was written. */
  unsetenv("LSB_TEST_HOSTS");
  unsigned int cnt = 0xABCDu;
  char *r = extract_nodelist_lsf("LSB_TEST_HOSTS", 1, &cnt);
  if (r != NULL) {
    fprintf(stderr, "FAIL lsf: unset env should return NULL\n");
    return 1;
  }
  if (cnt != 0) {
    fprintf(stderr, "FAIL lsf: unset env should yield count 0, got %u\n", cnt);
    return 1;
  }

  /* Case 2: a single host -> the empty list, count 0. */
  setenv("LSB_TEST_HOSTS", "host0", 1);
  cnt = 0xABCDu;
  char *list = extract_nodelist_lsf("LSB_TEST_HOSTS", 1, &cnt);
  if (list == NULL || list[0] != '\0') {
    fprintf(stderr, "FAIL lsf: single host should yield the empty list\n");
    arts_free(list);
    return 1;
  }
  if (cnt != 0) {
    fprintf(stderr, "FAIL lsf: single host should yield count 0, got %u\n",
            cnt);
    arts_free(list);
    return 1;
  }
  arts_free(list);

  /* The lsf setup over a single-host list: refused. */
  static struct arts_config_s cfg; /* defined across the stub's longjmp */
  memset(&cfg, 0, sizeof(cfg));
  unsetenv("LSB_MCPU_HOSTS");
  setenv("LSB_HOSTS", "host0", 1);
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_setup_lsf(&cfg);
    g_abort_armed = 0;
    fprintf(stderr, "FAIL lsf: a single-host list was accepted (%u node(s))\n",
            cfg.nodes);
    return 1;
  }
  g_abort_armed = 0;
  unsetenv("LSB_HOSTS");
  if (!g_abort_fired) {
    fprintf(stderr, "FAIL lsf: longjmp without abort\n");
    return 1;
  }

  /* The environment is read, never written. */
  static const struct {
    const char *var, *value, *launcher;
  } kept[] = {
      {"LSB_HOSTS", "launch c1 c2", "lsf"},
      {"LSB_MCPU_HOSTS", "launch 4 c1 4 c2 4", "lsf"},
      {"SLURM_JOB_NODELIST", "n[1-2,5]", "slurm"},
  };
  for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
    unsetenv("LSB_HOSTS");
    unsetenv("LSB_MCPU_HOSTS");
    unsetenv("SLURM_STEP_NODELIST");
    setenv("SLURM_NNODES", "3", 1);
    setenv(kept[i].var, kept[i].value, 1);
    memset(&cfg, 0, sizeof(cfg));
    if (strcmp(kept[i].launcher, "lsf") == 0) {
      config_setup_lsf(&cfg);
    } else {
      config_setup_slurm(&cfg);
    }
    const char *now = getenv(kept[i].var);
    if (now == NULL || strcmp(now, kept[i].value) != 0) {
      fprintf(stderr, "FAIL %s: the setup changed %s from '%s' to '%s'\n",
              kept[i].launcher, kept[i].var, kept[i].value,
              now ? now : "(unset)");
      return 1;
    }
    arts_config_destroy(&cfg);
    unsetenv(kept[i].var);
  }
  unsetenv("SLURM_NNODES");

  printf("PASS config_lsf_single_host_oob: no node_list[-1] OOB, cnt=0, "
         "no-node list refused, environment kept\n");
  return 0;
}
