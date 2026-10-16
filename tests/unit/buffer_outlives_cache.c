/* SPDX-License-Identifier: Apache-2.0
 *
 * A buffer can outlive its block's descriptor: a one-sided transfer's local
 * completion holds a reference the fabric releases whenever it likes, and the
 * block may be destroyed first.  Where the arm keeps a per-DB free-list, the
 * buffer keeps the descriptor's storage alive until it parks there, and the
 * storage (and the list) go when the last of the two lets go; elsewhere the
 * last drop touches nothing but the buffer.
 *
 * Properties:
 *   1. A drop after the destroy ends the buffer without touching freed memory,
 *      and the descriptor's storage is freed exactly once — where the arm
 *      keeps a list, only after both.
 *   2. With nothing out, the destroy frees the storage at once.
 *   3. The late drop may come on another thread, which then frees the storage
 *      where the arm keeps a list.
 *   4. Many late drops racing the destroy free the storage exactly once;
 *      where the arm keeps a list they race its drain of parked buffers.
 *   5. A landing returned unused ends, and the cache's storage is freed
 *      exactly once; where the arm keeps a list it is the next one that
 *      cache hands out.
 *   6. Where the arm keeps a list, a parked buffer too small for a later draw
 *      is never handed out, and the destroy gives it and every parked buffer
 *      back at once, even while another buffer is still out.
 *   7. Where the arm keeps a list, a buffer installed in another cache still
 *      parks on, and holds, only the cache it was drawn from.
 *   8. Where the arm keeps a list, a drawn buffer ended a second time before
 *      it is drawn again or freed stops the process instead of being parked
 *      twice.
 *
 * The descriptor a property creates last is filled with a pattern when it is
 * freed and held back, not released, until the property ends, so any write
 * after the free changes the pattern and fails the test without a sanitizer;
 * every buffer taken from the registered pool must be back there when the
 * property ends.
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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB_SIZE 256u
#define RACERS 8
#define RACE_ROUNDS 2000
#define PARKED 64
#define FREED_FILL 0xDF

static _Atomic unsigned int g_db_frees;
static _Atomic(void *) g_db_live;
/* The last freed descriptor, filled and held until checked. */
static _Atomic(void *) g_db_freed;
/* Set as the storage release begins: no free of the storage may come first. */
static _Atomic int g_released;
static _Atomic long g_pool_out; /* registered-pool buffers not given back */
static int fail;

static void check(int ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    fail = 1;
  }
}

/* The freed descriptor must still hold the fill, and every buffer must be
 * back in the registered pool; then the descriptor is released for real. */
static void settle(const char *what) {
  unsigned char *p = atomic_exchange(&g_db_freed, NULL);
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
  long out = atomic_exchange(&g_pool_out, 0);
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
#ifdef ARTS_DB_BUF_POOL
  arts_lf_pool_init(&db->cache.buf_freelist);
#endif
  atomic_store(&g_db_live, db);
  atomic_store(&g_db_frees, 0u);
  atomic_store(&g_released, 0);
  return db;
}

/* What freeing a descriptor does to the buffer plane: the destructor drops
 * the slot's reference, then the storage is released. */
static void destroy(struct arts_db_s *db) {
  arts_shared_slot_store(&db->cache.buffer, NULL);
  atomic_store(&g_released, 1);
  arts_db_buf_storage_release(&db->cache);
}

static void install(struct arts_db_s *db) {
  unsigned char payload[DB_SIZE];
  memset(payload, 0x11, sizeof(payload));
  check(arts_db_buf_install(&db->cache, 1u, payload, DB_SIZE) != NULL,
        "install");
}

static void *drop(void *arg) {
  arts_shared_ptr_t h = (arts_shared_ptr_t)arg;
  arts_shared_release(&h);
  return NULL;
}

static _Atomic int g_go;
/* One distinct buffer per racer, so the parks themselves race each other
 * and the destroy, not one buffer's reference count. */
static void *racer(void *arg) {
  while (!atomic_load(&g_go)) {
    sched_yield();
  }
  arts_db_buf_landing_return((struct arts_db_buffer_s *)arg);
  return NULL;
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
  install(db);
  arts_shared_ptr_t held = arts_db_buf_acquire(&db->cache);
  destroy(db);
#ifdef ARTS_DB_BUF_POOL
  check(atomic_load(&g_db_frees) == 0u,
        "the storage went while a buffer drawn from it was out");
#endif
  arts_shared_release(&held);
  check(atomic_load(&g_db_frees) == 1u,
        "the storage was not freed exactly once after the late drop");
  settle("a drop after the destroy");

  /* 2 */
  db = new_db();
  install(db);
  destroy(db);
  check(atomic_load(&g_db_frees) == 1u,
        "a destroy with nothing out did not free the storage exactly once");
  settle("a destroy with nothing out");

  /* 3 */
  db = new_db();
  install(db);
  held = arts_db_buf_acquire(&db->cache);
  destroy(db);
  pthread_t t;
  spawn(&t, drop, held);
  pthread_join(t, NULL);
  check(atomic_load(&g_db_frees) == 1u,
        "a late drop on another thread did not free the storage once");
  settle("a late drop on another thread");

  /* 4 */
  for (int round = 0; round < RACE_ROUNDS && !fail; round++) {
    db = new_db();
    install(db);
    pthread_t th[RACERS];
    struct arts_db_buffer_s *out[RACERS];
    struct arts_db_buffer_s *parked[PARKED];
    struct arts_rdzv_landing_s r;
    atomic_store(&g_go, 0);
    for (int i = 0; i < RACERS; i++) {
      out[i] = arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &r);
    }
    /* Where the arm keeps a list, parked buffers make the destroy's drain
     * long enough for the parks to land inside it. */
    for (int i = 0; i < PARKED; i++) {
      parked[i] = arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &r);
    }
    for (int i = 0; i < PARKED; i++) {
      arts_db_buf_landing_return(parked[i]);
    }
    for (int i = 0; i < RACERS; i++) {
      spawn(&th[i], racer, out[i]);
    }
    atomic_store(&g_go, 1);
    destroy(db);
    for (int i = 0; i < RACERS; i++) {
      pthread_join(th[i], NULL);
    }
    check(atomic_load(&g_db_frees) == 1u,
          "racing late drops did not free the storage exactly once");
    settle("late drops racing the destroy");
  }

  /* 5 */
  db = new_db();
  struct arts_rdzv_landing_s rdzv;
  struct arts_db_buffer_s *l1 =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &rdzv);
  arts_db_buf_landing_return(l1);
  struct arts_db_buffer_s *l2 =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &rdzv);
