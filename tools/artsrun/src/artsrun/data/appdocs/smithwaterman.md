# smithwaterman

*Tiled sequence-alignment DP over EDTs — a wavefront over a tile grid,
every tile a task, three neighbors its dependencies.*
Source: `third_party/ocr-apps/apps/smithwaterman/ocr/smithwaterman.c`
(~615 lines).

## Overview

Computes an alignment score between two nucleotide sequences by dynamic
programming, tiled into a `tile_width × tile_height` grid and executed as
a diagonal wavefront: each `smith_waterman_task` fills its tile's DP cells
from the tile to its west, north and northwest, using a transition/
transversion-aware scoring matrix (`MATCH=+2`, `TRANSITION_PENALTY=-2`,
`TRANSVERSION_PENALTY=-4`, `GAP_PENALTY=-1`). Despite the app's name, the
recurrence has no zero-floor and no local traceback — border cells
accumulate `GAP_PENALTY` linearly from the (0,0) corner, i.e. this is a
global (Needleman–Wunsch-style) alignment score, not a local
Smith–Waterman one. The single bottom-right-most tile prints the score and
calls `ocrShutdown()` inline, checked externally by the harness against a
score fixture file (catalog `expect`). Reads two sequence files and a
score file from disk (rank 0 only, native `fopen`/`fread`); the DAG shape
depends only on the two sequence *lengths* and the tile size — sequence
*content* only affects the numeric scores, not the task graph. The
program stresses wavefront scheduling and border-cell data movement, not
raw compute (each tile does `O(tile_width·tile_height)` cheap DP steps).

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `tileWidth` | DP columns per tile | required | ✓ `atoi` in `ioHandling` (non-`TG_ARCH` build) |
| `argv[2]` = `tileHeight` | DP rows per tile | required | ✓ same |
| `argv[3]` = `fileName1` | sequence 1 input file | required | ✓ passed straight to `read_file`; catalog names it via `fixtures` |
| `argv[4]` = `fileName2` | sequence 2 input file | required | ✓ same |
| `argv[5]` = `scoreFile` | expected-score fixture (ASCII integer) | required | ✓ same; parsed with `atoi` |

`n_tiles_width`/`n_tiles_height` are **not** direct CLI knobs — they are
derived (`ceil(seqLen / tileSize)`) from the tile size against whichever
fixture pair is named. `ioHandling` rejects the arguments that would make the
grid meaningless rather than failing later: fewer than five arguments, a
non-positive tile side, an unreadable file, and a sequence with no nucleotide
in it (which would leave an empty grid, so no task would ever reach the
bottom-right corner that ends the run) each print a line and shut down. The scoring constants (`GAP_PENALTY`,
`TRANSITION_PENALTY`, `TRANSVERSION_PENALTY`, `MATCH`) are `#define`s with
no argv path (✗). A `TG_ARCH` branch reinterprets `argv[3..5]` as raw
character counts instead of filenames; it is dead code in this build
(`TG_ARCH` is never defined for the ARTS/x86 target).

## Structure

Let `W = ceil(len1/tileWidth)`, `H = ceil(len2/tileHeight)`, where
`len1`/`len2` count only `A`/`C`/`G`/`T` characters after whitespace
stripping (not raw file bytes). Unlike nqueens/quicksort this shape is
fully closed-form — content never affects it, only the two lengths and
the tile size:

