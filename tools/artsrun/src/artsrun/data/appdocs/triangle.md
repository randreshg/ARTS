# triangle

*The 14-peg triangular-board solitaire puzzle, solved by recursive game-tree
search — the tree's own return path is the reduction, no shared counter
exists.*
Source: `third_party/ocr-apps/apps/triangle/refactored/ocr/intel/triangle.c`
(~535 lines).

## Overview

Despite the directory name, this is not a graph benchmark: it counts
solutions to the classic triangular peg-solitaire puzzle — the author's
board is the 15-hole 5-row triangle (`BOARDSIZE` = 15, `MOVESIZE` = 36
directed jumps, `BOTTOM` = 13 moves for a full solve), and `rows` (argv[3])
plays the same game on a larger triangle (see Parameters).
`triangleTask(nummoves, oldmove, ..., depth)` applies `oldmove`
to a copy of its parent's board, and either (a) the search has reached
`depth` moves — one solution, return 1 — or (b) it enumerates every currently
legal jump (`nlegal`) and spawns one child `triangleTask` per legal move plus
one `sumCountsTask` continuation that waits on all the children's counts,
sums them, and forwards the total upward — a leaf with no legal moves before
`depth` is a dead end and returns 0. The recursion's return path *is* the
reduction tree; there is no shared/global counter. `final count` is the
result scalar; at the author's board and full depth the known answer is
29760 solutions (checked in-source against a `PASS`/`FAIL` literal).

**Disclosed local adaptations to the published program** (base tier; all of
them are conformance or contract fixes, none changes the compute-phase
decomposition):

1. The published solution counter was a single shared block incremented
   under `DB_MODE_EW` from a per-solution `incrementTask` inside a FINISH
   EDT (upstream `d8f48ba6`), later made a local `atomic_fetch_add`
   (`ef66fa7b`). ARTS maps OCR `EW` to `RW` (per-node exclusive, OCR RW
   semantics), so concurrent increments lose updates — the contract cannot
   be honoured. `d89eafa5` replaced it with the per-node 8-byte count block
   and the `sumCountsTask` reduction tree that the return path already
   implied. Side effect worth stating plainly: this also deletes a
   node-serializing hot DB and drops the EDT count from ~41.3 M to 22.7 M at
   the calibrated arguments. The FINISH EDT itself is supported and was not
   the reason.
2. `a9586e01`: the initializer acquires `pmovesDb` `RW`, not `CONST` — a
   `CONST` acquire grants a copy that is not written back, so remote readers
   saw a zero jump table.
