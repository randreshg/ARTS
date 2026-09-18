# CoMD_sdsc

*Molecular dynamics on a link-cell grid — one datablock per cell, one EDT per
cell per phase, and a whole-grid join in the middle of every step.*
Source: `third_party/ocr-apps/apps/CoMD/refactored/ocr/sdsc/` (9 C files, ~2.6k
lines; `comd.c` drives, `atoms.c` + `timestep.c` + `lj.c` + `cells.c` hold the DAG).

## Overview

CoMD is the ExMatEx molecular-dynamics proxy: an FCC copper lattice integrated
with velocity Verlet under a Lennard-Jones (default) or EAM potential.  Space is
cut into *link cells* at least one cutoff wide, so the force on an atom needs
only its own cell and the 26 neighbours.  This is SDSC's first OCR port: one
datablock per cell, one EDT per cell per phase, and a barrier between phases.

The result scalar is `Final energy` — total energy per atom after the last step,
printed by `validate_result` beside the initial energy, so a value off by more
than the catalog's `1e-6` means the integration went wrong, not that the machine
was slow.  It is also a **cross-tier** check: this row and `CoMD_sdsc_dist` generate
the same instance from the same index-seeded functions and step it under the same
rules, so at the same `-x/-y/-z/-N/-n/-t` their `Final energy` must agree to
round-off — what differs between them is only the shape of the energy summation
(one linear join here, a reduction tree there) and the fixed order in which a cell's
26 neighbours are accumulated — while their `Total atoms` and `Max Link Cell
Occupancy` must be equal exactly.

Arithmetic per task is real but modest (~18 atoms per cell against 27 cells);
what the program stresses is **halo sharing** — every cell block is read
by 26 tasks while its owner writes it — one whole-grid serialization per step
(the atom-redistribution EDT) — and, above all, the wiring plane: the whole
`B`-wide five-phase DAG is re-created over the wire every step.  (A second
serialization the port used to fabricate — every per-box task taking the
shared simulation block RW while only reading it — was a mode misdeclaration
and is repaired to CONST; see Wiring.  The per-step redistribute's atom walk
is repaired there too.)

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|------------------|
| `-x/--nx`, `-y/--ny`, `-z/--nz` | unit cells per dimension | 20 | ✓ parsed in `mainEdt`, stored in the simulation DB that every EDT depends on — multinode-safe |
| `-N/--steps` | requested time steps | 100 | ⚠ honoured only at period boundaries: the run executes `period·ceil(steps/period)` steps, so `-N 2` with the default period simulates **10** |
| `-n/--period` | steps per outer block, status print interval | 10 | ✓ — and it, not `-N`, is the real step quantum |
| `-D/--dt` | time step (fs) | 1.0 | ✓ positions use `dt/mass`, the velocity kicks `0.5·dt` (block ends) and `dt` (inside a block) |
| `-T/--temp` | initial temperature (K) | 600.0 | ✓ drawn per atom at generation, then centred and rescaled by the init reduction |
| `-r/--delta` | initial random displacement (Å) | 0.0 | ✓ must lie in `[0, 0.25·lat)` — see below |
| `-l/--lat` | lattice constant (Å); `<0` = the potential's | -1.0 | ✓ — moves the cell grid, see Sizing |
| `-e/--doeam` | use EAM instead of Lennard-Jones | 0 | ✓ but reads `pot_dir/pot_name` from disk at init; needs the `pots/` dataset in the working directory |
| `-d/-p/-t` (`pot_dir`, `pot_name`, `pot_type`) | EAM table selection | `pots`, auto, `funcfl` | ✓ under `-e`, dead otherwise |
| `-h/--help` | print the option table | — | ✓ (prints the table, then the parameter echo, then runs) |

