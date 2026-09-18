# miniAMR_intel_dist

*The same block-structured adaptive mesh as `miniAMR_intel`, with the
continuation-cloning control plane replaced: a stage of a block is ONE task
over strips its neighbours wrote, every phase is emitted and collected by a
spawn tree indexed by rank, the checksum reduces over the block forest instead
of joining on one root, and the mesh each epoch runs on is a directory one
task derives from the object trajectory — because the published refinement
decision reads a block's level and centre and never a cell.*
Source: `third_party/ocr-apps/apps/miniAMR/refactored/ocr/intel-dist/`
(5 C files and one header, ~3.2k lines; `amr_dist.c` holds the spine and the
per-block tasks, `mesh.c` the refinement state machine and the published object
geometry, `physics.c` the stencil, the checksums and the strip arithmetic,
`fan.c` the phase trees, `command.c` the command line).

## Overview

miniAMR (Mantevo) sweeps geometric objects through a 3-D mesh; blocks the
objects touch refine 8-way, blocks they leave coarsen back, and between
refinement rounds every block runs a 7- or 27-point stencil over `num_vars`
variables with a 6-face halo exchange.  This row is the restructured tier of
`miniAMR_intel`: same stencil, same refinement and coarsening rules, same
object sweep, same checksum sweeps, same command line, same three printed
scalars, and the **same `Weighted Mesh Checksum`, digit for digit**.

What the base port measures is not that physics but its control plane.  There,
`blockClone_SoupToNuts` runs the algorithm as straight-line C and, at every
point where it must wait, snapshots a software stack into the block's meta and
hands the continuation to a freshly created EDT.  Four structures follow:

1. every wait is a new EDT plus a copy of its dependence vector — measured on
   the trend instance, ~7 EDTs and ~13 datablocks per block per stage;
2. the three halo axes are serialised, because each is a suspension point, so
   a stage costs three round trips instead of one;
3. every face rendezvouses through a labeled sticky event whose home is
   `index % nranks`, unrelated to either endpoint;
4. every checksum round is a `B`-way join walked on one root EDT, and the
   refinement round is a parent-mediated neighbour-consensus protocol with
   eleven suspension points.

The result is a per-block latency chain whose length does not fall with the
rank count, which is what the hinted tier could not fix — placement was never
the limit.  On the local trend instance (`8×8×4`, refine 1, published heights),
`arts_inv_wb`, e2e seconds at 1 / 2 / 4 / 8 ranks:

| tier | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| base | 4.34 | 176.74 | 139.31 | 101.75 |
| hinted | 4.33 | 3.85 | 3.17 | 3.80 |
| this row | 4.76 | **2.64** | **2.03** | **2.74** |

The one-rank cell is 10% slower than either base-tier row and that is the honest
price of the design: a stage is a tree-shaped barrier, and at one rank the
barrier buys nothing.  From two ranks on it is what the row is for.

This program replaces all four.  A stage of a block is one task: it takes the
strips its neighbours wrote at the previous stage, reflects the faces that are
the edge of the mesh, runs the stencil over every variable, takes its checksum
point where the stage is one, and writes the strips the neighbours read next.
Phases are emitted by a spawn tree and collected by a join tree, both indexed
by rank, so no single task stands between the machine and the work.  Nothing
is labeled, so nothing is homed at `index % nranks`.

The refinement round is where this row differs most from its base, and the
reason is a property of the published program rather than a simplification:
**the mesh is a pure function of the block geometry and the object
trajectory.**  `check_objects` reads a block's level, its integer centre, the
control record and the object array; `check_block` reads the object shapes; the
cell values never enter.  The intent cascade, the level-difference agreement,
the sibling unanimity and the coarsening wave are therefore all topology, and
this program computes them as one serial pass over an explicit block forest
instead of as a distributed protocol.  What remains distributed is the part
that moves data: one task per block of the new mesh, upsampling from its
ancestor, summing its eight children, or keeping what it had, and then packing
its strips.

Three stdout scalars, the same three as the base row:

- `Golden Checksum == <v> (weighted <w>)`, the pre-loop reference round.  A
  pure function of the geometry and `num_vars`; it validates initialisation.
- `Grand Total Checksum == <v> at timestep <t>`, printed once on the shutdown
  path.  The per-variable sums are conserved by construction (a split writes
  `v · 0.125` into each of eight cells and a join sums the eight back, both
  exact in binary floating point) and every round is checked in-run against the
  golden at `10^-error_tol`, so this is a completion marker.
