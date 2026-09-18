# CoMD_sdsc_dist

*The same link-cell molecular dynamics as `CoMD_sdsc`, with every phase's
fan-out, fan-in, atom exchange, generation and teardown rebuilt so that no
single task stands between the machine and the work, a step fused into one
task per cell, and the cells a step reads across a rank boundary aggregated
into one block per boundary layer run — so what crosses the fabric per step is
a fixed number of messages per run, never one per boundary cell.*
Source: `third_party/ocr-apps/apps/CoMD/refactored/ocr/sdsc-dist/` (9 C files,
~1.6k lines; `comd.c` drives the spine, `fan.c` builds every phase,
`timestep.c` holds the per-cell step, `lj.c` the pair kernels, `cells.c` the
exchange helpers and the halo).

## Overview

CoMD is the ExMatEx molecular-dynamics proxy: an FCC copper lattice integrated
with velocity Verlet under a Lennard-Jones potential.  Space is cut into *link
cells* at least one cutoff wide, so the force on an atom needs only its own
cell and the 26 neighbours.  This row is the restructured tier of `CoMD_sdsc`:
same potential, same lattice, same instance, same `Final energy` scalar, same
`-x/-y/-z/-N/-n` command line.

What the base port measures is not that physics but its wiring plane.  There,
every one of the `B` tasks of every phase is created by ONE task, and the
measured wall of each phase is its create + `ocrAddDependence` count times a
constant — the phases are issue-bound, not compute-bound, so the nominal width
`B` is never realised.  Four structures carry that:

1. one task emits a phase's whole fan-out;
2. one join task takes a dependence on every cell (`B+1` slots);
3. one redistribute task takes **every** cell for writing once per step, so
   each cell's write ownership leaves its rank and comes back every step;
4. generation and teardown are serial loops over all `B` cells in one task.

This program replaces all four.  Phases are emitted by a spawn **tree** and
collected by a reduction **tree**; cells are generated in place by the tasks
that will own them, from a generator that is a pure function of the atom's
global index; cells are released by their owners; and the mutable simulation
state is split away from the immutable configuration, so the wide phases
never depend on a block whose ownership moves.  Beyond the wiring plane, the
step itself is **one task per cell**: it takes in the atoms that drifted into
the cell, computes the forces, kicks, takes the energy point when the block
ends, kicks again and drifts, reading one generation of its own and its 26
neighbours' blocks and writing the other generation of its own — so a
neighbour's block is fetched once per step, invalidated once per step, and no
block has a reader and a writer at the same time.

That leaves one term, and it is the one that decides whether the row scales:
with one datablock per cell, a neighbour across a brick boundary is a distinct
remote block, so a rank's wire traffic is proportional to its brick's SURFACE
IN CELLS — tens of thousands of messages per rank per step at the campaign
size, which no per-message cost can absorb.  The **halo** removes it.  Each
brick owns, per direction its neighbouring brick reads from, one block per run
of at most `HALO_CHUNK` cells of that boundary layer; a step packs those runs
before it emits its cell tasks, one task on the reading brick pulls each run
once, and the cell tasks read their outside neighbours out of the run they
land in.  The halo carries exactly the cells the per-cell scheme fetched, in
`Σ_r Σ_d ceil(|L(r,d)|/HALO_CHUNK)` blocks instead of one per cell.  The
factor is the mean run length, which is `HALO_CHUNK` wherever a layer is
longer than a run and falls toward 1 as a brick's faces shrink below it:
57-64× over 2..32 ranks at `-x 193`, 47-64× at `-x 103`, 30-63× at `-x 64`,
and only 6-48× at the `-x 20` local toy, whose faces are 36 cells or fewer.

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|------------------|
| `-x/--nx`, `-y/--ny`, `-z/--nz` | unit cells per dimension | 20 | ✓ parsed in `mainEdt`, stored in the configuration DB every task reads |
| `-N/--steps` | requested time steps | 100 | ✓ honoured at period boundaries: the run executes `period·ceil(steps/period)` steps |
| `-n/--period` | steps per outer block, status print interval | 10 | ✓ — the real step quantum; also where the kinetic energy is reduced |
| `-w/--width` | items emitted by one spawn-tree leaf | 8 | ✓ the width knob: a leaf never crosses a rank's brick and each brick is split on whole leaf widths, so a phase is emitted by exactly `Σ_r ceil(B_r/w)` leaves — `ceil(B/w)` whenever `w` divides every brick.  A halo phase's items are the brick's halo blocks, so it is cut the same way |
| `-D/--dt` | time step (fs) | 1.0 | ✓ the drift uses `dt/mass`; inside a block a step kicks by `dt`, at a block's end by `dt/2` before and `dt/2` after its energy point |
| `-T/--temp` | initial temperature (K) | 600.0 | ✓ applied by the generation pass and one per-cell rescale |
| `-r/--delta` | initial random displacement (Å) | 0.0 | ✓ applied per atom before its cell is chosen; must stay below one lattice spacing |
| `-l/--lat` | lattice constant (Å); `<0` = the potential's | -1.0 | ✓ — moves the cell grid, see Sizing |
| `-h/--help` | print the option table | — | ✓ (prints instead of the parameter echo, then runs) |

