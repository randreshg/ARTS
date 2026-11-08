/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_size_bound — a block longer than ARTS_DB_SIZE_MAX ends the run like a
 * failed allocation: the create refuses it before it mints, sends or
 * allocates anything.  argv[1] picks the length: "over" is the bound plus
 * one, "huge" is the largest uint64_t.  The run ends with status 1.
 */

#include <stdint.h>
#include <string.h>

#include "arts.h"

static uint64_t g_len;

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *p = NULL;
  (void)arts_db_create(&p, g_len, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  arts_printf("db_size_bound: the create returned\n");
  arts_shutdown();
}

int main(int argc, char **argv) {
  g_len = (argc > 1 && strcmp(argv[1], "huge") == 0) ? UINT64_MAX
                                                      : ARTS_DB_SIZE_MAX + 1;
  return arts_rt(argc, argv);
}