- `Weighted Mesh Checksum == <v> at timestep <t>` — **the catalog's scalar**.
  The same sweep also accumulates each cell value times its linear index within
  its own block, which is *not* conserved across a split or a join, so it is a
  function of the mesh state the run actually reached.

**Scalar identity with the base row is exact, not to tolerance**, and it is
what gates this row.  Two things make it so.  The weighted reduction adds base
blocks left to right in mesh order, and each base block's subtree in prong
order — the base row's own grouping, reproduced by a *concatenation* tree over
the base blocks plus an arity-8 sum tree inside each subtree, rather than by a
reduction tree that would regroup the additions.  And the mesh this program
reaches is the base row's mesh, block for block, including two pieces of
published arithmetic that a tidier implementation would have got differently:
a child's centre is offset by half its own half-extent (and, at the deepest
level, by the object count, which is the word that precedes the table of
powers in the control record), and the octant a coarsening block lands in
reads its position bits in the opposite order to the one the split used.  Both
are reproduced verbatim, because the centre decides which blocks refine and
the octant decides what the weighted sum sees.

## Parameters

The command line is the base row's.  Every flag it accepts is accepted here
with the same default, the same validation and the same meaning; an
unrecognised flag prints the help text and ends the run.

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `--nx --ny --nz` | cells per block per axis (must be even, > 0) | 10 | ✓ |
| `--npx --npy --npz` | base (unrefined) mesh in blocks | 1 | ✓ **the width knob**, and **uncapped**: no task holds a dependence per block, so `MAX_NUM_UNREFINED_BLOCKS` has no counterpart here |
| `--num_refine` | max refinement levels (0–15) | 5 | ✓ — and unbounded above by any GUID range: there is no labeled reservation |
| `--refine_freq` | timesteps between refinement rounds | 5 | ✓ |
| `--block_change` | levels a block may change per round; 0 → `num_refine` | 0 | ✓ |
| `--uniform_refine` | refine everything once | 0 | ⚠ inert, exactly as in the base: the live decision comes from `check_objects` alone, and `--uniform_refine 1` with `--num_refine 0` leaves the mesh uniform |
| `--num_vars` | variables per cell | 40 | ✓ |
| `--comm_vars` | variables exchanged per batch | 0 | ⚠ parsed and validated; this program exchanges every variable of a face in one strip, so the flag changes no value and no message count (batching is value-neutral in the base too: a face is always packed from pre-stage values) |
| `--num_tsteps` | timesteps | 20 | ✓ |
| `--stages_per_ts` | comm+calc stages per timestep | 20 | ✓ |
| `--checksum_freq` | stages between checksums (0 = none) | 5 | ✓ — with 0 the two pre-loop rounds still run, as in the base, so a result line is still printed |
| `--stencil` | 7 or 27 | 7 | ⚠ both implemented; under refinement the 27-point form reads shell cells no exchange ever writes, and the base warns that answers diverge — bitwise identity is claimed for `7`, and for `27` only on a static mesh |
| `--error_tol` | checksum tolerance exponent | 8 | ✓ |
| `--num_objects` | refinement-driving objects | **1** | ✓ — naming it opts out of the published default object |
| `--object t b cx cy cz mx my mz sx sy sz ix iy iz` | one object | the published spec | ✓ must follow `--num_objects` |
| `--report_diffusion` | per-variable diffusion per round | 0 | ✓ — leave off, it is per-round stdout on the spine |
| `--permute` | rotate comm axis order per stage | off | ⚠ accepted; the axis order changes no value (a face is packed from interior cells only) and this program exchanges all six faces at once |
| `--refine_ghost` | include ghost cells in the refinement test | off | ✓ (flag) |
| `--code` | 0 minimal sends / 1 / 2 | 0 | ⚠ only 0 is implemented, and 1 or 2 is **reported and refused** rather than reaching an unimplemented case |
| `--plot_freq` | plot every N timesteps | 0 | ⚠ accepted at 0; a non-zero value is reported and refused, because this program writes no plot files |
| `--report_perf` | perf report level | 4 | ⚠ parsed, never read (as in the base) |
| `--width` | blocks emitted by one spawn-tree leaf | 8 | ✓ the issue-parallelism knob; a leaf never crosses a rank, so a phase is emitted by exactly `Σ_r ceil(L_r/w)` leaves |

