/* SPDX-License-Identifier: Apache-2.0
 *
 * route_table_db_shard — the route-table shard decision is a pure function
 * of the key and of the rank's geometry, agreed on by install and lookup for
 * EVERY key source:
 *
 *   1. Before node init (geometry 0) every key, DB or flat, resolves to a
 *      remote shard.
 *   2. Self-created DB keys resolve to the local per-thread table at
 *      (seq >> CHUNK_BITS) % num_tables — chunk-mates share a table.
 *   3. A foreign creator's DB key resolves to a remote shard; the SAME key
 *      seen on its creating rank resolves local (the local/remote split,
 *      recovered arithmetically).
 *   4. A reserved DB member (of a single-rank and of a round-robin range)
 *      resolves to route_table[(seq >> CHUNK_BITS) % num_tables] on the rank
 *      whose creator slice holds its seq, and to a remote shard on every
 *      other rank, sentinel szhint notwithstanding.
 *   5. A reserved flat GUID (arts_guid_reserve, an explicit-rank range's
 *      members, a round-robin range's members) resolves to its reserving
 *      thread's table on the reserving rank and to a remote shard on every
 *      other rank.
 *
 * No runtime: route_table.c + guid.c + shared.c #include'd; OoO drain no-op.
 * node_init runs the runtime's node init for one simulated rank.
 */

#include "arts/utils/stdatomic_compat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 4;
void arts_abort(uint8_t code) { _exit(code ? code : 70); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_calloc_aligned(size_t n, size_t s, size_t a) {
  void *p = NULL;
  if (posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, n * s)) {
    return NULL;
  }
  memset(p, 0, n * s);
  return p;
}
void arts_free(void *p) { free(p); }
#ifdef ARTS_DEBUG_INJECTION
/* route_table.c's windows are scheduling-fuzz points in an injection tree. */
void arts_sched_fuzz_point(void) {}
#endif