An unrecognised switch, `-e/--doeam` (only Lennard-Jones is implemented), a
grid below three cells per dimension, a width outside 1..1024, a zero step
count or period, and a displacement at or above one lattice spacing are each
reported and end the run rather than silently changing the instance measured.
A collector whose dependence count the runtime refuses (a leaf join holds one
slot per cell, so the ceiling scales with `-w`) also reports and ends the run
instead of continuing on an unwritten GUID, and so does a rank grid with more
ranks than link cells on some axis (a rank would own no brick).  At run time
a cell that would hold more than `MAXATOMS` atoms, or an atom that drifts to
a cell not adjacent to the one it left, reports and ends the run.

Compile-time only: `MAXATOMS` (64) fixes the per-cell atom capacity and hence
the block payload; `FAN_BRANCH` (8) is the spawn tree's arity; `HALO_CHUNK`
(64) is how many cells of a boundary layer one halo block carries, which sets
both the halo's block count (and so its message count) and the pack task's
dependence count — it holds one slot per cell it copies, plus the block.  The LJ
parameters (`sigma` 2.315, `epsilon` 0.167, cutoff `2.5·sigma`, lattice
3.615 Å) are constants of `init_lj`.

## Structure

With `g_i = floor(n_i·lat/cutoff) = floor(0.6246·n_i)` cells per dimension,
`B = g_x·g_y·g_z` cells, `B_r` the cells of rank `r`'s brick and `w` the width
knob, a phase's tree is **indexed by brick**: it first fans over the `nranks`
bricks (`min(8, nranks)` children per level — one level up to 8 ranks, two up
to 64), then splits each brick's cells on whole leaf widths.  It therefore has
`G = Σ_r ceil(B_r/w)` leaves — `ceil(B/w)` exactly whenever `w` divides every
brick, which holds at every campaign geometry below — and `I` interior nodes,
where `I` is what the arity-8 recursion `k = min(8, ceil(n/w))` yields per
brick plus the rank level.  `I` depends on the rank grid (by a few per cent of
a step's tasks, table below); `G` and the cell tasks do not.  `F = G + I` is
the number of tree nodes, each of which is two tasks (the node that emits and
the collector it created).  `P` = period, `K = ceil(steps/period)` blocks.

The **halo** is indexed the same way.  For rank `r`'s brick (extent `n`) and a
direction `d ∈ {-1,0,1}³ \ {0}`, the layer `L(r,d)` is the boundary plane, row
or corner of the brick facing `d`: `Π_i (d_i ? 1 : n_i)` cells, enumerated over
the axes `d` leaves free in the grid's own order, x fastest, and cut into runs
of at most `HALO_CHUNK` cells.  A layer exists **only where every axis `d`
moves on is split across ranks** — along an axis one rank owns whole a
neighbour never leaves its brick, so nothing would ever read such a layer.
`L(r,d)` therefore has exactly one reader, the brick one step along `d`, and
the layers of a brick hold exactly the cells the neighbouring bricks used to
fetch one by one.  Writing `H_r = Σ_d ceil(|L(r,d)|/HALO_CHUNK)` and
`H = Σ_r H_r`, the halo phase's tree is the phase tree over the interval
`[0, H_r)` of each brick, with `G_h = Σ_r ceil(H_r/w)` leaves and `F_h` nodes:

| object | count | size |
|--------|-------|------|
| cell DBs (`box`) | `2B` — two generations per cell, created by the leaf that owns them | 3336 B each (64 atom slots: gid, r, p) |
| halo DBs (`box[]`) | `H`, created once beside the cells they carry, by the generation leaf that opens their brick | `3336·min(HALO_CHUNK, layer remainder)` B each; `3336·Σ_d \|L(r,d)\|` per rank |
| block directory (`cell_list`) | 1, assembled bottom-up by the generation tree | `16·B + 8·H + 4·(26·nranks+1) + 64` B — cells, then the halo blocks, then where each brick's layers begin; read CONST by every tree task |
| configuration DB | 1, written once in `mainEdt`, never again | ~1 KB |
| simulation state, timer | 2 | ~80 B, 352 B |
| per-step partials | `B + F` per step | 24 B each (potential energy, kinetic energy, atom count), destroyed by the collector that reads them |
| EDTs per step | `B + 2F + 2` on one rank; `+ 2H + 2F_h + 1` where there is a halo | the cell tasks, their spawn and collect tree, the reduction's final and the control task after it; plus one `top` per block; plus the halo's pack and proxy tasks, its own tree, and the task between the two phases |
| events per step | 1 on one rank; `2H + F_h + 1` where there is a halo | the final's COUNTED completion; the cell tasks and the reducing tree hand partials up by dependence, but every pack, proxy and halo-tree collector carries a COUNTED completion of its own |

A step is ONE phase on one rank and **two** across bricks.  The cell phase is
unchanged: every cell's task reads generation `k & 1` of its own block and of
its 26 neighbours' (CONST) and writes generation `(k+1) & 1` of its own (RW),
so each generation is written every other step — one write per cell per step —
and read by 27 tasks in the step after it is written and by none while it is
written.  What differs is where a neighbour comes from: one inside the brick
is still its own block, one outside is a box inside a halo run.  The halo
phase runs first and packs, per `(r, d, run)`, that run's cells of the
generation the step is about to read into its halo block, then a task on the
reading brick pulls the block once.  One block per run — not one per
generation — is enough because the step's tree-shaped barrier separates a
step's readers of a run from the next step's write of it.
`Max Link Cell Occupancy` is printed
once and, unlike the base port's per-step whole-grid pass, is the maximum **at
generation**: refreshing it per step would cost a global max reduction every
step for a number printed once.

Worked numbers at `-x 39 -y 39 -z 39 -w 8` → `g = 24`, `B = 13 824`,
`G = 1728`, and on one rank `I = 585`, `F = 2313`: **18 452** tasks per step,
237 276 atoms, **92 MB** of cell payload plus a 221 KB directory.
`13 824 = 4 × 3456` — the frontier is an exact multiple of the largest
campaign geometry's worker count, and `13 824` divides every node count in
the sweep exactly.  At `-x 193` (`g = 120`, `B = 1 728 000`): `F = 253 449`
on one rank, **2.23 M** tasks per step, 11.6 GB of payload.

The halo is a rounding error against that.  At `-x 193 -w 8`, `H` and its
tasks' share of a step: 900 blocks / 0.1% at 2 ranks, 1840 / 0.2% at 4,
2896 / 0.3% at 8, 4000 / 0.4% at 16, 5312 / 0.5% at 32.  At `-x 103`
(`g = 64`, `B = 262 144`): 256 / 0.2%, 528 / 0.3%, 928 / 0.7%, 1344 / 0.9%,
1920 / 1.2%.  At `-x 39`, where a brick is small enough for its surface to
matter, it reaches 3.7% at 8 ranks and 11.2% at 32.

The tree's cost at `B = 13 824` on one rank, computed from the same recursion:

| `w` | leaves `G` | interior `I` | tree tasks `2F` | share of a step | tasks per step |
|-----|-----------|--------------|-----------------|-----------------|----------------|
| 16 | 864 | 425 | 2578 | 19% | 16 404 |
| 8 (default) | 1728 | 585 | 4626 | 33% | 18 452 |
| 4 | 3456 | 585 | 8082 | 58% | 21 908 |
| 2 | 6912 | 3401 | 20 626 | 149% | 34 452 |

Across the rank grid `G` is fixed and only `I` moves, because each brick's
recursion bottoms out differently (same size, `w = 8`; `F` and tasks per
step): 1 rank 2313 / 18 452, 2 ranks 2579 / 18 984, 4 ranks 2021 / 17 868,
8 ranks 2313 / 18 452, 16 ranks 2585 / 18 996, 32 ranks 2025 / 17 876 — within
±3.2% of the one-rank count (±10% at `w = 4`).

Counter cross-check (`arts_inv_wb`, `-x 103 -y 103 -z 103 -N 100 -n 10 -w 8`,
`perf` set, per rank-step; the run also pays generation, the rescale, step 0
and teardown once, which amortise to ~3% over 100 steps):
`NUM_EDT_CREATE` 43 730 measured against 43 724 predicted at 8 ranks and
178 970 against 179 110 at 2; `NUM_DB_CREATE` 38 527 against 38 480 at 8
ranks.  The formulas above are pinned to 0.1%.

## Wiring

`mainEdt` parses the command line, fills the configuration block and starts the
generation tree; every later control task carries the same four blocks — timer
(RW), simulation state, configuration (CONST) and the cell directory (CONST) —
plus, where it waits for a phase, one completion slot.

- **generation** (`init_fan_edt` → `init_merge_edt`): the same brick-indexed
  tree as the phases.  A leaf runs on the rank that owns its brick, creates
  both generations of each cell's block there, generates the atoms into the
  first, and publishes a directory slice plus its subtotals; an interior node
  concatenates its children's slices (a contiguous brick-local interval below
  a brick, whole bricks in rank order above) and adds their subtotals; the
  root places each entry at its cell index, so the block it produces **is**
  the cell-index ordered directory the rest of the run reads.  The leaf that
  opens a brick's range also creates that brick's halo blocks, so each of
  them appears once and on its owner; halo entries are concatenated at every
  level (a brick contributes them all from one leaf and the bricks arrive in
  rank order), and the root, which is the only node that sees every brick,
  lays down the table saying where each brick's layers begin.  Every merge
  node also takes the configuration CONST in slot 0, from which it derives
  the bricks.
