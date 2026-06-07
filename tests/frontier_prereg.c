/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file frontier_prereg.c
/// @brief Unit checks for owner-frontier pre-registration de-duplication.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "arts/gas/guid.h"
#include "arts/memory/frontier.h"
#include "arts/runtime_types.h"
#include "arts/utils/malloc.h"

static bool require_true(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    return false;
  }
  return true;
}

static unsigned int frontier_count(struct arts_db_list_s *list) {
  unsigned int count = 0;
  for (struct arts_db_frontier_s *frontier = list->head; frontier;
       frontier = frontier->next) {
    count++;
  }
  return count;
}

static struct arts_db_s *new_db(arts_guid_t guid) {
  struct arts_db_s *db =
      (struct arts_db_s *)arts_calloc(1, sizeof(*db) + sizeof(uint64_t));
  if (!db) {
    return NULL;
  }
  memset(db, 0, sizeof(*db));
  db->header.type = ARTS_DB;
  db->header.size = sizeof(*db) + sizeof(uint64_t);
  db->guid = guid;
  db->db_type = ARTS_DB_DEFAULT;
  db->db_list = arts_new_db_list();
  return db;
}

static void free_db(struct arts_db_s *db) {
  arts_delete_db_list((struct arts_db_list_s *)db->db_list);
  arts_free(db);
}

int main(void) {
  int ok = 1;

  struct arts_db_s *db = new_db(ARTS_GUID_MAKE(ARTS_DB, 0, 1));
  arts_guid_t edt_a = ARTS_GUID_MAKE(ARTS_EDT, 1, 10);
  arts_guid_t edt_b = ARTS_GUID_MAKE(ARTS_EDT, 1, 11);

  ok &= require_true(db != NULL, "DB allocation failed");
  if (!db) {
    return 1;
  }
  struct arts_db_list_s *list = (struct arts_db_list_s *)db->db_list;
  list->head->exEdtGuid = ARTS_GUID_MAKE(ARTS_EDT, 0, 99);
  ok &= require_true(arts_register_remote_ro_reader(db, 1, edt_a, 0),
                     "remote RO pre-registration failed");
  ok &= require_true(arts_register_remote_ro_reader(db, 1, edt_a, 0),
                     "duplicate remote RO pre-registration failed");
  ok &= require_true(arts_register_remote_ro_reader(db, 1, edt_b, 0),
                     "second remote RO pre-registration failed");

  struct arts_db_frontier_s *remote_ro_frontier = list->head->next;
  ok &= require_true(remote_ro_frontier != NULL,
                     "remote RO reader frontier was not appended");
  ok &= require_true(remote_ro_frontier->roReadersCount == 2,
                     "exact duplicate remote RO collapsed incorrectly");
  ok &= require_true(frontier_count(list) == 2,
                     "remote RO readers should share one appended frontier");
  ok &= require_true(
      arts_remote_ro_reader_preregistered_exact(db, 1, edt_a, 0),
      "exact remote RO lookup missed registered reader");
  ok &= require_true(
      !arts_remote_ro_reader_preregistered_exact(db, 1, edt_a, 1),
      "exact remote RO lookup ignored slot");
  free_db(db);

  db = new_db(ARTS_GUID_MAKE(ARTS_DB, 0, 2));
  arts_guid_t edt_local = ARTS_GUID_MAKE(ARTS_EDT, 0, 20);
  ok &= require_true(db != NULL, "DB allocation failed");
  if (!db) {
    return 1;
  }
  ok &= require_true(arts_register_local_ro_reader(db, edt_local, 0),
                     "local RO pre-registration failed");
  ok &= require_true(arts_register_local_ro_reader(db, edt_local, 0),
                     "duplicate local RO pre-registration failed");
  ok &= require_true(arts_register_local_ro_reader(db, edt_local, 1),
                     "distinct local RO slot pre-registration failed");
  list = (struct arts_db_list_s *)db->db_list;
  ok &= require_true(list->head->localRoReadersPos == 2,
                     "local RO duplicate handling lost distinct slot fanout");
  free_db(db);

  db = new_db(ARTS_GUID_MAKE(ARTS_DB, 0, 3));
  ok &= require_true(db != NULL, "DB allocation failed");
  if (!db) {
    return 1;
  }
  ok &= require_true(
      arts_register_remote_ew_writer(db, 1, edt_a, 0, DB_MODE_EW),
      "remote EW pre-registration failed");
  ok &= require_true(
      arts_register_remote_ew_writer(db, 1, edt_a, 0, DB_MODE_EW),
      "duplicate remote EW pre-registration failed");
  ok &= require_true(
      arts_register_remote_ew_writer(db, 1, edt_a, 1, DB_MODE_EW),
      "distinct remote EW slot pre-registration failed");
  list = (struct arts_db_list_s *)db->db_list;
  ok &= require_true(frontier_count(list) == 2,
                     "remote EW duplicate handling ignored slot identity");
  free_db(db);

  return ok ? 0 : 1;
}
