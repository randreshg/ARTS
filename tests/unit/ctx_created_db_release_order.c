/* SPDX-License-Identifier: Apache-2.0
 *
 * An EDT's thread-local context: the created-DB list
 * (libs/src/core/edt_context.c).
 *
 * Pinned properties (white-box reads on the running worker thread):
 *
 *   1. Footprint: the first DB created on a worker lazily allocates
 *      created_db_list with element_size == sizeof(arts_shared_ptr_t), and the
 *      block GROWS to hold what was pushed — NDB exceeds the small initial
 *      capacity, so the doubling is actually exercised, not just permitted.
 *
 *   2. One entry per hold: the list grows by one per acquiring create and
 *      shrinks by one per explicit release, and registering a NULL_GUID owned
 *      finish scope is a no-op that leaves it alone.
 *
 *   3. Drain: a successor EDT on the SAME worker observes a list holding only
 *      its own creations — the producer's entries are gone — and reads, through
 *      RO dependences, the values the producer wrote before releasing.  A
 *      release that never happened would leave those acquires ungranted
 *      (ctest TIMEOUT).
 */
#include "arts.h"
#include "arts/utils/vector.h"

#include "arts/edt_context.h" /* created-DB list, owned finish list */

#include <stdint.h>

/* Exceeds the container's initial capacity (8) so growth is exercised. */
#define NDB 12

static int g_failed = 0;

static void fail(const char *what) {
  arts_printf("FAIL ctx_created_db_release_order: %s\n", what);
  g_failed = 1;
  arts_shutdown();
}

/* NDB RO deps on the producer's blocks; runs after the producer's epilogue. */
void successor(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;

  for (int i = 0; i < NDB; i++) {
    uint64_t *p = (uint64_t *)depv[i].ptr;
    if (p == NULL || p[0] != (uint64_t)(0xD00D0000 + i)) {
      arts_printf("  dependence %d: ptr=%p val=%llu\n", i, (void *)p,
                  (unsigned long long)(p ? p[0] : 0));
      fail("a successor read the wrong value");
      return;
    }
  }

  void *sp = NULL;
  arts_guid_t sdb =
      arts_db_create(&sp, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  if (sp) {
    ((uint64_t *)sp)[0] = 0;
  }
  arts_vector_t *list = arts_get_created_db_list();
  uint64_t len = list ? arts_vector_count(list) : 0;
  arts_db_release(sdb, DB_MODE_RW);
  if (len != 1) {
    arts_printf("  successor's list length %llu, want 1\n",
                (unsigned long long)len);
    fail("the producer's entries outlived its epilogue");
    return;
  }

  arts_printf("PASS ctx_created_db_release_order: grow-to-fit footprint, one "
              "entry per hold, drained before the next EDT (%d DBs)\n",
              NDB);
  arts_shutdown();
}

/* Creates NDB blocks, writes a sentinel into each, releases them so the
 * successor's RO dependences can be granted, and hands them on. */
void producer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_guid_t dbs[NDB];
  for (int i = 0; i < NDB; i++) {
    void *p = NULL;
    dbs[i] =
        arts_db_create(&p, sizeof(uint64_t), ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    if (p) {
      ((uint64_t *)p)[0] = (uint64_t)(0xD00D0000 + i);
    }
  }

  arts_vector_t *list = arts_get_created_db_list();
  if (list->data == NULL || list->element_size != sizeof(arts_shared_ptr_t) ||
      list->capacity < (uint64_t)NDB) {
    arts_printf("  data=%p element_size=%zu capacity=%llu, want non-NULL, "
                "%zu, >= %d\n",
                list->data, list->element_size,
                (unsigned long long)list->capacity, sizeof(arts_shared_ptr_t),
                NDB);
    fail("the list's footprint");
    return;
  }
  if (arts_vector_count(list) != (uint64_t)NDB) {
    fail("the list does not hold one entry per acquiring create");
    return;
  }

  arts_owned_finish_register(NULL_GUID);
  if (arts_vector_count(list) != (uint64_t)NDB) {
    fail("registering a NULL owned finish scope changed the list");
    return;
  }

  for (int i = 0; i < NDB; i++) {
    arts_db_release(dbs[i], DB_MODE_RW);
    if (arts_vector_count(list) != (uint64_t)(NDB - 1 - i)) {
      fail("a release did not remove exactly its own entry");
      return;
    }
  }

  arts_edt_hint_t sh = ARTS_EDT_HINT_DEFAULTS;
  arts_guid_t s = arts_edt_create(successor, 0, NULL, NDB, &sh);
  for (int i = 0; i < NDB; i++) {
    arts_add_dependence(dbs[i], s, (uint32_t)i, DB_MODE_RO);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(producer, 0, NULL, 0, NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : g_failed;
}
