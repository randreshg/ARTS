# miniAMR_intel

*Block-structured adaptive mesh refinement as a continuation-cloning coroutine:
every block is a task that re-creates itself as a fresh EDT at each point where
it must wait for a neighbour or its parent.*
Source: `third_party/ocr-apps/apps/miniAMR/refactored/ocr/intel/` (13 `.c` files,
~1.2 MB; `root.c` `block.c` `parent.c` `comm.c` `refine.c` `chksum.c` carry the
structure).

## Overview

miniAMR (Mantevo) sweeps one or more geometric objects through a 3-D mesh; blocks
the objects touch refine 8-way, blocks they leave coarsen back, and between
refinement rounds every block runs a 7- or 27-point stencil over `num_vars`
variables with a 6-face halo exchange. This Intel port expresses each block as a
*suspendable function*: `blockClone_SoupToNuts` runs the whole algorithm as
straight-line C, and at every communication point a macro pair
(`SUSPEND__RESUME_IN_CLONE_EDT` / `SUSPENDABLE_FUNCTION_PROLOGUE`) snapshots a
1 kB software stack into the block's meta datablock and hands the continuation to
a newly created `blockClone` EDT. There is no persistent task per block — there
is a *chain* of thousands of them.

**Refinement is live by default.** With no object in the mesh the refine,
coarsen and parent machinery cannot run at all — `check_objects` returns
`LEAVE_BLK_AT_LVL` for every block forever — so the program supplies the
published one-object specification (`type 2, bounce 0, cen -0.11, move 0.04,
size 1.7, inc 0`, the port's own `Makefile.x86` workload) when the argument
vector names neither `--num_objects` nor `--object`. That spheroid boundary
crosses the mesh from timestep 0 and drifts out of it, so the run is a full
refine-then-coarsen sweep. `--num_objects 0` restores a static mesh, and an
explicit `--num_objects N` with no `--object` gives `N` inert objects — the
caller's stated intent, and the way to get closed-form counts.

**That path is unvalidated.** No recorded campaign has ever exercised refine,
coarsen or the parent machinery on this port, because the arguments made them
unreachable. A single-node smoke at the small argument set must pass before this
row is committed. If it aborts in `chksum.c` — the multi-level halo exchange not
conservative to `10^-error_tol` — the fallback is `--num_objects 0` and the
finding reopens. Raising `--error_tol` is never the answer.

Three stdout scalars, none of them per round:

- `Golden Checksum == <v> (weighted <w>)` (`chksum.c`), printed once for the
  pre-loop round. It is a pure function of `nx·ny·nz`, `npx·npy·npz` and
  `num_vars` (`init.c` fills every interior cell with
  `(2·xPos+i)·100 + (2·yPos+j)·10 + (4·zPos+k)`), so it validates initialization
  and nothing else. It is *not* what the catalog reads. Both reductions share one
  line on purpose: a separate line ending in the voted scalar's own label would
  be matched ahead of it by the driver's substring search.
- `Grand Total Checksum == <v> at timestep <t>` (`root.c`), printed **once** on
  the shutdown path from the last round's aggregate. The per-variable sums are
  **conserved by construction** — refine writes a parent cell as `v · 0.125` into
  each of eight children and coarsen sums the eight back, both exact in binary
  FP — and every round is checked in-run against the golden at
  `10^-error_tol`, so this value is numerically the initialization checksum at
  any tolerance the driver could vote with. It is a **completion marker**, not an
  oracle that sees the workload: a run that did not finish cannot print it.
- `Weighted Mesh Checksum == <v> at timestep <t>` (`root.c`) — **the catalog's
  scalar**. The same O(cells) sweep in `checksum_BlockContribution` accumulates
  each cell value times its linear index within its own block. That reduction is
  *not* conserved when a block is split eight ways or joined back, so its value
  is a function of the mesh state the run actually reached. It is aggregated up
  the same tree and is never compared against the golden. Reduction order is the
  block tree — cell loop order, parent slot order, root dependence order — which
  is identical at every node count and every runtime, so the value is bitwise
  reproducible across the twelve entries.

A configuration that aggregates no checksum round at all (`--checksum_freq 0`,
or too few timesteps to reach one) prints **no** result line, so it fails the
marker instead of voting the record's initial value.

The run-sensitive oracles on this row are therefore the weighted scalar, the
program's own in-run divergence abort, and the completion marker. A divergence
past the tolerance is the program aborting, not a scalar to re-pin.

What the program stresses is task creation and metadata churn far more than
arithmetic: a single interior block issues six EDT clones per stage.

## Parameters

Parsed twice — once in `rootLaunch_Func` (`root.c:100`, only `--num_objects` and
the `--np*` product, to size two datablocks) and once in full in `rootInit_Func`
(`root.c:216`). Everything lands in the `Control_t` datablock, which is copied
per block and reaches every rank as a dependence — so all of it is
multinode-safe. Every value-taking flag goes through one bounds-checked
accessor (`argValue`, `root.c:70`): a flag in the last argv position is reported
and the run refused rather than read past the end of the vector. An unrecognized
flag prints the help text and then executes `*((int *) 123) = 456` — the port's
assertion idiom is a deliberate SIGSEGV, not a clean exit.

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `--nx --ny --nz` | cells per block per axis (must be even, > 0) | 10 | ✓ propagated in `Control_t` |
| `--npx --npy --npz` | base (unrefined) mesh in blocks; their product is the block count and must be ≤ `MAX_NUM_UNREFINED_BLOCKS` | 1 | ✓ **the width knob** |
| `--num_refine` | max refinement levels (0–15); bounded further by the halo range (see Sizing) | 5 | ✓ |
| `--refine_freq` | timesteps between refinement rounds | 5 | ✓ |
| `--block_change` | levels a block may change per round; 0 → set to `num_refine` | 0 | ✓ |
| `--uniform_refine` | refine everything once, then never again | 0 (upstream README says 1) | ⚠ **inert in this port**: parsed and validated, but every read (`refine.c:1884/2005/2090`) is inside the dead `#if 0` at `refine.c:1847-2672`; the live path decides from `check_objects` alone (`refine.c:170`). Not a route to more width |
| `--num_vars` | variables per cell | 40 | ✓ |
| `--comm_vars` | variables exchanged per comm batch; 0 or > `num_vars` → `num_vars` | 0 | ✓ |
| `--num_tsteps` | timesteps | 20 | ✓ |
| `--stages_per_ts` | comm+calc stages per timestep | 20 | ✓ |
| `--checksum_freq` | stages between checksums (0 = none) | 5 | ✓ |
| `--stencil` | 7 or 27 | 7 | ⚠ both implemented (`stencil.c`), but the only implemented `--code` sends faces *without* edges and corners, so the 27-point form reads never-exchanged corner ghosts; `check_input` warns about divergence under non-uniform refinement |
| `--error_tol` | checksum tolerance exponent (`tol = 10^-e`) | 8 | ✓ |
| `--num_objects` | number of refinement-driving objects | **1** | ✓ validated `≥ 0` in both parses; naming it explicitly opts out of the default object |
| `--object t b cx cy cz mx my mz sx sy sz ix iy iz` | one object (14 values); must follow `--num_objects` | the published spec (see Overview) | ✓ |
| `--report_diffusion` | print per-variable checksum diffusion, per round | 0 | ✓ — leave off: it is per-round stdout on the root's critical path |
| `--code` | 0 minimal sends / 1 send ghosts / 2 process on send | 0 | ⚠ only 0 implemented; 1 and 2 reach `"case not yet implemented"` + SIGSEGV |
| `--permute` | rotate comm axis order per stage | off | ✓ (flag, no value) |
| `--refine_ghost` | include ghost cells in the refinement test | off | ✓ (flag) |
| `--plot_freq` | plot every N timesteps (0 = none) | 0 | ✓ — file output, off by default |
| `--report_perf` | perf report level | 4 | ⚠ parsed, stored, never read — `profile.c` is not in the target's SOURCES and every read is behind `//PROFILE:` or `#if 0` |
| `--max_blocks --target_active --target_max --target_min --inbalance --lb_hinted --reorder --init_x/y/z --blocking_send` | present in the reference miniAMR | — | ✗ not parsed here; an unknown flag is a hard error |
| `MAX_NUM_UNREFINED_BLOCKS` | cap on `npx·npy·npz` | 1000 | ✓ **build knob**: `#ifndef` in `root.h:38`, set per target with `EXTRA_DEFINES` in `benchmarks/apps/CMakeLists.txt`. Exceeding it prints advice and SIGSEGVs |
| `SIZEOFSTACK` | per-block continuation stack | 1024 B | ✗ `#define` in `clone.h:45` |

The pre-scan's `--num_objects` default and the full parse's must match — the
pre-scan sizes the `allObjects` datablock that the full parse's count indexes —
and both are `DEFAULT_NUM_OBJECTS` (`root.c:89`). `sizeof_AllObjects_t` is
`sizeof(AllObjects_t) + num_objects·sizeof(Object_t)` in `size_t` arithmetic, so a
negative count would underflow to ~2^64; it is rejected in both parses (the
second via `check_input`). Datablock payload is not zeroed at create, so every
object slot the command line does not describe is written explicitly — with the
default specification when argv named no objects at all, zeroed otherwise.

## Structure

Let `B = npx·npy·npz`, `V = num_vars`, `C = (nx+2)(ny+2)(nz+2)`, `S =
stages_per_ts`, `T = num_tsteps`, `K = ceil(S / checksum_freq)` checksums per
timestep, `f` a block's in-mesh neighbour faces (6 interior, fewer at the mesh
edge) and `E` the number of adjacent block pairs, so `Σ_blocks f = 2E`. Counts
below are for the **unrefined** mesh; with an object in the mesh the live block
population is data-dependent and time-varying (see Sizing).

