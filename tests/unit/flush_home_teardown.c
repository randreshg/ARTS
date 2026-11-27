/* SPDX-License-Identifier: Apache-2.0
 *
 * FLUSH's home teardown.  A destroy at a block's home returns the route slot
 * first, and only the destroy whose return detached the block tells the
 * sharers, each once: a notice never precedes the retire, and a second
 * destroy of the same home (a program error) detaches nothing and tells no
 * rank again.  The roster is read after the detach, under the count the
 * destroy runs under, so the descriptor stays until every frame that entered
 * a destroy on it has let go, and its deleter then runs once and frees the
 * roster.
 *
 * The arm's source is compiled in and driven through arts_db_destroy_arrived
 * on a home descriptor whose sharers are two other ranks; the retire is the
 * route table's single flight on that one descriptor, and the notices are
 * recorded where the wire sender would build them.
 *
 * PURE test: no arts_rt, no ports, no threads.
 */
#include "coherence/flush/flush.c"
#include "utils/shared.c"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define HOME 0u
#define CREATOR 1u
#define READER 2u
#define BYSTANDER 3u
#define SIZE 64u
#define GUID ARTS_GUID_MAKE(ARTS_GUID_DB, HOME, ARTS_GUID_DB_KEY(1u, 0x71u))

unsigned int arts_global_rank_id = HOME;
unsigned int arts_global_rank_count = 4u;
uint64_t arts_guid_table_count;
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

#include "arts/system/shutdown.h"
#include "counter_stubs.h"

static void fail(const char *what) {
  printf("FAIL flush_home_teardown: %s\n", what);
  fflush(stdout);
  _exit(1);
}

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  printf("FAIL flush_home_teardown: the runtime aborted\n");
  fflush(stdout);
  _exit(rc ? rc : 1);
}

/* The runtime's allocations not yet freed. */
static long g_live_allocs;
void *arts_malloc(size_t n) {
  g_live_allocs++;
  return malloc(n);
}
void *arts_calloc(size_t n, size_t s) {
  g_live_allocs++;
  return calloc(n, s);
}
void arts_free(void *p) {
  g_live_allocs -= (p != NULL);
  free(p);
}

unsigned int arts_guid_get_rank(arts_guid_t g) {
  (void)g;
  return HOME;
}

/* Everything a destroy never reaches. */
#define UNREACHED(sig)                                                         \
  sig {                                                                        \
    printf("FAIL flush_home_teardown: %s reached\n", __func__);                \
    fflush(stdout);                                                            \
    _exit(1);                                                                  \
  }
UNREACHED(void arts_transport_send_async(int rank, char *message,
                                         unsigned int length))
UNREACHED(arts_shared_ptr_t arts_db_buf_acquire(struct arts_db_cache_s *cache))
UNREACHED(void arts_db_buf_release(arts_shared_ptr_t *h))
UNREACHED(bool arts_db_buf_ensure(struct arts_db_cache_s *cache,
                                  uint64_t db_size))
UNREACHED(struct arts_db_buffer_s *arts_db_buf_install(
    struct arts_db_cache_s *c, uint64_t v, const void *d, uint64_t s))
UNREACHED(arts_shared_ptr_t arts_db_buf_detached(uint64_t size,
                                                 struct arts_db_buffer_s **out))
UNREACHED(uint64_t arts_db_first_fetch_size(const struct arts_db_cache_s *c))
UNREACHED(uint64_t arts_net_rdzv_txid_next(void))
UNREACHED(bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *a,
                                   uint64_t *k))
UNREACHED(void arts_net_rdzv_expect(uint64_t txid, uint64_t len,
                                    void (*on_data)(void *),
                                    void (*on_discard)(void *), void *arg))
UNREACHED(void arts_net_put_payload(int r, uint64_t a, uint64_t k, uint64_t t,
                                    const void *s, uint64_t l,
                                    void (*cb)(void *), void *arg))
#ifdef ARTS_DEBUG_INJECTION
UNREACHED(bool arts_net_send_held_read(int rank, const char *message,
                                       unsigned int length))
#endif
UNREACHED(void arts_db_await_ack(sem_t *cv))
UNREACHED(bool arts_route_table_live(arts_shared_ptr_t h, arts_guid_t k))
UNREACHED(void arts_db_resolve_slot(struct arts_edt_s *edt, unsigned int slot,
                                    arts_shared_ptr_t buf_h))
UNREACHED(void arts_db_acquire_resolved(struct arts_edt_s *edt,
                                        unsigned int slot))
