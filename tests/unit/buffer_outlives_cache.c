/* SPDX-License-Identifier: Apache-2.0
 *
 * A buffer can outlive its block's descriptor: a one-sided transfer's local
 * completion holds a reference the fabric releases whenever it likes, and the
 * block may be destroyed first.  A buffer names no cache, so its last drop
 * touches nothing but the buffer, which goes back to the registered pool.
 *
 * Properties:
 *   1. A drop after the destroy ends the buffer without touching the freed
 *      descriptor.
 *   2. With nothing out, the destroy ends the installed buffer at once.
 *   3. The late drop may come on another thread.
 *   4. Many late drops racing the destroy end the buffer exactly once.
 *   5. A landing returned unused goes back to the registered pool at once.
 *   6. A landing that loses to the installed buffer, adopted over it or
 *      installed at a stale version, goes back to the registered pool at
 *      once.
 *
 * A destroyed descriptor is filled with a pattern and held back, not
 * released, until the property ends, so any write after the destroy changes
 * the pattern and fails the test without a sanitizer; every buffer taken from
 * the registered pool must be back there when the property ends.
 *
 * Standalone: links buffer.c + shared.c with libc-backed alloc shims.  Where
 * the store is DIRECT the payload is not a buffer's, so that tree does not
 * build the test.
 */
#include "arts/coherence/buffer.h"
#include "arts/coherence/types.h"
#include "arts/memory/regpool.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

#include "arts/utils/stdatomic_compat.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_SIZE 256u
#define RACERS 8
#define RACE_ROUNDS 2000
#define FREED_FILL 0xDF

/* The destroyed descriptor, filled and held until checked. */
static unsigned char *g_db_freed;
static _Atomic(long) g_pool_out; /* registered-pool buffers not given back */
static int fail;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    fail = 1;
  }
}

static long pool_out(void) {
  return atomic_load_explicit(&g_pool_out, memory_order_relaxed);
}

/* The destroyed descriptor must still hold the fill, and every buffer must be
 * back in the registered pool; then the descriptor is released for real. */
static void settle(const char *what) {
  unsigned char *p = g_db_freed;
  g_db_freed = NULL;
  if (p != NULL) {
    for (size_t i = 0; i < sizeof(struct arts_db_s); i++) {
      if (p[i] != FREED_FILL) {
        printf("FAIL: %s: the freed descriptor was written at byte %zu\n", what,
               i);
        fail = 1;
        break;
      }
    }
    free(p);
  }
  long out = atomic_exchange_explicit(&g_pool_out, 0, memory_order_relaxed);
  if (out != 0) {
    printf("FAIL: %s: %ld buffer(s) never went back to the registered pool\n",
           what, out);
    fail = 1;
  }
}

static struct arts_db_s *new_db(void) {
  struct arts_db_s *db =
      (struct arts_db_s *)arts_malloc_aligned(sizeof(*db), 64);
  memset(db, 0, sizeof(*db));
  db->db_type = ARTS_DB;
  atomic_init(&db->cache.db_size, DB_SIZE);
  atomic_init(&db->cache.payload_pending, (uint8_t)1);
  return db;
}

/* What freeing a descriptor does to the buffer plane: the destructor drops
 * the slot's reference, then the storage is freed (here: filled and held). */
static void destroy(struct arts_db_s *db) {
  arts_shared_slot_store(&db->cache.buffer, NULL);
  memset(db, FREED_FILL, sizeof(*db));
  g_db_freed = (unsigned char *)db;
}

static void install(struct arts_db_s *db, uint64_t version) {
  unsigned char payload[DB_SIZE];
  memset(payload, 0x11, sizeof(payload));
  check(arts_db_buf_install(&db->cache, version, payload, DB_SIZE) != NULL,
        "install");
}

static void *drop(void *arg) {
  arts_shared_ptr_t h = (arts_shared_ptr_t)arg;
  arts_shared_release(&h);
  return NULL;
}

static _Atomic(int) g_go;
static void *racer(void *arg) {
  while (!atomic_load_explicit(&g_go, memory_order_acquire)) {
    sched_yield();
  }
  return drop(arg);
}

