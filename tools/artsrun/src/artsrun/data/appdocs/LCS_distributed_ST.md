# LCS_distributed_ST

*Same recursive quad-tree wavefront DP as `LCS_shared`, but with `S` and
`T` chopped into per-block labeled datablocks — the score array stays
one shared DB, only the read-only inputs are distributed.*
Source: `third_party/ocr-apps/apps/LCS/refactored/ocr/intel-jesmin-lcs_distributed_ST_datablocks/lcs_ST_distributed.c`
(~600 lines; author Jesmin Jahan Tithi, Intel 2016).

## Overview

Structurally identical to `LCS_shared`: the same `recLCSEdt` quad-tree
recursion (`x11` unblocked, `x12`/`x21` gated on `x11`, `x22` gated on
both), the same base-case `seqLCSEdt` antidiagonal kernel, the same
compact `O(N)` antidiagonal-offset score representation, and the same
`S[i] != T[j]` match term with `GAP_PENALTY = 0`. The one structural
difference — what "ST" (S/T-distributed) names — is that `S` and `T` are
no longer single datablocks: each is chopped into one tile per base case
(`L = N/base` tiles), allocated through `ocrGuidRangeCreate` /
`ocrGuidFromIndex` (labeled GUIDs) instead of one `ocrDbCreate` each. The
`score` array, by contrast, is **still one shared DB**, exactly as in
`LCS_shared` — this variant distributes only the read-only inputs, not the
read/write state, which is the deliberate midpoint between `LCS_shared`
(nothing distributed) and `LCS_all_db_distributed` (everything
distributed, including the score tiles).

`wrapupEdt` prints `LCS length:` (the answer cell `score[N]`, analytically
`N` for this collapsed zero-gap recurrence) and `LCS checksum:`, a 31-bit
FNV-1a digest of the whole final score array — the voted scalar. Because
this row computes the same recurrence over the same generated instance as
`LCS_shared`, the two rows print the same checksum at the same arguments.
DAG shape depends only on `N`/`base`, never on string content.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `N` | string length | 1024 | ✓ `atol` in `mainEdt`, reaches the recursion via `LCS_task_params.N` (`paramv`) — multinode-safe |
| `argv[2]` = `base` | recursion cut-off: the quad-tree halves until `n ≤ base`; also the string tile width | 256 | ✓ same, via `LCS_task_params.base` |

Both arguments are mandatory and validated loudly: a wrong argument count
prints the usage line and shuts down, and `N/base` must be a positive
power of two — the recursion halves `n` unconditionally, so only then is a
base case exactly `base` wide and therefore covered by exactly one string
tile. `GAP_PENALTY` is compile-time only, no argv path.

## Structure

`d` = number of `n←n>>1` shifts of `N` until `≤ base` = `log₂(N/base)`
under the enforced ratio; the quad-tree is a perfect 4-ary tree of depth
`d`. `L = 2^d = N/base` is the number of base-case columns, which is also
the number of `S` (and of `T`) tiles.

| object | count | size |
|--------|-------|------|
| `recLCSEdt` (incl. root) | `(4^(d+1)-1)/3` | — |
| `seqLCSEdt` (leaf) | `4^d` | — |
| `mainEdt` / `initEdt` / `fillStringEdt` / `wrapupEdt` / `shutDownEdt` | 1 / 1 / `2L` / 1 / 1 | — |
| DBs | `1 + 2L` — 1 shared `score` (ordinary, RW) + `L` labeled `S` tiles + `L` labeled `T` tiles | `score` = `16·(N+1)` B; `S`/`T` tile 0 = `4·(base+1)` B, tiles `1..L-1` = `4·base` B each |
| Events | `6·4^d + 3` — same closed form as `LCS_shared` (identical recursive wiring): 3 events per `recLCSEdt` create (app STICKY + runtime finish + runtime output), 2 per `seqLCSEdt` create, 3 for the FINISH `initEdt`, 1 for `wrapupEdt`'s output event, 0 for the fill tasks and `shutDownEdt` | — |
| EDT templates | `2·4^d + 4`, none destroyed | — |