UNREACHED(void *arts_db_acquire_local(struct arts_db_cache_s *cache))
UNREACHED(void arts_db_buf_ref_release_cb(void *arg))
#ifndef NDEBUG
UNREACHED(void arts_dep_acq_resolve_once(struct arts_edt_s *edt, uint32_t slot))
#endif
#ifdef ARTS_DEBUG_WAKE_GUID
UNREACHED(void arts_dep_waiter_check(const struct arts_edt_s *edt,
                                     arts_guid_t edt_guid, unsigned int slot))
#endif
#if ARTS_DB_CENSUS
ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
#endif

/* ---- the descriptor, its retire and the notices ---- */

static struct arts_db_s *g_db;
static int g_deleted;
/* Destroys whose frame is on the descriptor. */
static int g_in_destroy;
/* A retire detached the block. */
static bool g_retired;
/* Retires that found the block retired already. */
static int g_retire_misses;
/* The notices sent, per rank. */
static int g_told[4];
static int g_notices;

static void db_deleter(void *p) {
  if (p != g_db) {
    fail("the deleter ran on another object");
  }
  if (g_in_destroy != 0) {
    fail("the descriptor went inside a destroy's frame");
  }
  g_deleted++;
  arts_db_cache_destructor(&g_db->cache);
  free(g_db);
}

/* The route table's single flight: the first retire clears the control
 * block's tag and drops the install's reference, which the test's stands
 * for; a retire that finds the tag cleared detaches nothing, releases nothing
 * and answers false. */
bool arts_ooo_retire(arts_guid_t key, arts_shared_ptr_t cb) {
  if (cb != g_db->self_cb) {
    fail("a retire of another object");
  }
  if (arts_shared_tag(cb) != (uint64_t)key) {
    g_retire_misses++;
    return false;
  }
  arts_shared_set_tag(cb, 0u);
  g_retired = true;
  arts_shared_ptr_t h = cb;
  arts_shared_release(&h);
  return true;
}

/* A sharer is told of the destroy only by the frame whose retire detached
 * the block, so no notice precedes that retire. */
void arts_send_db_cache_destroy(unsigned int rank, arts_guid_t db_guid) {
  if (!g_retired) {
    fail("a destroy notice left before the block's retire");
  }
  if (db_guid != GUID || rank >= 4u || rank == HOME) {
    fail("a destroy notice names another block, or no sharer");
  }
  g_told[rank]++;
  g_notices++;
}

/* A destroy's frame on the home, as the engine runs it: on the descriptor
 * its dispatch pinned, the pin dropped when the body has returned. */
static void destroy_on(arts_shared_ptr_t *pin) {
  g_in_destroy++;
  arts_db_destroy_arrived(g_db);
  g_in_destroy--;
  arts_shared_release(pin);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  long allocs = g_live_allocs;
  size_t sz = (sizeof(struct arts_db_s) + 63u) & ~(size_t)63u;
  g_db = (struct arts_db_s *)aligned_alloc(64, sz);
  memset(g_db, 0, sz);
  g_db->db_type = ARTS_DB;
  /* The home descriptor of a block CREATOR made, which READER fetched: the
   * control block holds the test's reference, which stands for the
   * install's. */
  arts_db_cache_init(&g_db->cache, GUID, SIZE, ARTS_DB_INIT_HOME_RECV, CREATOR);
  g_db->self_cb = arts_shared_make(g_db, db_deleter);
  arts_shared_set_tag(g_db->self_cb, (uint64_t)GUID);
  sharers_mark(g_db, READER);
  if (!sharers_test(g_db, CREATOR) || !sharers_test(g_db, READER) ||
      sharers_test(g_db, BYSTANDER) || sharers_test(g_db, HOME)) {
    fail("the roster does not name the creator and the reader alone");
  }

  /* Two destroys reach the home while it is installed, each dispatched on
   * its own pin; the second is a program error. */
  arts_shared_ptr_t first = arts_shared_copy(g_db->self_cb);
  arts_shared_ptr_t second = arts_shared_copy(g_db->self_cb);
  destroy_on(&first);
  if (!g_retired) {
    fail("the destroy did not retire the block");
  }
  if (g_notices != 2 || g_told[CREATOR] != 1 || g_told[READER] != 1) {
    fail("the destroy did not tell each sharer exactly once");
  }
  if (g_deleted != 0) {
    fail("the descriptor went while a destroy's dispatch still held it");
  }
  destroy_on(&second);
  printf("     notices 2 then %d, retires that found the block retired %d\n",
         g_notices - 2, g_retire_misses);
  if (g_notices != 2) {
    fail("a second destroy told the sharers again");
  }
  if (g_deleted != 1) {
    fail("the deleter did not run once the last frame let go");
  }
  if (g_live_allocs != allocs) {
    fail("the deleter left an allocation behind");
  }
  printf("PASS flush_home_teardown\n");
  return 0;
}