Two failures end the run with a report rather than a fault: a value-taking flag
in the last argument position, and any flag the parser does not know.  The base
port's assertion idiom is a deliberate `SIGSEGV`; this program prints the same
text and shuts down cleanly, which is the only behaviour outside the physics
that it deliberately does not copy.

## Structure

Let `B = npx·npy·npz` base blocks, `L` the live block count (`B` at level 0,
`+7` per block that refines, `−7` per octet that coarsens), `V = num_vars`,
`S = stages_per_ts`, `T = num_tsteps`, `K = ceil(S / checksum_freq)` checksum
rounds per timestep, `w` the leaf width, `R` the rank count, and `F = G + I`
the nodes of a phase tree (`G = Σ_r ceil(L_r/w)` leaves, `I` the interior nodes
the arity-8 recursion yields per rank plus the rank level).

| object | count | size |
|--------|-------|------|
| block payload | one per live block | `(nx+2)(ny+2)(nz+2)·V·8` B — 552,960 B at the campaign grain |
| strip | 12 per live block: one per face per generation | `V · (the two block dims of the face) · 8` B — 32,000 B each, 384,000 B a block |
| checksum block | one per live block | `8 + 8V` B |
| directory (`mesh`) | one per epoch | `≈ 128·L + 32·L + 4·B + 32·I_f + 4R` B: the block records with their neighbour levels and the strip writer of every incoming slot, the reduction forest, and where each rank's blocks begin |
| handles (`gslice`) | one per epoch | `16 + 14·L` GUIDs' worth |
| EDTs per stage | `L + 2F` | the block tasks, plus each tree node and its collector |
| datablocks per stage | **0** | strips outlive the epoch; a stage writes into the ones it holds |
| events per stage | `L + F` | one COUNTED completion per block task and per collector |
| EDTs per checksum round | `I_f + ceil(B/w) + I_c + 1` | one per interior forest node, one per run of base blocks, the runs' own tree, and the final |
| EDTs per epoch | `L + 2F + L + 2F` | the build pass and the pass that reclaims the epoch it replaced |
| datablocks per epoch | `14·(new blocks) + L + F + 1` | the blocks that were made, the handle records the tree folds up, and the directory |

The stage is the whole cost of the row, and it is **one task and no datablock
per block per stage**, against the base row's ~7 and ~13.  Two properties make
that possible.  A strip carries a whole face however the mesh around it is
refined — the same buffer holds one full face, one quarter face summed 2×2 for
a coarser neighbour, or four quarter faces for four finer ones — so a strip is
created once for the block's lifetime and never resized.  And a strip has two
generations: a stage reads the generation the previous stage wrote and writes
the other, so **no strip has a reader and a writer in the same stage** and the
stage boundary needs nothing but the phase's own tree-shaped barrier.

A block's stage task holds `4 + f + s` dependences, where `f ≤ 6` is the faces
with a block behind them and `s ≤ 24` the strips it reads (one per face at the
same level or coarser, four where the face is finer) — so at most 34, and 16 on
a uniform interior block.  No task in the program holds a dependence per block:
that is what removes the base row's `MAX_NUM_UNREFINED_BLOCKS`.

Measured against the base row on the local trend instance (`8×8×4`, refine 1,
20 timesteps × 20 stages, `arts_inv_wb`, whole-run totals):

| | base / hinted | this row |
|---|---|---|
| `NUM_EDT_CREATE` | 697,353 | **156,100** |
| `NUM_DB_CREATE` | 1,297,254 | **9,850** |
| `NUM_DB_ACQUIRE_LOCAL_HIT` | 4,704,249 | 1,564,257 |

4.5× fewer tasks and 132× fewer datablocks: the strips outlive the epoch, so a
stage creates nothing at all, and what is left of the datablock count is the
handful of directories, handle arrays and reduction partials.  At eight ranks
the same instance sends 1,112,405 messages from the hinted tier and **618,106**
from this one — 77 k per rank against 139 k — for the same payload, and takes
**zero** write-ownership migrations against the base row's 2,352,209: a
payload is written only by tasks of the rank that owns it, for the whole run.

## Wiring

`mainEdt` parses the command line, derives the level-0 directory and opens the
first epoch.  Every control task afterwards holds the run state for writing and
the directory in force for reading, so the spine is serial by construction.

