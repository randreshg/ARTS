# hpcg_intel

*HPCG's preconditioned conjugate gradient over an SPMD grid of cube tiles —
a 26-neighbour halo exchange per operator application, a 4-level multigrid
V-cycle inside every iteration, three global sums to close it.*
Source: `third_party/ocr-apps/apps/hpcg/refactored/ocr/intel/hpcg.c`
(~2400 lines) + `timers.c`, linked against the `reduction` library
(`apps/libs/src/reduction/reduction.c`, `ARITY = 10`).

## Overview

Solves `Ax = b` for the HPCG matrix — a 27-point stencil, diagonal 26,
off-diagonals −1, `b` set to the row sums so the exact solution is all ones —
by preconditioned CG. The global grid is cut into `N = npx·npy·npz` cube tiles
of `m³` points; one *tile* (the port calls it a "rank", distinct from an ARTS
rank) is one serial EDT chain owning one large private datablock. The
preconditioner is a 4-level multigrid V-cycle smoothed by symmetric
Gauss-Seidel sweep pairs. At termination each tile computes `Σ(1−xᵢ)²` and a
final REDUCE prints `final deviation: …` — a convergence residual, not a
checksum, so the catalog's scalar moves with the grid, `m` and `maxIter`. Three
stresses at once: a multi-MB per-tile working set touched RW by every EDT in the
chain, 11 halo exchanges per iteration each fanning out to up to 26 neighbours,
and three latency-bound global reductions.

The matrix, the right-hand side and the halo descriptors are generated inside
each tile's own `initEdt`, on that tile's own node, as a pure function of the
tile index and `m` — no input file, no shared RNG, nothing node-count dependent.
Verification is the `Σ(1−xᵢ)²` sweep, one O(local) pass per tile through the
same reduction tree the algorithm already uses; there is no reference re-solve
and no gather. Nothing is written to disk at any debug level.

## Parameters

The parser accepts 0, 3, 4, 5 or 6 user arguments; each prefix extends the
previous one.

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `argv[1..3]` = `npx,npy,npz` | tile grid; `N = npx·npy·npz` | 3,4,5 (`N=60`) | ✓ parsed in `mainEdt`, copied into the shared DB every tile reads RO — multinode-safe |
| `argv[4]` = `m` | tile cube edge, **rounded up to a multiple of 16** | 16 | ✓ same path |
| `argv[5]` = `maxIter` | CG iterations; capped at `HPCGMAXITER` (50) | 50 | ✓ same path |
| `argv[6]` = `debug` | print level 0/1/2; `>0` also enables the per-iteration `rtr`/`rtz` trace | 0 | ✓ same path |
| 1 or 2 arguments | `bomb()` prints `ERROR … TERMINATING`, calls `ocrShutdown()` and exits(1) | — | ✓ fatal |
| `PRECONDITIONER`, `COMPUTE`, `AFFINITY`, `TIMER`; multigrid depth; `ARITY` | V-cycle / real arithmetic / native PD placement / per-phase timestamps; 4 levels (`m/2^l`); reduction fan-out | on,on,on,off; 4; 10 | ✗ compile-time (`NO_*` variants, hardwired levels, `reduction.h`) |

Every numeric argument is validated loudly: a non-positive `npx`, `npy`, `npz`,
`m` or `maxIter`, a `maxIter` above the cap, and a negative `debug` each reach
`bomb()`. The knobs are `u64`, so the checks parse into signed temporaries
first — before that a negative value wrapped and passed. `maxIter == 0`
specifically used to run: it skipped the loop at `hpcg.c:1520-1522` whose body
assigns the `x` pointer the terminating sweep then dereferences.

`expect_args` is kept **identical to `args`**, so the consensus pin rides the
campaign cell itself rather than a degenerate configuration. Because the scalar
is a convergence residual it moves with the grid, so `expect` is regenerated —
never edited — whenever the arguments change.