Argument handling is loud: an unrecognized switch, an unusable option type, a
unit-cell count / step count / period that is not a positive integer, a
non-positive `dt` and a zero `-l` each print an `ERROR` line and end the run
through `ocrShutdown` instead of silently falling back to the parse defaults.
The integer counts are range-checked **as signed**: the option table stores every
integer through an `int*` while the fields are `u32`, so `-x -1` used to reach
the lattice setup as 4 294 967 295 and overflow the grid and atom counts.  (`-n
0` used to divide by zero mid-run at `(++step) % period`, and a mistyped flag
used to run the *default* problem while still printing a plausible `Final
energy`.  `-x 0` and `-l 0` were already rejected before this, though
indirectly — `sanity_checks` runs before `init_lattice` and refuses any axis
shorter than two cutoffs; the explicit checks only name the real cause.)  The
`-h` flag's own storage is an `int`, matching the `'i'` handler that writes it —
it used to be a `u8` taking a 4-byte store.  A dangling `else` left the 13-line
parameter echo unconditional while guarding only its `Command Line Parameters:`
header; the block is now explicitly unconditional, which leaves stdout unchanged
at every argument set except `-h`, where that header line — previously
suppressed above its own body — is now printed.  `-r/--delta` is additionally
required to lie in `[0, 0.25·lat)`: an FCC site sits a quarter of a lattice spacing
from the nearest domain face and this program never wraps a position, so a larger
displacement used to reach `coordinates2box` with a negative coordinate, whose cast to
`u32` is undefined and whose result indexed the cell array.  Unreachable at the campaign
argv, which never passes `-r`.

Compile-time only: `MAXATOMS` (64) fixes the per-cell atom capacity and hence
the DB payload at 5896 B; the LJ parameters (`sigma` 2.315, `epsilon` 0.167,
cutoff `2.5·sigma`, lattice 3.615 Å) are constants of `init_lj`.
`sanity_checks` rejects a box smaller than two cutoffs, i.e. `n_i ≥ 4`.
`MAXATOMS` is a disclosed constant of every traffic number here: the FCC
lattice fills 442 368/24 389 = **18.1 of 64 slots** on average (max observed
32), so ~3.5× of every acquire is zero padding.  It stays a `#define` because
the box struct is fixed-size by construction and base preserves the published
program — but the wire-traffic figures must be read against that padding.

## Structure

With `g_i = floor(n_i·lat/cutoff) = floor(0.6246·n_i)` cells per dimension,
`B = g_x·g_y·g_z` cells, `P` = period, `K = ceil(steps/period)` blocks:

| object | count | size |
|--------|-------|------|
| cell DBs (`box`) | `B`, created serially in `mainEdt` without an acquire, filled in parallel | 5896 B each (64 atom slots: gid, r, p, f, U, a) |
| simulation DB | 1, dependence of nearly every EDT | 1064 B |
| cell-GUID list, timer, command | 3 | `8B` B, 352 B, ~38 KB |
| per-cell generation statistics | `B` + 1 reduced, once at init | 40 B each (count, occupancy, `Σp`, `Σ|p|²`), destroyed by the reducer |
| per-cell scalar results | `B` per force phase, `B` per block-end KE | 8 B each, destroyed by the reducer; homed at the reducer under `hinted` |
| EDTs total | `(4B+10) + K·(P·(3B+8) + 2B+5)` | `mainEdt` + 5 spine EDTs + `4(B+1)` init and warm-up fan-outs, then `3(B+1)` fan-outs + 1 redistribute + 4 spine EDTs per step |
| DBs total | `(4B+6) + K·(P·(B+1) + B+1)` | dominated by the `B` cells |
| events total | `(B+3) + K·(P·(2B+4) + B+2)` | ONCE events plus one output event per fan-out EDT |