| object | count | size |
|--------|-------|------|
| `blockLaunch` / `blockInit` EDTs | `B` each | — |
| `blockClone` EDTs | `B·(4 + T·(6S + K))` — 2 per comm axis per stage (an axis with no in-mesh neighbour is skipped), 1 per checksum, plus 4 outside the loop (first clone, two startup checksums, shutdown request) | — |
| `rootInit` + `rootClone` chain | `1 + (4 + 2KT)` — the root serves `2 + KT` checksum rounds and one shutdown round | — |
| `parentInit` + `parentClone` chain | one family per refine event | — |
| `block` DB | one per live block, any level | `8 + C·V·8` B |
| `meta` (`BlockMeta_t`) DB | one per live block | ~1.8 kB (1088 B continuation stack + event tables) |
| `control` / `allObjects` DB | one each per live block, private copies | ~208 B / `40 + 176·num_objects` B |
| `whoAmI` DB | **one per EDT created** (`gasket__ocrEdtCreate`, `util.c:205`) | 8 B |
| `depv` copy DB | one per `rootInit`/`blockLaunch`/`blockInit`, and one per `rootClone` **firing** (`root.c:615`) | `depc·16` B |
| face DB | `f` per block per stage | `40 + comm_vars·(two block dims)·8` B |
| `Checksum_t` DB | `B·(2 + KT)` — two per block before the loop, then one per block per round | `48 + V·8` B (the header, the weighted reduction, and `V` sums) |
| `scratchChecksum` DB | `1 + KT` — one per checksum round after the run's first | `48 + V·8` B |
| sticky events | 1 per block per service request; 36 labeled slots per block per level for halos | — |
| labeled GUID range | `B·(8^(num_refine+1)-1)/7 · 36` reserved once (`root.c`) | key space only, and **bounded**: see Sizing |

