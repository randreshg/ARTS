/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_config_find_variable: an environment variable overrides a cfg key.
 *
 * The lookup builds a node for the variable and splices it into the list: in
 * place of a node of that name, or appended when the list holds none.  This
 * test takes the append into a non-empty list ("alpha", looking up "beta"):
 * the appended node must end the list (its link NULL, so the walk below stops
 * after two nodes), and its value, terminating NUL included, must lie inside
 * its allocation (a sanitizer build reports a write past it).
 */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build a single list node with the given name/value via the same allocation
   discipline config_get_variables uses, so the list is well-formed input. */
static struct arts_config_variable_s *make_node(const char *name,
                                                const char *value) {
  unsigned int size = (unsigned int)strlen(value);
  struct arts_config_variable_s *v =
      (struct arts_config_variable_s *)arts_malloc(
          sizeof(struct arts_config_variable_s) + size + 1);
  v->size = size;
  v->next = NULL;
  strncpy(v->variable, name, 254);
  v->variable[254] = '\0';
  memcpy(v->value, value, size + 1);
  return v;
}

int main(void) {
  /* One node, "alpha"; the environment supplies "beta", which the list does
     not hold, so the override is appended after alpha. */
  struct arts_config_variable_s *head = make_node("alpha", "1");

  setenv("beta", "value_of_beta", 1);

  struct arts_config_variable_s *res = arts_config_find_variable(&head, "beta");
  if (res == NULL) {
    fprintf(stderr, "FAIL find_variable: override should return new node\n");
    return 1;
  }
  if (strcmp(res->value, "value_of_beta") != 0) {
    fprintf(stderr, "FAIL find_variable: wrong value '%s'\n", res->value);
    return 1;
  }

  /* The appended node is the tail after alpha: the walk must end there. */
  unsigned int count = 0;
  for (struct arts_config_variable_s *p = head; p != NULL; p = p->next) {
    /* Touch each node's name to force a real read of the (possibly wild)
       pointer chain. */
    volatile char c = p->variable[0];
    (void)c;
    if (++count > 16) {
      fprintf(stderr, "FAIL find_variable: list not terminated (cycle/wild)\n");
      return 1;
    }
  }
  if (count != 2) {
    fprintf(stderr, "FAIL find_variable: expected 2 nodes, walked %u\n", count);
    return 1;
  }

  /* Free the list. */
  config_free_variables(head);

  printf("PASS config_find_variable: override splice well-formed (%u nodes)\n",
         count);
  return 0;
}