- **phase tree** (`fan_edt`): a node covering more than one rank splits its rank
  interval into `min(8, ranks)` parts, one child per part placed on that part's
  first rank; a node covering one rank splits that rank's own range of the
  directory on whole leaf widths, or at `w` blocks or fewer emits the range's
  tasks.  A leaf therefore never crosses a rank, and every task it creates, and
  its collector, sit where the blocks live.  Each node creates its collector
  first, so the tree is built top-down and collected bottom-up.
- **collectors**: a phase that carries no value uses a join whose COUNTED
  completion is all its parent waits for; the build phase's collector
  *concatenates* instead, so the array the root hands up is the directory's own
  order and no task ever writes into another's slot.
- **stage** (`stage_edt`): the directory and the handles CONST, the block's
  payload **RW**, its checksum block RW where the stage takes its point, the
  strips it writes RW, and the strips it reads CONST.  Which strip feeds which
  face and which quarter of it is a pure function of the geometry, so the task
  that wires the dependences and the task that reads them derive it identically
  and nothing about the map travels in the parameters.
- **build** (`build_edt`, one per block of the new epoch): the directory CONST,
  the previous epoch's handles CONST, then the payloads this block draws from —
  none where it is a fresh level-0 block, its ancestor's where it was just split
  off (upsampled once per level it gained), all eight of the blocks that
  vanished where an octet joined, or its own where it survived.  It then packs
  every strip it sends, because what a strip holds depends on the level behind
  the face and a mesh change makes every strip of every block stale.  It hands
  its fourteen handles to its leaf collector.
- **release** (`release_edt`): gives back the payload, checksum block and twelve
  strips of a block the new mesh has no place for, on the rank that owned it.
- **the mesh** (`plan_edt`): one task derives the next directory from the
  current one, the objects and the timestep.  A refinement round is a sequence
  of these — one that splits every block the objects and the level-difference
  rule put a finer mesh under, then one per level that collapses the octets
  that may coarsen — and a phase that leaves the block set alone costs a
  directory and nothing else.
- **checksum** (`sum8_edt` / `cbase_edt` / `ccat_edt` / the final): an interior
  node of the block forest adds its eight children in prong order; a run of base
  blocks is laid out, not summed; the runs are concatenated; and the final adds
  the base blocks left to right, compares against the reference round and prints.
  A live block needs no task at all — its checksum block is a dependence, and the
  stage phase's barrier is what makes it safe to read one.

Three structural consequences.  A block's payload, its strips and its checksum
block are written only by tasks of the rank that owns it, and the placement map
is level-consistent, so a refinement never moves a payload between ranks.  The
only blocks a wide phase shares are the directory and the handle array, which
are written once per epoch and never again — they validate but never
invalidate.  And what crosses the fabric per stage is exactly the strips of the
blocks whose neighbours are on another rank: one fetch and one invalidation per
(strip, reading rank) per stage, which is the brick's surface in blocks, not
its volume.

## Flow

`mainEdt` → build epoch 0 (one task per block: fill, pack, take the reference
checksum point) → the reference round → a second round on the same values, as
the base emits → the initial refinement round → then

```
for ts in 1..T:
   for stage in 0..S-1:
      stage phase over every live block
      if checksum_freq and stage % checksum_freq == 0: a checksum round
   if num_refine and not uniform_refine:
      move the objects
      if ts % refine_freq == 0: a refinement round
print the last round's aggregate; ocrShutdown
```

Bulk-synchronous with ONE tree-shaped barrier per stage.  That barrier is what
the design costs: the base row's blocks are free to run ahead of each other
between checksum rounds, and here they are not.  It buys the property that
makes the stage a single task — with plain datablock dependences there is
nothing to order a writer's next write of a strip generation against a reader's
read of it, and the barrier is what supplies that ordering without an event per
(block, stage).  Its cost is one tree of depth `≈ ceil(log8 G)` per stage, at
`(L + 2F)/L ≈ 1 + 2/w` tasks per block-stage.

The instantaneous frontier is `L` and is actually reached: a stage's tasks are
issued by `G` leaves in parallel, so the time to put a stage on the machine is
one leaf's work plus the tree's depth, not `L` creates on one task.  The
narrowest points are the final of a checksum round and the one task that
derives each new mesh — both `O(L)` of arithmetic and no datablock movement.

Refinement is a sequence of epochs rather than a protocol: the round's split
pass, then one pass per level of coarsening, each of which is skipped entirely
when it leaves the block set alone.  A block that gains several levels in one
round upsamples once per level from a single source, so it takes one dependence
however deep it went.

