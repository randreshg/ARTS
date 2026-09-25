/* SPDX-License-Identifier: Apache-2.0
 *
 * The cxl_db_allocation_strategy cfg handler resolves three cases:
 *   a value starting with "round_robin" -> ROUND_ROBIN
 *   any other value                     -> STATIC, device read from
 *                                          cxl_db_allocation_device
 *   no value                            -> STATIC, device 0
 * The handler and the config fields it sets exist only in a CXL tree.
 */

#ifndef ARTS_USE_CXL

#include <stdio.h>
int main(void) {
  printf("SKIP config_cxl_alloc_strategy: requires ARTS_USE_CXL build\n");
  return 0;
}

#else /* ARTS_USE_CXL */

#include "../../libs/src/core/system/config.c"
#include "config_test_common.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

/* Build a one-node vars list "cxl_db_allocation_device=<dev>". */
static struct arts_config_variable_s *dev_vars(const char *dev_str) {
  unsigned int size = (unsigned int)strlen(dev_str);
  struct arts_config_variable_s *v =
      (struct arts_config_variable_s *)arts_malloc(
          sizeof(struct arts_config_variable_s) + size + 1);
  v->size = size;
  v->next = NULL;
  strncpy(v->variable, "cxl_db_allocation_device", 254);
  v->variable[254] = '\0';
  memcpy(v->value, dev_str, size + 1);
  return v;
}

int main(void) {
  /* round_robin. */
  {
    struct arts_config_s c;
    memset(&c, 0, sizeof(c));
    struct arts_config_variable_s *vars = NULL;
    handle_cxl_db_allocation_strategy(&c, "round_robin", &vars);
    if (c.cxl_db_allocation_strategy != ARTS_CXL_DB_ALLOC_ROUND_ROBIN) {
      fprintf(stderr, "FAIL cxl: round_robin -> strategy %d\n",
              (int)c.cxl_db_allocation_strategy);
      fails++;
    }
    config_free_variables(vars);
  }

  /* static + device=3. */
  {
    struct arts_config_s c;
    memset(&c, 0, sizeof(c));
    struct arts_config_variable_s *vars = dev_vars("3");
    handle_cxl_db_allocation_strategy(&c, "static", &vars);
    if (c.cxl_db_allocation_strategy != ARTS_CXL_DB_ALLOC_STATIC ||
        c.cxl_db_allocation_device != 3) {
      fprintf(stderr, "FAIL cxl: static dev3 -> strategy %d dev %u\n",
              (int)c.cxl_db_allocation_strategy, c.cxl_db_allocation_device);
      fails++;
    }
    config_free_variables(vars);
  }

  /* NULL value -> STATIC, device 0. */
  {
    struct arts_config_s c;
    memset(&c, 0, sizeof(c));
    struct arts_config_variable_s *vars = NULL;
    handle_cxl_db_allocation_strategy(&c, NULL, &vars);
    if (c.cxl_db_allocation_strategy != ARTS_CXL_DB_ALLOC_STATIC ||
        c.cxl_db_allocation_device != 0) {
      fprintf(stderr, "FAIL cxl: NULL -> strategy %d dev %u (want STATIC,0)\n",
              (int)c.cxl_db_allocation_strategy, c.cxl_db_allocation_device);
      fails++;
    }
    config_free_variables(vars);
  }

  if (fails) {
    return 1;
  }
  printf("PASS config_cxl_alloc_strategy: round_robin / static+dev3 / "
         "NULL->static,0\n");
  return 0;
}

#endif /* ARTS_USE_CXL */
