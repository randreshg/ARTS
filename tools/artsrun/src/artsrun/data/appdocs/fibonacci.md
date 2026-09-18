# fibonacci

*Recursive Fibonacci over EDTs — every call is a task, every argument a
datablock.*
Source: `third_party/ocr-apps/apps/fibonacci/ocr/fib.c` (~400 lines).

## Overview

Computes `fib(n)` by literal binary recursion: each `fibEdt(n)` with `n >= 2`
spawns `fibEdt(n-1)` and `fibEdt(n-2)` plus a `complete` EDT that sums their
results; `n < 2` is a leaf.  The result scalar (`answer is N`) is checked
inside the program against an O(n) iterative reference before `ocrShutdown()`.
There is no compute payload — each EDT does a handful of instructions — so the
program measures task creation, scheduling and fine-grain data movement, not
arithmetic.

## Parameters

| arg | meaning | accepted range | CLI reachability |
|-----|---------|----------------|------------------|
| `argv[1]` = `n` | recursion input; sizes the whole graph exponentially | `[2, 47]`, required — no default | ✓ parsed in `mainEdt` via `ocrGetArgv`, propagated through the argument DB — multinode-safe |
| `argv[2]` = scatter levels | how deep the spawn scatters across ranks before a subtree pins to the rank it landed on | `[1, n]`; absent ⇒ `FIB_RR_LEVELS` (14), which is itself range-checked, so `n < 14` must name a depth explicitly | ✓ parsed in `mainEdt`, carried in each task's paramv — no global, so every rank sees it |

The scatter depth used to be a compile-time constant.  It is the app's only
placement dial and is calibrated by measurement, so it is an argument; the
`#define` survives as the default when the argument is absent, and the base
build parses it identically but never converts it into a hint.

`mainEdt` validates **both** words the same way — `fibArgNumber` refuses a
leading sign and any trailing junk (`strtoull` alone would turn `abc`, `""` or
`11x` into a silently different run) — before any object is created, and calls
`ocrAbort(1)` with a printed reason rather than degrading:

- an argument count outside `[2, 3]` — the usage line is a refusal, not a
  fallback to a default size;
- a non-numeric or signed `argv[1]` / `argv[2]`;
- `n` outside `[2, 47]` — below 2 the root is already a leaf and the run would
  report a checked answer having created nothing; above 47 the 4-byte result
  block and the `u32` sum wrap identically to the reference, so the comparison
  checks nothing;
- a scatter depth outside `[1, n]` — depth 0 would pin every child to its
  creator and run the whole recursion on one rank, and no level deeper than
  the input exists to scatter (a depth above `n` only maximises wire traffic);
- a depth too shallow for the rank count.  The heaviest subtree at depth `d` is
  the one reached by taking the `n-1` branch every time and holds `F(n-d+1)`
  leaves; if that exceeds a rank's equal share `F(n+1)/nranks`, some rank can
  receive no subtree at all, so the run is refused.

## Structure

With `F` the Fibonacci sequence (`F(1) = F(2) = 1`) and `I(n) = F(n+1) - 1`
the number of internal (`n >= 2`) calls:

| object | count | size |
|--------|-------|------|
| EDTs total | `3·F(n+1)` — one `fibEdt` per call (`2·F(n+1) - 1`), one `complete` per internal call (`I(n)`), plus `mainEdt` and the final checker | — |
| DBs | `2·F(n+1) - 1` — one argument/result block per call | 4 bytes each |
| Events | `2·F(n+1) - 1` ONCE events (TAKES_ARG) | — |
| EDT templates | created and destroyed around every EDT create (pure churn) | — |

Examples: `n = 20` → ~32.8k EDTs; `n = 28` → ~1.54M; `n = 33` → ~17.1M EDTs,
~11.4M DBs; `n = 40` → ~497M EDTs, ~331M DBs.  Each +1 on `n` multiplies
everything by ~1.618; +5 is ~×11.  Payload memory is negligible (4 B per DB);
the footprint is runtime metadata plus paramv per object.  Eager expansion
outruns the fold-up wave, so a large fraction of the tree is live at peak and
the resident set tracks `F(n+1)`, not the instantaneous frontier.

Counter cross-check: verified (1 node, `n=10` vs `n=12`): ΔNUM_EDT_CREATE
= 432, ΔNUM_DB_CREATE = ΔNUM_EVENT_CREATE = 288, exactly the formulas'
deltas; the runtime adds a constant baseline of +1 EDT and +1 DB per run.

## Wiring