Worked numbers at the calibrated `args = ['409600', '1600']`
(`409600/1600 = 256 = 2⁸`, `d = 8`, `L = 256`): `recLCSEdt` = 87,381,
`seqLCSEdt` = 65,536, `fillStringEdt` = 512, total EDTs = 153,433, Events =
`6·4⁸+3` = 393,219, templates = 131,076; DBs = `1 + 512 = 513` (`score`
≈ 6.55 MB; 256 `S` tiles + 256 `T` tiles, 6.4 KB each).

As in `LCS_shared`, the upstream orphan STICKY event per leaf and per
`x22`, and the never-destroyed templates, are preserved.

## Wiring

- Root `recLCSEdt`'s template is `depc=1`, wired to the init task's
  completion event (`DB_MODE_NULL`): the recursion may not start before
  every string tile has been written. `recLCSEdt`'s body never touches
  `depv[]`; `S`/`T` travel down the recursion only as the
  `s_labels`/`t_labels` *range* GUIDs inside `paramv`.
- Internal (non-leaf) `recLCSEdt` calls carry no DB dependence, same
  event-chain wiring as `LCS_shared` (`x11` unblocked, `x12`/`x21` on
  `x11`'s event, `x22` on both).
- Each leaf resolves its own `S`/`T` tile via `ocrGuidFromIndex(...,
  (p->xi-1)/n)` / `(..., (p->xj-1)/n)` — the base case's own extent `n`,
  which the argument validation makes exactly the tile width — then wires
  `seqLCSEdt`'s 3 slots: `S` (RO, slot 0), `T` (RO, slot 1), `score` (RW,
  slot 2, the same single shared DB as `LCS_shared`).
- `wrapupEdt` takes the root's finish event and `score` RO; `shutDownEdt`
  is a separate task gated on `wrapupEdt`'s output event, so the digest and
  the final release sit inside the measured window.
- **DB concurrency**: `score` is unchanged from `LCS_shared` — RW is
  exclusive between nodes and shared within one, so concurrency is
  `min(ready leaves, workers)` on the grant-holding node and one node at a
  time; the cost of adding nodes is grant migration on most of the `4^d`
  turns, not lost concurrency. `S`/`T` are split into `2L` separate RO
  blocks, so read traffic spreads over `L` homes instead of concentrating
  on one — 4 KB RO snapshots against a 2 MB RW block that moves nearly
  every turn, which is why this rung's curve overlaps `LCS_shared`'s.

## Flow

`mainEdt` (rank 0) validates the arguments, creates the `score` DB and
writes its boundary values, reserves the two labeled ranges, and creates a
FINISH `initEdt` on whose completion the root recursion is gated.
`initEdt` creates the `2L` string tiles `NO_ACQUIRE` — the creating task
never touches them, so each is born at its labeled home instead of
materializing on the creating rank, and no post-init release loop is
needed — and one `fillStringEdt` per tile, each holding its own tile RW.
The fill is index-seeded: a tile's characters are a pure function of its
`(tile index, position)` through a self-contained `splitmix64`-style mixer
— no library generator, so the instance does not depend on the build host's
C library either — tile 0 carrying the kernel's leading sentinel. Seed numbering, tile widths and
sentinel placement are identical in `LCS_shared`,
`LCS_all_db_distributed` and `LCS_wavefront`, so all four rows compute the
same instance, independent of node count, worker count and task order.

The quad-tree then unfolds to depth `d`, `4^d` leaves each take an RW turn
on `score`, and `wrapupEdt` prints the answer and the checksum before a
separate `shutDownEdt` stops the runtime.

The serial `O(N²)` reference recomputation the published program ran in
`mainEdt` (`serial_lcs` under an unconditional `CHECK_RESULTS`) and the
`assert` that consumed it — which `-DNDEBUG` deleted from every Release
cell — are **gone**; verification is the printed checksum, which agrees
with `LCS_shared`'s cell for cell.

## Placement (base)

