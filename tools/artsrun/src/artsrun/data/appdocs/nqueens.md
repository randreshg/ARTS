# nqueens

*Backtracking N-Queens search over EDTs — every partial board is a task, a
reduction tree sums subtree solution counts.*
Source: `third_party/ocr-apps/apps/nqueens/refactored/ocr/nqueens.c` (~350
lines).

## Overview

Counts the solutions to the N-Queens problem (place N mutually
non-attacking queens on an N×N board) by column/diagonal-pruned
backtracking. Each partial placement is a `findSolutionsEdt`; once the
number of queens already placed exceeds `n - cutoff`, the EDT stops
spawning children and instead finishes its *entire* remaining subtree in
one call via plain sequential recursion (`count_solutions_seq`) — `cutoff`
is exactly the granularity knob between fine-grained EDT parallelism and
coarser in-task compute. Children's counts fold back up through a
`sumSolutionsEdt` continuation per spawning node. The result scalar
(`sols: N`) is checked externally by the harness (catalog `expect`), not
inside the program. Per-EDT work above the cutoff is a handful of
bitmask instructions (no floating point), so — like the cutoff frontier is
made coarser or finer — the program shifts between measuring pure
task/event/DB churn and measuring real backtracking compute.

## Conformance — the reduction is ours, and it is the whole DB/event population