- **spawn tree** (`fan_edt`): a node covering more than one rank splits its
  rank interval into `min(8, ranks)` parts, one child per part placed on that
  part's first rank; a node covering one rank's brick either splits its
  brick-local range into `min(8, ceil(n/w))` parts of whole leaf widths
  or, at `w` items or fewer, emits that range's tasks — so a leaf's items
  all share one owner.  The range is the brick's cells for the step, rescale
  and release phases and its halo blocks for the two halo phases; one tree
  serves either.  Each node first creates the collector its children
  report to, so the tree is built top-down and collected bottom-up.
- **collectors** (`sum_edt` / `join_edt`): the step's collector adds its
  inputs' three fields and hands one 24-byte contribution up; a non-reducing
  phase's collector (rescale, release) carries only its own completion.
  Neither ever holds more than `max(8, w)` dependences.  A collector adds in
  slot order and slots are assigned in ascending cell index (brick-local
  order is ascending cell index, bricks are visited in rank order).
- **halo** (`pack_edt` / `proxy_edt`): a leaf emits, per halo block of its
  brick, a pack task on the owner (the block **RW** in slot 0, the run's cells
  of the generation the step reads CONST after it — at most
  `HALO_CHUNK + 1` slots, all of them local) which copies each cell's box into
  its slot and returns the block's GUID, and a proxy task on the brick that
  reads the layer, whose one dependence is the pack's COUNTED output event.
  The proxy's body is empty: its acquire is the point, and it is what makes
  the run cross the fabric once however many cell tasks read it.  The owner
  creates both, so the pack's event never has to be published anywhere; the
  proxy's completion is what the halo phase's collector waits for.