**Local adaptations to the published program** (all conformance, none touching
the decomposition): `bomb()` exits instead of returning into the arguments it
just rejected; the `#define T` comment corrected (it is the default iteration
count, not a timestep count — the program has no timestep loop); the linked
reduction library frees its per-call accumulator and releases the `nrank == 1`
return block before satisfying it; the argument validation above; a debug-print
loop inside `haloExchangeEdt`'s pack loop that reused the pack counter — at
`debug ≥ 2` it left the counter past the array, so one direction was packed
instead of 26 and the run wedged — now prints the current direction; and the two
per-iteration convergence lines (`time … rtr`, `time … rtz`, tile 0 only) print
at `debug > 0` instead of always, so the measured window carries no
per-iteration stdout.

## Structure

Write `mt_l = (m/2^l)³`, `ht_l = (m/2^l+2)³`, let
`L = (3·npx−2)(3·npy−2)(3·npz−2) − N` be the number of directed neighbour links
(`26N` only if every tile were interior) and `I = ⌈(N−1)/10⌉` the ranks with a
child in the reduction tree.

| object | count | size |
|--------|-------|------|
| private block | `N` (one per tile) | `≈ 429·m³` B — 4 levels of matrix (`27·Σmt·8`) + column indices (`27·Σmt·4`) + diagonal indices, the halo'd `Z`/`P` vectors (`ht·8`) and `R/AP/X/B` (`mt·8`), behind a ~3.4 kB header |
| shared / reduction-private / scalar blocks | 1 / `N` / `N` | 56 B (RO to all `N` `initEdt`s) / struct / 8 B |
| halo blocks | `11·L` per iteration | 8 corners × 8 B, 12 edges × `8m_l` B, 6 faces × `8m_l²` B — `8((m_l+2)³−m_l³)` B per full exchange |
| EDT creates | `55N + 3(2N+I−1)` per iteration | 55 per tile: 5 `hpcg` + 17 `mg` + 11 `haloExchange` + 11 `unpack` + 7 `smooth` + 4 `spmv` |
| Event creates | `37N` per iteration | all `OCR_EVENT_ONCE_T`: 1 (`hpcg` p1) + 3 (`hpcg` p3) + 10 per MG level 0–2 + 3 at level 3 |
| DB creates | `11L + 9N − 6` per iteration | halo blocks + `3N−2` per ALLREDUCE × 3 |
| one-time | `9N + 3I` EDTs, `9N + L − 3` DBs, `6N + 3L + I − 4` events | `mainEdt` and `wrapUpEdt`, then four EDTs per tile: `initEdt`, `channelInitEdt`, the phase-0 `hpcgEdt` and the phase-1 clone that one creates. Blocks: the shared block, a private, a reduction-private and an 8-byte scalar block per tile, and a GUID carrier per link. Events: a `returnEVT` channel per tile, and per link a CHANNEL event plus a labeled sticky created from *both* ends (the shim counts both attempts). No finish EDTs |

On top of that the reduction contributes `2N+I−1` EDTs, `N−1` DBs and
`5(N−1)+I` events of one-off tree setup — per tree link a channel pair and a
labeled sticky attempted from both ends, plus the output events of its two
setup EDTs, the only `ocrEdtCreate`s in the program that pass one — and then
runs `3·maxIter + 2` times. Two of those calls
are outside the iteration loop: the phase-0 `rtr` ALLREDUCE (`2N+I−1` EDTs,
`3N−2` DBs, like any other) and the closing REDUCE, which skips the down pass
and costs `N+I` EDTs and `2N−1` DBs. All of this assumes `N > 1`; a single tile
short-circuits the tree entirely.

Instantiated at the catalog's `24 12 12 32 50` — `N = 3456`,
`m = 32`, `L = 77,464`, `I = 346`: private blocks **13.40 MiB each → 48.6 GB of
payload**, halo traffic ≈0.25 MiB per tile per iteration (4 exchanges at level 0,
3 at level 1, 3 at level 2, 1 at level 3). Setup is 32,142 EDTs / 108,565 DBs /
253,470 events; per iteration cluster-wide **211,851 EDT creates, 883,202 DB
creates, 127,872 event creates**; over the 50 iterations ≈10.6 M / ≈44.2 M /
≈6.4 M. The 2× candidate `24 24 12` doubles `N` and takes `L` to 159,688, so
every per-iteration figure roughly doubles; it was not the catalog's choice
(see Sizing).