Two habits set the recurring cost. `gasket__ocrEdtCreate` gives every EDT an
8-byte `whoAmI` block, so `NUM_DB_CREATE` carries the whole EDT count; and
`rootClone_Func` copies its `depv` into a fresh datablock on **every** firing.
A steady-state checksum round therefore costs *two* root firings, not one:
`checksum_RootFinalAggregation` allocates a `scratchChecksum` and suspends
before aggregating on every round except the run's first, so the round is split
across a clone boundary. A level-0 block also contributes two checksums before
the timestep loop — `init()` emits one and `blockClone_SoupToNuts` emits another
— so the root sees `2 + KT` rounds, of which only the first is "golden".

Events come from three sources. Explicit `ocrEventCreate`: one
`conveyServiceRequestToParent` per base block at startup, one fresh one per
checksum/plot/unrefine service a block requests, one fresh outgoing halo event
per face sent, 16 per refine fork (8 `conveyEighthBlockToJoin` + 8 new
service-request events), and the labeled halo events materialized on first use
through `ocrGuidFromIndex` + `GUID_PROP_IS_LABELED|GUID_PROP_CHECK` — each
directed channel is created by both endpoints, the loser installing nothing but
still counting as a create. Output events: **none** — every
`gasket__ocrEdtCreate` passes `NULL` for `outputEvent`. Finish EDTs: **none** —
every create uses `EDT_PROP_NONE`.

