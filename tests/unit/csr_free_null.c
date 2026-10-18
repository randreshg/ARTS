/* SPDX-License-Identifier: Apache-2.0
 *
 * Pure-unit test for arts_csr_free's NULL handling.
 *
 * Contract under test:
 *   arts_csr_free(csr) tolerates a NULL csr.  arts_csr_init returns NULL for
 *   a partition homed on another rank, and a "free everything" teardown loop
 *   over its results then calls arts_csr_free(NULL): it must neither
 *   dereference the NULL nor destroy anything.  A non-NULL csr's free reaches
 *   arts_db_destroy(partGuid) (verified via a destroy-recording stub), which
 *   proves the test exercises the real arts_csr_free body, not a fake.
 *
 * The ARTS runtime is NOT started.  csr.c references the edge_vector and
 * block_distribution helpers, so all three graph TUs are compiled via
 * #include.  Every other runtime symbol they reference is stubbed below; only
 * arts_db_destroy is actually reached.
 */

#define ARTS_SYSTEM_PRINT_H
#define ARTS_INFO(...) ((void)0)
#define ARTS_DEBUG(...) ((void)0)
#define ARTS_WARN(...) ((void)0)
#define ARTS_ERROR(...) ((void)0)

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/graph.h"

/* --- allocator contract (mirrors libs/src/core/utils/malloc.c) -------------
 */
void *arts_malloc(size_t size) { return size ? malloc(size) : NULL; }
void *arts_calloc(size_t nmemb, size_t size) {
  return (nmemb && size) ? calloc(nmemb, size) : NULL;
}
void *arts_realloc(void *ptr, size_t size) {
  if (!ptr) {
    return arts_malloc(size);
  }
  if (!size) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, size);
}
void arts_free(void *ptr) { free(ptr); }

/* --- stubs for the runtime symbols csr.c references -------------------------
 */
static arts_guid_t g_last_destroyed;
static int g_destroy_calls;
void arts_db_destroy(arts_guid_t guid) {
  g_last_destroyed = guid;
  g_destroy_calls++;
}
arts_guid_t arts_db_create(void **ptr, uint64_t len, arts_db_types_t db_type,
                           uint16_t flags, const arts_db_hint_t *hint) {
  (void)len;
  (void)db_type;
  (void)flags;
  (void)hint;
  if (ptr) {
    *ptr = NULL;
  }
  return 0;
}
bool arts_guid_is_local(arts_guid_t guid) {
  (void)guid;
  return true;
}
unsigned int arts_get_current_rank(void) { return 0; }
unsigned int arts_get_total_ranks(void) { return 1; }
arts_guid_t arts_guid_reserve(arts_guid_kind_t kind, unsigned int rank) {
  (void)kind;
  (void)rank;
  return 0;
}

/* Compile the three graph TUs into this test (single TU so the stubs above
 * satisfy their references). */
#include "../../libs/src/graph/block_distribution.c"
#include "../../libs/src/graph/csr.c"
#include "../../libs/src/graph/edge_vector.c"

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL csr_free_null: %s (%s:%d)\n", msg, __FILE__,       \
              __LINE__);                                                       \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

/* Happy path: arts_csr_free on a non-NULL csr reaches arts_db_destroy with the
 * stored partGuid -> confirms we are calling the real body. */
static void test_nonnull_free_reaches_destroy(void) {
  arts_csr_graph_t csr;
  memset(&csr, 0, sizeof(csr));
  csr.partGuid = (arts_guid_t)0x1234567ULL;
  g_destroy_calls = 0;
  g_last_destroyed = 0;
  arts_csr_free(&csr);
  CHECK(g_destroy_calls == 1, "non-NULL free calls arts_db_destroy once");
  CHECK(g_last_destroyed == (arts_guid_t)0x1234567ULL,
        "free passes csr->partGuid to arts_db_destroy");
}

static void test_free_null(void) {
  g_destroy_calls = 0;
  arts_csr_free(NULL);
  CHECK(g_destroy_calls == 0, "arts_csr_free(NULL) destroyed something");
}

int main(void) {
  test_nonnull_free_reaches_destroy();
  test_free_null();

  if (g_fail) {
    return 1;
  }
  printf("PASS csr_free_null: arts_csr_free(NULL) tolerated; non-NULL free "
         "reaches arts_db_destroy(partGuid)\n");
  return 0;
}
