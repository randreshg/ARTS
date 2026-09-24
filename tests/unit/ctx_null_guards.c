/* SPDX-License-Identifier: Apache-2.0
 *
 * T148 — the created-DB list and the owned-finish list on a NULL argument
 * (libs/src/core/edt_context.c).
 *
 *   - arts_owned_finish_register GUARDS NULL: a NULL_GUID registration is a
 *     no-op, and it does not perturb the created-DB list.
 *   - The created-DB list holds exactly one entry per create that took a hold:
 *     each entry is that create's descriptor handle, so the list grows by one
 *     per acquiring create and shrinks by one per explicit release, and the
 *     epilogue finds nothing left.
 *
 * White-box: both lists are the CURRENT worker's thread-locals, read from
 * inside an EDT body.
 */
#include "arts.h"
#include "arts/utils/vector.h"

#include "arts/edt_context.h" /* arts_track_created_db, register, get list */
#include "arts/utils/array_list.h"

#include <stdint.h>

static int g_failed = 0;

void probe(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  /* Baseline: create one real DB so the list exists with a known length. */
  void *p = NULL;
  arts_guid_t db =
      arts_db_create(&p, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (p) {
    ((uint64_t *)p)[0] = 7;
  }
  arts_vector_t *list = arts_get_created_db_list();
  uint64_t base = list ? arts_vector_count(list) : 0;
  if (list == NULL || base == 0) {
    arts_printf("FAIL ctx_null_guards: created_db_list not populated\n");
    g_failed = 1;
    arts_db_release(db, DB_MODE_RW);
    arts_shutdown();
    return;
  }

  uint64_t after_track = base;

  /* register HAS a NULL guard: must be a clean no-op (no crash). The
   * owned_finish_list is file-static and not directly observable; the contract
   * we can pin is that the call returns harmlessly and does not perturb the
   * created_db_list. */
  arts_owned_finish_register(NULL_GUID);
  uint64_t after_reg = arts_vector_count(arts_get_created_db_list());
  if (after_reg != after_track) {
    arts_printf(
        "FAIL ctx_null_guards: register(NULL) perturbed created_db_list "
        "(%llu -> %llu)\n",
        (unsigned long long)after_track, (unsigned long long)after_reg);
    g_failed = 1;
    arts_db_release(db, DB_MODE_RW);
    arts_shutdown();
    return;
  }

  /* A subsequent real DB create still tracks normally (NULL register left the
   * machinery intact). */
  void *p2 = NULL;
  arts_guid_t db2 =
      arts_db_create(&p2, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  uint64_t after_real = arts_vector_count(arts_get_created_db_list());
  if (after_real != after_reg + 1) {
    arts_printf("FAIL ctx_null_guards: real DB create after NULL-guard probes "
                "did not track (%llu -> %llu)\n",
                (unsigned long long)after_reg, (unsigned long long)after_real);
    g_failed = 1;
  }

  /* Explicit releases remove both entries; the epilogue finds nothing. */
  arts_db_release(db, DB_MODE_RW);
  arts_db_release(db2, DB_MODE_RW);
  uint64_t after_release = arts_vector_count(arts_get_created_db_list());
  if (after_release != after_real - 2u) {
    arts_printf("FAIL ctx_null_guards: releases left %llu entries of %llu\n",
                (unsigned long long)after_release,
                (unsigned long long)after_real);
    g_failed = 1;
  }

  if (!g_failed) {
    arts_printf("PASS ctx_null_guards: register(NULL) is a no-op, one list "
                "entry per acquiring create\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(probe, 0, NULL, 0, NULL);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return g_failed;
}