Counter cross-check: verified (1 node, `N=8`, `L=56`, `I=1`). `2 2 2 16 2` and
`2 2 2 16 5` measure 1,052/2,516 EDTs, 1,490/3,536 DBs, 805/1,693 events — the
per-iteration formulas give those deltas exactly (488/682/296 per iteration),
and the one-time terms give the absolutes (75/125/213 here) once the runtime's
constant +1 EDT and +1 DB per run are added. `2 2 2 16 3` and `2 2 2 32 3`
measure identical triples, 1,540/2,172/1,101, confirming that `m` moves block
sizes and flops but no object count. Getting there corrected all three setup
terms: the EDT count had omitted `wrapUpEdt`, both `hpcgEdt` creates per tile
and every reduction EDT; the DB count had omitted the per-tile scalar block and
the reduction's blocks; the event count had omitted each tile's `returnEVT`.

## Wiring

A tile's private block is the spine: `hpcgEdt` → `mgEdt` → `haloExchangeEdt` →
`unpackEdt` → `smoothEdt`/`spmvEdt` → back, each hop a fresh
`OCR_EVENT_ONCE_T` carrying the same block **RW**. Every stage releases before
satisfying, so exactly one EDT holds it at a time — a multi-MB block (13.4 MiB
at `m = 32`) with a strictly serial single-writer history and no sharing at all.
`haloExchangeEdt` is the only fan-out: it creates `unpackEdt` (26 RO slots),
packs each face/edge/corner into a fresh small DB, releases it and satisfies
the matching **channel** event, then hands the private block on and satisfies
a control `packEVT` (NULL payload) so the consumer waits for both the local
pack and the remote unpack. A halo block therefore has exactly one writer (the
packing tile, at create time) and exactly one RO reader (the neighbour's
`unpackEdt`, which destroys it) — one-shot migration, never shared.

Neighbour channels are established once: `initEdt` reserves `26N` labeled
STICKY GUIDs, creates a CHANNEL event per outgoing direction, ships its GUID in
an 8-byte DB through the sticky at index `26·myrank+ind`, and collects the
mirror at `26·neighbour + (25−ind)`; `channelInitEdt` installs the arrivals as
`haloRecvEVT[]`. Both ends create the same labeled slot, with `GUID_PROP_CHECK`,
and the rendezvous depends on that being *fail-if-exists*: the shim honours it
(`benchmarks/ocr_shim/arts_ocr.c:1252-1263` returns `OCR_EGUIDEXISTS` without
touching the caller's GUID, so the first creator wins and a satisfied event is
never replaced). Reductions use the same idiom: `reduction.c` builds a 10-ary
tree over labeled stickies and moves 8-byte blocks up (**RO** into the parent's
`yourdata` slots, destroyed there) and back down. The only DB with many
concurrent readers anywhere is the 56-byte shared block (`N` RO readers at
startup); the only many-to-one fan-in is the reduction tree's `ARITY`-way join.

## Flow

`mainEdt` (rank 0 only) reserves two GUID ranges and spawns `N` `initEdt`s;
each runs its own preamble — four `matrixfill` passes building `27·Σmt`
column indices, ≈1.0 M entries per tile at `m=32`, the dominant startup cost —
then the channel rendezvous. The preamble is parallel across tiles and index-pure
(a pure function of the tile index, the grid and `m`), so the only serial setup
in the measured window is the spawn loop itself, which is the published
task-graph construction. Each tile then runs its own chain: `hpcg` p0 seed
(`rtr`) → **p1** convergence test → `mg` V-cycle → **p2** `rtz` → **p3** halo +
SpMV → **p4** `pAp` → **p5** update `x`,`r`,`rtr` → p1. The V-cycle is `mgStep`
0..6 with `level = 3−|mgStep−3|`: levels 0–2 each smooth → SpMV → restrict →
recurse → prolong → smooth (5 `mgEdt` bodies, 3 exchanges), level 3 smooths
once and returns — 17 `mgEdt`, 10 exchanges, 7 smoothers, 3 SpMVs per cycle.