- **step** (`step_edt`, `3 + b` slots, `b ≤ 26`): configuration CONST, own
  cell's read generation CONST, own cell's written generation **RW**, then one
  slot per DISTINCT block its 26 neighbours are read from — a cell block for
  each neighbour inside the brick, a halo run for the ones outside, which
  several neighbours usually share.  Which slot and which box within it each
  neighbour is at is a pure function of the geometry, so the task that wires
  the dependences and the task that reads them (`cell_reads`) derive it
  identically and nothing about the map travels in the parameters — the same
  contract `box_neighbours` already carried.  Gathers the atoms whose positions are in the cell (its
  own block's stayers in stored order, then each neighbour's departures into
  it in neighbour order — `cell_classify` places every atom of a read block
  by its position, so writer and readers agree without the block saying
  where an atom went), computes the forces against every atom within reach
  (`lj_cell_self` for pairs inside the cell, `lj_cell_pair` against each read
  block with a per-atom periodic shift — an atom in a cell not adjacent to
  this one is out of reach and skipped), kicks, takes the energy point at
  block ends, kicks again, drifts and wraps, and hands `{pe, ke, n}` to its
  leaf collector.  Which kicks apply come from the step's place in its block
  (`step_launch`): `dt` inside a block, `dt/2` + energy point + `dt/2` at a
  block's end, no kick before step 0's energy point, no kick or drift after
  the last step's.
