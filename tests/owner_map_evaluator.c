/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file owner_map_evaluator.c
/// @brief Unit + negative checks for the distributed-DB owner-map evaluator.
///
/// Pure single-process test: the evaluator is a pure function of its arguments,
/// so node count and current node are passed directly (no runtime launch). The
/// positive checks pin exact owner ranks for the two committed kinds, sweep
/// whole block grids against an independent reference of the committed formula,
/// and check the owner survives the GUID rank-field ABI: it fits the rank field
/// and round-trips through ARTS_GUID_MAKE / ARTS_GUID_GET_RANK (the committed
/// formula itself is pinned by the independent boundary-interval reference, not
/// by that round-trip). The negative checks prove
/// every malformed/unsupported input fails closed and leaves the output
/// untouched.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts/gas/guid.h"
#include "arts/gas/owner_map.h"

static int failures = 0;

static bool require_true(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
  return condition;
}

/// Independent contiguous owner via boundary intervals: rank r owns
/// owner_linear in [ceil(r*S/N), ceil((r+1)*S/N)). This is a different
/// construction than the implementation's floor(owner_linear*N/S), so a shared
/// formula error cannot survive in both the test and the implementation.
static unsigned int boundary_contiguous_owner(int64_t owner_linear,
                                              int64_t owner_space,
                                              unsigned int nodes) {
  for (unsigned int r = 0; r < nodes; ++r) {
    int64_t lo = ((int64_t)r * owner_space + (int64_t)nodes - 1) / (int64_t)nodes;
    int64_t hi =
        ((int64_t)(r + 1) * owner_space + (int64_t)nodes - 1) / (int64_t)nodes;
    if (owner_linear >= lo && owner_linear < hi)
      return r;
  }
  return nodes; /// unreachable for owner_linear in [0, owner_space)
}

/// Independent reference. linear_mod_nodes uses the sweep's known linear index
/// directly (so the implementation's own re-linearization is checked against
/// ground truth); owner_dim_contiguous uses the boundary-interval method.
static unsigned int reference_owner(arts_owner_map_kind_t kind,
                                    const int64_t *grid, const int64_t *coords,
                                    size_t ndims, const int64_t *owner_dims,
                                    size_t owner_dim_count, unsigned int nodes,
                                    int64_t linear) {
  if (kind == ARTS_OWNER_MAP_LINEAR_MOD_NODES)
    return (unsigned int)(linear % (int64_t)nodes);
  int64_t owner_linear = 0;
  int64_t owner_space = 1;
  for (size_t k = 0; k < owner_dim_count; ++k) {
    int64_t d = owner_dims[k];
    owner_linear = owner_linear * grid[d] + coords[d];
    owner_space *= grid[d];
  }
  return boundary_contiguous_owner(owner_linear, owner_space, nodes);
}

/// Row-major un-ranking by an algorithm independent of the evaluator's: peel
/// the trailing dim first via modulo, so a shared linearization mistake cannot
/// hide in both the test and the implementation.
static void unrank(const int64_t *grid, size_t ndims, int64_t linear,
                   int64_t *coords) {
  for (size_t i = ndims; i-- > 0;) {
    coords[i] = linear % grid[i];
    linear /= grid[i];
  }
}