Per step the fan-outs are: advance-velocity (`B` + 1 join), advance-position
(`B` + 1), redistribute (**1** EDT), force (`B` + 1 join). The leading
`(4B+10)` is `mainEdt`(1) + the generation fan-out (`B+1`) +
`init_done_edt`(1) + the temperature fan-out (`B+1`) + `init_force_edt`(1) +
`main_edt2`(1) + the initial force fan-out (`B+1`) + `main_edt3`(1) + the
initial kinetic-energy fan-out (`B+1`) + `end_edt`(1, created once by the
final block's `bot_edt`) — `mainEdt`
itself is easy to miss since it never appears as a Wiring-section actor
(it only ever creates the first init EDT), but it is a genuine, distinct
`NUM_EDT_CREATE`-counted instance like every other app's entry EDT.

Worked numbers at `-x 48 -y 48 -z 48 -N 15 -n 5` → `g = 29`,
`B = 24 389`, `P = 5`, `K = 3`, i.e. **15 simulated steps**: ~1.34M EDTs,
~537k DBs, ~830k events, 442 368 atoms, **144 MB** of cell payload (live for
the whole run) plus ~190 KB of transient 8-byte results per phase and ~1 MB of
generation statistics once.  The catalog's own `-x 52 -y 52 -z 52 -N 10 -n 10`
gives `g = 32`, `B = 32 768`, `P = 10`, `K = 1` (10 simulated steps, 562 432
atoms, ~183 MB of cell payload): ~1.18M EDTs, ~492k DBs, ~721k events by the
same formulas (the previous rung, `-x 39` → `g = 24`, `B = 13 824`: ~498k /
~207k / ~304k).  The
completion-event conversion is count-neutral (each ONCE/minted output event
became one COUNTED event), so the verified per-step terms stand.

Counter cross-check: the per-step terms were verified against counters
(1 node, `-x4 -y4 -z4 -N1 -n1` vs `-x6 -y4 -z4 -N3 -n1`): NUM_EDT_CREATE
76 → 250, NUM_DB_CREATE 49 → 121, NUM_EVENT_CREATE 32 → 128 — exactly the
formulas above with the *pre-parallel-init* leading constants `(2B+6)` /
`(3B+6)` / `2` (app values 75/249, 48/120, 32/128), plus the runtime's
constant +1 EDT/+1 DB/+0 EVT baseline.  That measurement also fixed the EDT
formula's leading constant, which was short by exactly 1: `mainEdt` (created
by the ARTS-native `main_edt` bootstrap in `benchmarks/ocr_shim/arts_ocr.c`)
was never counted as one of the app's own EDTs.  The parallel init adds
`2B+4` EDTs, `B+1` DBs (`B` partials + 1 reduced, less the removed cell-
pointer scratch array) and `B+1` events to those leading constants, so the
same two runs now predict app-side **95 / 277** EDTs, **56 / 132** DBs and
**41 / 141** events — *calibration pending*, that re-run is the check.

## Wiring

`mainEdt` fixes the cell geometry and creates the `B` cell datablocks, then hands a
chain of control EDTs the timer DB (RW), the simulation DB and the cell-GUID list.
Each control EDT creates the next one and a fan-out whose join EDT satisfies the next
control EDT's last slot; there are no channel or latch events, only COUNTED completion
events (single consumer declared, reclaimed on delivery) and EDT output
events carried the same way.  The two init fan-outs use the same shape as the compute
phases:

- **generation** (`gen_edt`): simulation CONST, own cell **RW**.  Fills its own cell
  from the index-seeded generator and wires a 40-byte statistics DB straight into the
  reducer's slot; `gen_red_edt` adds the `B` of them in slot order and hands the sums to
  `init_done_edt`, which derives the centre-of-mass velocity and the temperature scale
  in closed form.
- **temperature** (`temp_edt`): own cell **RW** and nothing else — the two scalars ride
  in the task parameters, so this phase depends on no shared block at all.
- **force** (`lj_edt`, 28 slots): simulation CONST, own cell **RW**, 26
  neighbour cells RO.  Writes its cell's `f`/`U` and wires an 8-byte energy DB
  straight into the reducer's slot.
- **advance-velocity** (`av_edt`): simulation RO, own cell **RW**, a shared
  8-byte `dt` block CONST; **advance-position** (`ap_edt`): simulation RO, own
  cell **RW**.
