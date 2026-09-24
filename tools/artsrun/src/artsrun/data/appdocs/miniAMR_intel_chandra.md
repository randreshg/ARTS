# miniAMR_intel_chandra

*The same AMR proxy as an SPMD program: an `npx × npy × npz` grid of "ranks" is
forked onto the policy domains, and the entire potential octree beneath each rank
— every block at every refinement level — is materialized up front, then
activated or deactivated as refinement moves.*
Source: `third_party/ocr-apps/apps/miniAMR/refactored/ocr/intel-chandra/`
(15 `.c` files; `main.c` `init.c` `driver.c` `refine.c` `comm*.c` carry the
structure), linked against `ocrAppUtils`, `reduction` and `timer`.

## Overview

`mainEdt` parses the command line, packs it into one `globalParamH_t`
datablock, and forks `npx·npy·npz` SPMD `initEdt`s over a 3-D policy-domain grid.
Each `initEdt` walks levels `0 … num_refine` and, for every octree node it would
ever own, spawns one `blockSetupEdt` on its own policy domain. That task creates
the node's `rankH_t` handle datablock and a `channelSetupEdt` that rendezvouses
with the node's 6 face neighbours + parent + 8 children + 8 siblings through
labeled sticky events, and (inside that setup) the node's cell array. Only
level-0 nodes then start the driver; deeper nodes sit pre-wired and
pre-allocated, waiting to be switched on by a refinement.

The timestep DAG is a chain of small loop-driver EDTs per active block —
`timestepLoop → stageLoop → stage → varsLoop → vars → comm | calcLoop → calc` —
with a `checkSum` chain folded in on checksum timesteps and a `refineLoop` on
refinement timesteps. Cross-block coupling is a `reduction`-library tree over the
octree (block counts, refinement intent, checksums, coarsening consensus) plus
double-buffered halo events. `wrapUpEdt` prints `Shutting down`, calls
`ocrShutdown()` and prints `Done`.

**Initialization and oracle.** Every interior cell is filled from
`cellValueFromGlobalIndex` (`init.c`), a splitmix64 mixing of the cell's global
`(level, var, i, j, k)` index into a double in `[0,1)`. It is a pure function of
that index, so the field is identical at every node count, in every runtime and
on every run, with no shared generator state — and, unlike the constant field the
program's old verification mode produced, it is not a fixed point of the
stencil, so a wrong ghost value changes the answer. The 7-point average with the
mirror boundary condition conserves the grid sum exactly (each cell's value
enters the new sums with total weight 7: once as itself, once per interior
neighbour, once per boundary face), so the printed checksum equals the initial
sum to rounding and any halo, coherence or placement defect breaks it. The
program's own comparison against the previous checksum now feeds an exit verdict:
`VERIFICATION PASSED: <n> checksum comparisons within tolerance` or
`VERIFICATION FAILED: …`, printed once by the sequential-rank-0 block before
shutdown, and `CHECKSUM DIVERGENCE ts <t> variable <v> sum <s> old <o>` per
divergent comparison. The catalog's marker is that PASSED line alone, so a
diverging cell fails rather than passing quietly. It cannot also require the
`Done` line: the verdict is printed by the rank owning the sequential-rank-0
block and `Done` by an unhinted task the runtime places anywhere, so their order
in one merged log is a buffering artifact. Shutdown recognition is proven
independently by the runtime's `[E2E]` stamp, which the driver already requires
of a reaped run.

What the program stresses is fine-grain task chaining (hundreds of tiny loop EDTs
per block per timestep) plus a deep reduction tree — the cell arithmetic is a
small fraction of it.

## Parameters

`parseCommandLine` (`main.c`) fills a `Command` struct from `param.h` defaults,
`mainEdt` copies it into `globalParamH_t`, and every SPMD EDT receives that
datablock as a dependence — the whole surface is multinode-safe. An unrecognised
flag is loud: it prints `** Error ** Unknown input parameter …` and the help
text; `check_input` (`main.c`) validates the surface and prints one line per
violation. Every such error, `--help` included, returns from `parseCommandLine`
with a bail flag and `mainEdt` then calls `ocrShutdown()` and returns without
forking the rank grid, so the whole world ends instead of only the process that
read the arguments. The run produces no completion marker, which is what the
driver judges.