| object | count | size |
|--------|-------|------|
| `smith_waterman_task` | `W·H` | — |
| `mainEdt` | 1 | — |
| DBs | `5·W·H + 2·W + 3·H + 7` | per compute tile: 2 temp (destroyed same task, `4·(tileWidth+1)·(tileHeight+1)` B + `8·(tileHeight+1)` B) + 3 output (`4` B / `4·tileHeight` B / `4·tileWidth` B); border init: `2W+2H+1` (≤ `4·max(tileWidth,tileHeight)` B each); tile-matrix structure: `H+2`; shared params DB: 1, `8·(8 + ⌈len1/8⌉ + ⌈len2/8⌉)` bytes; **+3** for the three input-file buffer DBs `read_file` allocates (one `ocrDbCreate` each for `fileName1`, `fileName2`, `scoreFile`, called from `ioHandling`) — these three are *created* but no longer live: the score buffer is destroyed right after its `atoi` and the two sequence buffers once their bytes have been copied into the shared params DB, so they count in `NUM_DB_CREATE` and not in the resident set |
| Events | `3·(W+1)·(H+1)` STICKY events (readiness signals for the border-inclusive `(H+1)×(W+1)` tile grid) |

Worked numbers for the calibrated `args=[100, 100, ...cmp...]` (the
`string{1,2}-cmp.txt` fixtures: `len1=len2=200000` after stripping)
→ `W=H=2000`: EDTs = 4,000,001; DBs = 20,010,007; Events = 12,012,003.
The previous rung, the `cal` pair (`len1=140000, len2=140400`, `W=1400,
H=1404`), is 1,965,601 / 9,835,019 / 5,905,215 by the same formulas.
The shared params DB is ≈400 KB (`8·(8 + ⌈len1/8⌉ + ⌈len2/8⌉)` = 400,064 B
at the cmp pair; 280,464 B at the cal pair)
and is read (RO) by all 1,965,600 compute tasks.

Counter cross-check: verified (1 node, `4 4` tiny fixtures (`W=H=2`) vs
`4 4` small fixtures (`W=H=3`)): NUM_EDT_CREATE 6 → 11, NUM_DB_CREATE
38 → 68, NUM_EVENT_CREATE 27 → 48 — exactly `W·H+1` / `5WH+2W+3H+7` /
`3(W+1)(H+1)` (app values 5/10, 37/67, 27/48) plus the runtime's constant
+1 EDT/+1 DB/+0 EVT baseline. The DB formula's original `+4` constant
undercounted by exactly the 3 file-buffer DBs above; corrected to `+7`.

## Wiring

- Every dependence is `DB_MODE_CONST` → ARTS RO: slot 0 = west neighbor's
  `right_column_event`, slot 1 = north neighbor's `bottom_row_event`, slot
  2 = northwest neighbor's `bottom_right_event`, slot 3 = the shared
  params DB. There is no `DB_MODE_RW` anywhere in this app.
- Every border-crossing output DB (`br`/`rc`/`brow`) has at most one
  consumer — no fan-out; the grid's outer rim has none — **except** the
  shared params DB, read by every
  one of the `W·H` compute tasks: the one broadcast point in the app, but
  pure RO fan-out (never written), the natural case for RO-snapshot
  caching rather than lock contention.
- No separate reduction/finish EDT: the single bottom-right tile both
  computes its cell *and* performs verification/shutdown in the same EDT
  body once its own 3-way AND-join (which transitively needs the whole
  grid) fires. Verification is one integer compare against the score
  fixture plus one `ocrPrintf` — there is no serial recomputation of the
  alignment anywhere in the program, and it writes no output file.
- A consumer reclaims what it read: the three input blocks *and* the three
  readiness events that carried them, since each readiness event is named by
  exactly one dependence and its single consumer is therefore its last user.
  What that discipline cannot reach is the grid's outer rim — the last row's
  `bottom_row`, the last column's `right_column`, the corners on both, and
  the row-0/column-0 events that no seed ever satisfies: `3·(W+H+1)` events
  (8,415 of 5,905,215 at `W=1400, H=1404`, 0.14%) and the `≈1.1 MB` of
  blocks the rim ones carry survive the run. Reaching them would mean not
  publishing an edge the published program publishes, so they are left.

## Flow

