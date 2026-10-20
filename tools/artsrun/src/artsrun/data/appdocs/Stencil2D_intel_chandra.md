# Stencil2D_intel_chandra

*A 2-D five-point star stencil over a rank-per-tile SPMD grid, with a native
OCR-affinity-driven 2-D tile-block→node placement baked into the port.*
Source: `third_party/ocr-apps/apps/Stencil2D/refactored/ocr/intel-chandra/stencil_2d.c`
(~2000 lines, `PROBLEM_TYPE=2` is the header default) + `timers.c`. Byte-for-byte
the same source as `Stencil1D_intel_chandra`'s `stencil_1d.c` — the two
targets differ only in the `PROBLEM_TYPE` compile define.

## Overview

Ports the Intel PRK "Stencil" kernel onto OCR: a square `NP×NP` domain is
tiled into an `NR_X×NR_Y` grid of `NR` tiles ("ranks" in the port's own
vocabulary, distinct from ARTS ranks — see Placement), each running `NT+1`
timesteps of a radius-2 discrete-divergence stencil plus a 4-neighbor
(left/right/top/bottom) halo exchange over pairwise sticky events. At the
final round every tile feeds its local L1 norm and elapsed time into two
independent 10-ary reduction trees (`ADD`, `MAX`); `summary` compares the
reduced norm against the kernel's analytic answer `(NT+1)·2` and prints
`SUCCESS: L1 norm = …` / `Solution validates` on a match, `ERROR: …`
otherwise — the catalog's `L1 norm = ([\-+0-9.eE]+)` regex reads that value
off either line. `FULL_APP=1` (always, in this build) means the compute is
real, not skipped; the program stresses per-tile task/event churn and
4-way halo-exchange DB traffic at a grain set independently of the
per-tile compute (`np_x·np_y = (NP/NR_X)·(NP/NR_Y)`).

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `NP` | side of the square `NP×NP` domain | 1000 | ✓ parsed in `FNC_init_globalParamH`, propagated through `globalParamH_t` (`DB_MODE_CONST`/RO downstream) — multinode-safe |
| `argv[2]` = `NR` | number of tiles; factored into `NR_X×NR_Y` via `splitDimension` (near-square, largest divisor ≤ `√(NR+1)`) | 16 | ✓ same DB propagation |
| `argv[3]` = `NT` | timesteps; each tile runs `NT+1` rounds (round 0 untimed) | 10 | ✓ same DB propagation |
| all three, or none | `argc==4` (exactly 3 user args) is the only recognized case; 1 or 2 args are silently ignored, defaults used | — | ⚠ no partial-override path |
| `HALO_RADIUS` | stencil radius (2, 5-point-per-axis → 9-point 2-D stencil) | 2 | ✗ compile-time `#define` |
| `FULL_APP` | 1 = real compute; 0 = OCR-overhead skeleton | 1 | ✗ compile-time `#define` |
| `ARITY` | reduction-tree fan-out | 10 | ✗ compile-time in `reduction.h` |

`expect: '2202.000000'` in the catalog is a pin for the **calibrated** args
(`expect_args` equals `args`, `NT=1100` → `(1100+1)·2 = 2202`) — a
consensus check only fires when a cell's actual args equal `expect_args`.

## Structure

`NR_X,NR_Y = splitDimension(NR)`; `np_x≈NP/NR_X`, `np_y≈NP/NR_Y`. Setup is
the same fixed 10-EDT chain as the 1-D port, then 3 one-time EDTs per tile
(`rankInit`→`init_rankH`→`init_rankDataH`, one `rankInitSpawner` loop),
then `NT+1` rounds of 11 EDTs per tile (`rankMultiTimestepper`, `timestep`
[FINISH], `Lsend`/`Rsend`/`Lrecv`/`Rrecv`/`Bsend`/`Tsend`/`Brecv`/`Trecv`,
`update`) chained serially per tile via `timestep`'s output event.

Both reduction trees (norm `ADD`, timer `MAX`) route through the same
shared `reduction.c` library as the 1-D port (`type=REDUCE`, `ARITY=10`,
one `reductionLaunch` per tile per tree at its last round). With
`C(NR) = ⌊(NR-2)/10⌋ + 1` the count of tiles with at least one child in the
`ARITY`-ary tree (`NR≤11` ⇒ `C=1`, a single-level star), each tree
contributes exactly `3·NR + 2·C(NR) - 1` EDTs, `3·NR - 2` DBs and
`4·(NR-1) + C(NR)` events (the `NR-1` labeled sticky events among them are
created once each by `reductionRangeCreate` in `init_globalH_part1`) — see
the 1-D doc for the call-by-call derivation (identical logic, this port only
differs in the stencil dimensionality, not the reduction plumbing). Two
trees together: `6·NR + 4·C(NR) - 2` EDTs, `6·NR - 4` DBs,
`8·NR + 2·C(NR) - 8` events.

