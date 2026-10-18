# smithwaterman_example

*A sequence-alignment score computed as a wavefront of tiles: each tile EDT
receives its left, top and corner halos as data blocks through single-fire
events and emits its own three for the tiles right, below and diagonal of it.*
Source: `examples/proxy/smithwaterman/smithwaterman.c`, written against the
public `arts.h` alone; the native counterpart of the OCR `smithwaterman`
application, with its scoring and its input files.

## Overview

The DP matrix `M` has one row per character of the second string and one
column per character of the first; `M[i][j]` is the best of the diagonal
step (the pair's score: 2 for a match, −2 for a transition, −4 for a
transversion) and the two gap steps (−1), with the first row and column at
`−j` and `−i`. The answer is the bottom-right cell. The matrix is cut into
tiles of `tile_h × tile_w` cells; a tile needs the right column of its left
neighbour, the bottom row of its upper neighbour and the bottom-right cell of
its upper-left neighbour, computes its cells in a local scratch matrix and
passes its own right column, bottom row and corner on. The last tile puts its
corner cell in a score block for `done_edt` on rank 0, which prints
`SW-ARTS final score = <score> (expected <expected>) PASS` (or `FAIL`), the
expected score being the score file's.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`smithwaterman_example_arts_<variant>`), and no reference runtime
has a program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `argv[1]`, `argv[2]` | tile width and height in cells | must be positive |
| `argv[3]`, `argv[4]` | the two sequence files | only `A`, `C`, `G` and `T` are read, everything else skipped; each must hold at least one |
| `argv[5]` | the score file | its first integer is the expected score (0 if none) |

Fewer than five arguments, a non-positive tile size or a sequence with no
`ACGT` character prints the usage and shuts down; an unreadable sequence file
prints the path and shuts down. The checked-in pairs under
`examples/proxy/smithwaterman/datasets/` are tiny (8 × 8), small (9 × 9),
medium (50 × 50), medium-large (800 × 800) and large (100 713 × 101 133),
each with its score file. The smoke test runs the medium-large pair in
100 × 100 tiles, 64 tiles with expected score 1460.

## Structure

With `n_w = ⌈len₁ / tile_w⌉` and `n_h = ⌈len₂ / tile_h⌉`:

| object | count |
|---|---|
| `sw_tile_edt` | `n_h · n_w` |
| halo events | `3 · (n_h + 1) · (n_w + 1)` (right column, bottom row and corner per grid point, border included), plus the done event |
| params block | 1: tile and grid sizes and both strings |
| halo blocks | one per halo that has a consumer: from the tiles, `n_h · (n_w − 1)` right columns, `(n_h − 1) · n_w` bottom rows, `(n_h − 1) · (n_w − 1)` corners; from `main_edt`, the border's `n_w` rows, `n_h` columns and `n_w + n_h − 1` corners |
| score block, `done_edt` | 1 each |

## Wiring

Tile `(i, j)` takes, RO, the right-column event of `(i, j − 1)` on slot 0,
the bottom-row event of `(i − 1, j)` on slot 1, the corner event of
`(i − 1, j − 1)` on slot 2 and the params block on slot 3. It produces only
the halos whose consumer lies in the grid: a right column for `j < n_w`, a
bottom row for `i < n_h`, a corner for both; each halo block is created on
the tile's rank, filled, released, and passed by satisfying its event, and
its single consumer destroys it after its DP. A tile releases the params
block after its DP, before it satisfies any event, since `done_edt` destroys
it. `main_edt` seeds the border the same way. The last tile's score block satisfies the done event, which
`done_edt` takes RO.

## Flow

`main_edt` on rank 0 reads and checks the arguments and files, creates and
releases the params block, creates every event, `done_edt` and every tile
(wiring each), and then seeds the border halos: the zero corner, the top
row's bottom rows and corners, and the left column's right columns and
corners. The left halo of tile `(n_h, 1)` is its last satisfy, so every path
to the last tile follows everything `main_edt` does. The tiles then run as a
wavefront from the top-left corner. `done_edt` prints the score line,
destroys every event (an event whose halo has no consumer was never
satisfied), frees the event tables `main_edt` built, destroys the score and
params blocks and shuts the runtime down.

## Placement

Tile `(i, j)` runs on rank `⌊((i − 1) · n_w + (j − 1)) / 8⌋ mod N`: runs of
eight consecutive tiles in row-major order go to one rank, OCR's default
chunked distribution. Its halo blocks are created there, so a halo crosses
ranks when its consumer lies in another chunk. The params block, the border
halos, every event and `done_edt` are on rank 0, and every tile reads the
params block.

## Sizing

The DP does `len₁ · len₂` cell updates in all; the tile size sets the number
of tiles and the wavefront's width, at most `min(n_h, n_w)` tiles at once.
Each tile allocates a `(tile_h + 1) × (tile_w + 1)` scratch matrix. The
answer does not depend on the tile size or the rank count. The catalog row
runs the large pair in 1000 × 1000 tiles (102 × 101 tiles) and pins no
value; the program's own verdict compares against the score file.
