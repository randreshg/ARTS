# LCS_shared

*Recursive quad-tree wavefront DP over a single shared score array —
every leaf task writes the one DB, and only one node may hold it at a time.*
Source: `third_party/ocr-apps/apps/LCS/refactored/ocr/intel-jesmin-lcs_shared_datablocks/lcs.c`
(~500 lines; author Jesmin Jahan Tithi, Intel 2016).

## Overview

Computes a longest-common-subsequence-style alignment score between two
generated strings of length `N` by dynamic programming, using a
cache-oblivious recursive decomposition: `recLCSEdt(n)` with `n > base`
splits its `n×n` square region into four `(n/2)×(n/2)` quadrants — `x11`
(top-left, unblocked), `x12`/`x21` (top-right/bottom-left, both gated on
`x11`'s completion, running concurrently with each other), `x22`
(bottom-right, gated on both `x12` and `x21`) — each spawned as its own
`EDT_PROP_FINISH` sub-recursion. Once `n ≤ base`, the call instead spawns a
single `seqLCSEdt` that fills its whole `n×n` block with a serial
antidiagonal sweep. What makes this the **shared** variant: all three DP
inputs — `S`, `T`, and the DP score itself — are each held in exactly
**one** datablock for the entire run. The score DB is not the full `N×N`
matrix; it uses a compact antidiagonal-offset encoding (`idx = N +
(xj+j-xi-i)`, independent of absolute position) so only `O(N)` longs are
ever allocated, with distinct quadrants writing into overlapping index
ranges across time by design — the recursion's finish-EDT ordering is what
makes that reuse safe. Every one of the `seqLCSEdt` leaves takes an RW
turn on that same one DB.

The result is emitted by `wrapupEdt`: `LCS length:` (the answer cell,
`score[N]`, which for this collapsed zero-gap recurrence is analytically
`N` for any strings) and `LCS checksum:`, a 31-bit FNV-1a digest of the
whole final score array — the value the driver votes on, because it is a
function of the computed cells rather than of the argument list. The DAG
shape depends only on `N` and `base`, never on string content.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `N` | string length; sizes the whole DAG and every DB payload | 1024 | ✓ `atol` in `mainEdt`, propagated to the recursion through `LCS_task_params.N` in `paramv` — multinode-safe |
| `argv[2]` = `base` | recursion base case: quadrant side length at which the split stops | 256 | ✓ same, via `LCS_task_params.base` |

Both arguments are mandatory and validated loudly: a wrong argument count
prints the usage line and shuts down, and `N/base` must be a positive
power of two (the recursion halves `n` unconditionally, so any other ratio
silently drops the tail rows and columns). `GAP_PENALTY` (`= 0`) is
compile-time only, no argv path; there is no verification switch left in
the file (see Flow).

## Structure

Let `d` be the recursion depth: the smallest `d ≥ 0` with `N` right-shifted
`d` times (`n ← n>>1`, repeated) `≤ base`; with the power-of-two ratio the
validation enforces, `d = log₂(N/base)`. Every branch of the quad-tree
reaches the base case at the *same* depth `d`, so the recursion is a
perfect 4-ary tree of depth `d`:

| object | count | size |
|--------|-------|------|
| `recLCSEdt` (recursive, incl. root) | `(4^(d+1)-1)/3` — one node per tree position, levels `0..d` | — |
| `seqLCSEdt` (leaf DP kernel) | `4^d` — one per level-`d` node | — |
| `mainEdt` / `initEdt` / `fillStringEdt` / `wrapupEdt` / `shutDownEdt` | 1 / 1 / 2 / 1 / 1 | — |
| DBs | **3, fixed** — `S`, `T`, `score`; never depth-dependent | `S`=`T`=`4·(N+1)` B; `score`=`16·(N+1)` B (2 longs per position) |
| Events | `6·4^d + 3` — every `recLCSEdt` create (root + `x11`/`x12`/`x21`/`x22`, count `(4^(d+1)-1)/3`) is `EDT_PROP_FINISH` with a non-NULL output-event argument, so each costs 3 events (1 app `ocrEventCreate` STICKY + 1 runtime finish event + 1 runtime output event); every `seqLCSEdt` create (count `4^d`) costs 2 (app STICKY + output event, no finish); the FINISH `initEdt` costs 3 and `wrapupEdt`'s output event 1; the two fill tasks and `shutDownEdt` cost 0 | — |
| EDT templates | `2·4^d + 4`, none ever destroyed (3 per internal node + 1 per leaf + 4 top-level + 1 for the fill tasks) | — |

Worked numbers at the calibrated `args = ['409600', '1600']`: `409600/1600
= 256 = 2⁸`, so `d = 8`. `recLCSEdt` = 87,381; `seqLCSEdt` = 65,536; total
EDTs = 152,923; Events = `6·4⁸ + 3` = 393,219; templates = 131,076; DBs = 3
(`score` = 4.19 MB, `S` = `T` = 1.05 MB, unaffected by `d`). Each unit of
`d` (each doubling of `N/base`) multiplies EDT/event counts by ~4, while DB
*count* never changes and DB *payload* grows only linearly in `N`.

Two upstream inefficiencies are preserved (they are what a base tier
exhibits): one STICKY event per leaf and one per `x22` are created and then
overwritten by the `ocrEdtCreate` out-parameter, so `4^d + (4^d−1)/3` of
the events above are orphans that are never satisfied or destroyed; and no
EDT template is ever destroyed.

## Wiring

- The root `recLCSEdt` has a 1-slot template wired to the init task's
  completion event (`DB_MODE_NULL`): the recursion may not start before
  the two string tasks have written `S` and `T`. `recLCSEdt`'s body never
  touches `depv[]`.
- Internal (non-root, non-leaf) `recLCSEdt` calls carry **no** DB
  dependence at all — the 1-slot template they use is wired to
  `NULL_GUID`/`DB_MODE_NULL` (`x11`, unblocked) or to a sibling's STICKY
  output event (`x12`/`x21` wait on `x11`'s event; `x22` waits on both
  `x12`'s and `x21`'s). `S`/`T`/`score`'s GUIDs travel only as *values*
  inside `LCS_task_params` (`paramv`) — untouched by the coherence
  machinery until a leaf finally acquires them.
- Each leaf `seqLCSEdt` (3-slot template) is wired `S` (RO, slot 0), `T`
  (RO, slot 1), `score` (RW, slot 2) — this is where all real DB traffic
  happens, once per leaf, `4^d` times total.
- `wrapupEdt` takes the root's finish event (slot 0) and `score` RO
  (slot 1); `shutDownEdt` is a separate task gated on `wrapupEdt`'s output
  event, so the digest and the final release are inside the measured
  window and `ocrShutdown` is not called while a dependence is still held.
- **DB concurrency — the point of this variant.** `S` and `T` are RO
  everywhere, so any number of ready leaves may read them concurrently.
  `score` is RW everywhere it is touched, and ARTS's RW is exclusive
  *between nodes* and shared *within* one (the grant carries a writer
  count, not a lock bit): concurrency is `min(ready leaves, workers)` on
  whichever node holds the grant, and exactly one node at a time. The
  program is value-safe under that concurrency because concurrent
  quadrants write disjoint antidiagonal offsets and read only the shared
  centre cell their common ancestor already wrote. So the cost of adding
  nodes is not lost concurrency but grant migration: each of the `4^d` leaf
  turns that lands on a different node than its predecessor moves the whole
  block.

## Flow

`mainEdt` (rank 0) validates the arguments, creates the three DBs and
writes `score`'s boundary values, then creates a FINISH `initEdt` and gates
the root recursion on its completion. `initEdt` creates two
`fillStringEdt`s, one per string, each holding its DB RW and writing it
from an index-seeded generator: character `(tile, position)` is a pure
function of `(tile index, position)` through a self-contained
`splitmix64`-style mixer — no library generator, so the instance does not
depend on the build host's C library either — tile 0 carrying the kernel's
leading sentinel. The same generator, tile widths and seed numbering are used by
`LCS_distributed_ST`, `LCS_all_db_distributed` and `LCS_wavefront`, so all
four rows compute the same instance at the same `N`/`base`, and the
instance does not depend on node count, worker count or task order.

The root `recLCSEdt` then unfolds the quad-tree: `x11` first, `x12`/`x21`
concurrently once `x11`'s whole subtree completes, `x22` once both of those
complete — recursing to depth `d`, where `4^d` `seqLCSEdt` leaves each take
an RW turn on `score`. `wrapupEdt` fires when the root's finish scope
closes, prints the answer cell and the checksum, and `shutDownEdt` shuts
the runtime down.

Nothing else is inside the `[E2E]` window: the serial `O(N²)` reference
recomputation the published program ran in `mainEdt` (`serial_lcs` under an
unconditional `CHECK_RESULTS`) is **gone**, along with the `assert` that
`-DNDEBUG` deleted from every Release cell anyway. It cannot be restored as
a build-time option in this form — the strings no longer exist on rank 0
when `mainEdt` runs — and it is not needed: the printed checksum is a
cheap `O(N)` function of the same result, and it agrees cell by cell with
`LCS_distributed_ST`, which computes the same recurrence over the same
instance.

## Placement (base)

No `OCR_APP_OPTIMIZED_PLACEMENT` guard exists anywhere in this file —
every `ocrEdtCreate`/`ocrDbCreate` passes `NULL_HINT`. Effective policy:

- **EDTs**: NULL hint → shim's `ARTS_HINT_ANY_RANK` → runtime round-robin.
  Every `recLCSEdt`/`seqLCSEdt` instance lands on an independently chosen
  rank with no relation to its position in the quad-tree.
- **DBs**: NULL hint → home = creating rank. `S`, `T`, and `score` are all
  created inside `mainEdt`, so all three home at rank 0 for the whole run.

Consequence: consecutive leaf turns land on different ranks under
round-robin, so ~`(P−1)/P` of the `4^d` turns migrate the score block; the
home matters only for the directory hop. Adding nodes buys migrations, not
a shorter critical path — an anti-scaling shape by construction, which is
this row's exhibit.

## Placement (hinted)

A `hinted` variant is offered, and it is what takes the migration cost out of
this row's curve.  The map is the **subtree colocation** this document
recorded as an unmeasured candidate: the square is cut into a `G x G` grid of
equal quadrant subtrees, `G` the smallest power of two with `G^2 >= nranks`,
and each rank takes a contiguous band of that grid.  Every `recLCSEdt` and
every `seqLCSEdt` is hinted onto the band of its own region's top-left corner
-- a pure function of the `(xi, xj)` its parent already writes into `paramv`,
never `ocrAffinityGetCurrent`, which would funnel the tree back onto its
creator.

The band is **not** cut in row-major order.  Inside a recursion node the two
children that may run concurrently (`x12`, `x21`) take adjacent indices and
the two an event orders (`x11` first, `x22` last) take the other pair, so a
cut between ranks falls between children a dependence already separates rather
than between two that would otherwise want the score block on different nodes
at the same instant.  At two ranks that puts `{x11, x22}` on one node and
`{x12, x21}` on the other.

Nothing else changes.  `S`, `T` and `score` keep the homes `mainEdt` gives
them; no count, size, mode, wiring or decomposition moves; the `#else` of the
`OCR_APP_OPTIMIZED_PLACEMENT` guard is `NULL_HINT`, so the base build is the
program described above.  Balance is exact at every campaign rank count:
`G^2` is a power of four, its `G^2/nranks` quadrants per rank are equal in
size, and 1, 2, 4, 8, 16 and 32 each divide their `G^2` -- aggregate
imbalance 1.000.

**Measured** (15w+1p x 1/2/4/8 nodes, INV x WB, `[E2E]` seconds):

| nodes | base `262144 1024` | hinted | gain | base `524288 2048` | hinted | gain |
|---|---|---|---|---|---|---|
| 1 | 8.289 | 8.250 | 1.00x | 33.009 | 32.778 | 1.01x |
| 2 | 28.621 | 8.523 | **3.36x** | 85.620 | 33.561 | **2.55x** |
| 4 | 49.431 | 11.678 | **4.23x** | 134.963 | 44.225 | **3.05x** |
| 8 | 98.904 | 12.090 | **8.18x** | 243.050 | 45.047 | **5.40x** |

The right-hand pair is four times the tile work at the same `L = 256`
(`base` doubled, `N` doubled), one run per cell against an 1800 s budget so the
base 8-node cell is not censored; the left-hand pair is the median of three at
the trend arguments.  Both sizes give one checksum across all sixteen cells
(`673249177` and `1551169433`), so the map changes where the work runs and
nothing else.

The base tier degrades 11.9x (7.4x at the larger size) over the eight
nodes and the hinted tier 1.47x (1.37x).  That is the mechanism made visible: under a round-robin map
about `(P-1)/P` of the `4^d` leaf turns move the score block, and under
subtree colocation only the `O(P)` turns that cross a band boundary do.  The
map cannot turn the curve upward -- one RW block admits one node at a time by
construction, whatever the placement -- but removing almost all of the
migration cost is an improvement at every geometry the campaign runs, so the
tier is kept.  The row's exhibit is unharmed: the base tier is what shows the
anti-scaling shape, and it is untouched.

## Family shape (measured, 15w+1p x 1/2/4/8 nodes, `65536 1024`)

**Measured before the 2026-09-03 conformance change** — every cell below
includes the serial `O(N²)` preamble that has since been removed (roughly
80-88 % of the one-node window at these sizes), so the absolute numbers
are stale and only the *shape* survives. Re-trend before quoting.

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 4.29 | 5.78 | 6.76 | 7.97 |
| val_wb | 4.29 | 5.76 | 6.72 | 8.02 |
| inv_wb | 4.29 | 5.78 | 6.84 | 8.17 |
| excl_retain | 4.29 | 5.73 | 6.72 | 7.78 |

Every arm degrades identically: the migrating score turn costs ~0.9 ms per
remote hop regardless of family, because there is nothing for a coherence
protocol to cache when every turn moves the block. At the calibrated size
(`d = 8`, `4⁸ = 65,536` turns — 4× the previous calibration's 16,384) the
multinode cells are ordinary measurements, not censored points — the
projected 8-node cell is on the order of two minutes against a 900 s
budget.

## Sizing

`N` and `base` together set `d = log₂(N/base)`, which sets **both** the DB
*payload* (linear in `N`) and the total leaf/turn count (`4^d`). Width is
the row's declared deviation: the quad-tree's peak frontier is `2^d` (256
at the calibrated point) and its time-average parallelism is work/span =
`(4/3)^d` = 9.99, against 3456 workers at the widest geometry. Reaching a
3456-wide frontier needs `d ≥ 12`, i.e. 16.8 M leaf turns, which no cell
budget admits. **This row is a decomposition-ladder exhibit, not a
width-conforming scaling row**; the family's width requirement is carried
by `LCS_wavefront`.

- Smaller `base` relative to `N` → larger `d` → many more, cheaper turns,
  more of the run's cost is DB-acquire/coherence overhead rather than
  antidiagonal compute.
- Larger `base` relative to `N` → smaller `d` → fewer, larger turns, each
  doing more real antidiagonal work per acquire.
- `N/base` must be a power of two (enforced), so the two dials move in
  factor-of-two steps.
- Memory is never the limit: DB payload is `24·(N+1)` B ≈ 9.8 MB at
  `N = 409600`, and the runtime metadata for 153 K EDTs and 393 K events is
  tens of MB — orders of magnitude under the 190 GB one-node budget.

Calibrated (with the hinted tier kept, the window is the hinted-flat
case, 20 s at one node): `409600 1600`, derived rather than searched —
at a fixed tree depth (`N/base = 256`, the trend roster's) the surviving
law `T ≈ 3^d·base² / R` is quadratic in `N`, so 8.2 s at `262144 1024`
puts 20 s at `N = 409600`, and `base` widens to 1600 to keep the depth.
Measured on one node of 108 workers (INV × WB): base 19.6 s, hinted
20.0 s, 4.2 GB resident, checksum 1663678361. The voted scalar is the
`LCS checksum:` line — a function of every cell of the computed array,
and equal to `LCS_distributed_ST`'s at the same arguments; `LCS length:`
is still printed but remains analytically `N`, so it is a liveness
signal only.

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