/// Sweep every block of a grid: assert the coords and linear-index entry points
/// agree with each other, with the reference formula, with the [0, nodes) range
/// bound, and with the GUID rank field.
static void sweep(arts_owner_map_kind_t kind, const int64_t *grid, size_t ndims,
                  const int64_t *owner_dims, size_t owner_dim_count,
                  unsigned int nodes, const char *tag) {
  int64_t count = 1;
  for (size_t i = 0; i < ndims; ++i)
    count *= grid[i];

  for (int64_t linear = 0; linear < count; ++linear) {
    int64_t coords[ARTS_OWNER_MAP_MAX_RANK];
    unrank(grid, ndims, linear, coords);

    unsigned int expected = reference_owner(kind, grid, coords, ndims,
                                            owner_dims, owner_dim_count, nodes,
                                            linear);
    unsigned int by_coords = (unsigned int)-1;
    unsigned int by_linear = (unsigned int)-1;
    arts_owner_map_status_t sc = arts_owner_map_owner_for_coords(
        kind, grid, coords, ndims, owner_dims, owner_dim_count, nodes,
        &by_coords);
    arts_owner_map_status_t sl = arts_owner_map_owner_for_linear_index(
        kind, grid, ndims, linear, owner_dims, owner_dim_count, nodes,
        &by_linear);

    require_true(sc == ARTS_OWNER_MAP_OK && sl == ARTS_OWNER_MAP_OK, tag);
    require_true(by_coords == expected && by_linear == expected, tag);
    require_true(by_coords < nodes, tag);

    /// ABI sanity: the owner fits the GUID rank field and round-trips through the
    /// pack/unpack macros. The route formula itself is pinned above, not here.
    arts_guid_t guid = ARTS_GUID_MAKE(ARTS_DB, by_coords, (uint64_t)linear);
    require_true((unsigned int)ARTS_GUID_GET_RANK(guid) == by_coords, tag);
  }
}

/// Assert a coords-eval returns OK with an exact, hand-computed owner.
static void expect_owner(arts_owner_map_kind_t kind, const int64_t *grid,
                         const int64_t *coords, size_t ndims,
                         const int64_t *owner_dims, size_t owner_dim_count,
                         unsigned int nodes, unsigned int expected,
                         const char *tag) {
  unsigned int owner = (unsigned int)-1;
  arts_owner_map_status_t st = arts_owner_map_owner_for_coords(
      kind, grid, coords, ndims, owner_dims, owner_dim_count, nodes, &owner);
  require_true(st == ARTS_OWNER_MAP_OK, tag);
  require_true(owner == expected, tag);
}

/// Assert a linear-index eval returns OK with an exact, hand-computed owner,
/// exercising the un-ranking path with a value independent of the test's own
/// unrank() helper.
static void expect_owner_linear(arts_owner_map_kind_t kind, const int64_t *grid,
                                size_t ndims, int64_t linear,
                                const int64_t *owner_dims,
                                size_t owner_dim_count, unsigned int nodes,
                                unsigned int expected, const char *tag) {
  unsigned int owner = (unsigned int)-1;
  arts_owner_map_status_t st = arts_owner_map_owner_for_linear_index(
      kind, grid, ndims, linear, owner_dims, owner_dim_count, nodes, &owner);
  require_true(st == ARTS_OWNER_MAP_OK, tag);
  require_true(owner == expected, tag);
}

/// Assert a coords-eval fails closed with the expected status and leaves the
/// output untouched.
static void expect_reject_coords(arts_owner_map_kind_t kind,
                                 const int64_t *grid, const int64_t *coords,
                                 size_t ndims, const int64_t *owner_dims,
                                 size_t owner_dim_count, unsigned int nodes,
                                 arts_owner_map_status_t want,
                                 const char *tag) {
  unsigned int owner = 0xABCDu;
  arts_owner_map_status_t st = arts_owner_map_owner_for_coords(
      kind, grid, coords, ndims, owner_dims, owner_dim_count, nodes, &owner);
  require_true(st == want, tag);
  require_true(owner == 0xABCDu, tag);
}

/// Assert a linear-eval fails closed with the expected status and leaves the
/// output untouched.
static void expect_reject_linear(arts_owner_map_kind_t kind,
                                 const int64_t *grid, size_t ndims,
                                 int64_t linear, const int64_t *owner_dims,
                                 size_t owner_dim_count, unsigned int nodes,
                                 arts_owner_map_status_t want,
                                 const char *tag) {
  unsigned int owner = 0xABCDu;
  arts_owner_map_status_t st = arts_owner_map_owner_for_linear_index(
      kind, grid, ndims, linear, owner_dims, owner_dim_count, nodes, &owner);
  require_true(st == want, tag);
  require_true(owner == 0xABCDu, tag);
}