Parallel width is `N` and only `N`: a tile's chain is serial end to end,
V-cycle included, so the machine is busy only while `N ≳ nodes × workers`. The
coarse levels do **not** narrow the width — every tile stays active at every
level — they narrow the *work* by 8× per level while the exchange keeps its
full 26-message shape, so level 3 is essentially pure message latency. Each
reduction is a global barrier `2⌈log₁₀N⌉` hops deep; an iteration is 11
exchange barriers plus 3 reduction barriers. Serial points: `mainEdt`'s spawn
loop and the single `wrapUpEdt`, which prints the two result lines and shuts
down. Nothing else prints inside the window at `debug == 0`.

## Placement (base)

This is **not** a NULL-hint program, and no `OCR_APP_OPTIMIZED_PLACEMENT`
layer exists in the source — what follows is as published. `mainEdt` calls
`ocrAffinityCount(AFFINITY_PD, &PDcount)` (the ARTS node count) and maps each
tile through `getMyPD`, a **recursive bisection**: halve the PD range and the
tile grid's currently longest axis together, recurse. Tile `myrank`'s `initEdt`
is hinted onto the resulting PD; from `initEdt` on, every create re-reads
`ocrAffinityGetCurrent()` into `pbPTR->myAffinityHNT` and passes it to every
`ocrEdtCreate`, so a tile's whole chain — `hpcg`, `mg`, `haloExchange`,
`unpack`, `smooth`, `spmv` and its reduction EDTs — is pinned to one ARTS rank
for the run. DBs carry NULL hints, so home = creating rank = that same node.

Every rank is covered and the load imbalance is **1.000** at every node count of
the campaign sweep for the admissible grids (enumerated over `getMyPDc`:
`24 12 12` gives 3456/1728/864/432/216/108 tiles per domain at 1/2/4/8/16/32
nodes, `24 24 12` gives 6912/3456/1728/864/432/216). The 32-node sub-box is
`3×6×6` for the 1× grid — the minimum-surface rectangular tiling of 108 — and
`6×6×6` for the 2× one.

The consequence is the good one: the private block never leaves its node, and
the tile grid decomposes into contiguous 3-D sub-blocks, one per node, so
cross-node halo traffic is the *surface* of those sub-blocks rather than all
`26N` links. What still crosses: halo blocks straddling a block face, and every
8-byte reduction block — the reduction tree is indexed by tile id, not by node,
so its links ignore the placement entirely. Global objects created in `mainEdt`
(shared block, GUID ranges, `finalOnceEVT`, `wrapUpEdt`) are NULL-hinted, on
rank 0. The one-time channel and reduction stickies are outside the placement
altogether: `ocrGuidRangeCreate` takes no hint and the shim homes labeled index
`i` round-robin at `i % nranks`, so the `27N` rendezvous events are scattered
uniformly regardless of who owns the tile.

## Sizing

`npx·npy·npz` sets the number of tiles and `m` the edge of the cube each tile
owns.  Memory is `≈429·m³·N` bytes of payload.  `m` is not only a size: a tile
computes `∝m³` and exchanges `∝m²`, so **`m` is the granularity**, and the ratio
the runtime has to cover is `6/m`.

Four levels of multigrid halve the edge four times, so `m` must be a multiple
of 16 (the program rounds up) and 16 is the floor -- the coarsest grain the
program can be given.  That floor is where an earlier cycle left this row, and
it is why the row looked like something it is not:

| | `8 8 8 16 50` | `8 8 8 32 50` |
|---|---|---|
| 1 node x 15 workers | 6.31 s | 50.19 s |
| 2 nodes | 32.86 s | 45.49 s |
| | **0.19x -- collapses** | **1.10x -- scales** |