static void spawn(pthread_t *t, void *(*fn)(void *), void *arg) {
  if (pthread_create(t, NULL, fn, arg) != 0) {
    printf("FAIL buffer_outlives_cache: pthread_create\n");
    exit(1);
  }
}

int main(void) {
  /* Unbuffered, so a crash still leaves every verdict printed before it. */
  setvbuf(stdout, NULL, _IONBF, 0);
  /* 1 */
  struct arts_db_s *db = new_db();
  install(db, 1u);
  arts_shared_ptr_t held = arts_db_buf_acquire(&db->cache);
  destroy(db);
  check(pool_out() == 1, "the destroy ended a buffer still held");
  arts_shared_release(&held);
  settle("a drop after the destroy");

  /* 2 */
  db = new_db();
  install(db, 1u);
  destroy(db);
  check(pool_out() == 0, "a destroy with nothing out kept its buffer");
  settle("a destroy with nothing out");

  /* 3 */
  db = new_db();
  install(db, 1u);
  held = arts_db_buf_acquire(&db->cache);
  destroy(db);
  pthread_t t;
  spawn(&t, drop, held);
  pthread_join(t, NULL);
  settle("a late drop on another thread");

  /* 4 */
  for (int round = 0; round < RACE_ROUNDS && !fail; round++) {
    db = new_db();
    install(db, 1u);
    pthread_t th[RACERS];
    atomic_store_explicit(&g_go, 0, memory_order_relaxed);
    for (int i = 0; i < RACERS; i++) {
      spawn(&th[i], racer, arts_db_buf_acquire(&db->cache));
    }
    atomic_store_explicit(&g_go, 1, memory_order_release);
    destroy(db);
    for (int i = 0; i < RACERS; i++) {
      pthread_join(th[i], NULL);
    }
    settle("late drops racing the destroy");
  }

  /* 5 */
  struct arts_rdzv_landing_s rdzv;
  struct arts_db_buffer_s *l = arts_db_buf_landing_alloc(DB_SIZE, &rdzv);
  check(pool_out() == 1, "a landing was not drawn from the registered pool");
  arts_db_buf_landing_return(l);
  check(pool_out() == 0, "a landing returned unused was kept");
  settle("a landing returned unused");

  /* 6 */
  db = new_db();
  install(db, 3u);
  l = arts_db_buf_landing_alloc(DB_SIZE, &rdzv);
  check(!arts_db_buf_adopt_landing(&db->cache, 1u, l, DB_SIZE),
        "a landing was adopted over an installed buffer");
  check(pool_out() == 1, "a landing adopted over a buffer was kept");
  l = arts_db_buf_landing_alloc(DB_SIZE, &rdzv);
  check(arts_db_buf_install_landed(&db->cache, 2u, l, DB_SIZE) != l,
        "a stale landing displaced a newer buffer");
  check(pool_out() == 1, "a stale landing was kept");
  destroy(db);
  settle("a landing that loses to the installed buffer");

  printf(fail ? "FAIL buffer_outlives_cache\n"
              : "PASS buffer_outlives_cache\n");
  return fail;
}

/* libc-backed alloc shims, so the test links without the runtime; the
 * registered pool is counted. */
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
  void *p = arts_malloc_aligned(size, align);
  if (p != NULL)
    atomic_fetch_add_explicit(&g_pool_out, 1, memory_order_relaxed);
  return p;
}
void arts_regpool_free(void *p) {
  if (p != NULL)
    atomic_fetch_sub_explicit(&g_pool_out, 1, memory_order_relaxed);
  free(p);
}

#include "arts/runtime_state.h"
#include "arts/system/threads.h"
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void arts_abort(uint8_t code) { exit(code ? code : 1); }
bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *raddr,
                         uint64_t *rkey) {
  (void)len;
  *raddr = (uint64_t)(uintptr_t)p;
  *rkey = 1;
  return true;
}
uint64_t arts_net_rdzv_txid_next(void) { return 1; }