With `--num_objects 0` (static mesh) every counter is a closed form:

    NUM_EDT_CREATE   = 5 + 6B + T·(B·(6S + K) + 2K)
    NUM_DB_CREATE    = NUM_EDT_CREATE + 10 + 9B + T·(B·K + 2E·S + 3K)
    NUM_EVENT_CREATE = 3B + 4E + T·(B·K + 2E·S)

Counter cross-check: measured at 1 node with `--nx 4 --ny 4 --nz 4 --npx 2
--npy 2 --npz 2 --num_vars 4 --stages_per_ts 2 --checksum_freq 1 --num_refine 0
--num_objects 0` (`B = 8`, `S = 2`, `K = 2`, `E = 12`, `f = 3`), the three
formulas above give 169/321/136 at `T = 1` and 285/507/200 at `T = 2`; the
runtime reports 171/322/136 and 287/508/200, which is exact once the shim's own
bootstrap (1 `main_edt` + 1 trampoline EDT and 1 argv datablock) is subtracted.
The per-timestep deltas 116 EDT / 186 DB / 64 event match term for term. (That
cross-check predates the refinement default and was taken with objects off,
which is the configuration the closed form describes.)

With refinement live the same terms hold per *live* block, plus one
`parentInit`/`parentClone` family and 16 events per refine fork; the population
itself is the thing that is not closed-form.

## Wiring

The mesh is a two-level service hierarchy. `rootClone` holds one dependence slot
per base block (`serviceRequest_Dep[B]`, RO) plus `control` (RW),
`goldenChecksum` (RO) and `scratchChecksum` (RW); a block asks for a service by
satisfying its `conveyServiceRequestToParent` sticky event with a
`DbCommHeader_t`-prefixed datablock, and every request carries the *next*
("on-deck") event in its header so the root can rewire its successor clone. All
`B` slots must arrive before the root advances — the checksum is a hard barrier
across the whole mesh, and the root asserts that all `B` opcodes match.

When a block refines it forks into a `parentInit`/`parentClone` family plus eight
`blockClone` prongs; the parent then plays the same role for its eight children
that the root plays for base blocks. Unrefinement joins through the eight
`conveyEighthBlockToJoin` sticky events, with prong 000's clone becoming the
merged block.

Halo exchange never goes through a parent. Each face is a fresh `Face_t`
datablock satisfied into a **labeled sticky event** whose index encodes
(refinement level, linearized block position, one of 36 direction/quarter slots),
so a sender computes its neighbour's inbox GUID arithmetically without ever
learning the neighbour's identity. A face DB has exactly one producer and one
consumer and is destroyed after unpacking — no sharing.

