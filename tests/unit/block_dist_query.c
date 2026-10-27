/* SPDX-License-Identifier: Apache-2.0
 *
 * Pure-unit test of the block distribution
 * (libs/src/graph/block_distribution.c) on the real GUID generator.
 *
 * guid.c is #include'd with libc-backed shims and node_init runs the
 * runtime's node init for rank 0 of N ranks, so arts_block_dist_create
 * reserves its range through the real arts_guid_reserve_range and the GUID
 * queries derive members through the real arts_guid_from_index;
 * arts_get_total_ranks returns the same N.  Pinned:
 *
 *   - coverage: sum_{i in [0,num_blocks)} block_size(i) == num_vertices (no
 *     gap or overlap), for n divisible and not divisible by num_blocks, and
 *     num_blocks == 1.
 *   - contiguity: partition_end(i)+1 == partition_start(i+1) for non-last
 *     blocks; partition_end(last) == num_vertices-1.
 *   - get_owner boundary: vertex block_sz-1 -> block 0, block_sz -> block 1.
 *   - local-index round trip: get_local_index(v) == v - partition_start(owner).
 *   - placement, over N = 4 ranks, for block counts {6, 4, 9, 3, 12} and the
 *     degenerate shapes: partition p's GUID is homed on the rank that holds p
 *     (rank r holds num_blocks / N consecutive partitions, plus one for the
 *     first num_blocks % N ranks), its member index is below
 *     N * ceil(num_blocks / N), no two partitions share an index, and
 *     guid_for_vertex returns its block's GUID.
 *   - refusals, each in a forked child that must end through ARTS_ERROR: a
 *     distribution needing more than UINT_MAX names, a GUID query of a
 *     past-end partition, and a GUID query of a 0-block distribution.
 *   - a 0-block distribution reserves nothing (range NULL_GUID, block size 0)
 *     and get_owner never divides by zero on it (in a child, so a fault is
 *     observed rather than suffered).
 *   - degenerate shapes: fewer vertices than blocks leaves the blocks past
 *     the last vertex empty (size 0), and an empty graph's last partition
 *     does not end at UINT64_MAX.
 *   - the largest range: at N = 3, num_blocks = UINT_MAX - 1 (a range of
 *     UINT_MAX names) is accepted, its last partition's member index is below
 *     UINT_MAX, and a DB reserve made after it on every home lies above that
 *     home's last member.
 *
 * The ARTS runtime is NOT started.
 */

#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── runtime symbol shims (must precede the #include of guid.c) ─────────── */
unsigned int arts_global_rank_id = 0;
unsigned int arts_global_rank_count = 1;
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
unsigned int arts_get_total_ranks(void) { return arts_global_rank_count; }

#include "../../libs/src/core/gas/guid.c"
#include "../../libs/src/graph/block_distribution.c"

struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL block_dist_query: %s (%s:%d)\n", msg, __FILE__,    \
              __LINE__);                                                       \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

static void node_fini(void) {
  if (arts_node_info.keys != NULL) {
    free(arts_node_info.keys[0]);
    free(arts_node_info.keys);
    arts_node_info.keys = NULL;
  }
  arts_guid_generator_cleanup();
}

/* The runtime's node init for rank 0 of `nrank` with one thread. */
static void node_init(unsigned int nrank) {
  node_fini();
  arts_global_rank_id = 0;
  arts_global_rank_count = nrank;
  arts_node_info.total_thread_count = 1;
  arts_node_info.keys = (uint64_t **)calloc(1, sizeof(uint64_t *));
  arts_guid_node_geometry_init();
  arts_thread_info.thread_id = 0;
  arts_guid_key_generator_init();
}

/* Runs `fn` in a forked child; true when the child ended through
 * arts_abort(1), the exit every ARTS_ERROR takes. */