- `fibEdt(n)` (internal) creates: two argument DBs holding `n-1`/`n-2`, two
  child `fibEdt`s (each wired to its argument DB, RO), two ONCE events, and
  one `complete` EDT with three slots — slot 0/1 the child events (RO), slot
  2 its own argument DB (RW, the result carrier).
- A leaf (`n < 2`) satisfies its parent event with its own argument DB — the
  stored value already equals `fib(n)` for `n ∈ {0,1}`.
- `complete` writes `in1 + in2` into slot 2, destroys both child DBs,
  releases slot 2 and satisfies the parent's event with it.
- The root event feeds the final checker (RO), which verifies and shuts down.

Every DB access is dataflow-ordered — create-write → child RO read →
`complete` RW write → parent RO read → destroy.  No DB ever has two
concurrent accessors; there is no sharing fan-out, so all coherence traffic
is pure migration of 4-byte blocks.

Each task carries five paramv words: the parent's completion event, its level,
the start of its leaf interval, the tree's leaf total, and the scatter depth.
The last four exist for the placement layer, but both tiers carry **and
compute** them: every internal node derives `childLevel = level + 1` and, while
`childLevel <= rrLevels`, the second child's interval start `lo + F(n)` with an
`O(n)` `fibNumber` call — unguarded, so the base binary pays the 32 bytes of
paramv and that call over the whole scatter prefix.  Only the conversion of the
interval into a hint sits under the guard.  That is what keeps base and hinted
one program with one argv and one control flow; it is a disclosed deviation
from the published per-task cost.

## Flow

`mainEdt` validates the arguments, computes the expected answer with the O(n)
iterative recurrence (`fibNumber`, a few dozen integer adds), and seeds the
root.  The tree then unfolds: the active frontier grows ~×1.6 per level down to
the leaves (`F(n+1)` of them, i.e. hundreds of millions at campaign size), and a
completion wave of `complete` EDTs folds values back up over `n` levels.
Parallelism is never the constraint; per-task runtime overhead is the entire
cost.

Nothing serial sits in the timed window.  The reference used to be the same
naive exponential recursion the DAG performs (`2·F(n)-1` calls, ~0.3-0.5 s on
one rank-0 worker at `n = 40`); because both it and the DAG grow as `φⁿ` that
was a scale-invariant serial fraction — an Amdahl ceiling of ~57x, identical in
every runtime entry, i.e. ~35% of an ideally scaling 32-node cell.  The
iterative form removes the ceiling entirely.

## Placement (base)

The source passes `NULL_HINT` on every create.  Effective policy:

- **EDTs**: shim passes `ARTS_HINT_ANY_RANK` → runtime round-robin (per-rank
  atomic counter, modulo rank count) — `fibEdt`, `complete` and the checker
  all land on arbitrary ranks.
- **DBs**: NULL hint → home = creating rank (creator/first-touch).

Consequence at multinode: a child EDT rarely lands where its 4-byte argument
DB was created, and a `complete` rarely lands where any of its three DBs
live, so nearly **every dependence edge is a remote acquire of a 4-byte
block**.  The app is a worst-case fine-grain coherence stress by
construction; locality exists in the algorithm (subtrees) but the base
program never expresses it.  Balance is not what base fails at — the
round-robin spreads tasks evenly (`spread = 1.01` at 2 ranks); locality is.

## Placement (hinted)

The layer (`OCR_APP_OPTIMIZED_PLACEMENT` in `fib.c`) is hint construction only:
two helpers whose whole bodies build an `ocrHint_t` and whose `#else` arms
return `NULL_HINT`, over one shared affinity function.  It makes the crossing
edges a prefix of the tree — children at level <= the scatter depth are placed
by the map below, every deeper child pins to its creating rank, so each
scattered subtree runs wire-free beneath its root — and it places every **EDT**
the map can name: the root `fibEdt` and the final checker (both on the rank
that owns the root's interval, which is the rank `mainEdt` runs on and
therefore the home of the root result block), and the `complete` (sum) EDTs on
their creating rank.

**No DB is hinted, in either tier.**  Every `ocrDbCreate` passes `NULL_HINT`,
so a block is homed at its creating rank.  This is deliberate, not an
omission: only the `~2·2^d` crossing edges could be affected (1e-4 of the
spawns), and on such an edge the message count is a wash under VAL × WT (4
with the block at the child against 5 with it at the creator) and one message
**worse** under the default WB × RETAIN (6 against 5), so hinting them would
be an unmeasurable change on one arm and a small regression on the arm the
paper's conclusion rests on.