The published defaults disagree between sources for three flags; the value the
binary uses is always `param.h`, because `initCommand` includes it.

| flag | meaning | default (binary / other published) | CLI reachability |
|------|---------|---------|-------------------|
| `--npx --npy --npz` | **SPMD rank grid**; `npx·npy·npz` is the number of forked tasks and the unit of NODE distribution | 1 | ✓ |
| `--init_x --init_y --init_z` | coarse blocks per rank per axis — **the width knob** | 1 | ✓ |
| `--nx --ny --nz` | cells per block per axis (even, > 0) | 10 | ✓ |
| `--num_vars` | variables per cell | 40 | ✓ (bounded by `MAX_NUM_VARS`, checked) |
| `--comm_vars` | variables per comm batch; normalised after validation — 0 or > `num_vars` → `num_vars` | 0 | ✓ (must be ≥ 0, checked; the upper end is clamped, not rejected) |
| `--num_refine` | refinement levels — **also the eager allocation depth** | 5 | ✓ (bounded by `MAX_REFINE_LEVELS`, checked) |
| `--num_tsteps` | timesteps | 50 (`param.h`) / 20 (README) / 100 (`Makefile.x86-base`) | ✓ (must be ≥ 1, checked) |
| `--stages_per_ts` | stages per timestep | 20 | ✓ (must be ≥ 1, checked) |
| `--checksum_freq` | checksum every N *timesteps* (`driver.c` uses `ts % checksum_freq`, not the reference's `istage %`); 0 disables | 5 | ✓ |
| `--refine_freq` | timesteps between refinement rounds | 5 | ✓ (must be ≥ 1, checked) |
| `--report_diffusion` | **gates the checksum printout** (`FNC_print` in `driver.c`) | 0 | ✓ — without it the program prints no per-timestep number, only the verification verdict |
| `--num_objects` / `--object …` | refinement-driving objects; `--num_objects` must precede every `--object` (loud error otherwise) | 0 | ✓ (bounded by `MAX_OBJECTS` where it is parsed, before any `--object` can write the array) — with 0 objects `check_block` never intersects, so **no block ever refines** |
| `--error_tol` | checksum tolerance exponent, `tol = 10^-error_tol` | 8 | ✓ |
| `--stencil` | 7 or 27 | 7 | ✓ |
| `--uniform_refine` | refine every block to `num_refine` regardless of objects | 0 (`param.h`) / 1 (README) | ✓ |
| `--max_blocks` | ceiling on `init_x·init_y·init_z`; violation is a loud `max_num_blocks not large enough` | 500 | ✓ — pass it explicitly so the ceiling is visible |
| `--lb_opt` | load balancing (0 none / 1 each refine / 31, 33) | 0 (`param.h`) / 1 (README) | ⚠ parsed; `FNC_loadbalance`/`FNC_redistributeblocks` exist but 0 never reaches them |
| `--block_change --code --permute --refine_ghost --plot_freq --report_perf --blocking_send --target_* --inbalance --reorder` | reference knobs | `param.h` | ✓ parsed; `code`, `refine_ghost`, `stencil` are honoured, the rest are stored and mostly unread |
| `CHANNEL_EVENTS_AT_RECEIVER` | which side creates (and therefore homes) every halo channel event | **defined** by the ARTS build, as in `Makefile.x86-base` | ✗ compile-time |
| `OCR_APP_COUNTED_OEVT` | output events are app-supplied COUNTED events | **defined** by the ARTS build (local adaptation, memory conformance) | ✗ compile-time |
| `USE_STATIC_SCHEDULER` | use the static-scheduler fork | not defined | ✗ compile-time |
| `USE_LAZY_DB_HINT` | tag payload DBs with `OCR_HINT_DB_LAZY` | not defined | ✗ compile-time |
| `SHUTDOWN_LAG` / `PRINTBLOCKSTATS` | `sleep()` before shutdown / block-stat printouts | not defined (`sleep` would sit inside the `[E2E]` window) | ✗ compile-time |
| `MAX_OBJECTS` / `MAX_REFINE_LEVELS` / `MAX_NUM_VARS` | 10 / 8 / 40 | — | ✗ `param.h` |

## Structure

Let `P = npx·npy·npz` (SPMD ranks), `Q = init_x·init_y·init_z` (coarse blocks per
rank), `R = num_refine`, `V = num_vars`, `C = (nx+2)(ny+2)(nz+2)`, `S =
stages_per_ts`, `T = num_tsteps`. Octree nodes materialized at startup:
`N = P·Q·Σ_{l=0..R} 8^l`; at `R = 0`, `N = P·Q`.

| object | count | size |
|--------|-------|------|
| `initEdt` | `P` | — |
| `blockSetupEdt` | `N` (one per octree node, all levels) | — |
| `channelSetupEdt` | `N` | — |
| `rankH_t` DB | `N` | ~40 kB (block handle + shared-object GUID tables + timers) |
| cell array DB (`DBK_array`) | `N` | `V·C·8` B |
| work DB (`DBK_work`) | `N` | `C·8` B |
| peer halo send buffers | `N × 6 × 2` phases | `comm_vars·msg_len·8` B each (`msg_len` from `init.c`) |
| coarser/finer halo send buffers | `N × 6 × {coar, 4×refn} × 2` — **allocated only when `num_refine > 0`** | `comm_vars·msg_len·8` B each |
| refinement-intent buffers | `N × (6 × {curr, coar, 4×refn} × 2 + 8 siblings × 2)` — **allocated only when `num_refine > 0`** | 4 B each |
| reduction scratch DBs | `N × (1 + 3·MAX_REDUCTION_HANDLES)` — `octTreeRedH`, then `in`/`out`/`redRootH` per handle (`MAX_REDUCTION_HANDLES` = 26) | small |
| handshake copies | `N × 6` — a `sharedOcrObj_t` published into each outgoing setup event | ~33 kB |
| labeled sticky events | `6·(P·Q·8^l)` reserved per level `l` | key space only |
| per-timestep EDTs per active block | `1 + S·(16 + 2V)` at `comm_vars ≥ num_vars` | — |
| per-timestep events per active block | `4 + S·(30 + 4V)` | — |
| checksum EDTs | `2V` per active block on a checksum timestep (`checkSum` + `print` per variable) + a reduction tree per variable | — |

Datablocks per octree node: **101 at `num_refine 0`** (249 when refinement is
compiled into the run — the extra 148 are the coarser/finer halo buffers and the
refinement-intent buffers, which no code path can name at a single level: `comm.c`
selects them on `nei_level[i] != level`, and `comm_refine.c`/`comm_parent.c` are
reached only from `FNC_refineLoop`, which `FNC_driver` and `FNC_timestepLoop`
create only when `num_refine` is non-zero).

The timestep chain is exact. Per stage a block runs `stageLoop`, `stage`,
`varsLoop`, `vars`, then `V` × (`calcLoop` + `calc`) — and a **comm sub-chain of
12 EDTs**: `FNC_comm` walks the three axes as a linear chain of three instances,
and each instance creates one `commHaloNbrsEdt`, which in turn creates one
`packHalosEdt` and one `unpackHalosEdt`. The exchange is **6-face**
(`int nNbrs = 6`, `comm.c` walks three axes × two faces); the parent, 8-children
and 8-sibling edges are refinement wiring, dead at `num_refine 0`. The axis count
is fixed at 3 whatever the neighbour topology. Adding the one `timestepLoop` per
timestep gives `1 + S·(16 + 2V)`.

Events come from three sources and all three are on the steady-state path.
Every `EDT_PROP_FINISH` create that also takes an output event contributes
**two** (its finish event and the materialized output event); every such site is
immediately followed by a `createEventHelper` counted event that the successor
waits on, so the loop drivers cost **three** events per link. Per stage:
`stage`, `varsLoop`, `vars` 3 each, `vars` again for the `calcLoop` link 3, each
of the `V` `calcLoop`s 4 (its `calc` link plus a continuation event), each of the
3 `FNC_comm` instances 3 for its `commHaloNbrsEdt`, and each `commHaloNbrsEdt` 2
for `packHalosEdt` (output event only — that one is `EDT_PROP_NONE`). Setup adds
the rendezvous: per octree node, 4 channel events per reduction handle, 53
double-buffered halo channels, 7 per neighbour direction and 17 for
parent/children/siblings — 216, level-independent, since a slot that has no
peer at this level gets a pre-satisfied counted event instead of a labeled one.
Datablocks are allocated only at setup: nothing on the timestep path.

**A checksum timestep is a global barrier.** `checkSumEDT` is
`EDT_PROP_FINISH`, and inside its scope `printEDT` waits on the reduction's
downward channel event — the broadcast of the all-reduce over every active block.
The next variable's `calcLoop` waits on that finish scope, so at `istage 0` of
every checksum timestep the whole mesh rendezvouses once per variable:
`V·⌈T/checksum_freq⌉` global barriers over `S·T` stages. This is the row's
dominant scaling term and the reason `--checksum_freq` is held at its published
default rather than raised.

## Wiring

Setup is a rendezvous, not a tree. Each octree node computes the labeled GUIDs of
its 6 face neighbours (at its own level, at the coarser level, and the 4 finer
sub-faces), its parent, its 8 children and its 8 siblings — indices into
`haloRangeGUID[level]`, derived arithmetically from the block's global (i,j,k) —
creates each event with `GUID_PROP_CHECK` — whichever create reaches the
label's home first installs it and the others park behind it for the rest of
the run — satisfies its own `sharedOcrObj_t` handle into it, and makes its
`channelSetupEdt` depend on all `6·nNbrs + 17` of them. Because the meeting point
is the labeled GUID and not the order in which blocks were built, the blocks of
one rank are independent of one another: `initEdt` spawns one `blockSetupEdt` per
block on its own policy domain and they run concurrently. When a
`channelSetupEdt` fires, its node holds its peers' event GUIDs and datablock keys;
`init()` then allocates and fills the cell array and the level-0 nodes start the
driver.

Steady state: `FNC_comm` packs the outgoing faces into the current phase's send
buffers and satisfies the neighbour's halo event; `unpackHalos` writes into the
receiving block's array; a face on the domain boundary is filled by `apply_bc`
(mirror). Buffers are double-buffered per phase, so a block never overwrites a
buffer a neighbour has not yet consumed. Access modes: `DBK_rankH` is RW on every
loop EDT of its own block's chain (a strictly serial baton — the chain is linear
by construction), `DBK_octTreeRedH` is RW on the reduction path, send buffers are
RW on the sender and RW on the receiver as well (`comm.c`, `comm_refine.c` take
them `DB_MODE_RW` in `unpackHalos`), so a buffer's home only decides which end
pays the fetch. The genuinely shared objects are the
reduction handles: `FNC_createChildBlocks` takes the parent's `rankH` **and all
eight children's** RW simultaneously, which is the widest RW fan-in in the program
and the point where a refinement serializes an octree family.