No `OCR_APP_OPTIMIZED_PLACEMENT` guard in this file — every
`ocrEdtCreate` passes `NULL_HINT`. But **labeled GUIDs are placed
differently from ordinary ones**: a range created via `ocrGuidRangeCreate`
gets each index's home fixed round-robin (`home = index % nranks`) at
range-creation time, baked into the GUID's own rank bits — independent of
the hint passed to the later `ocrDbCreate` on that GUID, and independent
of which rank calls it. Effective policy:

- **EDTs**: NULL hint → round-robin (`ARTS_HINT_ANY_RANK`) —
  `recLCSEdt`/`seqLCSEdt`/`fillStringEdt` scatter with no relation to
  their tile position.
- **`score`**: ordinary `ocrDbCreate`, NULL hint → home = creator =
  rank 0.
- **`S`/`T` labeled tiles**: home = `label_index % nranks`, genuinely
  spread across every rank; with `NO_ACQUIRE` the create installs the home
  stub without pulling any payload to the creating rank.

Consequence: `score` behaves exactly as in `LCS_shared` — its grant
migrates on most of the `4^d` turns. `S`/`T` reads are spread over
`nranks` homes, but since the *EDT's* rank (a round-robin counter) and its
tile's home (`index % nranks`) come from unrelated schemes, a local
acquire is coincidence rather than design. This variant relieves pressure
on rank 0 for `S`/`T` traffic without making any individual acquire more
likely to be local, and leaves the `score` bottleneck exactly as severe as
`LCS_shared`'s — which is its point on the ladder.

## Placement (hinted)

A `hinted` variant is offered.  The map is the **subtree colocation** this
document recorded as an unmeasured candidate, and it is identical to
`LCS_shared`'s -- deliberately, so the two rungs of the ladder stay
comparable: the square is cut into a `G x G` grid of equal quadrant subtrees,
`G` the smallest power of two with `G^2 >= nranks`, each rank takes a
contiguous band of that grid, and every `recLCSEdt` / `seqLCSEdt` is hinted
onto the band of its own region's top-left corner -- a pure function of the
`(xi, xj)` already in `paramv`, never `ocrAffinityGetCurrent`.  The band is
cut in an order that indexes the two concurrent children (`x12`, `x21`)
adjacently and the two an event orders (`x11`, `x22`) as the other pair, so a
rank boundary never falls between two children that would want the score block
at the same instant.

One piece is specific to this rung: each `fillStringEdt` is hinted onto its own
tile's labeled home (`index % nranks`), so the one-time fill of a `NO_ACQUIRE`
tile happens where the tile was born.  The tiles themselves are **not** moved
-- a labeled range fixes each index's home at range-creation time and ignores
the hint on the later `ocrDbCreate` -- and with a two-dimensional quadrant map
there is no single consumer rank to move an `S` tile to anyway: tile `ti` is
read by the leaves of a whole tile row, which the map spreads over `G` bands.
The read-only tiles are small (4 KB) and cached per node after first touch, so
that is not where the wall time is.

Nothing else changes: no count, size, mode, wiring or decomposition moves, and
the `#else` of the `OCR_APP_OPTIMIZED_PLACEMENT` guard is `NULL_HINT`, so the
base build is the program described above.  Balance is exact at every campaign
rank count (aggregate imbalance 1.000 at 1, 2, 4, 8, 16 and 32).

**Measured** (15w+1p x 1/2/4/8 nodes, INV x WB, `[E2E]` seconds):

| nodes | base `262144 1024` | hinted | gain | base `524288 2048` | hinted | gain |
|---|---|---|---|---|---|---|
| 1 | 8.380 | 8.316 | 1.01x | 33.278 | 33.034 | 1.01x |
| 2 | 28.865 | 8.615 | **3.35x** | 85.561 | 33.775 | **2.53x** |
| 4 | 48.866 | 11.738 | **4.16x** | 136.343 | 44.118 | **3.09x** |
| 8 | 97.620 | 12.009 | **8.13x** | 238.794 | 44.809 | **5.33x** |