- **rescale** (`temp_edt`, 1 slot): generation 0 of the cell **RW**; the
  momentum correction rides in the parameters, so this phase touches no
  shared block at all.
- **release** (`release_edt`, 1 slot): destroys both generations of its cell
  on the rank that owns them; `halo_free_edt` does the same for one halo
  block, in the phase that runs just before it.

Three structural consequences.  A cell is written only ever by the task of the
rank that owns it, so its write ownership never leaves that rank for the whole
run — the base program's per-step whole-grid ownership sweep is gone.  The
only block a wide phase shares is the configuration, which is written once
before any phase runs and never again, so it validates but never invalidates.
And **no datablock has a reader and a writer in the same step**: the
generation a step reads was written by the previous step and is next written
by the step after, and a halo run is written by the step's first phase and
read only by its second, so every access to a block is event-ordered against
every write of it — DB-WRF-valid at DB granularity, not merely OCR-legal
(the row is outside DB-WRF overall on the hand-off pattern the catalog
records, not on this steady-state cell traffic).  A halo block is likewise
written only by its owner, so it adds no
ownership movement of its own: it moves as a reader copy and nothing else.

## Flow

Bulk-synchronous with ONE tree-shaped barrier per step on one rank and two
across bricks: `halo(H) → step(B) → halo(H) → step(B) → …`, each step's final
feeding the next step's control task, and at the end of every block of
`period` steps the kinetic energy reduced alongside the potential energy.  The
halo phase's collector feeds one control task that emits the cell phase, so
the second phase's tasks do not exist until every run of the first is packed
and pulled — which is what lets a cell task name a halo block by GUID and be
sure of finding it filled and at home.  Initialisation is the same trees:
generation, the temperature rescale, and step 0 (the initial force and energy
point, with the first half kick and drift folded in).  Teardown is two phases
too, the halo blocks and then the cells, each by its owner.

The instantaneous frontier is `B` and is actually reached: a step's tasks are
issued by `G` leaves in parallel, so the time to put a step on the machine is
one leaf's `w·30` operations plus the tree's depth (`≈ ceil(log8 G)` levels of
splits: four at `G = 1728`), not `B·30` on one rank.  The narrowest point of a
step is the final and the one control task after it, which do no cell work.