Access modes: `block`, `meta`, `allObjects`, `control`(root's copy),
`scratchChecksum` are RW and single-owner; a *predecessor* block/control/meta
handed to eight fork prongs is RO with **eight simultaneous readers** — the only
real fan-out in the program, and the widest at the moment a refinement wave
passes. `control` is RO on the block side after `blockInit` copies it, so no DB
takes RW from more than one block's chain. The contention point is the root's
`B`-way checksum rendezvous, not any single datablock.

The parent's eight-slot array carries **two roles**, and its acquire mode is
therefore decided per slot rather than per branch. In every round the slots hold
the children's request datablocks, which the parent only reads (RO). In the
unrefinement-consensus round the parent destroys those requests and puts eight
datablocks *of its own making* into the same slots, for its next continuation to
fill in with the consensus and satisfy back to the children — those slots are RW.
`ParentMeta_t.cloneWritesSlot` is the one-bit-per-slot record of which role a slot
plays, written where the reply datablock is created and read at the wiring site.
Getting this wrong is not a local matter: a datablock initialised through a
read-only acquire keeps its writes only while the writing task happens to run
where the datablock's payload already is, so the same program is correct on one
node and silently drops the consensus once continuations are free to move between
them (fixed 2026-09-04; it is what made the base tier fault at two ranks while the
hinted tier, whose map pins a parent's whole chain, passed).

## Flow

`mainEdt` → `rootLaunch` → `rootInit`, which creates the `B` `blockLaunch` EDTs
and the first `rootClone`; each `blockLaunch` → `blockInit` → the first
`blockClone`. Then every block independently runs: init (which emits the golden
checksum) → a second, redundant checksum from the driver itself →
initial refine → `for ts in 1..T { for stage in 1..S { comm(); stencil
per variable; checksum every checksum_freq stages } ; move(); refine every
refine_freq timesteps } `. The shutdown is a genuine all-blocks join: each block
sends `Operation_Shutdown` up its service event and the root prints the final
checksum and calls `ocrShutdown()` only after all `B` slots agree.

Parallel width is the live block count — `B` at level 0, growing by 7 live blocks
per block that refines and shrinking by 7 per octet that coarsens. Within a
stage, a block's three axes are serialized (axis 0 completes before axis 1
starts) because each is a suspension point, so per-block latency is
`3 · 2 · (clone + halo round-trip)` per stage. The serial bottlenecks are (a)
every checksum round, an all-blocks → root join walked on one EDT and a fresh
root clone; (b) every refine round, a parent-mediated neighbour-consensus
protocol (`refine.c` alone has 11 suspension points) that must converge before
any block proceeds; (c) `rootInit`'s single-EDT loop creating all `B` block
launchers.

## Placement (base)

Every `gasket__ocrEdtCreate` passes its hint through `amrEdtHintForBlock`
(`util.c:181`) / `amrEdtHintForPD` (`util.c:137`), whose `#else` branch — the
base build — returns `NULL_HINT`. The hinted flavour maps a block's
(x,y,z,level) onto a 3-D policy-domain grid; that layer is out of scope here.
Datablocks are always created with `NULL_HINT` in both flavours (`util.c`'s
`ocrDbCreate` call).

Effective base policy: **EDT → runtime round-robin**, **DB → home = creating
rank**. Consequences at multinode:

- A base block's four datablocks are homed wherever its `blockLaunch` happened to
  land, but its `blockInit` and its whole continuation chain are round-robined
  independently — so a block's *own* `block`/`meta` datablocks are remote to it
  roughly `(nodes−1)/nodes` of the time, and they migrate on nearly every clone.
  The block payload is the largest object in the program; this is the dominant
  traffic, and it is self-inflicted rather than algorithmic.
- Halo events live in a `GUID_USER_EVENT_STICKY` range reserved with ARTS's
  round-robin distribution, so the inbox for a given face is homed at
  `index % nranks` — unrelated to where either the sender or the receiver runs.
  The face payload itself still travels sender → receiver; only the control edge
  is third-party.
- The root's `B`-slot checksum join draws one datablock from every block on every
  round, from wherever that block last ran.

The algorithm has textbook spatial locality (a 3-D neighbour graph, refinement
strictly within a parent's octant) and the base program expresses none of it.

## Placement (hinted)

The layer (`amrBlockHomePD` at `util.c:95` / `amrEdtHintForPD` at `util.c:137`)
places a block's EDTs by the block's spatial position: the rank count is factored
into a near-cubic `PDx × PDy × PDz` grid, a block at (x,y,z) on refinement level
L is mapped through the effective mesh (`npx<<L, npy<<L, npz<<L`) onto that grid,
and the block's EDTs go there. The map is level-consistent —
`(2x)·PDx/(2·ex) = x·PDx/ex` — so a block keeps one home across its whole chain
and across refinement levels, and parents, children and siblings co-locate.
The root spine (`rootInit`, `rootClone`) is deliberately left **unhinted in both
tiers**, so it round-robins rather than becoming a permanent gather sink.

EDT affinity only, and that is measured rather than assumed: homing each refined
child's datablocks on the domain its own tasks run at changed nothing at 1, 2, 4
or 8 nodes. With the EDT layer on, `blockLaunch` already runs at the block's
domain and an unhinted `ocrDbCreate` homes at the creating rank, so the explicit
DB hint is redundant by construction.

Load balance is exact wherever the policy-domain grid divides the mesh. Against
a `24 × 24 × 12` base grid:

| ranks | PD grid | per-rank box | blocks/rank | `B`/ranks | imbalance |
|---|---|---|---|---|---|
| 2 | 2×1×1 | 12×24×12 | 3456 | 3456 | 1.00 |
| 4 | 2×2×1 | 12×12×12 | 1728 | 1728 | 1.00 |
| 8 | 2×2×2 | 12×12×6 | 864 | 864 | 1.00 |
| 16 | 4×2×2 | 6×12×6 | 432 | 432 | 1.00 |
| 32 | 4×4×2 | 6×6×6 | 216 | 216 | 1.00 |

**Building the hint is on the hot path, and it showed.** This program creates
tens of millions of EDTs; resolving the domain affinity and rebuilding an
`ocrHint_t` at each of them cost more than the placement saved — as first written
the layer *lost* to the tier it was supposed to improve. Caching one hint per
domain, caching the domain factorization, and handing back the cached hint by
reference is what makes it affordable; what remains at one node is the cost of
offering a hint at all, where a single domain has nothing to gain from one.

Those caches are shared by every worker thread, so they are published rather than
merely written: the domain count and the three grid factors are single atomic
words (the factors packed into one, so no reader can see a half-written triple),
and each hint slot is claimed by one filler with a CAS and published with a
release store — a reader that finds a slot unpublished builds into its own hint
instead of reading a half-built one. `ocrAffinityGetAt` leaves its output
unwritten for an out-of-range index, so its return is checked and the layer
degrades to `NULL_HINT`. `amr_factor3` factors the rank count against a cube
rather than against `(npx,npy,npz)`, which would matter for a lopsided mesh; at
the geometries above every factor divides its extent.

**Known cliff:** the hint cache holds `AMR_MAX_CACHED_PD = 256` domains
(`util.c:145`). Above 256 ranks the layer silently reverts to the per-create
rebuild, which is the ~50%-slower path the cache exists to remove. Not reachable
at 32 nodes.

**Residual remote edges, in both tiers and outside the layer's remit:** the
`B`-way checksum join to the unhinted root every round, and the halo *event*
rendezvous homed at `index % nranks` — OCR exposes no per-index affinity on a
labeled range, so changing it would be a shim or runtime change.

Scaling numbers: `calibration pending`. Every figure previously recorded here was
taken on a retired host, at 512 blocks over at most 8 nodes, with a static mesh
and a timestep count 1.5× the program's default — none of which is the
configuration this row now runs.

## Sizing

- **Width is the live block count.** Each block is a single continuation chain:
  every suspension point creates exactly one successor into
  `meta->blockClone_Edt`, so a block contributes at most one runnable EDT at a
  time. `W(0) = npx·npy·npz`; a refine round adds 7 live blocks per refining
  block, a coarsen round removes 7 per octet, and `W` collapses toward 1 at every
  checksum round (an all-blocks join walked on one EDT, twice per round after the
  first) and at every refine-consensus round. Total EDT count is **not** a
  coverage argument — the frontier is.
- **`npx·npy·npz` is the width knob**, capped by `MAX_NUM_UNREFINED_BLOCKS`,
  which is a build define set per target (`EXTRA_DEFINES` in
  `benchmarks/apps/CMakeLists.txt`); its only cost is `RootMeta_t.dbSize[]`, one
  `int` per dependence slot. Do not rely on refinement to reach a width floor:
  the multiplier is data-dependent. `24 × 24 × 12 = 6912` divides exactly by
  every policy-domain grid the hinted map produces over 1..32 ranks.
- **`--num_refine` is bounded at both ends: `1 ≤ R ≤ 4` at 6912 blocks.**
  At the bottom, `R = 0` makes refinement inert again — `check_objects`
  downgrades a `REFINE_BLK` decision to `LEAVE_BLK_AT_LVL` as soon as a block is
  at `num_refine` (`move.c:120`) — so `R = 0` is a static mesh and silently
  undoes the default object. At the top, the halo GUID reservation is
  `npx·npy·npz · 36 · (8^(num_refine+1)-1)/7`, and the count is carried in 32
  bits, where an over-large request truncates instead of failing — two faces
  would then share an inbox. The program refuses it loudly (`root.c`).
  Reachable pairs (`floor((2^32-1) / (36·(8^(R+1)-1)/7))`):

  | num_refine | slots per block | max `npx·npy·npz` |
  |---|---|---|
  | 0 | 36 | 119,304,647 |
  | 1 | 324 | 13,256,071 |
  | 2 | 2,628 | 1,634,310 |
  | 3 | 21,060 | 203,939 |
  | 4 | 168,516 | 25,487 |
  | 5 | 1,348,164 | 3,185 |

  So at 6912 blocks the published default `--num_refine 5` is not reachable
  (9,318,509,568 slots) and the argument list must name `1 ≤ R ≤ 4`.
- **`nx·ny·nz` and `num_vars` set grain**: block payload is
  `8 + (nx+2)(ny+2)(nz+2)·num_vars·8` B and face payload
  `40 + comm_vars·(dim1)(dim2)·8` B. Raising them makes each clone move more
  bytes without adding tasks — the lever for shifting the program from
  task-churn-bound to bandwidth-bound. They are not a width knob: EDT creations
  are identical at 16 and at 6 cells a block.
- **`num_tsteps × stages_per_ts` sets duration**: clones scale as
  `6·stages_per_ts + K` per block per timestep. Both stay at the program's own
  defaults; reach the window with the size knobs.
- **Memory.** `live blocks × block payload` is the floor; the observed
  level-0-payload-to-peak-RSS multiplier on this program is ≈8× (the allocator
  holding `whoAmI`/`depv`/face churn while the destroy path drains). At
  `nx=ny=nz=10, num_vars=40` the block payload is 552,968 B, so a 190 GB one-node
  budget allows on the order of 43,000 live blocks. `B = 6912` is 3.8 GB of
  level-0 payload; how many refinement levels fit inside the remainder depends on
  how many blocks the object's boundary touches and is `calibration pending` —
  measure RSS at one node before committing `--num_refine`.

Anchor time and per-cell RSS: `calibration pending`. The previously recorded
figures belong to a static mesh at 512 blocks with `--num_tsteps` above the
program's default, and do not transfer. Note when re-taking them that this
program is unusually sensitive to instrumentation — a campaign counterset cost it
~31% in the record — so calibrate on a counter-free tree.