The published OCR program returns nothing up the tree.  Every task is created
`EDT_PROP_FINISH`, so a task's scope completes only when its whole subtree has,
and each solution increments a **process-global `u32 solutions`** with a relaxed
atomic (`nqueens.h`'s `solution_found()`); the shutdown EDT reads that global.
It therefore creates **zero data blocks and zero per-node events** — the entire
DB/event population this row measures is introduced by the distribution
adaptation, and it must be read that way.

The adaptation is forced, not chosen for performance: a process-global counter
is per-rank state, so on any multi-rank runtime each rank counts its own share
and rank 0 prints a fraction of the answer.  The base tier therefore carries an
explicit per-edge reduction — each task returns its subtree count in an 8-byte
block (`return_count`, `nqueens.c:107`) satisfied onto a `ONCE` event its
parent wired into a per-parent `sumSolutionsEdt` (`nqueens.c:185-193`,
`:214-216`).  At `20 16 1 6` that is 721,635 blocks, 721,635 events and 67,087
summers: 100% of the coherence traffic the row exhibits.  The FINISH scopes
themselves were not the obstacle — the shim implements them
(`benchmarks/ocr_shim/arts_ocr.c:1071`).

Two other renderings were rejected:

- **One shared counter block with atomic read-modify-write** — the app README's
  own stated plan.  OCR has no atomic RMW on a data block: every leaf would
  take an exclusive write turn on one block, so the run would measure a single
  contended block's coherence turn-around rather than the search, and the
  serialization would grow with node count.
- **A per-rank accumulator block under FINISH, summed once by the shutdown
  EDT.**  Correct and cheap, but it makes the measured traffic a function of
  the node count (one block per rank) instead of the search, which erases the
  per-edge remote-acquire volume this row exists to exhibit.

The per-edge reduction was chosen because it is the rendering whose traffic is
a function of the published decomposition — one edge per tree edge, exactly the
edges the published FINISH scopes already imply.  Everything else in the
compute phase (the recurrence, the cutoff test, the pruning masks, the
sequential leaf kernel) is the published program unchanged.

## Parameters

| arg | meaning | published default | CLI reachability |
|-----|---------|-------------------|-------------------|
| `argv[1]` = `n` | board size; must be in `[1,30]` | `13` (`Makefile.x86-base:29` `WORKLOAD_ARGS ?= 13 5`; `Makefile.tg:35` publishes `5 2`) | ✓ parsed in `mainEdt` via `ocrGetArgv`, carried in `struct nqueens_args`/`shutdown_args` paramv — multinode-safe |
| `argv[2]` = `cutoff` | queens-placed depth at which an EDT switches from spawning children to a single sequential subtree search; must be in `[0,n)` | `5` (with `n=13`, same Makefile) | ✓ same paramv path — multinode-safe |
| `argv[3]` = `rounds` (optional) | repeats the whole search that many times, chained through `shutdownEdt`; only the final round prints/times | 1 (no upstream analogue — the argument is ours) | ✓ a value below 1 is **rejected loudly** with the usage line, never coerced |
| `argv[4]` = scatter levels (optional) | tree levels (queens placed) scattered across ranks before a subtree pins to the rank it landed on | `NQUEENS_RR_LEVELS` (3) — our `#define`, no upstream analogue | ✓ parsed in `mainEdt`, carried in `struct nqueens_args` paramv to every task — no global; values below **2** are rejected loudly (see below) |

Every argument is validated by an explicit print-and-abort, not by
`ocrAssert`: the reference runtime compiles `ocrAssert` away unless
`OCR_ASSERT` is defined, so an assert-only check is silent on two of the three
runtimes the row runs on.  Nothing is clamped — an out-of-range value ends the
run with the usage line rather than measuring a workload nobody asked for.

**Why scatter has a floor of 2.** The scatter depth is compared against the
number of queens already placed, so `scatter <= 1` places only the empty board
by key; every descendant then takes the creator-pin branch and the whole search
runs on the rank the root landed on.  That is the single-rank funnel the hinted
tier exists to avoid, so the program refuses it in both tiers.  The floor is
the funnel guard only: a *measurement* roster needs `scatter = max_set + 2`,
which is a stronger requirement the program does not enforce — see Placement
(hinted).

**The shipped arguments deviate from the published pair, deliberately.** The
campaign runs `20 16`, i.e. `max_set = n − cutoff = 4`, where the published
`13 5` fixes `max_set = 8`.  `n` is the size calibration (the published 13 is
milliseconds of work).  `cutoff` moves with it so the grain stays fixed:
`cutoff 5` at `n = 20` would mean `max_set = 15` — essentially the entire tree
expanded as EDTs — while `max_set = 4` still leaves 654,548 sequential-leaf
tasks, 189× the widest geometry's worker count.

**`rounds` is repetition, not refinement**: `shutdownEdt` restarts the
identical search, chained after the previous one finishes, so a round adds
wall time and nothing else.  It stays at its default 1 and the board size
carries the weight.  The scatter depth used to be a compile-time constant; it
is the app's only parallelism dial, so it is an argument, with the `#define`
surviving as the default.  Both arguments are parsed identically in the base
and hinted binaries; the base build reads neither, which is what makes the two
tiers argv-identical and the comparison hint-only.

## Structure

Let `d = popcount(cols)` (queens already placed). A `findSolutionsEdt` at
depth `d`: (i) if `d > n − cutoff`, finishes its whole remaining subtree
serially — no further EDTs; (ii) else if `cols` spans all `n` columns, it
is a leaf solution; (iii) else it creates one child `findSolutionsEdt` per
legal column (`available = ~(ldiag|cols|rdiag) & all`) plus one
`sumSolutionsEdt` continuation. There is no closed form for the node
count — which columns survive diagonal pruning at each depth is
board-state-dependent, the same reason N-Queens search trees have no
simple recurrence — but every other quantity reduces to it exactly. Let
`T(n,cutoff)` = total `findSolutionsEdt` invocations and `S(n,cutoff)` =
of those, the spawning ones (creators of a `sumSolutionsEdt`):

| object | count | size |
|--------|-------|------|
| `findSolutionsEdt` | `T(n,cutoff)` | — |
| `sumSolutionsEdt` | `S(n,cutoff)` | — |
| `shutdownEdt` | `rounds` (1 per round) | — |
| `mainEdt` | 1 | — |
| DBs | `T(n,cutoff)` — exactly one 8-byte result block per `findSolutionsEdt`, delivered directly (leaf/dead-end/complete) or through its `sumSolutionsEdt` | 8 bytes each |
| Events | `T(n,cutoff)` ONCE events — one `child_done` per non-root node plus the one `rootDone` | — |
| Templates | `find_template`/`shutdown_template` persist for the whole run; `sum_template` is created and destroyed by every one of the `S(n,cutoff)` spawning calls (pure churn) | — |

Both identities (DBs = Events = `T(n,cutoff)`) hold regardless of `n`/`cutoff`
because every tree node contributes exactly one result and one incoming
edge-or-root-event — and both populations are the distribution adaptation's,
not the published program's (see **Conformance** above: upstream returns
counts through a process-global under FINISH scopes and creates no blocks and
no per-node events at all). Worked numbers (static replay of the recurrence
above, not the compiled binary), two smaller cases kept because they are
cheap to re-derive by hand: `n=6,cutoff=2` → `T=149, S=99`;
`n=15,cutoff=8` → `T=8,586,246`, `S=2,461,096`, giving ≈11.05M EDTs and
≈8.59M DBs/events. Live-set tracks the active frontier, not the total, since
results are destroyed as soon as their consumer reads them.