Generation is a phase like any other: the tasks that will own a cell create
it, fill it from a generator that depends only on the atom's global index, and
report subtotals up the same tree.  The temperature is then set by one
per-cell map, because the mean momentum and the rescaling factor both follow
in closed form from the two sums the tree already returned.  Teardown is a
phase too — each cell is released by its owner — so the run does not end with
one task making `2B` round trips.

## Placement (base)

This program has one tier and places explicitly; there is no
`OCR_APP_OPTIMIZED_PLACEMENT` guard.

- **cell blocks** (both generations) are homed on their owner: the cell grid
  is cut into a three-dimensional grid of rank blocks (`splitDimension_Cart3D`
  over the rank count, then `rank_axis = (cell_axis · ranks_axis) /
  cells_axis` per axis).  A rank therefore owns a compact brick, and the
  share of a cell's 26 neighbours that is remote falls with the brick's
  surface-to-volume ratio.  At `g = 24` the split is exact at every campaign
  node count (1, 2, 4, 8, 16, 32 → bricks of 24³, 12·24·24, 12·12·24,
  12·12·12, 6·12·12, 6·6·12 cells), and so is `g = 120`, so the per-rank share
  is exactly `B/nranks`.
- **cell tasks** (step, rescale, release) are placed on their cell's owner, so
  a cell's blocks and every task that writes them live on one rank for the
  whole run.
- **tree tasks** below a brick are placed on the brick's owner, which owns
  every cell they cover: a leaf's creates, the dependences it adds and its
  collector are all local, and a collector's inputs (completion events or
  24-byte partials) come from its own rank.  The rank-level nodes — one per
  phase up to 8 ranks, nine up to 64 — sit on the first rank of their
  interval, and their remote child creates (one create plus two CONST
  dependences each) and the children's collector reports are the **only**
  cross-rank wiring a phase issues: `4·(nranks−1)` messages per step at
  ≤ 8 ranks, i.e. **4 / 12 / 28 per step at 2 / 4 / 8 ranks**, 60 at 16 and
  124 at 32, independent of `B` and of `-w`.  Generation is the same tree,
  so a cell is created on its owner by a task running there and nothing
  migrates at init.
- **halo blocks** are homed on the brick they belong to, so a pack task reads
  only cells of its own rank and writes only a block of its own rank; the
  proxy that pulls a run is placed on the one brick that reads it
  (`rank_shift`).  A halo block's ownership therefore never moves either: it
  is written at home and travels only as a reader copy.
- **what crosses the fabric per step is the halo, by the run**: the cells a
  brick's neighbours need are `C = Σ_r Σ_d |L(r,d)|` — 576 / 1 344 / 2 368 at
  `g = 12`; 6 084 / 12 792 / 20 188 / 27 584 / 35 668 at `g = 39`;
  57 600 / 117 120 / 178 624 / 240 128 / 303 616 at `g = 120`, for
  2 / 4 / 8 / 16 / 32 ranks.  That number is a property of the brick
  decomposition and no wiring can change it: it is the payload, `3336·C` bytes
  per step, and bandwidth is not what limits this row.  What the wiring
  decides is how many MESSAGES carry it.  With one datablock per cell it was
  one fetch and one invalidation round per cell of `C` — `4C` messages, tens of
  thousands per rank per step, which is what made the local trend flat.  With
  the halo it is a small constant per RUN: the owner creates the reading
  brick's proxy and registers the pack's event on it, the packed run is
  delivered and fetched once, the proxy reports its completion home, and the
  next step's pack invalidates that one copy.  The run count is
  `H = Σ_r Σ_d ceil(|L(r,d)|/HALO_CHUNK)` — 12 / 48 / 208 / 416 / 832 at
  `g = 12`; 96 / 216 / 460 / 736 / 1 200 at `g = 39`;
  256 / 528 / 928 / 1 344 / 1 920 at `g = 64`;
  900 / 1 840 / 2 896 / 4 000 / 5 312 at `g = 120`.  A run costs about eight
  messages against a cell's four, so the message count falls by roughly
  `C/(2H)`; measured at `-x 103` it is 40× at 2 ranks and 35× at 8 (table
  under Family shape).  The halo is still the only term that grows with the
  rank count; it is now three orders of magnitude below the cell tasks it
  accompanies.