struct arts_route_item_s;
void arts_ooo_drain(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_redrive_all(struct arts_route_item_s *s) { (void)s; }
void arts_ooo_free_all(struct arts_route_item_s *s) { (void)s; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/core/gas/route_table.c"
#include "../../libs/src/core/utils/shared.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#define FAIL(...)                                                              \
  do {                                                                         \
    (void)fprintf(stderr, "FAIL route_table_db_shard: " __VA_ARGS__);          \
    return 1;                                                                  \
  } while (0)

#define NRANK 4u
#define NTABLES 3u

static void rt_init(void) {
  arts_node_info.route_table =
      (arts_route_table_t **)calloc(NTABLES, sizeof(arts_route_table_t *));
  for (unsigned t = 0; t < NTABLES; t++) {
    arts_node_info.route_table[t] = arts_new_route_table(256, 8);
  }
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    arts_node_info.remote_route_table[s] = arts_new_route_table(64, 6);
  }
}

static void node_fini(void) {
  if (arts_node_info.keys != NULL) {
    for (unsigned t = 0; t < arts_node_info.total_thread_count; t++) {
      free(arts_node_info.keys[t]);
    }
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
  arts_guid_generator_cleanup();
}

/* The runtime's node init for rank `rank` of NRANK with NTABLES threads: the
 * geometry, then each thread's key counters.  Leaves thread 0 current. */
static void node_init(unsigned int rank) {
  node_fini();
  arts_global_rank_id = rank;
  arts_global_rank_count = NRANK;
  arts_node_info.total_thread_count = NTABLES;
  arts_node_info.keys = (uint64_t **)calloc(NTABLES, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  for (unsigned t = 0; t < NTABLES; t++) {
    arts_thread_info.thread_id = t;
    arts_guid_key_generator_init();
  }
  arts_thread_info.thread_id = 0;
}

/* The local table the classifier picks for `g`, -1 for the remote shard its
 * key's low bits name, -2 for anything else. */
static int table_of(arts_guid_t g) {
  arts_route_table_t *t = arts_get_route_table(g);
  for (unsigned i = 0; i < NTABLES; i++) {
    if (t == arts_node_info.route_table[i]) {
      return (int)i;
    }
  }
  uint64_t key = ARTS_GUID_GET_KEY(g);
  return t == arts_node_info
                     .remote_route_table[key & (ARTS_REMOTE_ROUTE_SHARDS - 1)]
             ? -1
             : -2;
}

/* The local table of a DB key on the rank whose creator slice holds it. */
static int db_table(arts_guid_t g) {
  uint64_t seq = ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(g));
  return (int)((seq >> ARTS_GUID_DB_CHUNK_BITS) % NTABLES);
}

#define MEMBERS 8u

int main(void) {
  rt_init();

  /* ---- 1. before node init: every key -> remote. ---- */
  if (table_of(ARTS_GUID_MAKE(ARTS_GUID_DB, 0,
                              ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, 5))) !=
          -1 ||
      table_of(ARTS_GUID_MAKE(ARTS_GUID_EVENT, 0, 5)) != -1) {
    FAIL("a key took a local table before node init\n");
  }

  /* ---- 2. self-created keys: local table, chunk-granular agreement. ---- */
  node_init(0);
  arts_guid_t a = arts_guid_create_for_rank(1, ARTS_GUID_DB);
  arts_guid_t b = arts_guid_create_for_rank(1, ARTS_GUID_DB);
  if (table_of(a) != db_table(a)) {
    FAIL("self key not at (seq>>CHUNK)%%tables\n");
  }
  if (table_of(b) != table_of(a)) {
    FAIL("chunk-mates split across tables\n");
  }

  /* ---- 3. foreign-creator key: remote here, local at the creator. ---- */
  uint64_t foreign_seq = arts_db_seq_budget * 2 + 5; /* rank 2's slice */
  arts_guid_t foreign = ARTS_GUID_MAKE(
      ARTS_GUID_DB, 0, ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, foreign_seq));
  if (table_of(foreign) != -1) {
    FAIL("foreign-creator DB key did not resolve remote\n");
  }
  node_init(2);
  if (table_of(foreign) != db_table(foreign)) {
    FAIL("creator's view of its own key is not local\n");
  }

  /* ---- 4. reserved DB members: local at the reserving rank only. ---- */
  node_init(1);
  arts_guid_t db_member[2 * MEMBERS];
  arts_guid_t single = arts_guid_reserve_range(ARTS_GUID_DB, MEMBERS, 3);
  arts_guid_t rr =
      arts_guid_reserve_range(ARTS_GUID_DB, MEMBERS, ARTS_HINT_ROUND_ROBIN);
  for (unsigned i = 0; i < MEMBERS; i++) {
    db_member[i] = arts_guid_from_index(single, i);
    db_member[MEMBERS + i] = arts_guid_from_index(rr, i);
  }
  for (unsigned i = 0; i < 2 * MEMBERS; i++) {
    if (table_of(db_member[i]) != db_table(db_member[i])) {
      FAIL("reserved DB member %u not in the creator-slice local table\n", i);
    }
  }
  for (unsigned r = 0; r < NRANK; r++) {
    if (r == 1) {
      continue;
    }
    node_init(r);
    for (unsigned i = 0; i < 2 * MEMBERS; i++) {
      if (table_of(db_member[i]) != -1) {
        FAIL("reserved DB member %u took a local table on rank %u\n", i, r);
      }
    }
  }

  /* ---- 5. reserved flat GUIDs: the reserver's table there, remote
   * elsewhere. ---- */
  enum { PER_THREAD = 1 + 4 + MEMBERS };
  arts_guid_t flat[NTABLES][PER_THREAD];
  node_init(2);
  for (unsigned t = 0; t < NTABLES; t++) {
    arts_thread_info.thread_id = t;
    unsigned n = 0;
    flat[t][n++] = arts_guid_reserve(ARTS_GUID_EVENT, 3);
    arts_guid_t er = arts_guid_reserve_range(ARTS_GUID_EDT, 4, 0);
    for (unsigned i = 0; i < 4; i++) {
      flat[t][n++] = arts_guid_from_index(er, i);
    }
    arts_guid_t frr = arts_guid_reserve_range(ARTS_GUID_EVENT, MEMBERS,
                                              ARTS_HINT_ROUND_ROBIN);
    for (unsigned i = 0; i < MEMBERS; i++) {
      flat[t][n++] = arts_guid_from_index(frr, i);
    }
    for (unsigned i = 0; i < PER_THREAD; i++) {
      if (table_of(flat[t][i]) != (int)t) {
        FAIL("flat GUID %u reserved by thread %u left its table\n", i, t);
      }
    }
  }
  arts_thread_info.thread_id = 0;
  for (unsigned r = 0; r < NRANK; r++) {
    if (r == 2) {
      continue;
    }
    node_init(r);
    for (unsigned t = 0; t < NTABLES; t++) {
      for (unsigned i = 0; i < PER_THREAD; i++) {
        if (table_of(flat[t][i]) != -1) {
          FAIL("flat GUID %u of thread %u took a local table on rank %u\n", i,
               t, r);
        }
      }
    }
  }

  node_fini();
  printf("PASS route_table_db_shard: remote before node init, chunk-granular "
         "locals, arithmetic creator recovery, reserved DB and flat GUIDs "
         "local at the reserver only\n");
  return 0;
}
