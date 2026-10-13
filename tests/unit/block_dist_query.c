/* SPDX-License-Identifier: Apache-2.0
 *
 * Pure-unit test of the block-distribution query arithmetic
 * (libs/src/graph/block_distribution.c).
 *
 * The query functions (block_size, get_owner, partition_start/end,
 * get_local_index, guid_for_vertex, guid_for_partition) and
 * arts_block_dist_internal_init are pure, single-threaded arithmetic over a
 * caller-owned arts_block_dist_t.  Dists are built by hand; the init() path's
 * arts_get_total_ranks()/arts_guid_reserve() are stubbed below so its
 * round-robin GUID->rank assignment runs too.  Pinned:
 *
 *   - coverage: sum_{i in [0,num_blocks)} block_size(i) == num_vertices (no
 *     gap or overlap), for n divisible and not divisible by num_blocks, and
 *     num_blocks == 1.
 *   - contiguity: partition_end(i)+1 == partition_start(i+1) for non-last
 *     blocks; partition_end(last) == num_vertices-1.
 *   - get_owner boundary: vertex block_sz-1 -> block 0, block_sz -> block 1.
 *   - local-index round trip: get_local_index(v) == v - partition_start(owner).
 *   - guid_for_partition / guid_for_vertex return the GUID reserved at init for
 *     that block, on the rank the round-robin assigned.
 *   - degenerate shapes: fewer vertices than blocks leaves no block larger
 *     than the graph, an empty graph's last partition does not end at
 *     UINT64_MAX, and a distribution with no blocks never divides by zero (run
 *     in a child, so a fault is observed rather than suffered).
 *
 * The ARTS runtime is NOT started.  block_distribution.c is compiled via
 * #include against in-test stubs.
 */

#define ARTS_SYSTEM_PRINT_H
#define ARTS_INFO(...) ((void)0)
#define ARTS_DEBUG(...) ((void)0)
#define ARTS_WARN(...) ((void)0)
#define ARTS_ERROR(...) ((void)0)

#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/gas/guid.h" /* ARTS_GUID_MAKE / ARTS_GUID_GET_RANK */

/* --- allocator contract (mirrors libs/src/core/utils/malloc.c) -------------
 */
void *arts_malloc(size_t size) { return size ? malloc(size) : NULL; }
void arts_free(void *ptr) { free(ptr); }

/* --- stubbed runtime symbols used only by the init() paths ------------------
 * A fixed rank count + a rank-encoding GUID reservation let us exercise the
 * real round-robin loop in arts_block_dist_init and then verify each block's
 * GUID carries the rank the round-robin assigned. */
#define STUB_TOTAL_RANKS 4u
static unsigned int g_reserve_key = 1;
unsigned int arts_get_total_ranks(void) { return STUB_TOTAL_RANKS; }
arts_guid_t arts_guid_reserve(arts_guid_kind_t kind, unsigned int rank) {
  /* encode the rank so guid_for_* can be checked against the round-robin. */
  return ARTS_GUID_MAKE((uint64_t)kind, (uint64_t)rank,
                        (uint64_t)(g_reserve_key++));
}
/* init_from_args parses argv; not exercised here (covered by runtime tests). */

#include "../../libs/src/graph/block_distribution.c"

static int g_fail = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL block_dist_query: %s (%s:%d)\n", msg, __FILE__,    \
              __LINE__);                                                       \
      g_fail = 1;                                                              \
    }                                                                          \
  } while (0)

/* Build a dist by hand (no GUID table contents needed for arithmetic tests). */
static arts_block_dist_t *make_dist(arts_graph_sz_t n,
                                    unsigned int num_blocks) {
  arts_block_dist_t *d = (arts_block_dist_t *)malloc(
      sizeof(arts_block_dist_t) + sizeof(arts_guid_t) * num_blocks);
  memset(d, 0, sizeof(arts_block_dist_t) + sizeof(arts_guid_t) * num_blocks);
  arts_block_dist_internal_init(d, n, /*m=*/0, num_blocks);
  return d;
}

/* ---- coverage invariant + contiguity for a (n, num_blocks) config ---------
 */
static void check_partition_invariants(arts_graph_sz_t n,
                                       unsigned int num_blocks) {
  arts_block_dist_t *d = make_dist(n, num_blocks);

  /* ceil block_sz */
  arts_graph_sz_t expect_block_sz = (n + num_blocks - 1) / num_blocks;
  CHECK(d->block_sz == expect_block_sz, "internal_init: ceil block_sz");

  /* sum of block_size(i) == num_vertices */
  arts_graph_sz_t sum = 0;
  for (unsigned int i = 0; i < num_blocks; i++) {
    sum += arts_block_dist_block_size(i, d);
  }
  CHECK(sum == n, "coverage: sum block_size == num_vertices");

  /* contiguity: end(i)+1 == start(i+1) for non-last; end(last)==n-1 */
  for (unsigned int i = 0; i < num_blocks; i++) {
    arts_vertex_t start = arts_block_dist_partition_start(i, d);
    arts_vertex_t end = arts_block_dist_partition_end(i, d);
    CHECK(start == (arts_vertex_t)d->block_sz * i, "partition_start == bsz*i");
    if (i + 1 < num_blocks) {
      arts_vertex_t next_start = arts_block_dist_partition_start(i + 1, d);
      CHECK(end + 1 == next_start, "contiguity end(i)+1 == start(i+1)");
    } else {
      CHECK(end == n - 1, "partition_end(last) == num_vertices-1");
    }
    /* block_size(i) == end-start+1 */
    CHECK(arts_block_dist_block_size(i, d) == end - start + 1,
          "block_size == end-start+1");
  }
  free(d);
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
  arts_block_dist_t *d = make_dist(8, 3); /* block_sz=3 -> blocks 0:[0..2]
                                             1:[3..5] 2:[6..7] */
  CHECK(arts_block_dist_get_owner(0, d) == 0, "owner(0)==0");
  CHECK(arts_block_dist_get_owner(2, d) == 0, "owner(block_sz-1)==0");
  CHECK(arts_block_dist_get_owner(3, d) == 1, "owner(block_sz)==1");
  CHECK(arts_block_dist_get_owner(5, d) == 1, "owner(5)==1");
  CHECK(arts_block_dist_get_owner(6, d) == 2, "owner(6)==2");
  CHECK(arts_block_dist_get_owner(7, d) == 2, "owner(last)==2");

  /* local-index round-trip: first vertex of block -> 0; last -> bsz-1 */
  for (arts_vertex_t v = 0; v < 8; v++) {
    unsigned int owner = arts_block_dist_get_owner(v, d);
    arts_vertex_t start = arts_block_dist_partition_start(owner, d);
    arts_local_index_t li = arts_block_dist_get_local_index(v, d);
    CHECK(li == v - start, "local_index == v - partition_start(owner)");
    CHECK(li < arts_block_dist_block_size(owner, d),
          "local_index < block_size(owner)");
  }
  CHECK(arts_block_dist_get_local_index(0, d) == 0, "first-of-block0 -> 0");
  CHECK(arts_block_dist_get_local_index(3, d) == 0, "first-of-block1 -> 0");
  CHECK(arts_block_dist_get_local_index(5, d) == 2, "last-of-block1 -> bsz-1");
  free(d);
}