3. `666a9c55` added `depth`, `1cbf94bf` added `rounds` (default 1),
   `3ffd7a07` added `rows` with its generator and the generator-vs-author
   oracle, plus the unconditional board/count destroys (without which the
   author's own puzzle dies at ~125 GB) and the `OCR_APP_COUNTED_EVENTS`
   gate, which `benchmarks/apps/CMakeLists.txt` sets for every backend.
4. The round's `oldboardDb` and `pmovesDb` are **knowingly retained**: one
   pair per round (288 B + 3,024 B at rows 8, so 3.3 KB at the default
   `rounds` = 1) is never destroyed. The only event that says the round is
   over is the root's count, and that count is satisfied from inside a task
   body — it orders the satisfy, not the release of the CONST acquires the
   satisfying tasks still hold on the jump table every node of the tree
   reads. Destroying there would be an unordered destroy of a globally
   shared block; the runtime implements no deferred destroy, so the leak is
   the cheaper defect and it is recorded rather than traded away. The point
   that *would* order it is a finish scope over the whole search, which is a
   compute-phase task-graph change and is not open to the base tier.
5. Every argument is validated loudly: a token that is not entirely a number
   (`strtol` with a whole-token check, so `8x` and an overflowing literal are
   rejected, not truncated), a `rows` outside `[3, 10]`, a `depth` outside
   `[1, holes-2]`, a `rounds` below 1, or more than three positional
   arguments prints a diagnostic and aborts. The published program silently
   reverted to the author's board or clamped to the full search, so a
   mistyped roster argument ran a different problem instance and still
   printed a marker-matching result line.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `depth` | number of moves to search; a full game is `holes-2` (`BOTTOM` = 13 on the author's board) | `holes-2` if omitted; a value outside `[1, holes-2]` **aborts** | ✓ parsed in `mainEdt`, carried via EDT paramv to every `triangleTask` create — multinode-safe |
| `argv[2]` = `rounds` | repeat the full (fresh, independent) search this many times, sequentially | 1 (the published behaviour — one search); a value below 1 **aborts** | ✓ parsed in `mainEdt`, carried via paramv through `wrapupTask`/`launch_round` — multinode-safe |
| `argv[3]` = `rows` | board rows: absent = the author's 5-row board driven by the author's hand table; given (5 included) = the jump-table generator drives, holes = rows(rows+1)/2, full solve = holes-2 moves; the generator is checked against the author's table whenever the board is the 5-row one | absent (author mode); a value outside `[3, 10]` **aborts** (the placement key is a `u64` hole bitmask, so holes ≤ 64) | ✓ parsed in `mainEdt`, geometry carried via paramv (`holes`, `nmoves`) to every task — multinode-safe |
| `BOARDSIZE` / `MOVESIZE` / `BOTTOM` | the author's-board instances of the geometry (15 holes, 36 jumps, 13-move full solve) | 15 / 36 / 13 | derived from `rows` when one is given; the `#define`s remain the author-mode values and the generator's oracle |
| scatter depth | hinted-placement-only: how many top levels of the tree are scattered by a hash of the board bitmask | **derived, no knob** — `triScatterLevels(nranks)` = `4 + ceil(log10(nranks))`, i.e. 4 at one rank, 5 at 2-10, 6 at 11-100 | ✗ not an argument and not a `#define`: it is a function of `ocrAffinityCount(AFFINITY_PD)`, so base and hinted keep byte-identical argv and the map follows the machine |
| `OCR_APP_COUNTED_EVENTS` | completion events carry their true consumer count | on, every backend | compile-time (`benchmarks/apps/CMakeLists.txt`) |

Each of the three arguments is parsed whole-token (`strtol`, trailing
characters and out-of-range literals rejected, never truncated), and a fourth
positional argument aborts.

## Structure

Branching (`nlegal` per node) is board-state-dependent, so total node counts
have no closed form — only per-node object counts do. Sizes are given for
the author's board (`rows` absent, holes = 15, nmoves = 36) and for the
calibrated board (`rows 8`, holes = 36, nmoves = 126):

| object | count | size (author board / rows 8) |
|--------|-------|------------------------------|
| `triangleTask` | 1 root, plus (data-dependent) `nlegal` children per internal node | — |
| `sumCountsTask` | 1 per internal node | — |
| `newboardDb` | 1 per child edge (`nlegal` per internal node) | `8·holes` = 120 B / 288 B |
| `pmovesDb` | 1 per round, created by `launch_round`, CONST-read by the whole tree | `8·3·nmoves` = 864 B / 3,024 B |
| `oldboardDb` / `boardDb` | 1 each per round, created by `launch_round` | `8·holes` = 120 B / 288 B each |
| count DB (`returnCount`) | 1 per `triangleTask` node — via its own direct call (terminal/dead) or its `sumCountsTask` (internal) | 8 B |
| COUNTED events | 1 per-node `once` broadcast (`nbDeps` = `nlegal`) + 1 `childDone` per child (`nbDeps` = 1) | — |
| `rootDone` event | 1 per round, COUNTED, `nbDeps` = 1 | — |

Every per-node DB is destroyed by the program: the children's count blocks
and the node's own board in `sumCountsTask`, a terminal or dead node's board
by the node itself, the root count block in `wrapupTask`. Live objects
therefore track the search frontier, not the tree's total size. The round's
`oldboardDb` and `pmovesDb` are the one exception — retained by design
(adaptation 4), 3.3 KB per round at rows 8.

Worked example (`depth = 2`, hand-traced from the fixed starting board): the
root has 2 legal opening moves; each of its 2 depth-1 children has 4 legal
moves of its own, for 8 depth-2 grandchildren, all terminal. That's 11
`triangleTask` (1 root + 2 + 8), 3 `sumCountsTask` (root's + its 2 children's),
17 EDTs total (+ `mainEdt`/`realmainTask`/`wrapupTask`), 24 DBs, 14 events.

Counter cross-check: verified (1 node, `depth=1` vs `depth=2`): measured
absolutes EDT 8/18, DB 9/25, EVT 4/14; subtracting the runtime's constant
baseline (+1 EDT, +1 DB, +0 EVT per run) gives app-side EDT 7/17, DB 8/24, EVT
4/14 — exactly the re-traced worked numbers above.

At the calibrated `8 1 8` the whole instance is known exactly by enumeration:
level widths L1=2, L2=8, L3=58, L4=562, L5=6,520, L6=87,096, L7=1,278,558,
L8=19,979,938 leaves; 21,352,743 nodes + 1,372,805 summers = 22,725,548 EDTs
(= counted `NUM_EDT_CREATE − 4`), 21,352,742 boards, and 19,979,938
solutions — the catalog's `expect`. No in-program oracle fires at those
arguments (the 29,760 literal and the generator cross-check are both
author-board gates), so the pin rests on that enumeration plus
cross-configuration consensus.

## Wiring

Every `triangleTask` copies its `oldboard` into its own `board`, applies
`oldmove`, and — if internal — creates a per-node `once` COUNTED event
that broadcasts its own `board` (CONST mode) to every one of its (up to
`nlegal ≤ nmoves`) children at once: this is the app's per-node RO
fan-out, bounded by the jump count (36 on the author's board, 126 at
rows 8). `pmovesDb`, the move table, has no such
bound — it is CONST-read by *every* `triangleTask` node in the whole tree, so
its concurrent-reader count is limited only by how many nodes are runnable at
once, up to the full worker count of the run. Each child also gets a fresh
`newboardDb` (RW, becomes its own `board`) and a `childDone` COUNTED event wired
into its parent's `sumCountsTask`. `sumCountsTask` waits on all `nlegal`
`childDone` events (RO), sums, destroys the children's count DBs and the
node's board, and forwards the total via `returnCount`. `pmovesDb` is therefore
the single dominant fan-in/contention point — small, hot, globally shared,
read-only — the structural reason a per-DB RO-request-combining mitigation has an
outsized effect on this app specifically. No placement hint can replicate a
DB, so that cost belongs to the coherence family, not to the hint layer.

## Flow

The search is depth-first in code structure but not in execution: a node
creates all of its children before any of them runs, so the actual unfolding
is scheduler-driven breadth much like a tree of independent forks. Depth is
bounded by `depth` (≤ holes-2); width at any instant is the number of
currently-live `triangleTask` nodes, pruned hard in practice since legal-move
count shrinks as the board empties (root always starts with exactly 2 legal
moves). At the calibrated `8 1 8` the instantaneous frontier passes 3,456
(the 32-node worker total) from level 6 onward — 87,096 at level 6, 25× the
bar, 370× at level 7. The reduction wave (`sumCountsTask`) follows strictly
behind the search wave. `mainEdt`/`realmainTask`/`launch_round` are a short
rank-0 preamble — three small DB creates plus an O(rows²) jump-table
generator (216 iterations at rows 8), the only serial construction in the
window; `rounds > 1` chains independent full searches serially through
`wrapupTask`, so rounds never overlap.

## Placement (base)

`OCR_APP_OPTIMIZED_PLACEMENT` gates the hint helpers; in base every
`triangleTask`/`sumCountsTask` create collapses to `NULL_HINT`. Effective
policy: EDT → runtime round-robin (per-creating-rank counter), DB → home =
creating rank. Consequence: a node's `once` broadcast delivers its own board
DB (homed on whichever rank the node itself was round-robin-placed onto) to
children that are themselves scattered onto arbitrary ranks, so almost every
child's `oldboard` read is a remote CONST acquire — one cold remote datablock
per tree edge, 21.35 M of them at the calibrated size; the same is true of
`pmovesDb` (homed wherever `launch_round` ran, effectively rank 0) against a
tree scattered across every rank. Locality the puzzle's tree structure would
allow (keeping a subtree together) is never expressed in base — why the app is
a strong RO-fan-in coherence stress case, and why read combining measurably
helps it.

## Placement (hinted)

Hint-only under the guard: the guarded regions add `mixKey`,
`triScatterLevels`, the two hint helpers (`triEdtHintAt`, `triLocalEdtHint`),
the board-bitmask key, the per-node `ocrAffinityCount` query with the
scatter decision and creating-rank hint hoisted out of the child loop, and
the child hint itself. Every `ocrDbCreate`
keeps `NULL_HINT` in both tiers, and the legality scan, the paramv, the event
creation and every `ocrAddDependence` sit outside the guard, so the task
graph, the DB granularity and the wiring are identical in the two binaries.

The map: a child at level ≤ `L` goes to `mixKey(childBits) % nranks`, where
`childBits` is the post-move hole bitmask — a hash of the *position*, so
placement is a pure function of the board and independent of creation order;
a child deeper than `L` and every summer pin to the creating rank, so each
scattered subtree computes, allocates and sums on one rank. Board and return
DBs keep `NULL_HINT` and follow the runtime's creator-home default, which
puts each board on the rank that will consume it.

`L` is **derived from the rank count**, not fixed:
`triScatterLevels(nranks) = 4 + ceil(log10(nranks))`. The scattered frontier
is the set of independent placement units, and its size grows geometrically
with depth (branching here is ~10× per level), so one extra level per decade
of ranks holds the units-per-rank ratio roughly constant. A fixed depth of 3
— what this layer shipped with — has only 58 units on the calibrated board
and therefore cannot cover a wide machine: at 16 ranks one rank gets no
subtree at all and at 32 ranks seven do, i.e. 756 of 3,456 workers
structurally unreachable, with the busiest rank at 2.70× the mean.

Balance of the derived rule on the calibrated instance (`8 1 8`, cost per
unit = exact subtree node count, key = the real `childBits`):

| ranks | derived `L` | units | max/mean | min/mean | idle ranks |
|---|---|---|---|---|---|
| 2 | 5 | 6,520 | 1.01 | 0.99 | 0 |
| 4 | 5 | 6,520 | 1.05 | 0.92 | 0 |
| 8 | 5 | 6,520 | 1.07 | 0.91 | 0 |
| 16 | 6 | 87,096 | 1.07 | 0.95 | 0 |
| 32 | 6 | 87,096 | 1.13 | 0.93 | 0 |

Static-map simulation, twice enumerated independently: exhaustive BFS of the
rows-8 board with memoized exact subtree node counts and the real `mixKey`,
so it is the map the binary computes, not a model of it. What it does not
show is scheduling — per-rank finish-EDT counts at 16 and 32 ranks are the
measurement that would confirm coverage in practice.

The locality it costs is the cumulative width of the scattered levels: 7,150
cross-rank board copies at `L = 5` and 94,246 at `L = 6`, against 21,352,742
tree edges — 0.03 % and 0.44 %. At one rank the scatter is a no-op (`% 1`).

Added cost of the layer, all of it hinted-only: one `ocrAffinityCount` and
one hint construction per internal node (the rank query and the deep-level
creating-rank hint are hoisted out of the child loop — every child of a node
shares its level), plus an O(holes) bitmask per internal node and, for a
scattered child only, one `ocrAffinityGetAt` and one hint construction. A
child below the scattered frontier reuses the node's hoisted hint and costs
nothing per edge.

## Family shape

Measured on ferrari, 15w+1p × 1/2/4/8 nodes, at `7 1 7` — a reduced-geometry
trend probe, NOT the calibration, and taken with the fixed scatter depth of 3.

hinted, e2e seconds — VAL alone anti-scales (the whole tree re-validates
the one move-table DB homed at rank 0 on every acquire, the family's
defining read cost), request combining erases exactly that, and INV/EXCL
are structurally immune (covering read / retained copy):

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 0.4 | 4.9 | 6.7 | 9.4 |
| val_wb | 0.4 | 0.5 | 0.3 | 0.1 |
| inv_wb | 0.5 | 0.3 | 0.2 | 0.1 |
| excl_retain | 0.6 | 0.3 | 0.2 | 0.1 |

base anti-scales on EVERY arm (2n: val_nocomb 24.5 / val 30.3 / inv 73.4 /
excl 39.1) — each tree node's board is a fresh remote datablock, a cold-read
storm no coherence family can serve locally, and INV pays its directory on
top. Base past 2n is unmeasured here; the 2n figure alone bounds the cell.
At the calibrated size the base multinode cells are therefore
reported as censored points (TIMEOUT, or the OOM kill an unbounded in-flight
backlog produces under VAL), never shrunk to fit. These numbers predate the
derived scatter depth, so hinted must be re-trended rather than compared
against them point by point.

## Sizing

"The calibrated `8 1 8`" elsewhere in this document names the arguments the
catalog is currently pinned to, and every structural number quoted against
them (level widths, EDT and board counts, the 19,979,938 pin) is exact by
enumeration and stays true for those arguments. The *choice* of those
arguments is measured on the Dane geometry: 18.6 s base / 19.0 s hinted, 4 GB
resident, against a 60 s target (base anti-scales, hinted scales).

The CLI is `depth [rounds [rows]]`. `rows` (absent = the author's 5-row
board, its hand-written jump table driving the run; given — 5 included — a
generator enumerates the board's jumps, and on the 5-row board it must
reproduce the author's table, which is checked at startup) picks the board
and with it the graph family: holes = rows(rows+1)/2, a full game is
holes-2 moves. `depth` is the size dial inside that board — it truncates
the search, and each +1 multiplies the tree by the board's branching
(measured ~6.4× at rows 6, ~12× at rows 7-8). `rounds` repeats the whole
search sequentially and stays 1: the graph is the dial, not repetition.

Both tiers run the same arguments, so the window belongs to the tier that
must survive the sweep. Base anti-scales by roughly two orders of magnitude
from 1 to 2 ranks, which makes the base curve, not the one-node anchor, the
binding constraint: the size must keep every base cell inside the profile's
`cell_timeout_s` (900 s on Dane, 600 s on ferrari).

Every number in the rest of this section below is a **ferrari-era**
measurement on the Dane-mirror geometry (1 node, 108w+4p, Release,
`rounds` = 1), taken before the Dane per-node width and the 900 s cell
budget were fixed and before the scatter depth became rank-derived; they
are the starting lattice the current choice was sized from, not the current
anchor (see above): `8 1 8` = 10.2 / 18.9 / 13.9 s across the three families
(count 19,979,938); `9 1 8` = 193.7 s (325,211,332); `9 1 7` = 72.0 s
(114,947,436); `10 1 6` = 49.5 s (75,516,988). One depth step multiplies the
tree by the branching, so the lattice is sparse — there is no rung between
depth 8 and depth 9 on any board, and that sparsity is a property of the
program, not of the host, so it survives re-sizing. `8 1 8` sits inside the
60 s target and leaves the base cells room inside 900 s; `9 1 7` is the
fallback if a wider base margin is ever needed. The counts at
the two neighbouring rungs are known by exhaustive enumeration
(depth 8 → 19,979,938, depth 7 → 1,278,558), so a re-size between them needs
no re-pin run.

Memory: the one-node bound is **measurement-anchored, not derived**. Peak
RSS is ~3.8 GB and flat across a 16× range of tree size — 3,980,004 /
3,987,320 / 3,981,496 KB at depth 7 / 8 / 9, rows 8, one node, 108w+4p,
`arts_ocr_val_wb`, recorded in
`logs/adhoc/2026-08-21-triangle-rows8-sweep/track.txt` (2026-08-21). The
per-DB formula `live_frontier × (8·holes + 8) + 8·3·nmoves` — 288 B per live
board, 8 B per live count block and 3,024 B for the jump table at rows 8 —
accounts only for application datablocks; `live_frontier` is itself
unbounded (it is what an unbounded in-flight backlog inflates, and what OOM-
kills the base multinode cells), and the formula omits the per-EDT and
per-event runtime metadata that dominates a 22.7 M-EDT run. So the flat
measurement, not the formula, is what puts this application far inside the
190 GB one-node budget: 4 GB resident at these arguments (see the anchor
above). A re-size would need the figure retaken on the Dane geometry, and it
says nothing about the multinode base cells.

The author's full puzzle (no `rows` argument, depth 13) remains the pinned
29,760-answer correctness case.