static bool dies(void (*fn)(void)) {
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int st = 0;
  if (pid < 0 || waitpid(pid, &st, 0) != pid) {
    return false;
  }
  return WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

static arts_block_dist_t make_dist(arts_graph_sz_t n, unsigned int num_blocks) {
  return arts_block_dist_create(n, /*m=*/0, num_blocks, ARTS_GUID_DB);
}

/* ---- coverage invariant + contiguity for a (n, num_blocks) config ---------
 */
static void check_partition_invariants(arts_graph_sz_t n,
                                       unsigned int num_blocks) {
  arts_block_dist_t d = make_dist(n, num_blocks);

  /* ceil block_sz */
  arts_graph_sz_t expect_block_sz = (n + num_blocks - 1) / num_blocks;
  CHECK(d.block_sz == expect_block_sz, "create: ceil block_sz");

  /* sum of block_size(i) == num_vertices */
  arts_graph_sz_t sum = 0;
  for (unsigned int i = 0; i < num_blocks; i++) {
    sum += arts_block_dist_block_size(i, &d);
  }
  CHECK(sum == n, "coverage: sum block_size == num_vertices");

  /* contiguity: end(i)+1 == start(i+1) for non-last; end(last)==n-1 */
  for (unsigned int i = 0; i < num_blocks; i++) {
    arts_vertex_t start = arts_block_dist_partition_start(i, &d);
    arts_vertex_t end = arts_block_dist_partition_end(i, &d);
    CHECK(start == (arts_vertex_t)d.block_sz * i, "partition_start == bsz*i");
    if (i + 1 < num_blocks) {
      arts_vertex_t next_start = arts_block_dist_partition_start(i + 1, &d);
      CHECK(end + 1 == next_start, "contiguity end(i)+1 == start(i+1)");
    } else {
      CHECK(end == n - 1, "partition_end(last) == num_vertices-1");
    }
    /* block_size(i) == end-start+1 */
    CHECK(arts_block_dist_block_size(i, &d) == end - start + 1,
          "block_size == end-start+1");
  }
}

static void test_partition_arithmetic(void) {
  check_partition_invariants(64, 2); /* divisible: bsz 32,32 */
  check_partition_invariants(8, 3);  /* not divisible: bsz 3 -> 3,3,2 */
  check_partition_invariants(10, 1); /* single block -> whole range */
  check_partition_invariants(100, 7);
  check_partition_invariants(1, 1);
}

/* ---- get_owner boundary + local-index round-trip --------------------------
 */
static void test_owner_and_local_index(void) {
  arts_block_dist_t d = make_dist(8, 3); /* block_sz=3 -> blocks 0:[0..2]
                                            1:[3..5] 2:[6..7] */
  CHECK(arts_block_dist_get_owner(0, &d) == 0, "owner(0)==0");
  CHECK(arts_block_dist_get_owner(2, &d) == 0, "owner(block_sz-1)==0");
  CHECK(arts_block_dist_get_owner(3, &d) == 1, "owner(block_sz)==1");
  CHECK(arts_block_dist_get_owner(5, &d) == 1, "owner(5)==1");
  CHECK(arts_block_dist_get_owner(6, &d) == 2, "owner(6)==2");
  CHECK(arts_block_dist_get_owner(7, &d) == 2, "owner(last)==2");

  /* local-index round-trip: first vertex of block -> 0; last -> bsz-1 */
  for (arts_vertex_t v = 0; v < 8; v++) {
    unsigned int owner = arts_block_dist_get_owner(v, &d);
    arts_vertex_t start = arts_block_dist_partition_start(owner, &d);
    arts_local_index_t li = arts_block_dist_get_local_index(v, &d);
    CHECK(li == v - start, "local_index == v - partition_start(owner)");
    CHECK(li < arts_block_dist_block_size(owner, &d),
          "local_index < block_size(owner)");
  }
  CHECK(arts_block_dist_get_local_index(0, &d) == 0, "first-of-block0 -> 0");
  CHECK(arts_block_dist_get_local_index(3, &d) == 0, "first-of-block1 -> 0");
  CHECK(arts_block_dist_get_local_index(5, &d) == 2, "last-of-block1 -> bsz-1");
}

/* ---- placement: partition -> home rank and range member --------------------
 */
/* Rank r holds num_blocks / N partitions, plus one for the first
 * num_blocks % N ranks, in contiguous partition-index order (rank 0 the
 * lowest indices).  Each partition's GUID must be homed there and be a
 * distinct member of the distribution's range. */
static void check_roundrobin(arts_graph_sz_t n, unsigned int num_blocks) {
  arts_block_dist_t d = make_dist(n, num_blocks);
  CHECK(d.num_blocks == num_blocks, "create num_blocks");
  CHECK(d.range != NULL_GUID &&
            arts_guid_get_rank(d.range) == ARTS_DISTRIBUTED_RANK,
        "create reserved a round-robin range");
  unsigned int nrank = arts_get_total_ranks();
  uint64_t size =
      (uint64_t)nrank * (num_blocks / nrank + (num_blocks % nrank != 0));
  unsigned char *seen = (unsigned char *)calloc(size, 1);
  unsigned int per_rank = num_blocks / nrank;
  unsigned int extra = num_blocks % nrank;
  unsigned int blk = 0;
  for (unsigned int r = 0; r < nrank; r++) {
    unsigned int owned = per_rank + (r < extra ? 1u : 0u);
    for (unsigned int j = 0; j < owned && blk < num_blocks; j++, blk++) {
      arts_guid_t g = arts_block_dist_guid_for_partition(&d, blk);
      CHECK(arts_guid_get_rank(g) == r,
            "guid_for_partition home == the rank holding the partition");
      CHECK(arts_guid_get_kind(g) == ARTS_GUID_DB, "guid_for_partition kind");
      int64_t idx = arts_guid_index_from(d.range, g);
      CHECK(idx >= 0 && (uint64_t)idx < size,
            "member index below N * ceil(num_blocks / N)");
      if (idx >= 0 && (uint64_t)idx < size) {
        CHECK(!seen[idx], "two partitions share a member index");
        seen[idx] = 1;
      }
    }
  }
  free(seen);
  CHECK(blk == num_blocks, "the ranks hold every block exactly once");
  /* guid_for_vertex: a vertex inside block num_blocks / 2 returns that
   * block's GUID. */
  unsigned int mid = num_blocks / 2u;
  arts_vertex_t v = (arts_vertex_t)mid * d.block_sz + (d.block_sz - 1) / 2;
  if (v < n) {
    CHECK(arts_block_dist_guid_for_vertex(v, &d) ==
              arts_block_dist_guid_for_partition(&d, mid),
          "guid_for_vertex == guid_for_partition of its block");
  }
}

/* Over N ranks: blocks not divisible by the ranks, one block per rank, more
 * ranks than blocks (some own none), many blocks per rank with and without a
 * remainder; then the degenerate shapes: one block, fewer vertices than
 * blocks, no vertices. */
static void test_placement(void) {
  static const unsigned int counts[] = {6, 4, 9, 3, 12};
  for (unsigned int i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    check_roundrobin(60, counts[i]);
  }
  check_roundrobin(60, 1);
  check_roundrobin(3, 5);
  check_roundrobin(0, 1);
}

/* ---- refusals -------------------------------------------------------------
 */
static void create_oversize(void) {
  /* N * ceil(UINT_MAX / N) names at N = 4 is 2^32. */
  (void)make_dist(10, UINT_MAX);
}

static void query_past_end(void) {
  arts_block_dist_t d = make_dist(60, 6);
  (void)arts_block_dist_guid_for_partition(&d, 6);
}

static void query_no_blocks(void) {
  arts_block_dist_t d = make_dist(10, 0);
  (void)arts_block_dist_guid_for_partition(&d, 0);
}

static void test_refusals(void) {
  CHECK(dies(create_oversize),
        "a distribution needing more than UINT_MAX names was not refused");
  CHECK(dies(query_past_end), "a past-end partition's GUID was not refused");
  CHECK(dies(query_no_blocks),
        "a GUID query of a 0-block distribution was not refused");
}

/* ======================= degenerate shapes ================================
 */

/* Fewer vertices than blocks: n=3, num_blocks=5 -> block_sz = ceil(3/5) = 1,
 * so blocks 3 and 4 start at or past the last vertex and are empty.  Their
 * size is 0 (n - start would wrap the unsigned size), and the sizes still sum
 * to n. */
static void test_last_block_no_underflow(void) {
  arts_block_dist_t d = make_dist(3, 5);
  CHECK(d.block_sz == 1, "setup: ceil(3/5)==1");
  arts_graph_sz_t sum = 0;
  for (unsigned int i = 0; i < 5; i++) {
    arts_graph_sz_t sz = arts_block_dist_block_size(i, &d);
    CHECK(sz == (i < 3 ? 1u : 0u),
          "n=3, 5 blocks: blocks 0..2 hold one vertex, blocks 3..4 none");
    sum += sz;
  }
  CHECK(sum == d.num_vertices, "n=3, 5 blocks: sizes sum to num_vertices");
}

static void test_empty_partition_end(void) {
  arts_block_dist_t d = make_dist(0, 1);
  arts_vertex_t end = arts_block_dist_partition_end(0, &d);
  CHECK(end != UINT64_MAX,
        "partition_end on an empty graph must not underflow to UINT64_MAX");
}

/* A 0-block distribution claims no name on any home. */
static void test_no_blocks_reserves_nothing(void) {
  unsigned int nrank = arts_get_total_ranks();
  uint64_t before[4];
  for (unsigned int h = 0; h < nrank; h++) {
    before[h] = atomic_load_explicit(&db_seq_next[h], memory_order_relaxed);
  }
  arts_block_dist_t d = make_dist(10, 0);
  CHECK(d.num_blocks == 0 && d.range == NULL_GUID && d.block_sz == 0,
        "a 0-block distribution: range NULL_GUID, block size 0");
  for (unsigned int h = 0; h < nrank; h++) {
    CHECK(atomic_load_explicit(&db_seq_next[h], memory_order_relaxed) ==
              before[h],
          "a 0-block distribution claimed names");
  }
  CHECK(arts_block_dist_block_size(0, &d) == 0,
        "a 0-block distribution's block is empty");
}

/* num_blocks == 0 gives block_sz == 0.  The child reaches _exit(0) only if
 * get_owner did not divide by zero; a raw SIGFPE or a sanitizer's abort ends
 * it any other way. */
static void child_no_blocks_owner(void) {
  arts_block_dist_t d = make_dist(10, 0);
  volatile unsigned int owner = arts_block_dist_get_owner(5, &d);
  (void)owner;
  _exit(0);
}

static void test_no_blocks_no_divzero(void) {
  pid_t pid = fork();
  if (pid == 0) {
    child_no_blocks_owner();
    _exit(99);
  }
  CHECK(pid > 0, "fork for the num_blocks==0 child");
  int status = 0;
  waitpid(pid, &status, 0);
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
    fprintf(stderr,
            "  get_owner with num_blocks==0 ended the child (%s, "
            "status=0x%x)\n",
            WIFSIGNALED(status) && WTERMSIG(status) == SIGFPE ? "SIGFPE"
                                                              : "abort",
            status);
    CHECK(0, "get_owner must not divide by zero (num_blocks==0)");
  }
}