Counter cross-check: verified (1 node, `6 2` vs `8 3`): NUM_EDT_CREATE
251 → 2591, NUM_DB_CREATE 150 → 1654, NUM_EVENT_CREATE 149 → 1653 —
exactly `T+S+2` / `T` / `T` (app values 250/2590, 149/1653, 149/1653)
plus the runtime's constant +1 EDT/+1 DB/+0 EVT baseline.

At the calibrated `20 16 1 6` (catalog `args` and `expect_args`):
`T = 721635`, `S = 67087`, of which `654548` are sequential leaves —
reproduced exactly by static replay.  Level census
`1 / 20 / 342 / 4964 / 61760 / 654548`: the runnable frontier passes the
widest geometry's 3456 workers already at level 3.

## Wiring

- Every dependence in this app is `DB_MODE_RO` (child results into
  `sumSolutionsEdt`, the root count into `shutdownEdt`) — there is no
  `DB_MODE_RW` anywhere. Each 8-byte result DB has exactly one producer and
  one consumer; there is no fan-out and no contention point.
- `findSolutionsEdt` wires a fresh ONCE event to `sumSolutionsEdt`'s next
  slot *before* creating the child that will satisfy it, so a child can
  never fire an unregistered event.
- `shutdownEdt` depends on the root's `rootDone` event (slot 0, RO); on
  `rounds_left > 1` it re-enters `solve_nqueens` instead of shutting down.

## Flow

`mainEdt` (rank 0) creates both templates once, then `solve_nqueens` seeds
one root `findSolutionsEdt` and its `shutdownEdt` per round. The tree
unfolds depth-first in creation order but executes with scheduler-driven
parallelism: width grows with the branching factor down through depth
`n − cutoff`, then hands off to `n − cutoff + 1`-depth EDTs that each run
an independent, embarrassingly-parallel sequential subtree search
(the actual backtracking compute). A completion wave of `sumSolutionsEdt`s
folds counts back up. `shutdownEdt` is the only serial join point; with
`rounds > 1` it re-seeds a fresh, independent (but identically-shaped,
since the search is deterministic) tree for each subsequent round before
the final one prints and calls `ocrShutdown()`.

## Placement (base)

Both hint helpers (`nqPlaceEdtHint`, `nqLocalEdtHint`) return `NULL_HINT`
outside `OCR_APP_OPTIMIZED_PLACEMENT`; there is no base affinity usage
to report. Effective policy:

- **EDTs**: NULL hint → shim passes `ARTS_HINT_ANY_RANK` → runtime
  round-robin. `findSolutionsEdt`, `sumSolutionsEdt` and `shutdownEdt` all
  scatter across ranks with no relation to which subtree they belong to.
- **DBs**: NULL hint → home = creating rank, i.e. wherever the producing
  `findSolutionsEdt`/`sumSolutionsEdt` happened to land.

Consequence: a child rarely executes on the rank that created its 8-byte
argument (arguments travel via paramv, so this costs nothing), but a
`sumSolutionsEdt` or `shutdownEdt` very often acquires its inputs from a
remote rank holding an 8-byte DB — the same fine-grain coherence stress
pattern as the app's sibling recursive tree-of-tasks benchmarks, at
N-Queens's combinatorial (not Fibonacci) growth rate.

