/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_dep_sort_and_classify — the acquire engine's one alias rule, driven
 * directly on synthetic dependence vectors.
 *
 * An EDT acquires each distinct block ONCE.  The engine decides that before
 * anything fires: it orders the deps by GUID (so same-block slots are adjacent
 * and a serialized walk takes blocks in one global order) with the strongest
 * mode first inside a block and ties keeping slot order, then names the first
 * slot of each block's group the OWNER of that block's single acquisition and
 * every later slot naming it an ALIAS.  Slots that acquire nothing — a NULL
 * GUID, a value slot (DB_MODE_NULL), a non-DB kind — are neither, and must come
 * back exactly as they went in.
 *
 * The function is pure: no runtime state, no allocation, no dispatch.  So this
 * test links the real symbol out of the per-config static libarts and calls it
 * on stack arrays; the runtime is never started, and the answers below are the
 * same in every build configuration.
 */

#include "arts.h" /* arts_edt_dep_t, DB_MODE_*, NULL_GUID */
#include "arts/gas/guid.h" /* ARTS_GUID_MAKE */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Defined in db.c, declared in the internal db.h; re-declared here to keep the
 * test header-light (the internal header drags the runtime's type graph in). */
void arts_dep_sort_and_classify(arts_edt_dep_t *depv, uint32_t depc,
                                uint32_t *sorted);

#define MAX_DEPS 8u

static int failures;

static arts_guid_t db_guid(uint64_t key) {
  return ARTS_GUID_MAKE(ARTS_GUID_DB, 0u, key);
}

static arts_guid_t event_guid(uint64_t key) {
  return ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0u, key);
}

/* Run one case and compare both answers: the visit order and, per slot, the
 * owner/alias verdict.  `want_alias` uses -1 for "the classifier must not touch
 * this slot" — the slot goes in pre-set to the opposite of what a classified
 * slot would get, so a stray write shows up. */
static void check(const char *name, arts_edt_dep_t *depv, uint32_t depc,
                  const uint32_t *want_sorted, const int *want_alias) {
  uint32_t got[MAX_DEPS];
  bool before[MAX_DEPS];
  for (uint32_t i = 0; i < depc; i++) {
    before[i] = depv[i].alias;
  }
  arts_dep_sort_and_classify(depv, depc, got);
  for (uint32_t k = 0; k < depc; k++) {
    if (got[k] != want_sorted[k]) {
      (void)fprintf(stderr,
                    "FAIL dep_alias_classify[%s]: order[%u] got %u want %u\n",
                    name, k, got[k], want_sorted[k]);
      failures++;
    }
  }
  for (uint32_t i = 0; i < depc; i++) {
    bool want = (want_alias[i] < 0) ? before[i] : (want_alias[i] != 0);
    if (depv[i].alias != want) {
      (void)fprintf(stderr,
                    "FAIL dep_alias_classify[%s]: slot %u alias=%d want %d%s\n",
                    name, i, (int)depv[i].alias, (int)want,
                    want_alias[i] < 0 ? " (must be untouched)" : "");
      failures++;
    }
  }
}