- **redistribute** (`redistribute_edt`, `B+1` slots): **every** cell **RW**
  plus the simulation DB **RW** (it writes `max_occupancy`), in one task.  It
  visits every atom once: a departure refills its slot from the cell's tail, and
  the published loop stepped over that refilled slot, so a migrating atom could
  spend a step or more in the wrong link cell — taking its force from the wrong
  27-cell neighbourhood — before being noticed.
- **kinetic energy** (`ke_edt`): simulation CONST, own cell RO; `ke_red_edt`
  sums `B` scalars (simulation **RW** — it writes `e_kinetic`) and satisfies
  a COUNTED completion event.

The simulation block and `dt` used to be declared **RW** by every per-box
reader — a misdeclaration OCR's racy model never charges for, but under a
runtime that honours write exclusivity it made every phase serialize on one
singleton's write grant migrating node to node.  The conformance repair
declares the access each task performs (CONST), leaving RW only where a task
writes (the two reducers, redistribute).  Every completion event is a COUNTED
event with its single consumer declared (`comdJoinEvt` /
`OEVT_COUNTED_PRE`+`PROP`, guarded by `OCR_APP_COUNTED_OEVT`): the reclaim
contract needs the count, and the undeclared per-box output events the av/ap
fan-outs used to mint lingered forever — ~48.8K events per step, unbounded in
the step count.

Remaining DB concurrency, in decreasing order of pain: the **redistribute
EDT** takes all `B` cells RW at once — every cell's ownership converges on
one node per step and scatters again.  Each **cell** has one writer and up to
**26 concurrent readers** in the force phase — a genuine DB-granular
read/write overlap, benign only because the writer touches `f`/`U` and the
readers touch `r`.

## Flow

Strictly bulk-synchronous: `velocity(B) → position(B) → redistribute(1) →
force(B) → velocity(B) …`, with a join EDT between every pair of phases, so no
cell ever runs ahead of another.  The **nominal** per-phase fan-out is `B`;
minimum is 1, three times per step (the two join EDTs and the redistribute EDT).
A block of `period` steps ends with a kinetic-energy fan-out and a status line.

The **realized** frontier is much smaller than `B`, and this is the row's
defining property: every phase's `B` tasks are emitted by one EDT
(`fork_lj_force`, `fork_advance_velocity`, `fork_advance_position`), so the
frontier is bounded by that single issuer's create/addDependence rate, not by
`B`.  Measured at 1 node / 112 workers, `-x48 -N15 -n5`: force 0.5868 s over
29 ops per box, velocity 0.1106 s over 6, position 0.0951 s over 5 — one
constant 0.83/0.76/0.78 µs per operation across three phases with three
different op counts, against a compute ceiling of ~11 ms for the same phase.
Per-step wall is therefore worker-count invariant.  Read every width number
below as nominal fan-out.

Initialization is two more fan-outs of the same shape, not a native preamble.
`mainEdt` still creates the `B` cell datablocks — that is the published construction —
but with `DB_PROP_NO_ACQUIRE`, because it never writes them; the state they hold is
produced by one task per cell:
`generation(B) → reduce(1) → temperature(B) → force(B) → kinetic(B) →` the step loop.
A generation task enumerates the lattice sites that can fall in its own cell, keeps the
ones that do, and derives each atom's identity, position, displacement and momentum from
`mk_seed(gid, call_site)` — a pure function of the atom's global index, with no shared
RNG state, no `rand()`/`srand()`, and no dependence on the rank count or on task order.
It reports its cell's atom count, occupancy and the two momentum sums; one reduction adds
those in cell-index order; the centre-of-mass velocity and the temperature rescale follow
in closed form (`Σ|p−v|² = Σ|p|² − N|v|²`), so a single per-cell apply pass finishes the
job.  Consequences worth stating plainly:

- **The initial whole-grid redistribution is gone.**  An atom is generated into the cell
  that contains its *displaced* position, so the pass the published program ran after
  placement had nothing left to do.  `max_occupancy` — its only other output — is now the
  reduction's maximum, printed with the same text and the same meaning.
- **The wiring cost moved, it did not vanish.**  Both init fan-outs are still issued by a
  single task, exactly like every compute phase: ~`7B` create/`ocrAddDependence`
  operations where the serial pass had none.  At the per-operation constants measured
  above that is tens of milliseconds at one node but seconds at a wide geometry, so this
  phase inherits the row's defining wall.  Removing *that* is the restructured row's
  brief; parallel generation is a conformance requirement, not an optimization.
- `end_edt` still destroys all `B` cells serially, unchanged.

For reference, the pass this replaced — `B` `ocrDbCreate` calls plus 442 368 atom
placements, three whole-grid momentum passes and a whole-grid redistribution at `-x 48`,
all native code on the rank-0 worker — measured together with the teardown at ~0.47 s of
the 16.09 s 1-node window (**2.9%**).

## Placement (base)

The `OCR_APP_OPTIMIZED_PLACEMENT` layer in `cells.h` is compiled out here —
its four helpers resolve to `NULL_HINT` — and every other create passes
`NULL_HINT` literally, so the base binary contains no affinity call at all.
Effective policy:

- **EDTs** → runtime round-robin (`ARTS_HINT_ANY_RANK` from the shim), so a
  cell's force/position/velocity EDT lands on an arbitrary rank and on a
  *different* rank each step.
- **DBs** → home = creating rank.  Every cell, the simulation block, the timer
  and the cell list are created inside `mainEdt`, so **all of them are homed on
  rank 0**; only the transient scalars — the 8-byte force and kinetic-energy
  results, and the 40-byte generation statistics — are homed where their producer
  ran.