## Placement (hinted)

As-born is placement-blind (see above): `findSolutionsEdt` scatter round-robin
with no relation to their subtree, and every 8-byte result DB homes wherever
its producer landed, so the summing side acquires almost everything remotely.

The layer (`nqPlaceEdtHint` in `nqueens.c`) keys on the subtree's own board
state and uses the column mask's popcount as the tree level: a task whose
number of queens already placed is **less than** the scatter depth (argument,
default 3, **6** in the campaign) is placed by `nqStateKey(board) % nranks`; at
or beyond that depth the task stays on its creating rank (`nqLocalEdtHint`
likewise pins the sum EDTs), so a pinned subtree — its spawn tree, its result
DBs, and its sums — stays on one rank.  Result DBs keep
`NULL_HINT`: the runtime's creator-home default gives the one-shot 8-byte
blocks the local home the 2026-07-09 A/B (2n 48s->250s without it) showed they
must have.

**The key is the whole board, not the column mask.**  A partial board is the
triple `(cols, ldiag, rdiag)`, and `cols` alone is an order-*independent* set:
every permutation of one column set carries the same mask, so keying on it
alone sends all of them to one rank.  At `20 16` the 654,548 leaf boards carry
only `C(20,5) = 15,504` distinct `cols` values — 42 boards per key — and that
correlation, not the hash, is what showed up as load spread.  `nqStateKey`
folds all three masks in (`mixKey(mixKey(ldiag<<32 | rdiag) ^ cols)`), which
costs one extra mix per create and no struct, wiring or control-flow change.
Static replay of the shipped tree, scattered-task max/mean (min/mean), no idle
rank in any cell:

| ranks | `mixKey(cols)` | `nqStateKey` | OR-packed variant | ideal (multinomial) |
|---|---|---|---|---|
| 2 | 1.0060 (0.9940) | 1.0015 (0.9985) | 1.0001 | ~1.002 |
| 8 | 1.0249 (0.9730) | 1.0052 (0.9980) | 1.0031 | ~1.005 |
| 16 | 1.0371 (0.9549) | 1.0096 (0.9903) | 1.0079 | ~1.010 |
| 32 | 1.1168 (0.9067) | 1.0164 (0.9869) | 1.0135 | ~1.016 |

The widened key lands at the spread a perfectly random assignment of 721,635
items to 32 bins would show.  A second candidate — packing the three masks by
shifted OR into one word before mixing — measures within noise of it and, on
these particular cells, marginally under it (1.0122 vs 1.0169 by count and
1.0285 vs 1.0366 work-weighted, both at 32 ranks): the two keys and the
multinomial floor are indistinguishable, so the choice between them is not a
balance argument.  `nqStateKey` was taken because it is lossless in its inputs,
where the OR-pack overlaps `ldiag` and `rdiag` inside 64 bits once the board is
deep enough.  Work-weighted (nodes visited per scattered task, enumerated
exhaustively at `17 13`) moves the same way: 1.1664 (0.8261) → 1.0366 (0.9701)
at 32 ranks.  Both tables are static replays of the real pruned tree, not
runs; the enumerator that produces them is
`logs/adhoc/2026-09-03-dane-reaudit/units/nqueens/nqkey.c`.

**Scatter depth is a measurement invariant, not a free knob.**  A task at or
beyond the scatter depth is pinned, and every task below it in the tree
inherits that rank, so lowering scatter confines whole subtrees.  Charging
every task to the rank it actually runs on (`nqscat.c` in the same directory),
at `20 16`:

| scatter | 8 ranks | 16 ranks | 32 ranks | idle ranks at 32 |
|---|---|---|---|---|
| 2 | 2.3613 | 3.1994 | 3.6646 | **15** |
| 3 | 1.2504 | 1.6259 | 2.2664 | 0 |
| 4 | 1.0397 | 1.1664 | 1.1742 | 0 |
| 5 | 1.0250 | 1.0313 | 1.0724 | 0 |
| 6 (`max_set + 2`) | 1.0052 | 1.0096 | 1.0164 | 0 |