Same tile count, same number of exchanged blocks, four times the face.  At the
floor the exchange is not covered by the compute and the row falls apart at the
first node boundary; one step above it, the row scales.  The earlier reading ran
the causation backwards -- it saw the collapse at `m = 16`, concluded the row was
an anti-scaler, and then used the 10-30 s window that judgment implies to keep
`m` at the floor.

At a correctly grained size the row is a strong scaler, and the gain does not run
out by eight nodes (measured at `16 16 16 32 50`, the previous argument set):

| geometry | `16 16 16 32 50` | cumulative | per doubling |
|---|---|---|---|
| 1 node x 15 workers | 524.7 s | 1.00x | -- |
| 2 nodes | 288.6 s | 1.82x | 1.82x |
| 4 nodes | 188.4 s | 2.78x | 1.53x |
| 8 nodes | 130.7 s | **4.02x** | 1.44x |

All four give the same deviation, holding 87-141 GB.  That argument set reached
the ~150 s anchor at 155.2 s, 160.9 s and 159.0 s on the three coherence
families, holding 125 GB -- an RSS/payload ratio of 2.17 against its 57.6 GB of
payload, the factor used for planning below.

A trend read at a smaller grid understates this row badly -- `8 8 8 32 50` gives
2.05x over the same eight nodes against 4.02x here.  The reason is the same
surface-to-volume ratio that makes `m` matter: at two nodes the bisection puts
25% of an 8³ grid's tiles on the cut plane against 12.5% of a 16³ one, so the
small grid is the least favourable size this application has.

**Width is the rank grid.**  A tile is one runnable EDT at a time -- the private
block is handed RW from stage to stage through fresh ONCE events -- so the
instantaneous width is exactly `N = npx·npy·npz`, the structure is
spawn-and-join, and `N` must be an integer multiple of the widest geometry's
worker count (3456 on the campaign profile).  `16 16 16 = 4096` is 1.185x of it:
tiles per worker are fractional at 4/8/16/32 nodes, and each of the 14 barrier
intervals per iteration then costs `⌈t/w⌉` rounds -- a 1.055/1.055/1.266/1.688x
quantisation tax that grows exactly where the scaling claim is made.  The two
grids that satisfy the rule are `24 12 12` (3456, 1x) and `24 24 12` (6912, 2x);
both balance exactly under the app's own bisection.  Memory decides between
them:

| grid | `N` | `m` | payload @ 1 node | RSS @ 2.17x |
|---|---|---|---|---|
| `24 12 12` | 3456 | 16 | 6.1 GB | ~13 GB |
| `24 12 12` | 3456 | 32 | 48.6 GB | **~105 GB** |
| `24 24 12` | 6912 | 16 | 12.3 GB | ~27 GB |
| `24 24 12` | 6912 | 32 | 97.1 GB | **~211 GB** |

`m` has no rung between 16 and 32.  So the 2x width is reachable only at the
granularity floor, where this row stops scaling at all; 1x at `m = 32` is the
point that satisfies width, memory and grain together, and it is the map
`experiments/profiles/dane.yaml` prescribes for this app.  Its work is 0.844x of
the `16 16 16` anchor, so expect a ~131-140 s band rather than a point estimate:
a `3x6x6` sub-box carries ~3.7% more directed cross-node links per tile than the
`4x4x8` the old grid gave.  `24 12 12 32 50` is the catalog's final argument
set, with `expect` re-pinned at it (`14248044.343658`).

`maxIter` stays at the application's own cap of 50: in HPCG the CG iteration
count is fixed by the algorithm and the benchmark's variable is the grid.  `T`
is that cap's default when the run gives no iteration argument -- its comment
used to read "number of time steps", which is what led this catalog to describe
the run as 50 timesteps of 50 iterations.

## Placement (hinted)

A placement layer was written for this row -- the one map the program leaves
to a hint, see the table below -- and measured from several angles.  No map
tried beats the base's own, so **the base's map is the best hinted map** and
the base tier stands in for the hinted comparison -- see the verdict at the end
of this section for what that rests on.

### What a hint can move at all

Every create on the step path, with its placement and its cross-node reader:

| created per step | count | base placement | who reads it | can a hint move it |
|---|---|---|---|---|
| 55 EDTs per tile per iteration (`hpcg`, `mg`, `haloExchange`, `unpack`, `smooth`, `spmv`) | `55N` | `pbPTR->myAffinityHNT` = `ocrAffinityGetCurrent()` recorded in `initEdt` = the tile's own node | the tile's own chain | no -- already pinned, and the 13.4 MiB private block is handed RW along that chain, so any other placement moves it |
| 37 `OCR_EVENT_ONCE_T` per tile per iteration | `37N` | created by the tile's own EDTs | the tile's own chain | no -- `ocrEventCreate` takes no hint |
| halo blocks | `11L` per iteration (`L` = directed neighbour links) | `NULL_HINT` -> home = creating rank = the **producer**'s node | exactly one reader: the neighbour's `unpackEdt`, RO, which then destroys it | **yes** -- the reader's node is known at `initEdt` time |
| reduction partials / results | `3(3N-2)` per iteration | `NULL_HINT` inside `reduction.c` | the participant's parent in the 10-ary tree, RO, destroyed there | not from this application: `reduction.c` is compiled once as the `ocr_lib_reduction` OBJECT library and the `_hinted` flavour define is applied to the executable target only, so a guard in that file is never enabled |
| private / reduction-private / scalar block per tile | `3N`, one-time | creating rank = the tile's node | the tile's own chain | nothing to move |
| shared block (56 B) | 1, one-time | rank 0 | `N` `initEdt`s, RO | one home cannot be local to `N` readers |
| channel carrier (8 B) + labeled stickies | `26N` + `27N`, one-time | carrier: creating rank; sticky: `index % nranks` | the neighbour's `channelInitEdt` | one-time; outside the measured steady state |

So **one** recurring object is both placed by a hint and created more than once
per run, and the layer is that one: the halo block, homed on its consumer.
The hint is computed once per direction in `initEdt` from the neighbour's rank
through the program's own `getMyPD` bisection, and only for directions whose
neighbour is on another node -- a direction inside the node keeps the default
home, so nothing that was already local is disturbed.

### What it should save per step

Per iteration a tile packs 11 exchanges over its (up to 26) outgoing
directions.  Only the directions that leave the node cost anything, and the
app's own bisection decides how many those are:

| directed links leaving the node, per tile | 2n | 4n | 8n | 16n | 32n |
|---|---|---|---|---|---|
| trend grid `10 6 6` (`L` = 6808) | 1.42 | 3.73 | 5.76 | 7.93 | 11.36 |
| flatter `24 5 3` (same 360 tiles) | 0.51 | 1.52 | 3.54 | -- | -- |
| campaign grid `24 12 12` (`L` = 77464) | 0.67 | 2.01 | 3.27 | 4.45 | 6.82 |

For one such block the wire cost is:

- **base** (home = producer): the consumer's RO acquire is a remote request to
  the producer, which is both home and owner -- request + payload -- and the
  destroy is one more message back to that home.  Three messages, and the
  round trip sits on the consumer's critical path.
- **hinted, write-through**: the create installs the directory entry at the
  consumer (one message, no payload), the producer's release writes the payload
  through to that home, the consumer's acquire is a local hit and the destroy is
  local.  Two messages, and the payload arrives before the consumer asks -- the
  round trip leaves the critical path.
- **hinted, write-back**: the payload stays with the last writer, so the
  consumer's acquire resolves at its own home, is forwarded to the owner and
  answered from there.  The create message replaces the destroy message and the
  round trip stays where it was.

The prediction is therefore sharp: the layer buys a round trip per cross-node
halo block under WT and close to nothing under WB, and whatever it buys grows
with the table above.

### Measured

The layer was re-implemented exactly as described and measured against the base
tier in the same campaigns (15w+1p x 1/2/4/8 nodes, `[E2E]` seconds, median of
two; every cell of every table printed the same `final deviation`, so the map
moved nothing but placement):