`mainEdt` (rank 0): reads both sequence files and the score file, builds
the `(H+1)×(W+1)` event grid (`3·(W+1)(H+1)` STICKY events plus the
`Tile_t` matrix DBs), seeds border row/column 0 with linear gap-penalty
values (`2W+2H+1` DBs), builds the shared params DB, then issues `W·H`
`ocrEdtCreate` calls in a plain nested loop — **all** of this, including
the create loop itself, is serial, single-rank work that must finish
before the first compute tile can run (over 1M creates at the calibrated
size). Execution then follows an anti-diagonal wavefront: tile `(i,j)`
is ready once its west/north/northwest neighbors have published their
border arrays. Parallel width at diagonal step `k = i+j`
(`2 ≤ k ≤ W+H`) is `|{i : max(1,k-W) ≤ i ≤ min(H,k-1)}|`, ramping
`1 → min(W,H) → 1` over `W+H-1` steps; average concurrency
`≈ W·H/(W+H-1)`, well under the peak. Completion is implicit in the last
tile's own dependency join — no separate barrier phase.

**The serial construction is the exhibit, not an accident of packaging.**
Building the whole DAG from one task on one rank *is* this program's published
decomposition, so the base tier keeps it: the row is here to show what that
costs, and the measurement it produces is a create/dependence-registration
comparison across the runtimes far more than a coherence-traffic one. Nothing
else shares the window with it — the three input files are the program's own
input, read once in `mainEdt` (0.001 s at the calibrated size against a
whole-run anchor in seconds) into three blocks that are destroyed as soon as
their last reader is done with them, there is no generated data, no
verification pass beyond one integer compare, and no output file. Where that construction cost
is the thing to *break* rather than to display, the restructured row
(`smithwaterman_dist`) is where each place builds its own band.

## Placement (base)

The source carries an `OCR_APP_OPTIMIZED_PLACEMENT` guard around two
helpers, `swBandEdtHint` and `swBandDbHint` (built as `smithwaterman_hinted`,
`HINTED_PLACEMENT` in `benchmarks/apps/CMakeLists.txt`; catalog
`hinted: true`) — see the next section. Outside that guard every
`ocrEdtCreate`/`ocrDbCreate` call passes `NULL_HINT`, and base is the guard
off. Effective policy:

- **EDTs**: NULL hint → round-robin (`ARTS_HINT_ANY_RANK`) —
  `smith_waterman_task` instances scatter across ranks with no relation to
  the wavefront's 2D adjacency.
- **DBs**: NULL hint → home = creating rank. Border/output DBs are homed
  wherever the *producing* tile's round-robin-placed EDT landed, so a
  consumer usually acquires each of its 3 inputs from a different, likely
  remote, rank.

Consequence: nearly every one of the wavefront's dependency edges is a
remote acquire of a small (≤400 B) block — the algorithm's real locality
(adjacent tiles) is never expressed by placement — layered under one
large, always-resident, RO-fan-out params DB (homed once at rank 0,
read-shared everywhere, never migrated).

## Placement (hinted)