/* ---- the largest range ------------------------------------------------------
 */
static void test_largest_range(void) {
  node_init(3);
  const unsigned int nb = UINT_MAX - 1;
  arts_block_dist_t d = make_dist(1000, nb);
  CHECK(d.num_blocks == nb && d.range != NULL_GUID,
        "a distribution of UINT_MAX names was not reserved");
  if (d.range == NULL_GUID) {
    return;
  }
  int64_t last = arts_guid_index_from(
      d.range, arts_block_dist_guid_for_partition(&d, nb - 1));
  CHECK(last >= 0 && last < (int64_t)UINT_MAX,
        "the last partition's member index is not below UINT_MAX");
  /* Home h's last partition ends its run of nb / 3 partitions, plus one for
   * h < nb % 3. */
  unsigned int end = 0;
  for (unsigned int h = 0; h < 3; h++) {
    end += nb / 3 + (h < nb % 3 ? 1u : 0u);
    arts_guid_t member = arts_block_dist_guid_for_partition(&d, end - 1);
    arts_guid_t later = arts_guid_reserve(ARTS_GUID_DB, h);
    CHECK(arts_guid_get_rank(member) == h && arts_guid_get_rank(later) == h,
          "a home's last partition or later reserve is homed elsewhere");
    CHECK(ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(later)) >
              ARTS_GUID_DB_GET_SEQ(ARTS_GUID_GET_KEY(member)),
          "a DB reserve made after the largest range lies inside it");
  }
}

int main(void) {
  node_init(4);
  test_partition_arithmetic();
  test_owner_and_local_index();
  test_placement();
  test_refusals();
  test_last_block_no_underflow();
  test_empty_partition_end();
  test_no_blocks_reserves_nothing();
  test_no_blocks_no_divzero();
  test_largest_range();
  node_fini();

  if (g_fail) {
    fprintf(stderr, "FAIL block_dist_query: one or more checks failed\n");
    return 1;
  }
  printf("PASS block_dist_query: coverage/contiguity/owner/local-index "
         "round-trip, placement on the real generator, refusals, no "
         "underflow/div-by-zero, the largest range\n");
  return 0;
}