| | | 1n | 2n | 4n | 8n |
|---|---|---|---|---|---|
| `10 6 6`, `m = 32`, INV x WB | base | 35.569 | 25.798 | 23.161 | 19.480 |
| | hinted | 35.536 | 28.973 | 29.566 | 25.282 |
| | hinted/base | 1.00x | 1.12x | 1.28x | 1.30x |
| `10 6 6`, `m = 32`, INV x WT | base | 35.555 | 25.869 | 23.200 | 19.547 |
| | hinted | 35.512 | 30.707 | 34.187 | 27.806 |
| | hinted/base | 1.00x | 1.19x | 1.47x | 1.42x |
| `10 6 6`, `m = 16`, INV x WB | base | 4.355 | 15.241 | 20.302 | 19.632 |
| | hinted | 4.337 | 21.247 | 27.737 | 25.718 |
| | hinted/base | 1.00x | 1.39x | 1.37x | 1.31x |
| `10 6 6`, `m = 16`, INV x WT | base | 4.377 | 16.503 | 19.973 | 18.430 |
| | hinted | 4.382 | 23.344 | 32.913 | 26.787 |
| | hinted/base | 1.00x | 1.41x | 1.65x | 1.45x |

A hint is a no-op at one node and the one-node cells confirm it to three
digits.  Everywhere else the layer costs time, at the calibrated granularity
and at the floor alike, and it costs more in the arm the analysis said it
should help.

**Geometry sensitivity.**  The same 360 tiles and the same `m = 32` on a
flatter box (`24 5 3`, which the app's own bisection cuts into domains with
0.51 / 1.52 / 3.54 crossing links per tile at 2 / 4 / 8 nodes against
1.42 / 3.73 / 5.76 for `10 6 6`):

| | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| INV x WB base | 35.634 | 20.090 | 16.060 | 13.927 |
| INV x WB hinted | 35.465 | 20.294 | 18.669 | 18.226 |
| INV x WT base | 35.556 | 20.018 | 15.989 | 14.445 |
| INV x WT hinted | 35.410 | 19.858 | 21.777 | 20.278 |

The penalty is a function of exactly the traffic the layer touches: where
crossings are fewest (2 nodes here, 0.51 per tile) it is a wash in both arms,
and it grows with the crossing count -- +0.2 s at 0.51 crossings per tile,
+2.6 s at 1.52, +4.3 s at 3.54, +6.4 s at 3.73, +5.8 s at 5.76 (INV x WB).

**Counters** (8 nodes, `10 6 6 32 50`, counterset `perf`, cluster totals):

| | base WB | hinted WB | base WT | hinted WT |
|---|---|---|---|---|
| `NUM_DB_CREATE` | 3,916,146 | 2,776,546 | 3,916,146 | 2,776,546 |
| `NUM_DB_ACQUIRE_REMOTE` | 1,237,628 | 1,237,747 | 1,237,624 | **97,968** |
| `NUM_REMOTE_SEND` | 8,616,480 | 10,895,680 | 8,620,624 | 12,039,420 |
| `BYTES_REMOTE_SENT` | 1,024,540,700 | 1,197,759,900 | 1,024,673,308 | 1,270,826,636 |
| `BYTES_DB_PAYLOAD_SENT` | 717,479,912 | 717,479,912 | 717,496,488 | 717,496,488 |

`NUM_DB_CREATE` counts only the local-create branch, so the difference,
1,139,600 in both arms, is the number of remote-homed creates -- exactly
`11 exchanges x 2072 crossing links x 50 iterations`, the bisection's own
count, which is the check that the layer moves the blocks it means to and no
others.  The rest reads as the analysis predicted, and settles the question:

- under **write-back** the layer changes nothing it targets.  Remote acquires
  are the same to five digits and the payload byte count is identical; all it
  adds is two messages per moved block (+2,279,200) and 17 % more wire bytes,
  because the reader still has to fetch from the last writer whatever the home
  says.
- under **write-through** it does what it was designed to do -- remote acquires
  fall by 92 %, from 1,237,624 to 97,968, essentially one removed per moved
  block -- and still loses, because the same payload now travels as a push
  instead of a pull (`BYTES_DB_PAYLOAD_SENT` identical) while the message count
  rises by three per moved block (+3,418,796).