The right-hand pair is four times the tile work at the same `L = 256`
(`base` doubled, `N` doubled), one run per cell against an 1800 s budget so the
base 8-node cell is not censored; the left-hand pair is the median of three at
the trend arguments.  Both sizes give one checksum across all sixteen cells
(`673249177` and `1551169433`), so the map changes where the work runs and
nothing else.

The curve still overlaps `LCS_shared`'s in both tiers, which is this rung's
point -- tiling the read-only inputs changes nothing while the score block is
the object that moves.  The base tier degrades 11.7x (7.2x at the larger size) over the eight
nodes and the hinted tier 1.44x (1.36x).
What the map changes is how often the block moves: from about
`(P-1)/P` of the `4^d` leaf turns to the `O(P)` that cross a band boundary.
It cannot turn the curve upward (one RW block admits one node at a time
whatever the placement), but the improvement holds at every geometry the
campaign runs, so the tier is kept, and the base tier -- the tier that exhibits
the shape -- is untouched.

## Family shape (measured, 15w+1p x 1/2/4/8 nodes, `65536 1024`)

**Measured before the 2026-09-03 conformance change**: every cell includes
the serial `O(N²)` preamble that has since been removed (~80-88 % of the
one-node window at these sizes) and a rank-0 serial tile fill. Absolute
numbers are stale; only the shape survives. Re-trend before quoting.

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 4.29 | 5.76 | 6.75 | 7.99 |
| val_wb | 4.29 | 5.75 | 6.79 | 7.95 |
| inv_wb | 4.29 | 5.76 | 6.85 | 8.14 |
| excl_retain | 4.31 | 5.84 | 6.93 | 8.09 |

The curve overlaps `LCS_shared`'s on every arm: tiling the read-only
inputs changes nothing while the single score block still migrates every
turn.

## Sizing

Same dials and the same anti-scaling shape as `LCS_shared` — `d` (via `N`,
`base`) sets both the tile payload and the `4^d` score turns that dominate
wall time; node count changes how much of that traffic crosses the
network, not how much of it is concurrent. Width is the declared
deviation: peak frontier `2^d` = 256 and time-average parallelism
`(4/3)^d` = 9.99 at the calibrated point, against 3456 workers; no feasible
argument pair reaches 3456 (it needs `d ≥ 12`, 16.8 M leaf turns). **This
row is a decomposition-ladder exhibit, not a width-conforming scaling
row.**

Memory is never the limit: `score` 6.55 MB + `2L` tiles ~3.28 MB + runtime
metadata for 153 K EDTs / 393 K events / 513 DBs — well under 1 GB against
the 190 GB one-node budget.

Calibrated (with the hinted tier kept, the window is the hinted-flat
case, 20 s at one node): `409600 1600`, derived rather than searched —
at a fixed tree depth (`N/base = 256`, the trend roster's) the time is
quadratic in `N`, so 8.2 s at `262144 1024` puts 20 s at `N = 409600`,
and `base` widens to 1600 to keep the depth. Measured on one node of
108 workers (INV × WB): base 19.7 s, hinted 19.2 s, 4.2 GB resident,
checksum 1663678361 — the same arguments and the same digest as
`LCS_shared`, which is the ladder's cell-by-cell check. The voted scalar
is the `LCS checksum:` line; `LCS length:` remains analytically `N` and
is a liveness signal only.

The digest is a strictly stronger oracle than the old single-cell pin — it
covers the `2N+1` written cells (indices `0..2N` — the unwritten cell
`2N+1` is deliberately outside the walk, since a datablock's payload is not
zero-initialised and digesting it would make the voted scalar
allocator-dependent), so a leaf that never ran, ran twice, or wrote the
wrong index range changes it — but it is not content-sensitive: this
collapsed max-recurrence saturates to a value that depends only on `N`, not
on the generated characters (verified by simulation over random and constant
strings). It witnesses the run's shape and the arithmetic's placement, not
the comparison itself; content sensitivity in this family lives in
`LCS_all_db_distributed`/`LCS_wavefront`, which compute a real table.