/* ---- init() round-robin GUID->rank assignment -----------------------------
 */
/* Rank r owns num_blocks / ranks blocks, plus one for the first
 * num_blocks % ranks ranks, filled in contiguous partition-index order (rank 0
 * the lowest indices).  The rank packed into each block's GUID must match. */
static void check_roundrobin(unsigned int num_blocks) {
  g_reserve_key = 1;
  arts_block_dist_t *d =
      arts_block_dist_init(/*n=*/60, /*m=*/0, num_blocks, ARTS_GUID_DB);
  CHECK(d != NULL, "init returned non-NULL");
  if (d == NULL) {
    return;
  }
  CHECK(d->num_blocks == num_blocks, "init num_blocks");
  unsigned int per_rank = num_blocks / STUB_TOTAL_RANKS;
  unsigned int extra = num_blocks % STUB_TOTAL_RANKS;
  unsigned int blk = 0;
  for (unsigned int r = 0; r < STUB_TOTAL_RANKS; r++) {
    unsigned int owned = per_rank + (r < extra ? 1u : 0u);
    for (unsigned int j = 0; j < owned && blk < num_blocks; j++, blk++) {
      arts_guid_t g = arts_block_dist_guid_for_partition(d, blk);
      CHECK((unsigned int)ARTS_GUID_GET_RANK(g) == r,
            "guid_for_partition rank == round-robin");
    }
  }
  CHECK(blk == num_blocks, "round-robin covers every block exactly once");
  /* guid_for_vertex: a vertex inside block num_blocks / 2 returns that
   * block's GUID. */
  unsigned int mid = num_blocks / 2u;
  arts_vertex_t v = (arts_vertex_t)mid * d->block_sz + 1u;
  CHECK(arts_block_dist_guid_for_vertex(v, d) ==
            arts_block_dist_guid_for_partition(d, mid),
        "guid_for_vertex == guid_for_partition of its block");
  free(d);
}

/* Over STUB_TOTAL_RANKS ranks: blocks not divisible by the ranks, one block
 * per rank, more ranks than blocks (some own none), and many blocks per rank
 * with and without a remainder. */
static void test_init_roundrobin(void) {
  static const unsigned int counts[] = {6, 4, 9, 3, 12};
  for (unsigned int i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    check_roundrobin(counts[i]);
  }
}

/* ======================= degenerate shapes ================================
 */

/* Fewer vertices than blocks: n=3, num_blocks=5 -> block_sz = ceil(3/5) = 1,
 * so (num_blocks-1)*block_sz exceeds n and the last block must not take the
 * unsigned difference. */
static void test_last_block_no_underflow(void) {
  arts_block_dist_t *d = make_dist(3, 5);
  CHECK(d->block_sz == 1, "setup: ceil(3/5)==1");
  arts_graph_sz_t last = arts_block_dist_block_size(4, d);
  CHECK(last <= d->num_vertices,
        "last block_size must not exceed num_vertices when n < num_blocks");
  free(d);
}

static void test_empty_partition_end(void) {
  arts_block_dist_t *d = make_dist(0, 1);
  arts_vertex_t end = arts_block_dist_partition_end(0, d);
  CHECK(end != UINT64_MAX,
        "partition_end on an empty graph must not underflow to UINT64_MAX");
  free(d);
}

/* num_blocks == 0 gives block_sz == 0.  The child reaches _exit(0) only if
 * get_owner did not divide by zero; a raw SIGFPE or a sanitizer's abort ends
 * it any other way. */
static void child_no_blocks_owner(void) {
  arts_block_dist_t *d = make_dist(10, 0);
  volatile unsigned int owner = arts_block_dist_get_owner(5, d);
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

int main(void) {
  test_partition_arithmetic();
  test_owner_and_local_index();
  test_init_roundrobin();
  test_last_block_no_underflow();
  test_empty_partition_end();
  test_no_blocks_no_divzero();

  if (g_fail) {
    fprintf(stderr, "FAIL block_dist_query: one or more checks failed\n");
    return 1;
  }
  printf("PASS block_dist_query: coverage/contiguity/owner/local-index "
         "round-trip + round-robin GUID rank; no underflow/div-by-zero\n");
  return 0;
}
