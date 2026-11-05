/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* app_status_reaches_exit — a verdict a task records on the last rank is that
 * rank's exit status, returned by arts_rt through arts_app_exit_status, with
 * no check in main.  The run is expected to end with status 1.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

static void fail_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("APP_STATUS_RECORDED app_status_reaches_exit\n");
  arts_test_fail();
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_hint_t hint = {.rank = arts_get_total_ranks() - 1u};
  (void)arts_edt_create(fail_edt, 0, NULL, 0, &hint);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
