/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_config_count_nodes, its use at load, and the ssh launcher's
 * node_count agreement (config.c).
 *
 * Property: arts_config_count_nodes() returns the number of physical nodes a
 * `nodes` value expands to:
 *   "a"                    -> 1
 *   "a,b,c"                -> 3
 *   "node[01-10]"          -> 10  (bracket range)
 *   "a,b[1-3],c"           -> 5   (mixed)
 *   "node[10-01]" reversed -> 10  (abs diff)
 *   "node[5]"              -> 1   (single number)
 * and refuses at load (ARTS_ERROR) every value the routing-table parser would
 * read differently: an entry holding ':' ("node[1-5]:[50000-50004]", "a:22"), an
 * unbalanced '[', bracket content too long for the parser's buffer, a bracket
 * that is not digits with at most one '-', text after ']', empty entries, a
 * range bound above UINT_MAX and a total that does not fit an unsigned int.
 * A malformed value is refused at load whatever the launcher (here local, with
 * a node_count, which places no rank by the list).  Under launcher=ssh a
 * node_count that disagrees with the list is refused too, since the table is
 * sized by one and filled from the other; no nodes line lists localhost.
 *
 * The ARTS_ERROR death paths go through the armed longjmp stub in
 * config_test_common.h.
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void expect(const char *literal, unsigned int want) {
  char buf[256];
  strncpy(buf, literal, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  unsigned int got = arts_config_count_nodes(buf);
  if (got != want) {
    fprintf(stderr, "FAIL count_nodes(\"%s\"): got %u want %u\n", literal, got,
            want);
    fails++;
  }
}

/* Static: its contents stay defined across the abort stub's longjmp. */
static char g_buf[256];

static void expect_refused(const char *literal) {
  strncpy(g_buf, literal, sizeof(g_buf) - 1);
  g_buf[sizeof(g_buf) - 1] = '\0';
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    unsigned int got = arts_config_count_nodes(g_buf);
    g_abort_armed = 0;
    fprintf(stderr, "FAIL count_nodes(\"%s\"): accepted as %u node(s)\n",
            literal, got);
    fails++;
    return;
  }
  g_abort_armed = 0;
  if (!g_abort_fired) {
    fprintf(stderr, "FAIL count_nodes(\"%s\"): longjmp without abort\n",
            literal);
    fails++;
  }
}

static struct arts_config_s g_cfg;
static struct arts_config_variable_s *g_vars;

/* Drive the launcher setup over a cfg text; returns 1 when it aborted. */
static int setup_aborts(const char *launcher, const char *cfg_text) {
  char text[256];
  strncpy(text, cfg_text, sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
  FILE *f = fmemopen(text, strlen(text), "r");
  g_vars = arts_config_get_variables(f);
  (void)fclose(f);
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.launcher = arts_config_make_new_var(launcher);
  int aborted = 0;
  g_abort_fired = 0;
  g_abort_armed = 1;
  if (setjmp(g_abort_jmp) == 0) {
    config_setup_launcher(&g_cfg, &g_vars);
  } else {
    aborted = g_abort_fired;
  }
  g_abort_armed = 0;
  arts_config_destroy(&g_cfg);
  config_free_variables(g_vars);
  g_vars = NULL;
  return aborted;
}

int main(void) {
  /* Well-formed grammar. */
  expect("a", 1);
  expect("a,b,c", 3);
  expect("node[01-10]", 10);
  expect("node[1-5]", 5);
  expect("a,b[1-3],c", 5);   /* 1 + 3 + 1 */
  expect("node[10-01]", 10); /* reversed handled by abs diff */
  expect("node[5-5]", 1);    /* single-element range */
  expect("node[5]", 1);      /* single number */

  /* Malformed: refused. */
  expect_refused("node[1-5]:[50000-50004]");
  expect_refused("a:22");
  expect_refused("node[1-5");
  expect_refused("node[1-5]x");
  expect_refused("node[a-b]");
  expect_refused("node[1-2-3]");
  expect_refused("node[]");
  expect_refused("a,,b");
  expect_refused("a,");
  {
    /* Bracket content of 80 characters: longer than the parser's buffer. */
    char big[160];
    strcpy(big, "node[");
    for (int i = 0; i < 80; i++) {
      strcat(big, "1");
    }
    strcat(big, "]");
    expect_refused(big);
  }
  expect_refused("node[1-4294967296]");
  expect_refused("a[0-4294967294],b[0-4294967294]");

  /* Refused at load whatever the launcher: launcher=local with a node_count
     places no rank by the list, and still refuses a malformed one. */
  if (!setup_aborts("local", "nodes=a:1,b:2\nnode_count=2\n")) {
    fprintf(stderr, "FAIL local: a malformed nodes list was accepted\n");
    fails++;
  }
  if (setup_aborts("local", "nodes=a,b\nnode_count=2\n")) {
    fprintf(stderr, "FAIL local: a well-formed nodes list was refused\n");
    fails++;
  }

  /* launcher=ssh: node_count must agree with the list (an agreeing pair is
     the full load config_destroy_leak drives). */
  if (!setup_aborts("ssh", "nodes=a,b,c\nnode_count=2\n")) {
    fprintf(stderr, "FAIL ssh: node_count=2 with three hosts was accepted\n");
    fails++;
  }
  if (!setup_aborts("ssh", "nodes=a,b\nnode_count=3\n")) {
    fprintf(stderr, "FAIL ssh: node_count=3 with two hosts was accepted\n");
    fails++;
  }
  if (!setup_aborts("ssh", "node_count=4\n")) {
    fprintf(stderr, "FAIL ssh: node_count=4 with no nodes list was "
                    "accepted\n");
    fails++;
  }

  if (fails) {
    fprintf(stderr, "FAIL config_count_nodes: %d checks failed\n", fails);
    return 1;
  }
  printf("PASS config_count_nodes: ranges + mixed counted; malformed lists "
         "and node_count disagreements refused\n");
  return 0;
}