### Verdict

**Hinted was tried and none of it beats the base: every map this program
leaves to a hint was measured, the base's own map is the best of them, so the
base tier stands in for the hinted comparison.**  That is now
measured rather than argued.  The layer exists in the source behind
`OCR_APP_OPTIMIZED_PLACEMENT` / `HPCG_HALO_HOME_HINT` and the tables above are
it; the catalog carries `hinted: false` and the rosters run base alone.

The cost and the benefit of moving a halo block's home scale with the same
quantity -- the number of links that cross a domain boundary -- so a wider
geometry cannot flip the sign, it can only widen the gap.  On the campaign grid
`24 12 12` that quantity goes 0.67 / 2.01 / 3.27 / 4.45 / 6.82 crossings per
tile at 2 / 4 / 8 / 16 / 32 nodes, so 16 and 32 nodes sit past every point
measured here, on the losing side.

The historical record this section used to carry -- the same layer measured
1.09x / 1.11x / 1.21x *faster* at `m = 16` on two, four and eight nodes -- does
not reproduce.  It predates the coherence rebuild (the VAL / EXCL / INV
families and the WT / WB axis) and no run of it survives under `logs/`, so it
is history, not evidence; the tables above replace it.

One further map was tried in the past and also lost: a reduction tree
renumbered into domain order (the library builds its tree on the participant
index while the ranks are placed by bisection, so the two maps are unrelated) --
1.00x, 0.92x, 0.84x at two, four and eight nodes, worsening with node count,
since domain-ordered numbering concentrates the tree's upper levels in one
domain.  It is unreachable from this application anyway, for the reason the
table above gives.

### The restructured tier: `hpcg_intel_dist`

This section used to end the row with "no restructured tier either: the row
scales, and its setup is already parallel".  The second clause is true and
stays true -- `matrixfill` runs inside each tile's own `initEdt` on that tile's
own node, so the serial-init floor that forced the `hpgmg` rewrite is absent.
The first clause does not survive the row's own numbers.

Read against what the machine can give (`15N/16`, because a one-node cell
reclaims its progress thread as a worker), the local trend is **72 % / 43 % /
25 %** of achievable at 2 / 4 / 8 nodes, and the run burns **4.03x** the
core-seconds at 8 nodes that it does at 1.  What sets that is a count, not a
volume: **286 datablock creates and 286 destroys per tile per iteration**
(26 directions x 11 exchanges), 220 of them carrying 92 B, so the cross-node
BLOCK count per node per iteration is flat in the node count while compute per
node falls as 1/N -- 12,716 / 19,074 / 15,521 / 10,576 / 8,104 at 2 / 4 / 8 /
16 / 32 campaign nodes.  At 8 nodes that is 1.077 M remote messages per node
through one progress thread in 18.82 s: one every 17.5 us, against the 4.3 us
linear speedup would require and the 26.0 us a remote read-acquire round trip
measures.  The count does not fall with the node count, so more nodes cannot
buy the budget.

Neither knob this row has can reach it.  Placement was measured from four
angles and lost in every one (above): a home is where a block LIVES, and the
count is set by the decomposition.  Grain would dilute the per-message cost --
a tile computes as `m^3` and exchanges as `m^2` while the message count is
independent of `m` -- which is exactly why `m = 16` collapses and `m = 32`
scales; but `m` must be a multiple of 16 and `m = 48` needs ~356 GB against a
~190 GB node budget, so **`m = 32` is the last rung that fits** and the knob is
already at its maximum.

`hpcg_intel_dist` changes the exchange's container instead: one persistent,
double-buffered block per (tile, destination policy domain) per level, so a
payload crosses once per reading domain rather than once per reading tile.
Identical bytes, identical values, identical synchronisation -- 5.9x fewer
cross-node halo blocks at 32 nodes and 42.6 M creates plus 42.6 M destroys
removed from the run.  See that row's document.  The measurement above is what
pointed at it: what costs here is the per-exchange datablock itself, not where
it is homed.
