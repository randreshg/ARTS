/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_config_get_variables on its edge lines (read via fmemopen, so EOF and
 * the missing final newline are exact).
 *
 *   (1) strtok never yields an empty token.  A final line "key=" with no
 *       newline therefore has no value token and is skipped like a line with
 *       no '=', while "empty=\n" has the token "\n" and stores the empty
 *       string.
 *   (2) A line's end, "\n" or "\r\n", is no part of its value: "crlf=local"
 *       written with "\r\n" stores "local", and every blank (space or tab)
 *       is dropped from a key and its value, so "tabbed\t=\tlocal\t" stores
 *       "local" under "tabbed".
 *   (3) A key longer than the 255-byte name field: the name is cut to fit and
 *       stays NUL-terminated inside its field, the node's value is intact, and
 *       the value's terminating NUL lies inside the node's allocation (a
 *       sanitizer build reports a write past it).
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  /* The final line is "key=" with no newline, so it has no value token. */
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
                   "good=val\n"           /* normal: parsed */
                   "noequals\n"           /* skipped (val NULL) */
                   "crlf=local\r\n"       /* CRLF line end */
                   "tabbed\t=\tlocal\t\n" /* blanks around both */
                   "empty=\n"             /* value token "\n": stores "" */
                   "key=");               /* no value token at EOF: skipped */
  (void)n;

  FILE *f = fmemopen(buf, strlen(buf), "r");
  if (!f) {
    fprintf(stderr, "FAIL get_variables: fmemopen failed\n");
    return 1;
  }
  struct arts_config_variable_s *vars = arts_config_get_variables(f);
  (void)fclose(f);

  /* "noequals" has no '=', and the trailing "key=" has no value token. */
  unsigned int count = 0;
  const char *crlf = NULL;
  const char *empty = NULL;
  const char *tabbed = NULL;
  for (struct arts_config_variable_s *p = vars; p; p = p->next) {
    count++;
    if (strcmp(p->variable, "crlf") == 0) {
      crlf = p->value;
    } else if (strcmp(p->variable, "empty") == 0) {
      empty = p->value;
    } else if (strcmp(p->variable, "tabbed") == 0) {
      tabbed = p->value;
    }
  }
  if (count != 4 || crlf == NULL || strcmp(crlf, "local") != 0 ||
      empty == NULL || empty[0] != '\0' || tabbed == NULL ||
      strcmp(tabbed, "local") != 0) {
    fprintf(stderr,
            "FAIL get_variables: expected good, crlf=\"local\", empty=\"\" "
            "and tabbed=\"local\" (4 nodes), got %u node(s)%s%s%s\n",
            count,
            crlf && strcmp(crlf, "local") != 0 ? ", crlf kept its \\r" : "",
            empty && empty[0] ? ", empty not empty" : "",
            tabbed == NULL ? ", no key \"tabbed\" (a tab kept in it)"
            : strcmp(tabbed, "local") != 0 ? ", tabbed kept a tab"
                                           : "");
    config_free_variables(vars);
    return 1;
  }
  config_free_variables(vars);

  /* A 300-character key. */
  char big[512];
  memset(big, 'k', 300);
  big[300] = '=';
  big[301] = 'v';
  big[302] = '\n';
  big[303] = '\0';
  FILE *f2 = fmemopen(big, strlen(big), "r");
  if (!f2) {
    fprintf(stderr, "FAIL get_variables: fmemopen(big) failed\n");
    return 1;
  }
  struct arts_config_variable_s *vars2 = arts_config_get_variables(f2);
  (void)fclose(f2);
  if (vars2 == NULL || vars2->next != NULL) {
    fprintf(stderr, "FAIL get_variables: long key should yield one node\n");
    config_free_variables(vars2);
    return 1;
  }
  if (strnlen(vars2->variable, sizeof(vars2->variable)) !=
          sizeof(vars2->variable) - 1 ||
      strcmp(vars2->value, "v") != 0) {
    fprintf(stderr,
            "FAIL get_variables: long key not cut to %zu "
            "terminated characters, or value not \"v\"\n",
            sizeof(vars2->variable) - 1);
    config_free_variables(vars2);
    return 1;
  }
  config_free_variables(vars2);

  printf("PASS config_get_variables_oob: empty value, CRLF line end, blanks "
         "and long key handled\n");
  return 0;
}