`--num_objects 0` (the default and the catalog's setting) with `--num_refine 0`
means `check_block` never intersects, `bp->refine` is never set to `REFINE`, and
none of that machinery ever runs: `refine.c`, `move.c`, `comm_refine.c`,
`comm_parent.c`, `load_balance.c` and the child-block machinery in `block.c` are
dead in every campaign cell. What runs is a uniform block mesh doing a 7-point
stencil with a 6-face halo exchange and an arity-10 all-reduce.

## Flow

`mainEdt` (rank 0, serial: parse + `initGlobalOcrParamH`'s
`MAX_REDUCTION_HANDLES + num_refine + 2` GUID-range reservations) → `P` `initEdt`s
in one loop → each spawns its `N/P` `blockSetupEdt`s, which run concurrently on
that rank's workers → a global rendezvous as all `N` `channelSetupEdt`s complete
→ an init-checksum reduction → `FNC_miniamrMain` → `FNC_driver` →
`FNC_timestepLoop`.

Parallel width is the number of **active** blocks: `P·Q` at level 0, ×8 per
level actually refined. Within a block everything is serial — the stage loop, the
vars loop and the calc loop are linear EDT chains, one variable at a time, so a
block's timestep is a chain of ~1900 tasks with no internal parallelism. The
machine is therefore kept busy by block count, never by per-block width, and
never by the rank count.

Serial or global terms: `mainEdt` itself; each `initEdt`'s `N/P` spawn loop (now
one EDT create per block rather than the block's ~590 object creations); every
checksum timestep, which is `V` global rendezvouses; and every refinement round,
which is an intent reduction plus a parent-and-eight-children RW join.

## Placement (base)

The placement below *is* the base program, and every object placement in it
is explicit and right.  The one decision it leaves to a near-cube heuristic —
which factorisation of the node count the rank grid is cut into — is what the
hinted tier re-chooses (see Placement (hinted)).

- `forkSpmdEdts_Cart3D` (`ocrAppUtils.c`) splits the policy domains into a 3-D
  grid via `splitDimension_Cart3D`, partitions the `npx × npy × npz` rank grid
  onto it contiguously with `getPartitionID`, and sets `OCR_HINT_EDT_AFFINITY`
  per `initEdt`. This is an index/ownership-range map: for the `4×4×2` rank grid
  the PD grids at 1/2/4/8/16/32 nodes are `1×1×1`, `2×1×1`, `2×2×1`, `2×2×2`,
  `4×2×2`, `4×4×2`, so every domain receives exactly 32/16/8/4/2/1 ranks —
  **load imbalance exactly 1.00 at every node count in the sweep, by
  construction**.
- Inside a rank, `getAffinityHintsForDBandEdt` (`ocrAppUtils.c`) captures
  `ocrAffinityGetCurrent()` into `rankH->myEdtAffinityHNT` /
  `myDbkAffinityHNT` / `myDbkAffinityHNT_lazyDB`, and every subsequent create —
  every loop EDT, every handle and cell and halo datablock, every child block on
  refinement — carries it. This is *not* creator-pinning in the forbidden sense:
  the creator is the per-rank `initEdt` (and the `blockSetupEdt` it places on the
  same domain), which the index map already spread, so the recursion reproduces
  the map instead of funnelling onto rank 0. The only NULL-hint creates are the
  shutdown EDT and the two argv datablocks in `mainEdt`.
- The `reduction` library inherits the same placement: `reductionLaunch` captures
  `ocrAffinityGetCurrent()` and every reduction EDT uses it.
- Consequence: a block's data is always local to the tasks that touch it, and the
  only cross-node traffic is halo events and reduction edges on partition
  boundaries — exactly the MPI-like pattern the port is imitating.
- The one placement the app does not control: the labeled halo/reduction GUID
  ranges are reserved round-robin by the shim, so the *event* for a boundary face
  or a reduction rendezvous is homed at `index % nranks` rather than at either
  endpoint. No OCR hint can move it; it is a runtime question, not an application
  one.

**The rank grid is the node partition, not the width.** With the defaults
(`--npx 1 --npy 1 --npz 1`) there is a single SPMD rank and every EDT and
datablock is pinned to one node. Mirror the rank grid to the largest node count
and buy width with `init_x·init_y·init_z`.

## Placement (hinted)

Audited 2026-09-08 from source (every create on the step path carries the
block's own affinity — `stage`/`stageLoop`/`varsLoop`/`vars`, the 3-axis comm
sub-chain, `calcLoop`/`calc`, the checksum and reduction EDTs; events take no
hint and are homed where created; no DB is created per stage; the reduction
library's `NULL_HINT` transients are creator-homed and unreachable from the
app since `reduction.c` is an OBJECT library).  What the structural argument
never checked is the one placement decision `mainEdt` makes: which
factorisation of the node count the rank grid is cut into.
`splitDimension_Cart3D` derives it from the node count alone and returns a
near-cube; the mesh it partitions is a slab (`24 × 24 × 6` blocks at the
campaign arguments), so a near-cube draws its cut planes through the mesh's
largest faces — a z-cut costs 1152 directed halo faces per stage against 288
for an x or y cut.  All six peer halo messages are the same size (`n²` at
cubic blocks), so counting faces is counting bytes.  Enumerated over every
factorisation that fits (`pdgrid.py` in the audit unit):

| nodes | 1 | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|---|
| campaign `4×4×2` ranks × `6×6×3` blocks, base | 1×1×1 | 2×1×1 | 2×2×1 | 2×2×2 | 4×2×2 | 4×4×2 |
| minimum-crossing | same | same | same | **2×4×1** | **4×4×1** | same (only fit) |
| directed faces per stage | 0 | 288 | 576 | **1728 → 1152 (−33 %)** | **2304 → 1728 (−25 %)** | 2880 |

Every improved candidate keeps imbalance 1.00.  On the trend grid (`2×2×2`
ranks × `5×3×2`) the base is minimum-crossing at 1/2/4/8 — at 8 nodes `2×2×2`
is the only factorisation that fits — so the local anti-scaling
(7.9/7.4/7.2/8.3 s) is not attributable to the map.  The other candidate
freedom, homing the halo send buffers at the consumer, is real but useless:
the consumer takes them RW (`comm.c`, `comm_refine.c`), so the home only
decides which end pays the fetch, and the identical construction on hpcg
measured 1.1–1.5× slower.  The three anti-scaling terms (per-variable
all-reduce barriers, the serial block chain, the 3-axis comm serialisation)
and the arity-10 reduction heap (87 % cross-node under every map) are
placement-invariant, as the structural argument said.

**The layer** (`haloSurfacePdGrid_Cart3D` in `util.c` under
`OCR_APP_OPTIMIZED_PLACEMENT`; `mainEdt` forks through
`forkSpmdEdts_onPdGrid_Cart3D`): every factorisation is scored by the mesh
area its cut planes cross, the near-cube is the incumbent and is replaced only
on a strict improvement at no worse balance.  Hint values only.

**Measured** (INV × WB, 15w+1p × 1/2/4/8, two repeats, scalars identical):

| rank grid | tier | 1n | 2n | 4n | 8n |
|---|---|---|---|---|---|
| `2×2×2` ranks, `5×3×2` blocks (trend; maps identical) | base | 7.95 | 7.40 | 7.12 | 8.26 |
| | hinted | 7.89 | 7.41 | 7.13 | 8.36 |
| `4×4×2` ranks, `3×3×1` blocks (the catalog's rank grid at a local block count; maps differ at 8n: 384 → 192 faces) | base | 9.12 | 8.75 | 7.19 | 10.48 |
| | hinted | 9.06 | 8.82 | 7.13 | **8.99** |

Identical where the maps coincide; 14 % faster at 8 nodes where they differ.
On the campaign grid the layer changes the cut at 8 and 16 nodes only
(−33 % / −25 % faces; 22.1 → 14.7 GB and 29.5 → 22.1 GB of halo payload per
run) and is the identity at 1/2/4/32.

## Sizing

- `npx·npy·npz` sets the **node partition**. Mirror it to the largest geometry in
  the sweep and pick a factorization the PD grid divides evenly (the PD grid is a
  contiguous 3-D partition of the rank grid). It is *not* the width knob: the EDT
  count is set by the block total, not the rank count — 518,117,863 EDTs at 32
  ranks against 518,119,559 at 1728 for the same mesh — so an oversized grid buys
  no parallelism, only overhead, at an equal EDT count.
- `init_x·init_y·init_z` is the **width knob** and the size knob: it multiplies
  blocks per rank without changing the node partition. Total level-0 blocks
  (= concurrently runnable block chains) `W = npx·npy·npz · init_x·init_y·init_z`;
  keep `W` an integer multiple of the campaign's total worker count. Blocks per
  rank must stay under `--max_blocks`.
- `nx·ny·nz` and `num_vars` set grain: `num_vars·(nx+2)(ny+2)(nz+2)·8` B per
  block, halo messages `comm_vars·msg_len·8` B. Cells per block are deliberately
  *not* the calibration knob: halo goes as the block face and compute as its
  volume, so shrinking the block raises the communication share — the direction
  that flatters a coherence measurement.
- `num_refine` is expensive **even when nothing refines**: the octree is
  allocated eagerly, so memory and setup scale as `Σ_l 8^l` — 9× at 1, 585× at 3,
  37449× at the default 5. The campaign runs at 0, which is also what removes the
  coarser/finer halo and refinement-intent allocations.
- `num_tsteps`, `stages_per_ts` and `checksum_freq` stay at their published
  defaults; the E2E window is reached with `init_x/init_y/init_z`.

**Memory formula (per level-0 block, `num_refine 0`):**

```
payload(block) = 8·V·C                       cell array,  C = (nx+2)(ny+2)(nz+2)
               + 8·C                         work array
               + 8·comm_vars·Σ(12 peer faces) peer halo send buffers
               + ~275 kB                     rankH, 6 handshake copies, reduction handles
```

At `nx=ny=nz=n` the halo term is `8·V·12·n²`. With `n = 12`, `V = 40`:
`320·(2744 + 1728) + 21,952 + 275 kB ≈ 1.73 MB per block` in **101 datablocks**
(before the `num_refine 0` trim it was 2.42 MB in 249). Total payload
`= W · 1.73 MB`.

Resident memory is dominated by the in-flight EDT/event population, not the
payload. Subtracting the payload the measured runs actually carried (2.42 MB per
block before the trim, so 33.5 GB at 13,824 blocks) from the 13,824-block /
18-timestep anchor gives the runtime state per block-timestep:
`(114.4 − 33.5)/(13,824·18) = ` **~325 kB** on a typical coherence arm and
`(192 − 33.5)/(13,824·18) = ` **~637 kB** on the heaviest one. So

```
RSS(1 node) ≈ W·1.73 MB + W·T·R,   R = 0.325 MB typical, 0.637 MB heaviest arm
                                   (single-node; divides with node count)
```

At `W = 13,824`, `T = 20` that is `23.9 + 89.9 = ` ~114 GB typical but
`23.9 + 176.1 = ` **~200 GB on the heaviest arm — over the 190 GB budget**. This
size does not fit and must come down: `W = 6,912` (2× a 3,456-worker floor)
predicts ~57 GB typical and ~100 GB worst case. Take the campaign size from the
width floor, not from the largest mesh that appears to fit, and re-measure RSS
per coherence family on the 1-node cell before committing.

`--stages_per_ts 20` and `--num_vars 40` are stated even though they are the
defaults. They are the multipliers: per block per timestep the program creates
about **2,000 EDTs** — `1 + S·(16 + 2V)` from the stage/comm/calc chains, plus
the checksum chain on checksum timesteps — and an argument list that leaves them
implicit hides where half a billion EDTs come from.

Anchor at these arguments, one node of 108 workers: 26.6 s, 22 GB resident;
target 20 s (both tiers anti-scale, clamped). The pre-recalibration figure
(143.3 s at `--num_tsteps 18 --checksum_freq 2` with the constant
initialisation) does not carry over: the initialisation, the timestep count
and the checksum frequency have all changed since.
