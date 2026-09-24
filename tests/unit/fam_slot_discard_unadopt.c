/// @file fam_slot_discard_unadopt.c
/// @brief A store that goes back leaves nothing naming it — neither the cache
/// that allocated it nor a descriptor handed out over it — and the block can
/// be given a store again afterwards.
///
/// Whitebox, because the state asserted has no public reader and the sequence
/// has no public spelling: a create that mints a store and then turns out to
/// have created nothing is decided by a word no program can steer.  The cache
/// here is private — never installed under a label — so the sequence is the
/// create path's, with nothing else able to reach it.
///
/// The property is one invariant, stated on either representation: a
/// descriptor that outlived its store would hand the next legitimate first
/// user a pointer into a granule that belongs to some other block by then.
/// Where the descriptor carries its payload the storage never left with the
/// store, so the same assertions hold by a different route.

#include "arts.h"

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/excl/types.h"
#include "arts/db.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BYTES 256u
#define SEED 0x5EEDu

/* Never installed under a label: the create path's own sequence, with no
 * holder, no home and no route slot that could reach it. */
static struct arts_db_s g_db;

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL fam_slot_discard_unadopt: %s\n", what);
  arts_test_fail();
}

/* The storage this cache hands out, or NULL when it has none. */
static void *payload_of(struct arts_db_cache_s *cache) {
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(h);
  void *p = (b != NULL) ? (void *)b->data : NULL;
  arts_db_buf_release(&h);
  return p;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int rank = arts_get_current_rank();
  memset(&g_db, 0, sizeof(g_db));
  g_db.db_type = ARTS_DB;
  arts_db_cache_init(&g_db.cache, arts_guid_reserve(ARTS_GUID_DB, rank), BYTES,
                     ARTS_DB_INIT_STUB, rank);
  struct arts_db_cache_s *cache = &g_db.cache;

  if (!arts_db_fam_slot_create(cache, /*zero_first=*/false)) {
    fail("a sized block with no store was given none");
    arts_shutdown();
    return;
  }
  uint64_t first = arts_db_fam_slot_addr(cache);
  (void)arts_db_buf_ensure(cache, BYTES);
  if (payload_of(cache) == NULL) {
    fail("a materialized block has no storage");
    arts_shutdown();
    return;
  }

  arts_db_fam_slot_discard(cache);
  if (arts_db_fam_slot_addr(cache) != 0) {
    fail("a cache that handed its store back still names one");
  }
  if (payload_of(cache) == (void *)(uintptr_t)first) {
    fail("a store that was handed back is still handed out");
  }

  /* And the cache is left able to take a store again: a block whose first
   * user has still to come must be able to be given one.  Which granule that
   * is says nothing — a store handed back is one the next allocation may be
   * handed straight back out — so what is asserted is that the block has
   * storage and that it really is storage. */
  if (!arts_db_fam_slot_create(cache, /*zero_first=*/false)) {
    fail("a block that gave its store back was given no other");
    arts_shutdown();
    return;
  }
  (void)arts_db_buf_ensure(cache, BYTES);
  volatile unsigned int *second = (volatile unsigned int *)payload_of(cache);
  if (second == NULL) {
    fail("a block given a store again has no storage");
  } else {
    second[0] = SEED;
    if (second[0] != SEED) {
      fail("the storage a block was given again does not hold a write");
    }
  }

  arts_db_fam_slot_discard(cache);
  arts_db_cache_destructor(cache);
  if (arts_test_status() == 0) {
    printf("PASS fam_slot_discard_unadopt\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