## Placement

This program has one tier and places explicitly; there is no
`OCR_APP_OPTIMIZED_PLACEMENT` guard and no `_hinted` family.

- **blocks are mapped by position**: the rank count is factored into a
  near-cubic `PDx × PDy × PDz` grid and a block at `(x,y,z)` on level `L` is
  mapped through the effective mesh (`npx<<L, npy<<L, npz<<L`) onto it.  The map
  is **level-consistent** — doubling the position and the level's extent
  together leaves the quotient alone — so a block, its parent and its children
  share a rank wherever the rank grid divides the mesh, and a refinement moves
  no payload across the fabric.
- **every datablock a block owns is created by a task placed on its owner**, so
  its home is its owner and its write ownership never leaves that rank for the
  block's whole life.
- **the directory is grouped by rank and ordered by position inside a rank**, so
  a phase tree cuts it into whole-rank ranges and a leaf's blocks share one
  owner.  A block's neighbour is found by computing the owner from the position
  (the map is a closed form) and one binary search in that owner's range.
- **the cross-rank wiring of a phase** is the rank-level nodes: one create plus
  its dependences per child, `min(8, ranks)` children per level.  That is a
  handful of messages per phase, independent of `L` and of `w`.
- **the spine** — the control task chain, each new mesh, and the final of each
  checksum round — sits on rank 0.  These are singletons of the program, not a
  confined parallel phase.

Load balance is the per-axis floor/ceil of the mesh over the rank grid, exact
wherever the grid divides the extents; against a `24 × 12 × 12` base grid the
splits are `1×1×1`, `2×1×1`, `2×2×1` and `2×2×2` at 1 / 2 / 4 / 8 ranks, all
exact.  Under refinement the live population is data-dependent and the balance
is whatever the object's boundary makes it — the same for both tiers, since
both put a block on the domain its position names.

## Sizing

- **Width is the live block count**, and `npx·npy·npz` is the knob.  Unlike the
  base row it is capped by nothing but memory: no task holds a dependence per
  block, and no labeled GUID range has to be reserved, so neither
  `MAX_NUM_UNREFINED_BLOCKS` nor the `36 · (8^(R+1)−1)/7` slot bound applies.
  `--num_refine` is likewise bounded only by the level byte (15) and by memory.
- **Issue frontier**: `G = Σ_r ceil(L_r/w)` leaves emit a phase; `w = 8` puts it
  at `L/8`.  Smaller `w` buys issue parallelism at `2/w` tree tasks per block.
- **`nx·ny·nz` and `num_vars` set grain, not width**: the payload is
  `(nx+2)(ny+2)(nz+2)·num_vars·8` B and a strip `num_vars·(two block dims)·8` B.
  Raising them moves more bytes per task without adding tasks.
- **`num_tsteps × stages_per_ts` sets duration**; both stay at the program's own
  defaults and the window is reached with the size knobs.
- **Memory** is `937 KB` per live block at the campaign grain — the payload
  (552,960 B) plus twelve strips (384,000 B) plus the checksum block — against
  the base row's payload alone.  The strips are the price of the one-task
  stage: they are 69% on top of the payload, and they are what a run's resident
  set shows against the base tier's.  Measured at the 1× lattice
  (`--npx 24 --npy 12 --npz 12 --num_refine 1`), one node of 108 workers,
  INV × WB: **17.1 s and 18.4 GB**, against the base row's 21.7 s and 13.6 GB
  on the same run — the same `Weighted Mesh Checksum`, to the digit.
- **Anchor**: `calibration pending`.  One node of 108 workers, INV × WB, the
  published heights, only the block grid growing:

  | lattice | blocks | e2e | max RSS |
  |---|---|---|---|
  | `24×12×12` (1×) | 3,456 | 17.1 s | 18.4 GB |
  | `48×24×12` (4×) | 13,824 | 88.8 s | 50.8 GB |
  | `48×24×24` (8×) | 27,648 | 184.8 s | 96.4 GB |

  The 120 s window sits between the 4× and 8× lattices, at about `48×24×18`
  (20,736 = 6 × 3456, ~135 s and ~72 GB by interpolation, not yet measured).
  Every candidate is an integer multiple of the 3456 width floor and splits
  exactly over the rank grid at 1..32 nodes.
