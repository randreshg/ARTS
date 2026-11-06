/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file quiescence_walk_excludes_caller.c
/// @brief The EDTs that call arts_shutdown, and the blocks they hold, are
///        exempt from the teardown's quiescence walk.
///
/// A caller of arts_shutdown never returns from its body, so it never
/// finishes and never releases what it holds; the walk counts neither it as
/// admitted work nor its holds as residue, and names the exempt blocks in
/// one CALLER-INFO line.
///   argv[1] "one" (default): main_edt creates a block (its creator hold is a
///     write acquisition) and calls arts_shutdown.
///   argv[1] "two": two EDTs, each holding a block of its own through a
///     write dependence, call arts_shutdown at the same moment; one takes the
///     shutdown point and the other enters after it and parks.
///   argv[1] "released": one EDT with write dependences on two blocks
///     releases the first in its body and calls arts_shutdown holding the
///     second: only the second is exempt.
/// Each shutdown leaves no other work, so it is quiescent.

#include "arts.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "../test_failure_status.h"

/* Both callers' arrivals, so neither calls before the other holds its block.
 * Only its own value. */
static _Atomic(unsigned int) g_arrived;

void caller(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc != 1 || depv[0].ptr == NULL) {
    arts_printf("  FAIL: the caller holds no block\n");
    arts_test_fail();
  } else {
    *(unsigned int *)depv[0].ptr = 1u;
  }
  atomic_fetch_add_explicit(&g_arrived, 1u, memory_order_relaxed);
  while (atomic_load_explicit(&g_arrived, memory_order_relaxed) < 2u) {
  }
  arts_shutdown();
}

void releaser(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  if (depc != 2 || depv[0].ptr == NULL || depv[1].ptr == NULL) {
    arts_printf("  FAIL: the releaser holds no blocks\n");
    arts_test_fail();
    arts_shutdown();
  }
  *(unsigned int *)depv[0].ptr = 1u;
  *(unsigned int *)depv[1].ptr = 2u;
  arts_db_release(depv[0].guid);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  arts_db_hint_t hint = ARTS_DB_HINT_DEFAULTS;
  if (argc > 1 && strcmp(argv[1], "released") == 0) {
    arts_printf("WALK_EXCLUDES_CALLER: the caller released one block\n");
    arts_guid_t r =
        arts_edt_create(releaser, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0});
    for (uint32_t i = 0; i < 2; i++) {
      void *p = NULL;
      arts_guid_t db = arts_db_create(&p, sizeof(unsigned int), ARTS_DB,
                                      ARTS_DB_PROP_NO_ACQUIRE, &hint);
      arts_add_dependence(db, r, i, DB_MODE_RW);
    }
    return;
  }
  if (argc > 1 && strcmp(argv[1], "two") == 0) {
    arts_printf("WALK_EXCLUDES_CALLER: two callers\n");
    for (int i = 0; i < 2; i++) {
      void *p = NULL;
      arts_guid_t db = arts_db_create(&p, sizeof(unsigned int), ARTS_DB,
                                      ARTS_DB_PROP_NO_ACQUIRE, &hint);
      arts_guid_t c =
          arts_edt_create(caller, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
      arts_add_dependence(db, c, 0, DB_MODE_RW);
    }
    return;
  }
  arts_printf("WALK_EXCLUDES_CALLER: main_edt is the caller\n");
  unsigned int *p = NULL;
  (void)arts_db_create((void **)&p, sizeof(*p), ARTS_DB, ARTS_DB_PROP_NONE,
                       &hint);
  *p = 1u;
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