int main(void) {
  const arts_guid_t g1 = db_guid(0x11u);
  const arts_guid_t g2 = db_guid(0x22u); /* g1 < g2: same kind and rank */

  /* Two write slots on one block: the first is the acquisition, the second
   * aliases it.  Order is already slot order — the tie keeps it. */
  {
    arts_edt_dep_t d[2];
    memset(d, 0, sizeof(d));
    d[0].guid = g1;
    d[0].mode = DB_MODE_RW;
    d[1].guid = g1;
    d[1].mode = DB_MODE_RW;
    const uint32_t order[2] = {0u, 1u};
    const int alias[2] = {0, 1};
    check("rw+rw", d, 2u, order, alias);
  }

  /* Declared read first, written second.  The WRITE must own the acquisition
   * whatever its slot index: a write released as a read is not exclusive, and
   * where the release is what carries the bytes home it is dropped. */
  {
    arts_edt_dep_t d[2];
    memset(d, 0, sizeof(d));
    d[0].guid = g1;
    d[0].mode = DB_MODE_RO;
    d[1].guid = g1;
    d[1].mode = DB_MODE_RW;
    const uint32_t order[2] = {1u, 0u};
    const int alias[2] = {1, 0};
    check("ro+rw", d, 2u, order, alias);
  }

  /* Read, write, read: one owner in the middle, two aliases, and the two reads
   * keep their slot order behind it. */
  {
    arts_edt_dep_t d[3];
    memset(d, 0, sizeof(d));
    d[0].guid = g1;
    d[0].mode = DB_MODE_RO;
    d[1].guid = g1;
    d[1].mode = DB_MODE_RW;
    d[2].guid = g1;
    d[2].mode = DB_MODE_RO;
    const uint32_t order[3] = {1u, 0u, 2u};
    const int alias[3] = {1, 0, 1};
    check("ro+rw+ro", d, 3u, order, alias);
  }

  /* Two blocks interleaved: the sort gathers each block's slots, and each block
   * gets its own owner — a block's classification never depends on another's. */
  {
    arts_edt_dep_t d[4];
    memset(d, 0, sizeof(d));
    d[0].guid = g1;
    d[0].mode = DB_MODE_RO;
    d[1].guid = g2;
    d[1].mode = DB_MODE_RW;
    d[2].guid = g1;
    d[2].mode = DB_MODE_RW;
    d[3].guid = g2;
    d[3].mode = DB_MODE_RO;
    const uint32_t order[4] = {2u, 0u, 1u, 3u};
    const int alias[4] = {1, 0, 0, 1};
    check("two blocks", d, 4u, order, alias);
  }

  /* A NULL GUID, a value slot naming a block, and an event slot acquire
   * nothing: they are neither owner nor alias, and a value slot naming a block
   * does not open that block's group either — the write below is still the
   * block's first acquiring slot, hence its owner.
   *
   * The order is by GUID as a signed value (arts_guid_t is intptr_t), so a kind
   * whose tag sets the sign bit — EVENT here — sorts ahead of NULL and of every
   * DB GUID.  Only DB GUIDs are ever acquired and their tag leaves the sign
   * clear, so the slots that matter are still in ascending GUID order. */
  {
    arts_edt_dep_t d[5];
    memset(d, 0, sizeof(d));
    d[0].guid = NULL_GUID;
    d[0].mode = DB_MODE_RO;
    d[0].alias = true;
    d[1].guid = g1;
    d[1].mode = DB_MODE_NULL;
    d[1].alias = true;
    d[2].guid = event_guid(0x33u);
    d[2].mode = DB_MODE_RO;
    d[2].alias = true;
    d[3].guid = g1;
    d[3].mode = DB_MODE_RW;
    d[3].alias = true;
    d[4].guid = g1;
    d[4].mode = DB_MODE_RO;
    const uint32_t order[5] = {2u, 0u, 3u, 4u, 1u};
    const int alias[5] = {-1, -1, -1, 0, 1};
    check("unacquired slots", d, 5u, order, alias);
  }

  /* A slot already carrying a payload is resolved, so it is not part of the
   * acquisition the engine is about to make — and the next slot naming the
   * block is that acquisition's owner, not an alias of a resolved slot. */
  {
    arts_edt_dep_t d[2];
    unsigned int storage = 0u;
    memset(d, 0, sizeof(d));
    d[0].guid = g1;
    d[0].mode = DB_MODE_RO;
    d[0].ptr = &storage;
    d[0].alias = true;
    d[1].guid = g1;
    d[1].mode = DB_MODE_RO;
    const uint32_t order[2] = {0u, 1u};
    const int alias[2] = {-1, 0};
    check("pre-filled slot", d, 2u, order, alias);
  }

  if (failures != 0) {
    return 1;
  }
  printf("PASS dep_alias_classify: owner/alias and visit order on six shapes\n");
  return 0;
}
