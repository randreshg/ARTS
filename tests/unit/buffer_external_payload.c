/* SPDX-License-Identifier: Apache-2.0
 *
 * The DIRECT descriptor: the payload is external, so the bytes are the
 * caller's and the descriptor only refers to them.
 *
 * Properties:
 *   1. adopt_external installs a descriptor whose data IS the pointer given,
 *      at the version given, and reports true exactly once for a cache.
 *   2. A second adopt of the SAME pointer LOSES the install-once CAS: it
 *      reports false, the first descriptor stands at its own version, and the
 *      descriptor the loser built is recycled -- exactly one of them -- onto
 *      the per-DB free-list, its external bytes untouched.  (A second adopt of
 *      a DIFFERENT pointer is the install-once violation and is fatal by
 *      design -- two stores for one block -- so no in-process test can
 *      exercise it.)
 *   3. A NULL payload installs nothing and reports false.
 *   4. buf_for_payload recovers that descriptor from the payload pointer and
 *      the cache, and NULL for a payload the cache does not hold.
 *   5. The deleter does not free or write the external bytes: the last drop
 *      recycles the installed descriptor itself -- exactly one node, that
 *      descriptor -- and a sentinel written before the drop is still there.
 *
 * Standalone: links buffer.c + shared.c with libc-backed alloc shims.
 */
#include "arts/coherence/buffer.h"
#include "arts/coherence/types.h"
#include "arts/memory/regpool.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_SIZE 256u

static struct arts_db_cache_s g_cache;
static unsigned char g_external[DB_SIZE];
static unsigned char g_other[DB_SIZE];

int main(void) {
#ifndef ARTS_FAM_DIRECT
  printf("SKIP buffer_external_payload (not a DIRECT build)\n");
  return 0;
#else
  int fail = 0;
  memset(&g_cache, 0, sizeof(g_cache));
  g_cache.db_size = DB_SIZE;
  g_cache.payload_pending = 1u;
  memset(g_external, 0x5a, sizeof(g_external));
  memset(g_other, 0x33, sizeof(g_other));

  if (arts_db_buf_adopt_external(&g_cache, NULL, 1u, DB_SIZE)) {
    printf("FAIL: a NULL payload was installed\n");
    fail = 1;
  }
  if (!arts_db_buf_adopt_external(&g_cache, g_external, 1u, DB_SIZE)) {
    printf("FAIL: first adopt did not install\n");
    fail = 1;
  }
  struct arts_db_buffer_s *b = arts_db_buf_for_payload(&g_cache, g_external);
  if (b == NULL || (void *)b->data != (void *)g_external || b->version != 1u) {
    printf("FAIL: descriptor does not name the external payload\n");
    fail = 1;
  }
  /* Install-once: the same store adopted twice keeps one descriptor.  The
   * flag the first adopt cleared is the fast exit, so restore it -- otherwise
   * this call returns before the CAS and the branch under test never runs. */
  g_cache.payload_pending = 1u;
  if (arts_db_buf_adopt_external(&g_cache, g_external, 2u, DB_SIZE)) {
    printf("FAIL: a second adopt of the same store installed again\n");
    fail = 1;
  }
  if (arts_db_buf_for_payload(&g_cache, g_external) != b || b->version != 1u) {
    printf("FAIL: the installed descriptor did not stand\n");
    fail = 1;
  }
  /* The loser's descriptor -- and only it -- came back to the per-DB pool. */
  arts_lf_link_t *loser = arts_lf_pool_pop_or_null(&g_cache.buf_freelist);
  if (loser == NULL || (struct arts_db_buffer_s *)loser == b) {
    printf("FAIL: the losing adopt did not recycle its own descriptor\n");
    fail = 1;
  }
  if (arts_lf_pool_pop_or_null(&g_cache.buf_freelist) != NULL) {
    printf("FAIL: more than one descriptor was recycled\n");
    fail = 1;
  }
  if (loser != NULL) {
    arts_regpool_free(loser);
  }
  if (arts_db_buf_for_payload(&g_cache, g_other) != NULL) {
    printf("FAIL: buf_for_payload does not discriminate\n");
    fail = 1;
  }
  /* Drop the cache-hold ref: the deleter runs and recycles the installed
   * descriptor onto the per-DB free-list.  The external bytes are not its to
   * touch, and nothing else is recycled with it. */
  arts_atomic_shared_store(&g_cache.buffer, NULL);
  arts_lf_link_t *node = arts_lf_pool_pop_or_null(&g_cache.buf_freelist);
  if ((struct arts_db_buffer_s *)node != b) {
    printf("FAIL: the last drop did not recycle the installed descriptor\n");
    fail = 1;
  }
  if (arts_lf_pool_pop_or_null(&g_cache.buf_freelist) != NULL) {
    printf("FAIL: the last drop recycled more than the descriptor\n");
    fail = 1;
  }
  if (node != NULL) {
    arts_regpool_free(node);
  }
  for (unsigned int i = 0; i < DB_SIZE; i++) {
    if (g_external[i] != 0x5a) {
      printf("FAIL: the deleter touched the external bytes\n");
      fail = 1;
      break;
    }
  }
  printf(fail ? "FAIL buffer_external_payload\n"
              : "PASS buffer_external_payload\n");
  return fail;
#endif
}

/* -- libc-backed alloc shims so the test links without the ARTS runtime -- */
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void arts_free(void *ptr) { free(ptr); }
void *arts_malloc_aligned(size_t size, size_t align) {
  void *p = NULL;
  size_t a = align < sizeof(void *) ? sizeof(void *) : align;
  if (posix_memalign(&p, a, size) != 0) {
    return NULL;
  }
  return p;
}
void *arts_regpool_alloc_aligned(size_t size, size_t align) {
  return arts_malloc_aligned(size, align);
}
void *arts_regpool_zalloc_aligned(size_t size, size_t align) {
  void *p = arts_regpool_alloc_aligned(size, align);
  if (p) memset(p, 0, size);
  return p;
}
void arts_regpool_free(void *p) { free(p); }

/* Rendezvous-plane stubs: buffer.c's landing helpers reference the net core's
 * advertisement/txid primitives and the runtime's fatal-print externs; a pure
 * unit run never allocates a landing, so inert stubs satisfy the link. */
#include "arts/runtime_state.h"
#include "arts/system/threads.h"
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { exit(code ? code : 1); }
bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *raddr,
                         uint64_t *rkey) {
  (void)p;
  (void)len;
  (void)raddr;
  (void)rkey;
  return false;
}
uint64_t arts_net_rdzv_txid_next(void) { return 1; }