#ifdef ARTS_DB_BUF_POOL
  check(l2 == l1, "an unused landing was not handed out again");
#endif
  arts_db_buf_landing_return(l2);
  destroy(db);
  check(atomic_load(&g_db_frees) == 1u,
        "the storage was not freed exactly once after a landing round");
  settle("a landing returned unused");

#ifdef ARTS_DB_BUF_POOL
  /* 6 */
  db = new_db();
  struct arts_db_buffer_s *small =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE / 4, &rdzv);
  arts_db_buf_landing_return(small);
  struct arts_db_buffer_s *big =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &rdzv);
  check(big != small, "a parked buffer too small for a draw was handed out");
  struct arts_db_buffer_s *spare =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &rdzv);
  arts_db_buf_landing_return(spare);
  destroy(db);
  check(atomic_load(&g_pool_out) == 1,
        "the destroy kept parked or set-aside buffers while one was out");
  arts_db_buf_landing_return(big);
  check(atomic_load(&g_db_frees) == 1u,
        "the storage was not freed exactly once after a size round");
  settle("a parked buffer too small for a later draw");

  /* 7 */
  for (int adopt = 0; adopt < 2; adopt++) {
    struct arts_db_s *from = new_db();
    struct arts_db_buffer_s *moved =
        arts_db_buf_landing_alloc(&from->cache, DB_SIZE, &rdzv);
    destroy(from);
    db = new_db();
    if (adopt) {
      check(arts_db_buf_adopt_landing(&db->cache, 1u, moved, DB_SIZE),
            "a landing from another cache was not adopted");
    } else {
      check(arts_db_buf_install_landed(&db->cache, 2u, moved, DB_SIZE) ==
                moved,
            "a landing from another cache was not installed");
    }
    destroy(db);
    check(atomic_load(&g_db_frees) == 1u,
          "a buffer installed in another cache changed that cache's storage");
    settle("a buffer installed in another cache");
  }

  /* 8 */
  db = new_db();
  struct arts_db_buffer_s *twice =
      arts_db_buf_landing_alloc(&db->cache, DB_SIZE, &rdzv);
  /* A SIGCHLD ignored by whoever started the test would reap the child before
   * waitpid could see how it ended. */
  signal(SIGCHLD, SIG_DFL);
  /* The child would print its inherited unflushed output again on reopening
   * its streams. */
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);
    arts_db_buf_landing_return(twice);
    arts_db_buf_landing_return(twice);
    _exit(0);
  }
  int status = 0;
  int waited = pid > 0 && waitpid(pid, &status, 0) == pid;
  check(waited, "the child that ends a buffer twice could not be waited for");
  check(!waited || !(WIFEXITED(status) && WEXITSTATUS(status) == 0),
        "a buffer ended twice was parked twice");
  arts_db_buf_landing_return(twice);
  destroy(db);
  check(atomic_load(&g_db_frees) == 1u,
        "the storage was not freed exactly once after a double end");
  settle("a drawn buffer ended twice");
#endif

  printf(fail ? "FAIL buffer_outlives_cache\n"
              : "PASS buffer_outlives_cache\n");
  return fail;
}

/* libc-backed alloc shims, so the test links without the runtime; the
 * descriptor free is held back (see settle) and the registered pool counted. */
void *arts_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void arts_free(void *ptr) { free(ptr); }
void arts_db_descriptor_free(struct arts_db_s *db) {
  void *ptr = db;
  void *live = ptr;
  if (ptr != NULL && atomic_compare_exchange_strong(&g_db_live, &live, NULL)) {
    if (!atomic_load(&g_released)) {
      /* Whatever runs next would read the storage; stop at the verdict. */
      printf("FAIL: the storage was freed before its release\n");
      exit(1);
    }
    atomic_fetch_add(&g_db_frees, 1u);
    memset(ptr, FREED_FILL, sizeof(struct arts_db_s));
    atomic_store(&g_db_freed, ptr);
    return;
  }
  if (ptr != NULL && ptr == atomic_load(&g_db_freed)) {
    atomic_fetch_add(&g_db_frees, 1u); /* a second free: counted, not done */
    return;
  }
  free(ptr);
}
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
    atomic_fetch_add(&g_pool_out, 1);
  return p;
}
void arts_regpool_free(void *p) {
  if (p != NULL)
    atomic_fetch_sub(&g_pool_out, 1);
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