Only `scatter = max_set + 2` — every task placed by key — meets R2's
"imbalance close to 1 at 2, 8, 16 and 32 ranks".  **A measurement roster must
use it.**  Values in `[2, max_set + 1]` are legal (the program accepts them and
they are useful for a gate — `experiments/experiments/paper-gate.yaml` runs a
small one), but at 32 ranks scatter 2 leaves 15 of 32 ranks with no task at
all, and scatter 3 still runs 2.27× imbalance.  The program rejects only the
total funnel (`scatter < 2`, which places nothing but the empty board); the
rest of the range is the roster's responsibility.

**What the layer actually buys, at this grain.**  With `scatter = 6` and
`max_set = 4`, tasks exist only at popcounts `0…5`, so *every* search EDT takes
the hash branch and the creator-pin branch is reached only by the summers.  The
hash therefore does not make any dependence edge local — siblings scatter in
both tiers and the result DB is creator-homed in both.  The layer's mechanism
at this grain is `nqLocalEdtHint`: the summer runs on the rank that created the
`child_done` events it waits on, which removes one of base's three crossings
per tree edge.  The "scatter the top levels, then run each subtree wire-free"
description applies only when `scatter <= max_set + 1`.

## Sizing

`n` and `cutoff` move parallelism in different ways: `n` scales total
search size combinatorially; `cutoff` trades EDT-tree depth/width for
per-leaf serial compute (small `cutoff` → deep tree, many tiny EDTs, more
churn; large `cutoff` → shallow tree, fewer but heavier leaf EDTs).
`rounds` only extends wall time linearly (independent repeats), it does
not change per-round parallelism.

- Pick `cutoff` so the number of depth-`(n−cutoff+1)` leaf EDTs
  (the embarrassingly-parallel serial-search frontier) comfortably exceeds
  total workers — that count is not closed-form, but it grows with the
  branching factor at that depth, so raising `cutoff` by 1–2 is normally
  enough headroom. Pick `n` so `T(n,cutoff)` ≫ total workers for the
  scheduling/tree-churn phase.
- Measured at the Dane anchor node (108w+4p, `cutoff 12`, one round):
  14 → 0.01 s, 16 → 0.11, 17 → 0.81, 18 → 6.6, 19 → 62.9, 20 → 624.8.  The
  ladder is a factor of ~8 per step.  **`20` is the catalog's calibrated
  size**; at the calibrated grain (`20 16 1 6`) its one-node anchor is
  **289.0 s** (the cutoff sweep below), not the 624.8 s the port's inherited
  `cutoff 12` produced at the same `n`.  The step below, `19 15 1 6`, anchors
  at ~30.5 s — an order of magnitude apart, with no rung in between.
- **The solution count needs 64 bits from `n = 19` on** (4 968 057 848 for 19,
  39 029 188 884 for 20).  The port truncated it to `u32` when printing, so
  every size at or above 19 reported a wrong answer until this cycle.
- **`cutoff` and the scatter depth are one plane, not two knobs.**  `cutoff`
  fixes `max_set = n - cutoff`, so tasks exist at popcounts `0 … max_set+1`
  and the ones at `max_set+1` are the sequential leaves.  A scatter of
  `max_set+1` therefore means "scatter every spawning task, leave the
  sequential leaves where they were created", `max_set+2` means "scatter
  those too", and anything beyond saturates: at `cutoff 16`, `s5` = 295.1 s
  leaves the leaves local, `s6` = 287.2 scatters them, and `s7` = 287.3 is
  the same run again.  The plane at `n = 20` over 8 ferrari nodes, E2E seconds:

  | cutoff (max_set) | s2 | s3 | s4 | s5 | s6 | s7 | s8 |
  |---|---|---|---|---|---|---|---|
  | 13 (7) | — | 373.1 | — | 318.3 | — | 321.6 | 540.3 |
  | 14 (6) | — | — | — | — | 305.8 | — | — |
  | 15 (5) | — | 361.1 | 339.3 | 308.9 | **299.8** | 308.8 | — |
  | 16 (4) | 552.3 | 345.6 | 325.3 | 295.1 | **287.2** | 287.3 | — |
  | 17 (3) | 530.1 | 332.2 | 312.4 | **283.6** | — | — | — |
  | 18 (2) | — | — | 304.2 | 301.9 | — | — | — |

  What the plane says is not "scatter as deep as possible": the cost tracks
  the **number of tasks scattered**, and that is exponential in depth (at
  `n = 20`, levels 1…4 hold 67,086 boards, level 7 holds 45,562,852).
  Scattering every spawning level is right only while that count stays near
  1e5 — `c13 s8` scatters 52.2 M tasks and costs 540 s, nearly twice the
  optimum.  Too little scatter is just as bad: the `s2` column is 1.8× the
  optimum because seven ranks sit idle.
