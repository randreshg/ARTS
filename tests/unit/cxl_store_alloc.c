/// @file cxl_store_alloc.c
/// @brief The store seam over the shared arena: every slot is a cache-line
/// boundary inside the store, slots are disjoint, a zero-byte request still
/// yields a line, and the static strategy's one arena serves whatever device
/// the cfg named.  Slots come from this rank's pool: consecutive slots are
/// adjacent except where the pool refilled, and a slot larger than the pool
/// is carved on its own without disturbing the pool.  Exhaustion is probed in
/// a forked child: it must end the child with the fatal error naming the
/// arena size, never a null slot.
#include "arts.h"
#include "arts/cxl/store.h"
#include "arts/runtime_state.h"
#include "../test_failure_status.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOTS 64u
#define BYTES 1000u /* not a multiple of 64 on purpose */

static void fail(const char *what) {
  (void)fprintf(stderr, "FAIL cxl_store_alloc: %s\n", what);
  arts_test_fail();
}

static void check_alloc(void) {
  unsigned char *p[SLOTS];
  for (unsigned i = 0; i < SLOTS; i++) {
    p[i] = (unsigned char *)arts_cxl_store_alloc(BYTES);
    if (!arts_cxl_store_contains(p[i])) {
      fail("a slot is not inside the store");
    }
    if (((uintptr_t)p[i] & 63u) != 0u) {
      fail("a slot is not 64-byte aligned");
    }
    for (unsigned j = 0; j < i; j++) {
      if (p[j] < p[i] + 1024u && p[i] < p[j] + 1024u) {
        fail("two slots overlap");
      }
    }
    memset(p[i], (int)i, BYTES);
  }
  /* One arena: a slot follows its predecessor unless the pool ran dry
   * between them, which happens at most once per pool's worth of slots (+1
   * for the pool the run had already started). */
  if (arts_node_info.cxl_db_dev_count == 1u) {
    uint64_t pool = arts_node_info.cxl_db_pool_bytes;
    unsigned breaks = 0;
    for (unsigned i = 1; i < SLOTS; i++) {
      if (p[i] != p[i - 1u] + 1024u) {
        breaks++;
      }
    }
    if ((uint64_t)breaks > (SLOTS * 1024ull) / pool + 1u) {
      fail("consecutive slots are not served from one pool");
    }
  }
  for (unsigned i = 0; i < SLOTS; i++) {
    if (p[i][0] != (unsigned char)i || p[i][BYTES - 1u] != (unsigned char)i) {
      fail("a slot does not hold its bytes");
    }
  }
  if (!arts_cxl_store_contains(arts_cxl_store_alloc(0))) {
    fail("a zero-byte request yields no slot");
  }
  if (arts_cxl_store_contains(&p[0]) || arts_cxl_store_contains(NULL)) {
    fail("contains answers true outside the store");
  }
}

/* A slot larger than the pool bypasses it: it is still an aligned slot in
 * the store, disjoint from the pool's, and the small slot after it follows
 * the one before it.  Tried twice, since a pool refill may fall between the
 * small ones once.  Only where the pool is small beside the arena (the
 * 1n_pool_small cfg): under the default a lone rank's pool is the whole
 * arena, and a slot larger than it could not be served at all. */
static void check_bypass(void) {
  uint64_t pool = arts_node_info.cxl_db_pool_bytes;
  if (arts_node_info.cxl_db_dev_count != 1u || pool < 4u * 1024u ||
      pool > (uint64_t)ARTS_CXL_DB_ARENA_SIZE_BYTES / 8u) {
    return;
  }
  unsigned adjacent = 0;
  for (unsigned t = 0; t < 2u; t++) {
    unsigned char *a = (unsigned char *)arts_cxl_store_alloc(BYTES);
    unsigned char *big = (unsigned char *)arts_cxl_store_alloc((size_t)pool + 64u);
    unsigned char *c = (unsigned char *)arts_cxl_store_alloc(BYTES);
    if (!arts_cxl_store_contains(big) || ((uintptr_t)big & 63u) != 0u) {
      fail("an oversized slot is not an aligned slot in the store");
    }
    if (big < a + 1024u && a < big + pool + 64u) {
      fail("an oversized slot overlaps a pooled one");
    }
    memset(big, 0x5a, (size_t)pool + 64u);
    if (c == a + 1024u) {
      adjacent++;
    }
  }
  if (adjacent == 0u) {
    fail("an oversized slot disturbed the pool");
  }
}

static void check_exhaustion(void) {
  pid_t pid = fork();
  if (pid == 0) {
    for (;;) {
      (void)arts_cxl_store_alloc(64u * 1024u * 1024u);
    }
  }
  int st = 0;
  (void)waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) == 0) {
    fail("exhausting the arena did not end the child with the fatal error");
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_current_rank() == 0u) {
    check_alloc();
    check_bypass();
    check_exhaustion();
    if (arts_test_status() == 0) {
      (void)printf("PASS cxl_store_alloc\n");
    }
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