- **the control spine** (the final and one control task per step, `top` and
  `bot` per block, the generation root) is placed on rank 0, where the timer
  and the mutable simulation state live.  These are singletons of the
  program, not a confined parallel phase.

Load balance is the per-axis floor/ceil of `cells/ranks`, i.e. exactly 1.000 at
every campaign geometry when `g` is divisible by the rank grid, and at worst
`prod_i ceil(g_i/p_i) / (B/nranks)` otherwise.

## Sizing

`n_x,n_y,n_z` set both parallel width and memory: cells `≈ (0.6246·n)³`, atoms
`4·n³`, payload `6672 B` per cell (two 3336-B generations) regardless of
occupancy, plus a `16 B` directory entry.  Task grain is fixed by the cutoff —
a step task always compares one cell against 27 — so `n` buys *more* tasks,
never bigger ones.  `-N`/`-n` scale time and nothing else; `-w` sets how
finely the spawn tree is cut and does not change the work.

- **Width**: the per-step frontier is `B`.  On a cubic grid `B` is an
  integer multiple of 3456 exactly when `g ≡ 0 (mod 24)`: `-x 39 -y 39 -z 39`
  gives `g = 24` and `B = 13 824 = 4 × 3456` (`-x 40` gives the same `B` but
  sits 0.06% below the `g = 25` boundary, so 39 is the member of that band to
  use); the catalog's `-x 193 -y 193 -z 193` gives `g = 120`,
  `B = 1 728 000 = 500 × 3456`.
- **Spawn frontier**: `G = Σ_r ceil(B_r/w)`, which is `ceil(B/w)` exactly at
  every campaign geometry (each brick of `g = 24` or `g = 120` holds a
  multiple of 16 cells at 1..32 ranks).  At `B = 13 824`, `w = 8` gives
  `G = 1728`; `w = 4` gives exactly 3456 and `w = 2` gives 6912, so the knob
  reaches and passes the largest geometry's worker count.  Smaller `w` buys
  issue parallelism and costs tree tasks — 33% of a step's tasks at `w = 8`,
  58% at `w = 4`, 149% at `w = 2` on one rank (table above; a few per cent
  more or less across the rank grid).
- **Non-cubic sizes were also considered, at the base row's scale**: the
  three axes are independent knobs, and `-x 39 -y 26 -z 29` gives
  `g = (24, 16, 18)`, `B = 6912 = 2 × 3456` at 0.5× the per-step cost of the
  cubic candidate at that scale.  That grid also splits exactly over the
  rank grid at every campaign node count
  (`(1,1,1) (2,1,1) (2,2,1) (2,2,2) (4,2,2) (4,4,2)`).  This row instead runs
  the far larger cubic geometry above (`-x 193 -y 193 -z 193`,
  `B = 1,728,000`), sized to the restructured tier's 120 s target rather
  than the base row's shorter one.
- **Height**: unlike the base row this program is not sized by its step count.
  `-N 100 -n 10` are the published defaults and are what this row runs; the
  campaign window is reached with the size knob above.
- **Memory (1 node)**: `6688 · B + 56` bytes of application payload — 46 MB at
  `B = 6912`, 92 MB at `B = 13 824`, 397 MB at `B = 59 319`, **11.6 GB at
  `B = 1 728 000`** — plus a `(B+F)·24 B` transient per step (48 MB at
  `B = 1 728 000`).  Runtime metadata dominates below ~10⁵ cells; the row is
  time-bound, never memory-bound, at every campaign geometry.  A halo adds
  `3336·Σ_d |L(r,d)|` per rank and nothing on one rank: 22 MB per rank at
  `-x 103` and 74 MB at `-x 193`, 8 ranks — 5% of that rank's cells, and it
  falls with the rank count as the surface does.
