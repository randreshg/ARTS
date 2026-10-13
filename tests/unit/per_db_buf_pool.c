/* SPDX-License-Identifier: Apache-2.0
 *
 * A buffer whose last reference drops parks on its cache's free-list, and the
 * next install on that cache draws it back.  Only the arms that keep a per-DB
 * free-list (VAL, INV) have one to test. */
#include "arts/coherence/buffer.h"
#include "arts/coherence/types_common.h"
#include "arts/memory/regpool.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef ARTS_DB_BUF_POOL
#error "per_db_buf_pool is built only where the arm keeps a per-DB free-list"
#endif

/* Minimal cache init: what arts_db_cache_common_init sets that the free-list
 * needs, so the test stands alone. */
static void mini_cache_init(struct arts_db_cache_s *c, uint64_t db_size) {
  memset(c, 0, sizeof(*c));
  c->db_size = db_size;
  arts_lf_pool_init(&c->buf_freelist);
  /* buffer slot starts NULL */
}

int main(void) {
  /* No ARTS runtime bootstrap here — arts_db_buf_install draws its buffers
   * from the registered pool, so this standalone test must init it itself,
   * the same precondition arts_runtime_node_init establishes before any
   * real worker or progress thread runs. */
  assert(arts_regpool_init(NULL, NULL, (size_t)64 * 1024 * 1024, 1));

  struct arts_db_cache_s c;
  const uint64_t sz = 128;
  mini_cache_init(&c, sz);

  /* v1's last reference is the slot's: emptying the slot parks it. */
  char payload[128];
  memset(payload, 0xAB, sz);
  struct arts_db_buffer_s *b1 = arts_db_buf_install(&c, 1, payload, sz);
  assert(b1 != NULL);
  void *a1 = (void *)b1;
  arts_atomic_shared_store(&c.buffer, NULL);

  /* The next install draws the parked buffer back. */
  struct arts_db_buffer_s *b2 = arts_db_buf_install(&c, 2, payload, sz);
  assert(b2 != NULL);
  assert((void *)b2 == a1 && "v2 must reuse the parked v1 buffer");
  arts_atomic_shared_store(&c.buffer, NULL);
  arts_regpool_cleanup();
  printf("RECYCLE_OK reused=%p\n", (void *)b2);
  return 0;
}