int main(void) {
  const int64_t dim0[1] = {0};
  const int64_t dim1[1] = {1};
  const int64_t dims01[2] = {0, 1};
  const int64_t dims10[2] = {1, 0};

  /// --- linear_mod_nodes: exact hand-computed owners (cyclic). ---
  {
    const int64_t grid[1] = {4};
    const int64_t c2[1] = {2};
    const int64_t c3[1] = {3};
    expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c2, 1, NULL, 0, 2, 0,
                 "linear_mod_nodes [4] n2 block2 -> 0");
    expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c3, 1, NULL, 0, 2, 1,
                 "linear_mod_nodes [4] n2 block3 -> 1");
    expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c3, 1, NULL, 0, 1, 0,
                 "linear_mod_nodes [4] n1 block3 -> 0");
  }

  /// --- linear_mod_nodes ignores owner_dims: supplying a (non-all-dims) owner
  ///     dim set must neither change the owner nor be validated/consumed. The
  ///     compiler stamps a non-empty owner_map_dims for this kind, so a caller
  ///     forwarding committed facts hits exactly this shape. ---
  {
    const int64_t grid[2] = {2, 4};
    const int64_t c12[2] = {1, 2}; /// linear 1*4+2=6, 6%3=0
    const int64_t c13[2] = {1, 3}; /// linear 1*4+3=7, 7%3=1
    expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c12, 2, dim1, 1, 3, 0,
                 "linear_mod_nodes ignores owner_dims (6%3=0)");
    expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c13, 2, dim1, 1, 3, 1,
                 "linear_mod_nodes ignores owner_dims (7%3=1)");
  }

  /// --- owner_dim_contiguous: exact hand-computed owners (contiguous). ---
  {
    const int64_t grid[1] = {4};
    const int64_t c0[1] = {0};
    const int64_t c1[1] = {1};
    const int64_t c2[1] = {2};
    const int64_t c3[1] = {3};
    /// n2: [0,0,1,1]
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c0, 1, dim0, 1, 2, 0,
                 "contiguous [4] n2 block0 -> 0");
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c1, 1, dim0, 1, 2, 0,
                 "contiguous [4] n2 block1 -> 0");
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c2, 1, dim0, 1, 2, 1,
                 "contiguous [4] n2 block2 -> 1");
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c3, 1, dim0, 1, 2, 1,
                 "contiguous [4] n2 block3 -> 1");
    /// n4: [0,1,2,3]
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c3, 1, dim0, 1, 4, 3,
                 "contiguous [4] n4 block3 -> 3");
  }

  /// --- owner_dim_contiguous: only the owner dim changes the owner. ---
  {
    /// grid [4,2], owner dim 0 -> owner depends on coord0 only.
    const int64_t grid[2] = {4, 2};
    const int64_t leading[2] = {1, 0}; /// coord0=1 -> 0
    const int64_t trailing[2] = {2, 1}; /// coord0=2 -> 1
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, leading, 2, dim0, 1,
                 2, 0, "contiguous [4,2] dim0 n2 (1,0) -> 0");
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, trailing, 2, dim0, 1,
                 2, 1, "contiguous [4,2] dim0 n2 (2,1) -> 1");
  }
  {
    /// grid [2,4], non-leading owner dim 1 -> owner depends on coord1 only.
    const int64_t grid[2] = {2, 4};
    const int64_t low[2] = {1, 1};  /// coord1=1 -> 0
    const int64_t high[2] = {0, 3}; /// coord1=3 -> 1
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, low, 2, dim1, 1, 2,
                 0, "contiguous [2,4] dim1 n2 (1,1) -> 0");
    expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, high, 2, dim1, 1, 2,
                 1, "contiguous [2,4] dim1 n2 (0,3) -> 1");
  }

  /// --- owner_dim_contiguous via the linear-index entry point: hand-computed
  ///     owners over multi-dim grids, pinning the impl's leading-dim-stride
  ///     un-ranking with values NOT derived from the test's own unrank(). ---
  {
    /// grid [4,2] dim0 n2, linear 5 -> coords (2,1); owner_linear=2, space=4;
    /// floor(2*2/4)=1.
    const int64_t g42[2] = {4, 2};
    expect_owner_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g42, 2, 5, dim0, 1,
                        2, 1, "contiguous-linear [4,2] dim0 n2 idx5 -> 1");
    /// grid [2,4] dim1 n2, linear 7 -> coords (1,3); owner_linear=3, space=4;
    /// floor(3*2/4)=1.
    const int64_t g24[2] = {2, 4};
    expect_owner_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g24, 2, 7, dim1, 1,
                        2, 1, "contiguous-linear [2,4] dim1 n2 idx7 -> 1");
  }

  /// --- non-dividing node counts: uneven contiguous blocks, the realistic
  ///     regime where total_nodes does not divide the (sub-)space. ---
  {
    /// contiguous [6] dim0 n4: floor(i*4/6) = [0,0,1,2,2,3].
    const int64_t grid[1] = {6};
    const unsigned int want[6] = {0, 0, 1, 2, 2, 3};
    for (int64_t i = 0; i < 6; ++i) {
      const int64_t c[1] = {i};
      expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c, 1, dim0, 1, 4,
                   want[i], "contiguous [6] dim0 n4 uneven");
    }
  }
  {
    /// contiguous [5] dim0 n2: floor(i*2/5) = [0,0,0,1,1].
    const int64_t grid[1] = {5};
    const unsigned int want[5] = {0, 0, 0, 1, 1};
    for (int64_t i = 0; i < 5; ++i) {
      const int64_t c[1] = {i};
      expect_owner(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, c, 1, dim0, 1, 2,
                   want[i], "contiguous [5] dim0 n2 uneven");
    }
  }
  {
    /// linear_mod_nodes [5] n2: i % 2 = [0,1,0,1,0].
    const int64_t grid[1] = {5};
    const unsigned int want[5] = {0, 1, 0, 1, 0};
    for (int64_t i = 0; i < 5; ++i) {
      const int64_t c[1] = {i};
      expect_owner(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, c, 1, NULL, 0, 2,
                   want[i], "linear_mod_nodes [5] n2 uneven");
    }
  }

  /// --- whole-grid sweeps against the reference formula + GUID rank field. ---
  {
    const int64_t g6[1] = {6};
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g6, 1, NULL, 0, 1, "sweep lmn [6] n1");
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g6, 1, NULL, 0, 2, "sweep lmn [6] n2");
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g6, 1, NULL, 0, 3, "sweep lmn [6] n3");
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g6, 1, NULL, 0, 6, "sweep lmn [6] n6");

    const int64_t g23[2] = {2, 3};
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g23, 2, NULL, 0, 2,
          "sweep lmn [2,3] n2");
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g23, 2, NULL, 0, 3,
          "sweep lmn [2,3] n3");

    const int64_t g4[1] = {4};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g4, 1, dim0, 1, 1,
          "sweep contig [4] dim0 n1");
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g4, 1, dim0, 1, 2,
          "sweep contig [4] dim0 n2");
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g4, 1, dim0, 1, 4,
          "sweep contig [4] dim0 n4");

    const int64_t g42[2] = {4, 2};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g42, 2, dim0, 1, 2,
          "sweep contig [4,2] dim0 n2");
    const int64_t g24[2] = {2, 4};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g24, 2, dim1, 1, 2,
          "sweep contig [2,4] dim1 n2");
    const int64_t g22[2] = {2, 2};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g22, 2, dims01, 2, 4,
          "sweep contig [2,2] dim01 n4");
    const int64_t g33[2] = {3, 3};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g33, 2, dims01, 2, 3,
          "sweep contig [3,3] dim01 n3");

    /// non-dividing node counts (uneven block sizes); reuses g6 = {6} above.
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g6, 1, dim0, 1, 4,
          "sweep contig [6] dim0 n4 uneven");
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g6, 1, NULL, 0, 4,
          "sweep lmn [6] n4 uneven");
    const int64_t g32[2] = {3, 2};
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g32, 2, dims01, 2, 4,
          "sweep contig [3,2] dim01 n4 uneven");
    /// non-identity owner-slot order: owner_dims {1,0} must honor committed order
    /// (no internal sort); reference_owner iterates dims in the given order, so a
    /// future reorder/sort regression diverges and is caught.
    sweep(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, g32, 2, dims10, 2, 4,
          "sweep contig [3,2] dim10 n4 order");
    const int64_t g5[1] = {5};
    sweep(ARTS_OWNER_MAP_LINEAR_MOD_NODES, g5, 1, NULL, 0, 2,
          "sweep lmn [5] n2 uneven");
  }

  /// --- negative / fail-closed: unrealized and unknown kinds. ---
  {
    const int64_t grid[1] = {4};
    const int64_t coords[1] = {0};
    expect_reject_coords(ARTS_OWNER_MAP_OWNER_DIM_GRID, grid, coords, 1, dim0, 1,
                         2, ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND,
                         "owner_dim_grid rejected");
    expect_reject_coords(ARTS_OWNER_MAP_EXPLICIT_RANK_TABLE, grid, coords, 1,
                         dim0, 1, 2, ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND,
                         "explicit_rank_table rejected");
    expect_reject_coords((arts_owner_map_kind_t)99, grid, coords, 1, dim0, 1, 2,
                         ARTS_OWNER_MAP_ERR_UNKNOWN_KIND, "unknown kind rejected");
    expect_reject_linear(ARTS_OWNER_MAP_OWNER_DIM_GRID, grid, 1, 0, dim0, 1, 2,
                         ARTS_OWNER_MAP_ERR_UNSUPPORTED_KIND,
                         "owner_dim_grid rejected (linear)");
  }

  /// --- negative / fail-closed: malformed structural inputs. ---
  {
    const int64_t grid[1] = {4};
    const int64_t coords[1] = {0};
    const int64_t bad_grid[2] = {4, 0};
    const int64_t bad_coords[2] = {0, 0};
    const int64_t overflow_grid[2] = {(int64_t)INT32_MAX, 2};
    const int64_t big_rank[ARTS_OWNER_MAP_MAX_RANK + 1] = {1, 1, 1, 1,
                                                           1, 1, 1, 1, 1};
    const int64_t big_coords[ARTS_OWNER_MAP_MAX_RANK + 1] = {0};
    const int64_t oob_coords[1] = {4};
    const int64_t neg_coords[1] = {-1};

    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, coords, 1, NULL,
                         0, 0, ARTS_OWNER_MAP_ERR_NODE_COUNT,
                         "zero node count rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, coords, 0, NULL,
                         0, 2, ARTS_OWNER_MAP_ERR_RANK, "zero rank rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, big_rank, big_coords,
                         ARTS_OWNER_MAP_MAX_RANK + 1, NULL, 0, 2,
                         ARTS_OWNER_MAP_ERR_RANK, "oversized rank rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, NULL, coords, 1, NULL,
                         0, 2, ARTS_OWNER_MAP_ERR_NULL_ARG, "null grid rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, NULL, 1, NULL, 0,
                         2, ARTS_OWNER_MAP_ERR_NULL_ARG, "null coords rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, bad_grid, bad_coords,
                         2, NULL, 0, 2, ARTS_OWNER_MAP_ERR_DB_SIZE,
                         "non-positive extent rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, overflow_grid,
                         bad_coords, 2, NULL, 0, 2,
                         ARTS_OWNER_MAP_ERR_SIZE_OVERFLOW,
                         "grid-product overflow rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, oob_coords, 1,
                         NULL, 0, 2, ARTS_OWNER_MAP_ERR_COORD_RANGE,
                         "out-of-range coord rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, neg_coords, 1,
                         NULL, 0, 2, ARTS_OWNER_MAP_ERR_COORD_RANGE,
                         "negative coord rejected");
    expect_reject_coords(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, coords, 1, NULL,
                         0, (unsigned int)INT32_MAX + 1u,
                         ARTS_OWNER_MAP_ERR_NODE_COUNT,
                         "oversized node count rejected");

    /// out parameter NULL (cannot check unchanged; status only).
    arts_owner_map_status_t st = arts_owner_map_owner_for_coords(
        ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, coords, 1, NULL, 0, 2, NULL);
    require_true(st == ARTS_OWNER_MAP_ERR_NULL_ARG, "null out rejected");
  }

  /// --- negative / fail-closed: owner_dim_contiguous owner-dim faults. ---
  {
    const int64_t grid[1] = {4};
    const int64_t coords[1] = {0};
    const int64_t oob_dim[1] = {1}; /// dim 1 >= rank 1
    const int64_t dup_dims[2] = {0, 0};
    expect_reject_coords(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, coords, 1,
                         NULL, 0, 2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "empty owner dims rejected");
    expect_reject_coords(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, coords, 1,
                         oob_dim, 1, 2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "out-of-range owner dim rejected");
    expect_reject_coords(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, coords, 1,
                         dup_dims, 2, 2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "duplicate owner dim rejected");
    expect_reject_coords(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, coords, 1,
                         NULL, 1, 2, ARTS_OWNER_MAP_ERR_NULL_ARG,
                         "null owner dims with count rejected");
  }

  /// --- negative / fail-closed: linear-index range faults. ---
  {
    const int64_t grid[1] = {4};
    expect_reject_linear(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, 1, 4, NULL, 0, 2,
                         ARTS_OWNER_MAP_ERR_LINEAR_RANGE,
                         "linear index == count rejected");
    expect_reject_linear(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, 1, -1, NULL, 0,
                         2, ARTS_OWNER_MAP_ERR_LINEAR_RANGE,
                         "negative linear index rejected");
    expect_reject_linear(ARTS_OWNER_MAP_LINEAR_MOD_NODES, grid, 1, 0, NULL, 0, 0,
                         ARTS_OWNER_MAP_ERR_NODE_COUNT,
                         "zero node count rejected (linear)");
  }

  /// --- negative / fail-closed: the linear-index entry point delivers the same
  ///     structural and owner-dim contract as the coords path. Grid faults must
  ///     fire before the linear-range check consumes the block count; owner_dims
  ///     faults are delivered by delegation (un-rank uses only the grid, then
  ///     owner_for_coords validates), so a future inline refactor cannot quietly
  ///     drop them. ---
  {
    const int64_t bad_grid[2] = {4, 0};
    const int64_t overflow_grid[2] = {(int64_t)INT32_MAX, 2};
    const int64_t grid[1] = {4};
    const int64_t oob_dim[1] = {1};
    const int64_t dup_dims[2] = {0, 0};

    expect_reject_linear(ARTS_OWNER_MAP_LINEAR_MOD_NODES, bad_grid, 2, 0, NULL, 0,
                         2, ARTS_OWNER_MAP_ERR_DB_SIZE,
                         "non-positive extent rejected (linear)");
    expect_reject_linear(ARTS_OWNER_MAP_LINEAR_MOD_NODES, overflow_grid, 2, 0,
                         NULL, 0, 2, ARTS_OWNER_MAP_ERR_SIZE_OVERFLOW,
                         "grid-product overflow rejected (linear)");
    expect_reject_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, 1, 0, NULL, 0,
                         2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "empty owner dims rejected (linear)");
    expect_reject_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, 1, 0, NULL, 1,
                         2, ARTS_OWNER_MAP_ERR_NULL_ARG,
                         "null owner dims with count rejected (linear)");
    expect_reject_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, 1, 0, oob_dim,
                         1, 2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "out-of-range owner dim rejected (linear)");
    expect_reject_linear(ARTS_OWNER_MAP_OWNER_DIM_CONTIGUOUS, grid, 1, 0,
                         dup_dims, 2, 2, ARTS_OWNER_MAP_ERR_OWNER_DIMS,
                         "duplicate owner dim rejected (linear)");
  }

  /// --- status strings: every defined status maps to a distinct, non-default
  ///     name, guarding the default-less switch against a future enumerator
  ///     silently falling through to "unknown status". ---
  {
    for (int s = ARTS_OWNER_MAP_OK; s <= ARTS_OWNER_MAP_ERR_UNKNOWN_KIND; ++s) {
      const char *name = arts_owner_map_status_str((arts_owner_map_status_t)s);
      require_true(name != NULL, "status_str non-null");
      require_true(strcmp(name, "unknown status") != 0,
                   "status_str maps a defined status");
    }
  }

  if (failures == 0)
    printf("owner_map_evaluator: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