- **Halo against volume**: a rank's per-step wire cost is a small constant
  times `H_r` runs against `|brick(r)|` cell tasks of compute, and only the
  instance sets that ratio — 116 runs against 32 768 cell tasks at `-x 103`
  and 362 against 216 000 at `-x 193`, for 8 ranks.  Before the aggregation
  the same term was `4·Σ_d |L(r,d)|` messages, a third of the cell tasks at
  `g = 39` and a tenth at `g = 120`, which is what held the local trend flat
  from 2 ranks to 8; the run count is 50-60× smaller, so the term is well
  under a per-cent of the compute at every campaign geometry and the size at
  which the row scales is no longer set by it.
- **Hard ceiling**: no cell task takes more than 29 dependences, no pack task
  more than `HALO_CHUNK + 1` and no tree node more than `max(8, w)`, so the
  base row's `B+1`-slot join limit does not apply here; `B` is bounded only by
  memory, and only `-w` (bounded 1..1024, and checked at the create) can
  approach a dependence limit at all.

## Family shape

`calibration pending` for the Dane window; the local shape is settled.  Every
trend below is `15w+1p` per rank on `arts_inv_wb`, `-N 100 -n 10 -w 8`, e2e
seconds at 1 / 2 / 4 / 8 ranks:

| instance | before the halo | with the halo | `S8` |
|----------|-----------------|---------------|------|
| `-x 20` (`g = 12`, `B = 1728`) | 0.8 / 5.0 / 5.6 / 8.7 | 0.60 / 0.51 / 0.48 / 0.64 | 0.9 |
| `-x 64` (`g = 39`, `B = 59 319`) | 18.1 / 26.8 / 24.8 / 22.7 | 18.21 / 11.19 / 6.98 / 4.75 | 3.8 |
| `-x 103` (`g = 64`, `B = 262 144`) | 75.2 / 72.7 / 70.2 / 71.2 | 74.97 / 43.25 / 25.10 / 14.65 | 5.1 |

Two earlier states are worth keeping for the shape of the argument.  At
`-x 20` **before the tree was indexed by brick** the curve was
0.76 / 54.1 / 29.7 / 19.6 — a 2-rank cliff that was the tree's own wiring
(~58 400 messages per step at every rank count), and indexing the tree by
brick removed it.  At `-x 64` **before the step was fused into one task per
cell** it was 23.1 / 49.7 / 50.2 / 51.8, the excess over the 1-rank compute
being two per-cell blocks fetched and invalidated per boundary pair per step.
Fusion took that to one fetch and one invalidation per (cell block, reader
rank) per step — and the curve stayed FLAT (the `before the halo` column
above), because the remaining term was still one message set per boundary
CELL: at `-x 103` a rank's single progress thread was servicing 33-45 k
remote messages per step at ~17 µs each, 0.57-0.69 s of message service
against 0.09 s of compute at 8 ranks.  The 1-rank compute (~0.7 s/step) and
that floor happened to coincide, which is what made the curve look flat
rather than cliff-shaped.

The halo removes the term rather than shrinking it, and the counters say so
(`arts_inv_wb`, `-x 103`, per rank-step, before → after):

| | 2 ranks | 8 ranks |
|---|---|---|
| remote acquires | 33 235 → **130** | 45 485 → **118** |
| remote messages | 41 539 → **1 042** | 33 174 → **950** |
| invalidations sent | ~boundary cells → **129** | ~boundary cells → **117** |
| remote payload | 29.0 MB → 27.7 MB | 23.4 MB → 22.1 MB |

The payload is unchanged, as it must be — the same cells still cross — and the
message count falls 40× and 35×.  The remote acquire count lands on the run
count `H_r` (128 at 2 ranks, 116 at 8) within 2%, which is the model stated
outright: **one remote acquire, one delivery and one invalidation per halo
run per rank per step**, and about eight wire messages in all per run.

What is left is compute plus a per-step barrier, so the row scales: 5.1× at 8
ranks over `-x 103`, against a base and hinted `CoMD_sdsc` that anti-scale
monotonically over the same instance.  The one-rank cell is untouched by any
of this (a single rank has no halo, and the program takes the same one-phase
path it always did), which is the control: 74.97 s here against 75.2 s before.
