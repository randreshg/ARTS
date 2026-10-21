/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_create_minted_guid_refused — a pre-reserved GUID handed to a create must
 * come from arts_guid_reserve or arts_guid_reserve_range.  A reserved GUID is
 * accepted; a GUID an earlier create minted (it carries a size hint) is
 * refused, and the run aborts with the refusal, which is this test's
 * expected verdict (exit status 1 and the refusal's text).
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_guid_t r = arts_guid_reserve(ARTS_GUID_DB, 0u);
  void *p = arts_db_create_with_guid(r, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p == NULL) {
    arts_printf("FAIL: db_create_minted_guid_refused: a reserved GUID's "
                "create gave no storage\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  void *q = NULL;
  arts_guid_t g = arts_db_create(&q, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  (void)arts_db_create_with_guid(g, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  arts_printf("FAIL: db_create_minted_guid_refused: the minted GUID was "
              "accepted\n");
  arts_test_fail();
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