The algorithm's locality (a cell's neighbours are fixed for the whole run) is
therefore never expressed.  Each force EDT pulls 27 cells ≈ 159 KB, so the force
phase issues **`27·B` cell acquires per step** (`B` = cells, so the absolute
count follows the size: 373 248 at the catalog's `-x 39`, i.e. `g = 24`),
of which round-robin placement makes a
`(nodes−1)/nodes` fraction remote before any reuse.  The simulation block is a
much smaller term than it looks: after the declared-access repair (see Wiring)
only three tasks per step take it **RW** — the force reducer, the redistribute
EDT and the one spine EDT that increments the step counter — plus the kinetic
reducer once per block, so its write grant moves `O(1)` times per phase, not
`2B` times per step.  What round-robin still costs is that each of those RW
acquires lands on a different rank.  Treat this port as a worst-case coherence
stress, not a scaling benchmark.

## Placement (hinted)

As-born homes every cell on rank 0 and lands each cell's force/energy/advance
task on an arbitrary — and each step a different — rank, so every box's atoms
travel every timestep.

The layer (in `cells.h`, guarded by `OCR_APP_OPTIMIZED_PLACEMENT`) is four
`static inline` hint helpers plus the pure index map they share
(`comdBlockRank`) and nothing else: no control flow, partition, DB count,
event count or wiring differs between the two binaries, and at `nranks <= 1`
every helper returns `NULL_HINT`, so the 1-node cell is shared by construction.

- **`comdBlockEdtHint`** places box `b`'s per-box tasks — the generation and
  temperature tasks in `atoms.c`, the force pair-spawn in `lj.c`, the
  kinetic-energy / advance-velocity / advance-position loops in `timestep.c`, and
  the pair-force / embedding-energy / rho loops of the EAM path in `eam.c` — on
  the rank that owns `b` under `comdBlockRank`.
- **`comdBlockDbHint`** (`init_boxes`) homes box `b`'s datablock on that same
  rank, so a box's directory lives where the four tasks that touch it every step
  run instead of all `B` boxes being homed on the one rank that ran `mainEdt`.
  Its *contents* are now written by the generation task on that same rank.
- **`comdHomeEdtHint`** pins the control spine — the per-phase continuations,
  every join, the serial redistribute — to rank 0, so the simulation
  singleton's writers all run where it lives instead of the spine
  round-robining to a fresh rank each phase and dragging the write grant along.
- **`comdHomeDbHint`** homes the per-box reduction results (`gen_edt`, `lj_edt`,
  `ke_edt`, and the EAM `eam_edt`/`rho_edt`) on the same rank, so each of the
  join's `B` inputs is looked up on the rank that reads it instead of on the
  rank that happened to produce it.  What that buys is policy-dependent and is
  **not** a free win: under `WT` the producer's release publishes the payload to
  the home, so the join's read is local; under the default `WB` the payload
  stays with the last writer and the join still fetches it — only the directory
  lookup is saved.  It is also paid for: it concentrates one datablock create
  and one destroy per box per reduction — `B` per step for the force reducer
  and another `B` once per block for the kinetic one — on the same rank the
  spine already pins, which is the rank this row's critical path already runs
  on.  Whether it nets out positive is a
  measurement, not a derivation.

`comdBlockRank` is a **recursive bisection of the box grid**: the rank set is
halved, the sub-grid is cut on whichever axis's nearest-integer cut leaves the
two box counts closest to proportional (ties to the longest axis), and the
descent repeats.  Every rank gets one compact 3-D block.  This replaces the
earlier band map `(b·nranks)/B`, which balanced exactly but was a **slab**:
boxes are linearized x-fastest, so a band is a stack of whole x-y planes, and
once a band is thinner than ~2 planes its 18 out-of-plane neighbours are all
remote — the regime every wide geometry runs in.

Derived per-rank share of the `B` per-box tasks (`max block / mean`) and the
remote fraction of the 26·B neighbour read edges:

| `g` | ranks | band imbalance | band remote | block imbalance | block remote |
|---|---|---|---|---|---|
| 29 (`-x 48`) | 2 | 1.000 | 4.8% | 1.034 | 4.8% |
| | 8 | 1.000 | 19.5% | 1.107 | 14.0% |
| | 16 | 1.001 | 39.0% | 1.181 | 18.2% |
| | 32 | 1.001 | 70.1% | 1.181 | **22.6%** |
| 24 (`-x 39`) | 2 | 1.000 | 5.8% | 1.000 | 5.8% |
| | 8 | 1.000 | 23.1% | 1.000 | 16.4% |
| | 16 | 1.000 | 46.8% | 1.000 | 21.5% |
| | 32 | 1.000 | 70.5% | **1.000** | **26.4%** |
| 12 (`-x 20`) | 8 | 1.000 | 47.4% | 1.000 | 30.9% |
| | 32 | 1.000 | 74.5% | 1.000 | 48.0% |

No rank is empty at any of those geometries.

**The map's balance is a hard constraint on the sizing decision, not a
property of the map alone.**  A rectangular block partition can be exactly
balanced only when the cuts it needs land on grid planes: exhaustively over the
sweep's rank counts, **every axis count `g_i` divisible by 4 gives imbalance
exactly 1.0000 at 2/4/8/16/32 ranks** (verified for `g` = 12, 16, 20, 24, 28,
32, 36, 40, 48 and for the non-cubic `g = (24,16,18)`), while a `g` with an odd
or prime axis cannot: `g = 29` (today's `-x 48`) gives 1.034 / 1.070 / 1.107 /
1.181 / 1.181, because `29³/32` is not an integer and no axis-aligned
decomposition of a 29-cube into 32 boxes is even.  Splitting the rank set in
proportion to the realized sub-volumes instead of in half was evaluated as the
alternative and recovers only 1.181 → 1.102 at 32 ranks while *losing* locality
(22.6% → 25.7% remote), so it was not taken: the size, not a cleverer map, is
what makes this exact.  **`-x/-y/-z` must therefore be pinned so that
`g_i = floor(0.6246·n_i)` is divisible by 4 on all three axes** — which the R6
width rule already forces for a cubic multiple of 3456 workers (`g ≡ 0 mod 24`,
i.e. `-x 39`).  Since the per-box tasks are issued serially (see Flow), wire
traffic dominates worker-side imbalance by a wide margin — but at a qualifying
size neither has to be traded away.

The hinted preamble costs `B` **remote creates and nothing else**: `mainEdt` creates
each box at its block home with `DB_PROP_NO_ACQUIRE`, never writes the payload, and the
generation task on that same rank is the block's first writer — no hold, no release,
nothing to publish.  (Before, the creator held and released every block, moving
`B · 5896 B` of it to the block homes once.)  Three bounds the layer cannot cross
remain, all disclosed rather than fixed:

- The **redistribute EDT takes every box RW on the pinned spine rank once per
  step**, so each box's write ownership leaves its block rank and returns every
  step no matter what the map says.  The block map can only reduce the force
  phase's neighbour reads; it cannot touch the per-step whole-grid ownership
  sweep.
- The shared 8-byte `dt` block is created by the spine and read CONST by all
  `B` advance-velocity tasks: an inherent per-phase broadcast with no better
  static home.
- Homing the reduction results on the spine rank (`comdHomeDbHint`, above)
  concentrates their create/destroy churn on that one rank; under a write-back
  policy it saves the directory lookup but not the payload fetch.  It is
  disclosed here as a bound, not claimed as a gain.

## Sizing

`n_x,n_y,n_z` set both parallel width and memory: cells `≈ (0.6246·n)³`, atoms
`4·n³`, payload `5896 B` per cell regardless of occupancy (mean `4n³/B` ≈ **18 of
64 slots** — 18.1 at `-x 48`, 17.2 at `-x 39`, the ratio moving with where
`floor(0.6246·n)` truncates — so ~3.5× of every acquire is zero padding).  Task
grain is fixed by the cutoff — a force EDT always compares
one cell against 27 — so `n` buys *more* tasks, never bigger ones; `-l/--lat` is
the only knob that changes atoms per cell.  `-N`/`-n` scale time and nothing
else.

- **The size knob is `-x/-y/-z`, not `-N`.** `-n` is argv and stays at its
  published default (10); `-N` is argv too, but the catalog pins it at
  **10**, a forced deviation from the published 100 (see below, and
  **Anti-scaling** in Flow) — the window is reached with the grid. Dane size:
  `-x 52 -y 52 -z 52` (`g = 32`, `B = 32,768`), under a hard constraint from the
  hinted map: the pinned `-x/-y/-z` must make `g_i = floor(0.6246·n_i)`
  divisible by 4 on every axis, or the hinted tier's load imbalance leaves
  1.000 (see Placement (hinted)).  `B` must clear the 3,456-worker geometry —
  the floor rung `g = 24` (`-x 39 -y 39 -z 39`, `B = 13,824`, exactly 4× the
  floor, dividing every node count and the 108-worker share) runs 4.0 s at one
  node, under the 20 s window the kept hinted tier prescribes; the calibrated
  rung is the next admissible one that meets it: `g = 32` is divisible by 4,
  `B = 32,768` is a power of two so every node count of the sweep divides it
  (the 108-worker share is 303.4, a 0.2% quantisation), and it was chosen from
  two measured points rather than searched — `g = 24` 4.0 s and `g = 40`
  (`-x 65`) 80.4 s at one node give `T ~ B^1.95`, which puts 20 s at
  `B ≈ 31.5k`; measured 18.7 s base / 19.5 s hinted, 4.9 GB.  The truncation
  `g = floor(0.6246·n)` gives 32 for `n ∈ [51.23, 52.83)`, so `-x 52` sits
  mid-band.  Nominal fan-out is what this buys; the realized frontier is the
  single spawner's issue rate (Flow).
- **A residual `-N` deviation may be unavoidable and must be recorded as one.**
  Per-step wall at `-x 48` was 12.08 / 18.96 / 23.69 s at 2/4/8 ferrari nodes —
  increments 6.88 then 4.73, so a decaying fit gives ~29 s/step at 32n and a
  linear one ~38; at `B = 13,824` that is ~16.5-21.5 s/step, i.e. `-N 30 -n 10`
  lands 496-645 s of Dane's 900 s cell.  `-N 100` and a width above the floor do
  not both fit; the size comes first and the shortened `-N` is the disclosed
  deviation.  The catalog pins `-N` more conservatively still, at **10**, and
  records the shortfall rather than assuming the `-N 30` extrapolation holds
  on Dane's own fabric; `-N` returns toward 30 only once a re-measurement there
  confirms the per-step wall is low enough.  `-n` returns to the published 10
  (nothing upstream publishes 5;
  the published periods are 10 and 1).  A non-dividing period does **not** make
  the final kinetic term stale — it is recomputed at the block end immediately
  before `end_edt` — it makes the run execute `period·ceil(steps/period)` steps.
- **Hard ceiling**: the join EDTs take `B+1` dependences and the ARTS shim packs
  `depc` into a 20-bit field (`ARTS_TPL_DEPC_MAX` = 1,048,574), so `B` must stay
  below that — `-x 162` (`g = 101`, `B = 1,030,301`, 6.1 GB of payload) is the
  largest cubic run under that ceiling, with the reference runtimes' own limits
  applying independently.  A larger grid is reported and the run ends: every
  join checks the create and shuts down naming the dependence count it could not
  satisfy.
- **Memory (1 node)**: `B · 5896 B` of cell payload plus `8B` of transient
  scalars per phase, `40·(B+1)` of generation statistics once, and kilobytes of
  singletons — 81.5 MB at `g = 24`, 144 MB at `g = 29`.  Runtime metadata
  dominates the RSS; against a 190 GB budget this row is time-bound, never
  memory-bound.

## Family shape (measured, reduced trend ladder: 15w+1p × 1/2/4/8 nodes, `-x 48 -N 15 -n 5`)

hinted, e2e seconds.  The energy pin held in every cell:

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 11.5 | 192.0 | 298.9 | 377.0 |
| val_wb | 11.5 | 139.2 | 191.7 | 250.3 |
| inv_wb | 11.9 | 150.2 | 222.1 | 267.2 |
| excl_retain | 13.2 | 150.3 | 235.0 | 312.2 |

base val_wb_nocomb, same ladder, on the application's own `total` timer:
11.69 / 263.21 / 269.66 / 318.83.  Against hinted's 11.51 / 191.82 / 298.64 /
376.75 the band map was worth **1.37× at 2n and 1.11-1.18× WORSE at 4n and
8n** — the earlier ">=2x at 4-8n" reading compared a completing hinted E2E
against a base cell that had finished its workload and was then killed at the
590 s ceiling inside runtime shutdown, so it compared shutdown behaviour, not
the timed window.  Two consequences: the base 4n/8n shutdown stall is a
standing risk for Dane (it is rank-count driven, not size driven), and **the
whole table above predates the band→block map change** — it is the baseline the
new map has to beat, not evidence for it.  Every arm anti-scales monotonically:
the per-step wall is the wiring plane (the arm-invariant DAG re-wiring census
established on the sibling cell-grain port), with VAL's re-validation adding
~1.4-1.5x over the combining arm on top.  The structural answer to this wall is
a rewrite of *this* program's decomposition — a spawn tree in place of the single
per-phase issuer, a per-cell publish/arrive migration in place of the whole-grid
redistribute EDT, and reduction trees in place of the `B+1`-dependence joins —
which is the restructured row's brief; `CoMD_intel_chandra_tiled` is a different
program, not this instance restructured.  (Parallel initialization is *not* on
that list: it is an R9 conformance adaptation and is present identically in all
three tiers.)

The table above also predates the parallel-init change and the redistribute repair,
which alter the object counts and move `Final energy`; the energy pin and the ladder
both need re-running (*calibration pending*).