| object | count | notes |
|--------|-------|-------|
| EDTs | `8 + 9·NR + 11·NR·(NT+1) + 4·C(NR)` | `8+9·NR` folds the two reduction trees into the fixed 10-EDT chain (incl. `mainEdt`) + `3·NR` one-time per-tile setup |
| DBs | `28·NR - 1` | `3+22·NR` (global + per-tile handle/payload DBs, NT-independent) plus the two reduction trees' `6·NR-4` |
| DB payload | `xIn`: `8·(np_x+4)·(np_y+4)` B; `xOut`: `8·np_x·np_y` B; L/R halo bufs `8·HALO_RADIUS·np_y` B; T/B halo bufs `8·HALO_RADIUS·np_x` B | `xIn`+`xOut` dominate |
| Events | `7 + 32·NR + 25·NR·(NT+1) + 2·C(NR)` | `25` fresh events per tile per round (not 22 — see below) plus `24·NR` one-time per-tile phase-buffer events at init, plus the two reduction trees' `8·NR+2·C(NR)-8` |

Each round's 25 events are 13 from EDT creation (`timestep`'s
explicit+output+finish = 3; the 8 halo EDTs' (`Lsend`/`Rsend`/`Bsend`/
`Tsend`/`Lrecv`/`Rrecv`/`Brecv`/`Trecv`) output events; `update`'s
explicit+output = 2) plus 12 more from `update`'s own body, which destroys
and recreates the tile's 12 phase-buffered sticky events (the 1-D port's 6
event types, doubled for the extra top/bottom direction) every round. The
initial 12-events-×-2-phase-buffers (`init_rankH`, once per tile) is the
`24·NR` term.

Calibrated args `['25600', '13824', '1100']` (NP=25600, NR=13,824,
NT=1,100): `splitDimension(13824)=108×128`, `np_x≈237,np_y=200`
→`xIn`+`xOut`≈0.77 MB/tile → **≈10.7 GB** across 13,824 tiles;
`C(13824)=1,383`; EDTs ≈ `8+124,416+167,422,464+5,532` = **≈167.6M**; DBs ≈
`28·13824-1` = **387,071**; Events ≈ `7+442,368+380,505,600+2,766` =
**≈381.0M**.

Counter cross-check: verified (1 node, `NP=64 NR=4 NT=2` vs `NP=64 NR=4
NT=4`, plus `NP=64 NR=16 NT=2` for a second `C(NR)`): predicted absolutes
181/112/437, 269/112/637 and 689/448/1723 (`NUM_EDT_CREATE` /
`NUM_DB_CREATE` / `NUM_EVENT_CREATE`) match the measured counters exactly,
against a runtime baseline of `+1 EDT, +1 DB, +0 EVT`. As in the 1-D port,
the fixed control-chain/payload-DB formulas (`10+3·NR+11·NR·(NT+1)` /
`3+22·NR`) were already exactly right — the gap was the un-derived
reduction trees (`+26` EDT / `+20` DB at `NR=4`, identical to the 1-D
port since the reduction plumbing doesn't depend on stencil dimension)
plus an event slope of 22 instead of the source's actual 25 per
tile-round.

## Wiring

Each tile's 12 payload DBs plus its double-buffered `rankEventH` pair are
private state, touched only by that tile's own chain and its 4 immediate
neighbors' send/recv EDTs. `Lsend`/`Rsend`/`Bsend`/`Tsend` take `xIn` RO
plus their own phase send buffer RW; `Lrecv`/`Rrecv`/`Brecv`/`Trecv` take
`xIn` RW (halo write-in, same buffer as the send side reads, but disjoint
regions by construction — no runtime-visible conflict). `update` takes
`xIn`/`xOut`/`refNorm` RW, `weight` CONST. Cross-tile edges are event-only:
`Lsend` on tile `(ix,iy)` satisfies `EVT_Rrecv_start` on the `(ix-1,iy)`
tile's current-phase `rankEventH`, wired RO into that neighbor's `Lrecv`
(and symmetrically for the other 3 directions) — a halo DB has exactly one
producer and one consumer per round, never concurrent RW from two tiles.
The neighbor's whole `rankH` family (passed `DB_MODE_CONST`, resolving the
same as RO) is the only cross-tile-boundary object besides the halo buffer
itself. The two reduction trees are the sole many-to-one fan-in, mediated
entirely by `reduction.c`'s own events — the labeled sticky events
`reductionRangeCreate` creates once in `init_globalH_part1` and the channel
events the tiles create at each tree's first use — outside the app's own DB
graph.