- **The fastest cell is not the calibrated one.**  `c17 s5` is 283.6 s and
  `c16 s6` is 287.2 s, but the placement counters at 8 nodes say why the
  slower one is chosen: `c17 s5` leaves 72 417 units (600 per worker here,
  **21 per worker at 32 nodes**) and already shows 1.19× spread in
  `TIME_EDT_EXEC` with 173× spread in steal attempts, while `c16 s6` leaves
  788 725 units, 1.05× and 47×.  The 1.3% is real, not noise — the `s6`/`s7`
  pair of the same row is an accidental repeat of one configuration (scatter
  saturates at `max_set+2`, so both scatter everything) and reproduced to
  287.2 / 287.3 s, putting the run-to-run band under 0.1%.  The 1.3% is
  knowingly paid for 10× the load-balancing headroom at the geometry the
  campaign actually ends at.
- The one-node anchor cannot decide any of this: scatter is a no-op there
  (every affinity resolves to the only rank), so the anchor sees grain only
  and prefers ever-coarser cutoffs — 301.3 / 289.0 / 276.8 s for c15 / c16 /
  c17.  The 8-node plane is what the calibration rests on.
- Memory is not the limiting factor (8-byte DBs, destroyed on consumption);
  size for total EDT count and remote-acquire volume, not footprint.  Upper
  bound at `20 16 1 6`, assuming the whole leaf frontier were live at once
  (it is not): 788,724 EDTs × ~200 B + 721,635 events × ~128 B + 721,635 DBs ×
  (8 B + descriptor + route entry, ~200 B) + the summers' dependence arrays ≈
  **0.4 GB**, against a 190 GB per-node budget.  Memory never binds this row.
- **The hinted tier is what the app is for — but its evidence is
  grain-specific.**  Base at `n=18, cutoff 12`: 32.7 s at one ferrari node,
  151.3 at two, 132.7 at four.  Hinted at the same size: 32.7 / 18.5 / 10.0 /
  5.2 over 1/2/4/8 nodes.  Those numbers are at `max_set = 6`, where 17.2 M of
  17.6 M tasks sit below the scatter depth and the layer creator-*pins* them
  all; at the campaign's `max_set = 4` nothing is pinned but the summers, so
  that mechanism is absent by construction and the ratio does not carry.  At
  the campaign grain the only same-campaign both-tier record is `17 13 1 6`
  (ferrari 1/2/4/8, three arms), where hinted/base is 1.0–1.3× and 1.0× at 8
  nodes — a size too small to separate the tiers (every arm converges on a
  ~4 s floor).  **The R3 gate for this row is therefore open** and must be run
  at the campaign grain and a size that resolves shape (`18 14 1 6`), base and
  hinted interleaved, after the key widening rather than across it.
- At the trend size with the calibrated grain (`18 14 1 5`) the four coherence
  arms run 30.8 / 16.8 / 9.8 / 5.1 and sit within **0.5%** of each other — the
  coherence configuration is nearly not a variable for this application, and
  that is itself the result.  The placement counters say why: 99.8% of acquires
  are local hits, with `NUM_EDT_FINISH` spread 1.10x and `TIME_EDT_EXEC` 1.07x
  across ranks.
