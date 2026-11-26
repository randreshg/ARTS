/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* guid_reserve_refused — a GUID is reserved, and a range is read, for EDTs
 * and events only.  A data block's GUID is issued by its create, so
 * arts_guid_reserve and arts_guid_reserve_range refuse a data-block kind,
 * ARTS_GUID_RESERVED and a value that is no kind, and arts_guid_from_index
 * and arts_guid_index_from refuse a range that is a data block's name or
 * NULL_GUID.  Each refusal ends the run with status 1 and its message, which
 * is this test's expected verdict.  A call that is let through, or a case
 * name the program does not know, prints a FAIL line and the run ends 0: the
 * status is then not the refusal's.
 *
 * argv[1] names the case: <db|reserved|nokind>_<one|range> for the reserve
 * entries, <db|null>_<index|index_from> for the index entries.
 */

#include <stdint.h>
#include <string.h>

#include "arts.h"

static void let_through(const char *name) {
  arts_printf("FAIL: guid_reserve_refused: %s was let through\n", name);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  const char *name = argc > 1 ? argv[1] : "";

  if (strcmp(name, "db_one") == 0) {
    (void)arts_guid_reserve(ARTS_GUID_DB, 0u);
  } else if (strcmp(name, "reserved_one") == 0) {
    (void)arts_guid_reserve(ARTS_GUID_RESERVED, 0u);
  } else if (strcmp(name, "nokind_one") == 0) {
    (void)arts_guid_reserve(ARTS_GUID_LAST, 0u);
  } else if (strcmp(name, "db_range") == 0) {
    (void)arts_guid_reserve_range(ARTS_GUID_DB, 4u, 0u);
  } else if (strcmp(name, "reserved_range") == 0) {
    (void)arts_guid_reserve_range(ARTS_GUID_RESERVED, 4u, 0u);
  } else if (strcmp(name, "nokind_range") == 0) {
    (void)arts_guid_reserve_range(ARTS_GUID_LAST, 4u, 0u);
  } else if (strcmp(name, "db_index") == 0 ||
             strcmp(name, "db_index_from") == 0) {
    void *p = NULL;
    arts_guid_t db = arts_db_create(&p, 64u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    if (strcmp(name, "db_index") == 0) {
      (void)arts_guid_from_index(db, 0u);
    } else {
      (void)arts_guid_index_from(db, arts_guid_reserve(ARTS_GUID_EVENT, 0u));
    }
  } else if (strcmp(name, "null_index") == 0) {
    (void)arts_guid_from_index(NULL_GUID, 0u);
  } else if (strcmp(name, "null_index_from") == 0) {
    (void)arts_guid_index_from(NULL_GUID,
                               arts_guid_reserve(ARTS_GUID_EVENT, 0u));
  } else {
    arts_printf("FAIL: guid_reserve_refused: unknown case '%s'\n", name);
    arts_shutdown();
    return;
  }
  let_through(name);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