As-born scatters the W x H wavefront round-robin, so a tile's three inputs
(West's right column, North's bottom row, NW's corner) almost always live on
three different remote ranks (see above).

The layer is hint-only — two helpers and three call sites, no control flow,
partition, layout, DB/event count or wiring inside the guard:

| object | site | hint |
|---|---|---|
| tile EDT `smith_waterman_task(i,j)` | the single create loop, via `swBandEdtHint` | `rank = min(P-1, ⌊(i-1)·P / H⌋)` |
| left-column seed block `rc(i,0)` | `initialize_border_values`, via `swBandDbHint` | `rank = min(P-1, ⌊(i-1)·P / H⌋)` — the band of its consumer, tile `(i,1)`, i.e. the same band as that tile's EDT |
| left-column corner seed `br(i,0)` | `initialize_border_values`, via `swBandDbHint` | `rank = min(P-1, ⌊i·P / H⌋)` — the band of row `i`, not of row `i-1`: a corner seed is a diagonal input, so its consumer is the tile one row below (`(i+1,1)`) |
| top-row seeds `brow(0,j)`, `br(0,j)` | — | `NULL_HINT` → creator home = rank 0 = band 0, which is where their row-1 consumers run |
| tile output blocks `br`/`rc`/`brow(i,j)` | — | `NULL_HINT` → creator home = the producing tile's band rank |
| shared params DB | — | `NULL_HINT` → rank 0 |

Each seed is therefore homed on the band of the tile that reads it: the
column seed's consumer sits in the same row, the corner seed's one row below,
and the two hints differ exactly on the `P-1` band-boundary rows.

It is the same contiguous row-band map as LCS_all. A tile's West neighbour
shares its row and therefore its rank; North/NW share its band on all but the
`P-1` band-boundary rows, so the dominant row-to-row payload stays rank-local
while the anti-diagonal frontier still reaches every band once it is a band
tall. The map is a function of the tile index and never of
`ocrAffinityGetCurrent`, so it cannot collapse onto the creating rank.

**Coverage and balance.** Rank `r` receives `⌈(r+1)H/P⌉ - ⌈rH/P⌉` rows, i.e.
`⌊H/P⌋` or `⌈H/P⌉`, so every rank gets tiles and the load imbalance is
`⌈H/P⌉·P / H`. At `H=1404`: **1.000 at P=2, 1.0028 at P=8, 1.0028 at P=16,
1.0028 at P=32** — and by construction it is below `1 + P/H` at any campaign
size, since `H` is in the thousands and `P ≤ 32`. (An earlier note put P=32 at
1.014; that was the "differs by at most W tiles" upper bound, not the ratio.)

**Precondition, and what happens below it.** One band per rank spans every
rank only while `H ≥ P`. With fewer tile rows than ranks the map's image would
have at most `H` distinct ranks and the rest would receive nothing — an R2
coverage failure the program could not report. Both helpers therefore return
`NULL_HINT` when `nranks <= 1` or `nrows < nranks`, deferring to the runtime's
no-preference placement, which reaches every rank. The Dane sweep never
approaches it (`H ≥ 1404` against `P ≤ 32`), and neither does the gate roster
(`H = 200` against `P ≤ 8`); the guard exists so no future fixture can trip it
silently. `nranks <= 1` also keeps the 1-node cells of the two tiers identical,
which is what anchors the size.

**The one object that stays on rank 0 is not a funnel.** The shared params DB
is created in `mainEdt` with `NULL_HINT`, so creator-home puts it on rank 0,
and all `W·H` tiles take it at slot 3. No hint-only placement improves that: a
hint would only move the single home to some other single rank. It is read-only
and never written, so each node acquires one snapshot and serves every local
tile from it — one fetch per rank for the run, not `W·H` round trips — which is
why it is left unhinted in both tiers.

**The cost side of the DB half.** `swBandDbHint` is applied to the `2H`
left-column seed blocks, which are built inline in `mainEdt` on rank 0, so
every seed outside band 0 becomes a remotely-homed create in the serial phase
that dominates this row's window. That cost grows linearly with `H`, against a
benefit that grows with `W·H`; it is the right trade at every size this row
runs, and it is the term to re-check if the size ever moves by an order of
magnitude.

## Sizing

The total DP work is the product of the two sequence lengths and does **not**
depend on the tiling.  So the tile size sets width and grain, while the run
length is set by the DATASET -- which makes the dataset this row's size knob.
There is no iteration, timestep or round count anywhere in the program, so
calibration moves the size and nothing else.  The published tile is 10 (the
app README's own run line, over a 51-character sequence); the campaign runs
100, a deliberate departure recorded here because the tile is also the width
lever and cannot simultaneously be pinned at a published grain.

| geometry, 178k pair, 15 workers/node | time |
|---|---|
| 1 node | 16.37 s |
| 2 nodes | 316.06 s |
| 4 nodes | 283.89 s |

A 19x degradation across the first node boundary.  Calibrated (with the hinted
tier kept, the window is the hinted-flat case, 20 s at one node): the `cmp`
pair, `200,000 / 200,000`, derived rather than searched -- the wavefront's time
is quadratic in the length, and the `cal` pair (`140,000 / 140,400`) measured
9.4 s at one node of 108 workers (INV × WB, `/usr/bin/time -v` anchor), which
puts 20 s at ~204k.  Measured at the cmp pair, same geometry: **23.0 s base /
22.1 s hinted**, 10.3 GB resident.  Earlier figures in this document (14.5 /
14.7 / 15.3 s for the three families, 11.8 s base / 11.0 s hinted, and the
9.411 s `arts_val_wb` cal-pair anchor of `logs/exp/20260831-204917`) are
previous rungs or predate the readiness-event reclamation -- do not build a
sizing argument on them.

The cal pair's score 86360 comes from an independent sequential reference of
the same recurrence, validated by reproducing the 515,000-pair's long-pinned
318128; the cmp pair is past what that reference computes, so its expected
123196 is the value the program itself prints in agreement across runtimes and
node counts -- for this row's calibrated arguments the consensus is a
cross-runtime check, not an independent oracle (disclosed in the catalog).

This row also anti-scales INSIDE a node: 7.91 s at 15 workers against 10.88 s
at 112, on the same problem.  The run is bounded below by a single-threaded
creation loop, so workers added around it only contend.  (An earlier note
modelled this as `max(creation, work/workers)` with creation at 9.82 s; that
cannot be right, since `max(9.82, x) >= 9.82` while the 15-worker point is
7.91 s.  Creation is not a worker-count-independent constant, and the two
measured points are what stands.)

**Deliberate deviation from the width rule.**  A wavefront's instantaneous
frontier peaks at `min(W,H)`, so offering width `X` costs `X²` tiles under any
tile aspect ratio and under either lever.  The rule's 1x floor (3,456) is
11.94 M tiles and its 4x slack (13,824) is 191 M.  Memory is not what blocks
it: 11.94 M tiles is `3·3457²` = 35.9 M events at ~508 B/event -- a constant
apportioned from the measured ~3.0 GB resident set over the 5,905,215 events
of the calibrated instance, so the formula reproduces that anchor by
construction and is an extrapolation anywhere else -- about 18 GB at one node -- comfortably inside the 190 GB budget.  The window
is.  This row builds its whole graph in `mainEdt`, so creation cost tracks the
task count directly, and the catalog already holds the measured point that
prices the tile lever: **tile 50 at 100.8 s with width 0.81x**, i.e. the tile
route leaves the window before it reaches 1x.  Scaling the 1-node anchor by
the task count puts 11.94 M tiles at ~57 s on one node, which is survivable --
but the multi-node cells are the exhibit, and this row multiplies by 19-37x at
the first node boundary, which lands them past a 900 s cell.  So the tile
stays 100, the width is given up at **0.41x** (`W=1400`, `H=1404`,
peak `min(W,H)=1400`, average `W·H/(W+H-1)=701` = 0.20x), and the width the
base row cannot reach is supplied by the restructured row
(`smithwaterman_dist`) at 2.31x.  Both tiers share these arguments -- they are
one catalog entry and differ only in the binary stem -- and the 1-node cells
coincide, which is what makes the anchor comparable.

An earlier note blamed 210 GB of tile memory.  That was wrong twice over: the
tiles ARE reclaimed -- the task destroys the three blocks it read -- and the
figure was arithmetic for a 515,000 pair this row no longer runs, while the
arguments beside it were a 140,000 one at 0.41x rather than the 1.49x claimed.
What did accumulate was the readiness events, three per tile and never
destroyed; they are reclaimed now by the same consumer that frees the blocks
they carried, which is 9.10 s and 2 GB before against 7.99 s and 1 GB after
(same-vintage pair, at the trend size).  That reclamation is a disclosed
adaptation, not upstream behaviour, and it has two halves: it frees three
events per tile, and it grows each create's `paramv` by three GUIDs, which
lands on the serial phase that dominates the window.

**Memory formula (1 node).**  The live set is dominated by the readiness
events, all created before any tile runs:
`3·(W+1)·(H+1)` events at ~508 B each (the per-event constant is apportioned
from the measured ~3.0 GB over the 5,905,215 events of that instance, not an
independent per-event measurement), plus the `H+2` tile-matrix structure DBs
-- one pointer DB of `8·(H+1)` B and `H+1` row DBs of `24·(W+1)` B each, where
`24 = 3·sizeof(ocrGuid_t)` under 64-bit GUIDs (`Tile_t` is three event GUIDs;
the build defines no `OCR_ENABLE_128_BIT_GUID`) -- the params DB
(`8·(8 + ⌈len1/8⌉ + ⌈len2/8⌉)` B), and per-worker temporaries bounded by the
worker count rather than by the frontier
(`workers · (4(tw+1)(th+1) + 8(th+1))` B, ~4.5 MB at 108 workers and tile
100), since each task destroys its temporaries before returning.  At
`W=1400, H=1404`: 3.00 GB of events + 47.2 MB of tile-matrix DBs
(`1405·24·1401` B + `8·1405` B) + 280 KB of params + 4.5 MB of temporaries
≈ **3.05 GB**, ~1.6% of the 190 GB budget; the budget is first reached around
`W=H≈11,165` (L≈1.12 M at tile 100) on the same apportioned constant, well
past any size the window allows.

**What the placement layer buys, and at which size.**  The figures below were
taken at the **70,000/70,000 trend pair** (`W=H=700`) on **four nodes**, not
at the calibrated pair; they have not been re-taken at the calibrated size.
The EDT counts are already even without the layer -- 122,500 finished per rank
either way, imbalance 1.00, because a hintless create round-robins -- so what
the layer changes is not who works but where the data is: remote acquires fall
from **49.95% to 0.27%** (978,954 of 1,959,999 against 5,250) and the bytes
crossing from 605 MB to 262 MB.  Banding by row makes each tile's row
neighbour rank-local, so the ranks stop waiting on remote acquires.

The layer's own trend, same binaries, same arguments, 15 workers/node
(`logs/adhoc/2026-08-27-five`): base 1.446 / 51.27 / 41.18 / 51.77 s against
hinted 1.431 / 25.61 / 26.48 / 35.23 s over 1/2/4/8 ranks -- identical at one
node by construction, 2.00x / 1.56x / 1.47x ahead at the rest, and still
anti-scaling, so the layer sharpens the exhibit rather than hiding it.  A
campaign at the gate instance agrees in direction (2.06x / 1.40x / 1.30x,
`logs/exp/20260902-222905`), but that instance is 40,000 tiles of 100 DP cells
against the calibrated 1,965,600 of 10,000, so it prices per-message overhead
rather than payload locality; the calibrated-regime claim rests on the trend
set above.  A 1-node A/B at the campaign's own gate arguments gives 0.118 s
base against 0.114 s hinted, confirming the `nranks <= 1` no-op.

A blocked-cyclic band (`⌊(i-1)/B⌋ mod P`, `B = H/(kP)`) would shorten the
pipeline fill `k`-fold at `k` times the crossings.  It is not worth an A/B on
*this* tier: the create loop is row-major on one rank and creation dominates,
so rank `P-1`'s tiles are not even created until ~`(1-1/P)` of the creation
time has passed -- exactly the interval the band map's fill would have cost.
The knob is a question for `smithwaterman_dist.ownerOf`, where both phases are
parallel across bands and the fill is real idle time.