## Flow

Setup is a short serial prefix, parallel only in its last step (`NR`
independent tile-init chains, launched from one `rankInitSpawner` loop).
Compute is `NR` independent per-tile chains of `NT+1` serial rounds; each
round's own 8-EDT halo exchange is internally parallel, and `update` gates
the next round via its own output event so one tile's slow round only ever
stalls its 4 immediate neighbors, not the whole grid. Parallel width is
`NR` throughout compute (bounded, does not grow with `NT`). Every tile
independently launches its two reductions at its own final round; both
trees' roots (tile 0) satisfy the two result events `summary` waits on.
`summary` prints and returns, and a dedicated `shutdown` EDT, which depends
on `summary`'s output event, calls `ocrShutdown()`: that event fires only once
`summary` has released its dependences, so shutdown never cuts those releases
off.

## Placement (base)

Not a NULL-hint program: `ENABLE_EXTENSION_AFFINITY` is on project-wide
(`OCR_EXT_DEFINES`), and the source uses it unconditionally.
`ocrAffinityCount(AFFINITY_PD, …)` returns the ARTS run's actual node count
(`arts_get_total_ranks()`), independent of the app's own `NR`.
`rankInitSpawner` factors that count into a `PD_X×PD_Y` grid
(`splitDimension`) and maps each tile's 2-D coordinate `(id_x,id_y)` onto a
PD-grid block coordinate via `getPartitionID` on each axis independently —
the same block-partition algorithm used to carve the spatial domain — then
hints the tile's `rankInit` EDT and its `rankH` DB (`DB_PROP_NO_ACQUIRE`,
so the creator never touches the payload) onto that PD via
`ocrAffinityGetAt(AFFINITY_PD, pd, …)`. Everything created from `rankInit`
onward re-derives `ocrAffinityGetCurrent()` and stays put, so a tile's
whole private state and compute chain are pinned to one ARTS rank for the
run. Net effect: the `NR_X×NR_Y` tile grid decomposes into up-to-`PD_X×PD_Y`
contiguous 2-D sub-blocks, one per ARTS rank — **neighbor tiles in both
directions are usually co-resident**, and cross-rank halo traffic is
limited to the tile-boundary pairs that straddle a block edge (perimeter of
the blocks, not their area) — real algorithmic locality the port
expresses. Global bookkeeping blocks (`globalH`, `globalParamH`, created
from `mainEdt`/`globalInit`/`globalCompute`) use `NULL_HINT` and so follow
the ordinary creator-home policy: each lives on the rank of the task that
created it, and those setup tasks themselves carry `NULL_HINT`, which ARTS
places round-robin, so only `globalH` (created by `mainEdt`) is pinned to
rank 0. The reductions' labeled events, created by `reductionRangeCreate`
in `init_globalH_part1`, are homed by index (`idx % nranks`).

## Sizing

`NR` sets SPMD width and (via `splitDimension`) the 2-D PD-block shape;
`NP` sets per-tile compute grain and memory (`np_x·np_y`); `NT` sets serial
chain depth. For N nodes × C workers, pick `NR` a small multiple of `N·C`
(and prefer `NR` with a near-square `splitDimension` factorization, so the
2-D PD-block partition is itself near-square and per-block halo perimeter
stays low relative to block area), then size `NP` for the target memory
footprint (`8·2·np_x·np_y` bytes resident per tile), and `NT` for wall-clock
length. The calibrated args `25600 13824 1100` give a 108×128 tile grid,
`np_x≈237,np_y=200` (≈0.77 MB/tile, ≈10.7 GB total) — four tiles per worker
at the widest campaign geometry (32 nodes × 108 workers = 3,456), and
proportionally more per worker at every smaller node count. As in the 1-D
port, memory scales with `NP²/NR` per tile while `NR` itself is free to raise
(more cross-node parallelism) without moving memory per tile.