**The map is weight-aware.**  Order the leaves of the whole tree left to right
and give each rank an equal contiguous share of that order; a subtree goes to
the rank that owns the start of its own leaf interval.  A node of value `v`
owns `F(v+1)` leaves and its children split them exactly `F(v) / F(v-1)`, so
the interval arithmetic is exact integer arithmetic on numbers the tree already
determines — `lo` and the tree total travel in paramv, and the rank is
`lo · nranks / total`.

That exactness is the point.  A rank's load differs from an equal share by at
most one straddling subtree, so with scatter depth `d` the static imbalance is
bounded by

    max/mean  <=  1 + nranks · F(n-d+1) / F(n+1)

At `n = 40`, `d = 14` that is **1.002 / 1.010 / 1.019 / 1.038** at 2 / 8 / 16 /
32 ranks; at `d = 11` it is 1.010 / 1.040 / 1.080 / **1.161**, which is why the
depth is 14.  A hash of the child's path id — the previous map — has no such
bound: its assignment is weight-blind, and the level-`d` subtree weights span
`F(n-d+1)` down to `F(n-2d+1)` (199x at `d = 11`), giving a relative sd of
`sqrt((k-1)·(1+φ⁻²)^d/φ^(2d))` = 16.6% at 32 ranks and an expected max/mean of
~1.3-1.4.  ARTS does not steal across ranks, so a static skew is a hard ceiling
on the widest geometry.

Coverage is checked, not assumed: `mainEdt` refuses a depth whose heaviest
subtree exceeds a rank's share (see Parameters), which is the same inequality
the bound above is derived from and is what makes "every rank gets work" a
property of the run rather than of the argument.

Crossing edges are `~2·2^d` spawns (32768 at `d = 14`) out of `2·F(n+1)-1`
= 3.3e8, i.e. ~1e-4 of them; that is why the coherence arms land within a few
percent of each other on this tier, and why the map is judged on balance and
on the wire-free subtrees below the frontier, not on the crossing edges
themselves.

## Sizing

`n` is the only dial, and it scales *work*, not per-task size:

- Pick `n` so total EDTs (`3·F(n+1)`) ≫ total workers; the runnable frontier is
  `2^L` at level `L` while `L <= (n-2)/2`, so it passes 3456 workers at level
  12 and peaks at `F(n+1)` leaves — 1.66e8 at `n = 40`, ~4.8e4 × 3456.
- Measured on the 108w+4p anchor geometry (on ferrari, single node, counters
  off, `logs/adhoc/2026-08-31-base-calib/track.txt:9,10,18,19`), with the
  previous map and the serial reference still in place: 37 → 4.75 s / 23.0 GB,
  38 → 7.80 / 35.7, 39 → 13.81 / 84.0, 40 → 23.52 / 103.0; `ocr_inv_wb` and
  `ocr_excl_retain` repeat 39/40 within 5%.  The observed per-rung time factor
  is ~1.70 (1.642 / 1.771 / 1.703), not φ — do not project a rung with φ.
  Removing the serial reference takes ~0.4 s off every rung and the fifth
  paramv word adds ~2.5% to the resident set, so **calibration pending** for
  the final size.
- Memory is a real bound here, not a formality, and it is known **only at the
  rungs that were measured**: 23.0 / 35.7 / 84.0 / 103.0 GB at 37 / 38 / 39 /
  40.  Those rungs do not follow one law (the per-rung RSS ratios are 1.55,
  2.35, 1.23), so `n >= 41` is **unmeasured — calibration pending**, not
  "≈ 171 GB".  What is demonstrated: `n = 40` at ~106 GB (103 GB measured plus
  ~2.6 GB for the added paramv word, 8 B × `2·F(n+1)`) is the largest rung with
  measured evidence under the 190 GB one-node budget.  Measure `n = 41` before
  using it; read peak RSS out of the track file and never infer it from "it
  ran".
- Scatter depth `14` gives 16384 distinct subtrees — 512 per rank at 32 nodes —
  and all of them are live at any campaign size (the shallowest level-14 node
  has value `n-28`, so `n >= 30`).  It sits on the measured plateau (at 4 nodes
  9/11/14 gave 4.75/4.77/4.89 s and at 8 nodes 2.26/2.28/2.37; only `6` and
  `16` fell off) and it is the shallowest depth whose imbalance bound is under
  1.05 at 32 ranks.
- **The hinted tier is what the app is for.**  Base at `n = 38`: 78.9 s at one
  ferrari node, and TIMEOUT past 400 s at two and at four — every recursion
  child is a remote spawn.  Hinted at the same size: 78.8 / 43.2 / 25.0.  At the
  calibrated campaign size every multinode base cell is expected to censor at
  the profile timeout; that is the anti-scaling verdict being displayed, and the
  base row must be reported as censored rather than drawn as a curve.
