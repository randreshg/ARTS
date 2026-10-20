# miniAMR_forkbomb

> **`kind: toy`.**  Its EDT set is a strict
> subset of `miniAMR_intel_bryan`'s — the same block chain with halo send and
> receive, minus the checksum, reduction and wrap-up path — which is why it
> computes no result to pin.  What it exercises is the refinement spawn storm
> its name promises, so it is a runtime probe rather than an application, and
> is off the roster.

*miniAMR stripped down to its task-spawn skeleton: the halo exchange and the
stencil are commented out, so what remains is an unbounded chain of EDTs per
block that occasionally splits eight ways.*
Source: `third_party/ocr-apps/apps/miniAMR/refactored/ocr/forkbomb/mainOCR.c`
(~530 lines) + `refineOCR.h` (59 lines) + `SPMDappUtils.h` (a private copy of the
`ocrAppUtils` fork helpers); the directory's `main.c`, `comm.c`, `refine.c`, …
are the reference MPI sources and are **not compiled** into this target.

## Overview

`mainEdt` parses the command line into a 35-`int` datablock and forks an
`nx × ny × nz` grid of SPMD `blockInit` tasks over the policy domains (yes, `nx`
— this port reuses miniAMR's *cell-size* flags as its SPMD grid dimensions).
Before the fork, `mainEdt` creates every labeled sticky event of the neighbour
rendezvous, each exactly once; each `blockInit` names six of them for a one-shot
rendezvous and hands off to `connectEdt`, which builds a `block_t` and starts the
block's driver chain.

From there each block runs `blockEdt → stencilEdt → blockEdt → …` once per
timestep until `timestep == numTsteps`. `stencilEdt`'s body is entirely commented
out; it returns `NULL_GUID` immediately, and its only role is to be an output
event the next `blockEdt` waits on. The six `haloRcv`/`haloSnd` creates and the
six neighbour satisfies in `blockEdt` are likewise commented out
(`mainOCR.c:133–170`), so the channel wiring built at setup is never used after
`connectEdt`. Every 50 timesteps the block instead runs `refineControlEdt`, which
throws a die fixed by the block's id (about one id in 20 hits) and, on a hit
below `maxRefLvl`, replaces itself with **eight** independent child chains.

So the program is a pure EDT-creation and paramv-copy benchmark: no datablock is
created or read in steady state, no arithmetic happens, and the entire block
state (640 B, 80 `u64` params) is copied into every task's parameter block. The
marker is `BLOCK 0 finished` and the catalog records `scalar_kind: bool` — there
is no numeric result.

One property is worth knowing before reading any of the counts below: the
refinement die is the first `random_r` draw from a generator state that
`refineControlEdt` keeps on its own stack and seeds with
`initstate_r((id + 1) * 2654435761, …)`, tested `% 20 == 0`.  The state is the
same 128-byte additive generator `srand`/`rand` use, so the draw equals what
`srand(seed); rand()` returns, but no other task shares it.  The seed is a
fixed odd-constant mix of `id` alone (the `+1` keeps id 0 off the zero seed), and
it is reseeded at every checkpoint, so a block draws the same value at every
checkpoint, deterministic run to run and independent of what other blocks draw
concurrently: a given id either hits at every checkpoint or never.  For the
catalog's single-chain configuration (block id 0) the draw is `1404792907`,
which misses `% 20 == 0`, so block 0 never refines.

## Parameters

Parsed in `mainEdt` (`mainOCR.c:397`) into file-scope globals declared in
`block.h`, then packed into `params[0..34]` and handed to every `blockInit` as a
dependence. Only three entries are ever read again. Unknown flags are ignored
silently.

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `--nx --ny --nz` | **the SPMD block grid** — `numRanks = nx·ny·nz` and `gridDims = {nx, ny, nz}` (`mainOCR.c:509/526`). Nothing sizes a cell array; the name is inherited from the reference program | 10 | ✓ read directly by `mainEdt` |
| `--num_tsteps` | timesteps per block chain | 20 | ✓ `params[20]` → `block_t.numTsteps` |
| `--num_refine` | maximum refinement level (`block_t.maxRefLvl`) | 5 | ✓ `params[2]` |
| `--npx --npy --npz` | would be the process grid in the reference program | 1 | ⚠ parsed into `params[13..15]`, never read — `--nx/--ny/--nz` play this role here |
| `--refine_freq` | refinement interval | 5 | ⚠ parsed into `params[17]`, never read; the interval is the literal `timestep % 50` at `mainOCR.c:105` |
| `--num_objects` / `--object …` | refinement-driving objects | 0 | ⚠ `--num_objects` is parsed; `--object` is **not** a recognized flag in this port, and nothing reads the object count — refinement is the per-id die |
| `--max_blocks --target_* --uniform_refine --block_change --num_vars --comm_vars --init_x/y/z --reorder --inbalance --lb_hinted --report_diffusion --error_tol --stages_per_ts --checksum_freq --stencil --permute --report_perf --plot_freq --code --blocking_send --refine_ghost` | reference knobs | `param.h` | ⚠ all parsed and packed, none read |
| `FORKBOMB_MAX_TIMESTEPS` | passed as `-DFORKBOMB_MAX_TIMESTEPS=3` by `benchmarks/apps/ocr_apps.cmake` | 3 | ⚠ **no source file references it** — a dead build define; `--num_tsteps` is the real cap |
| `MAX_REF` | — | — | ✗ not used in this port (unlike `miniAMR_intel_bryan`); the cap is `--num_refine` |

## Structure

Let `G = nx·ny·nz` (initial blocks), `T = num_tsteps`, `L = num_refine`, and `A`
the number of chains alive at a given time (`A` starts at `G` and multiplies by 8
at each refinement event, up to `G·8^L`).

| object | count | size |
|--------|-------|------|
| setup EDTs | `1` `mainEdt` + `G` `blockInit` + `G` `connectEdt` + `G` first `blockEdt` (created by `connectEdt`) | — |
| `blockEdt` / `stencilEdt` | `≈ 2 · Σ_chains (remaining timesteps)` — 2 per chain per timestep | paramv 640 B copied per create |
| `refineControlEdt` | one per chain per 50 timesteps | — |
| `comm_t` DB | `G` (created in `blockInit`, destroyed in `connectEdt`) | 48 B |
| `connect_t` DB | `6·G` (setup rendezvous payload, created unconditionally for all 6 directions regardless of boundary) | 8 B |
| args DB / range DB | 1 `int[35]` + 1 `range_t` | 140 B / 48 B |
| labeled GUID ranges | `6` ranges of `G` sticky events (`mainOCR.c:512`) | key space only |
| channel events | `6` per block, unconditional (`rcv[6]`, `maxGen 4, nbSat 1, nbDeps 1`) | — |
| labeled sticky events | `6G`, each created once by `mainEdt` (`mainOCR.c:514–521`) for all six directions of every block, boundaries included; `blockInit` only derives their GUIDs | — |
| EDT templates | `blockTML`, `refineCtrlTML`, `stencilTML`, `haloSndTML`, `haloRcvTML` per block; `blockTML` is additionally created **and destroyed inside every `blockEdt` and every `refineControlEdt`** | — |
| datablocks in steady state | **none** | — |

Event accounting: explicit `ocrEventCreate` gives `6G` channel events (always,
unlike `miniAMR_intel_bryan` there is no `rRcv` — this port never wires a
refine-channel pair) plus the `6G` labeled sticky events `mainEdt` creates,
one create per label: a neighbour's side only derives the label's GUID with
`ocrGuidFromIndex`, so a link between two blocks costs no second create. Output
events add exactly one per `stencilEdt` (`ocrEdtCreate(…, &stencilOutEVT)`),
i.e. one per chain per timestep — the dominant and essentially only
steady-state event source. Every other create passes `NULL` for the output
event, and **no EDT is created with `EDT_PROP_FINISH`**, so there are no
finish events at all. A refinement checkpoint (every 50th timestep) creates
no `stencilEdt` and therefore no event, so `NUM_EVENT_CREATE ≈ 12G +
Σ_chains (normal timesteps executed)` — `12G` is one-time setup, the sum is
`T − ⌈T/50⌉` per chain.

Setup totals `1+3G` EDTs, `2+7G` DBs, `12G` events — at `G=1` (`nx=ny=nz=1`)
that is `4`/`9`/`12`.

Worked numbers for the calibrated args (`--nx 1 --ny 1 --nz 1 --num_tsteps
1200000 --num_refine 2`): `G = 1`, so the only block has id 0 — and by the
fixed-seed property above it never refines, making `--num_refine 2` inert. The
run is one strictly serial chain of 1.2M timesteps: **~2.4M EDTs, ~1.176M
output events, 9 datablocks total (all at setup, zero in steady state)**, all
on one worker of one node. That is what makes this entry `default_enabled:
false` — it is a single-threaded spawn-latency measurement, not a parallel
workload. Memory stays flat; nothing is allocated per timestep. A grid with
more than one block (`nx·ny·nz > 1`) gets `G` chains, each with a die fixed by
its id: a chain whose id hits below `maxRefLvl` fans out ×8 at its first
checkpoint, its children's ids draw again at theirs, and no chain is ever
dropped (see Flow).

Counter cross-check: verified (1 node, `--nx 1 --ny 1 --nz 1 --num_tsteps 100`
vs `--num_tsteps 200`, the only reproducible shape — measured totals
205/10/110 EDT/DB/EVT and 405/10/208, identical when re-measured on the
source whose labels `mainEdt` creates). `NUM_DB_CREATE` is flat at `10`
regardless of `T`, matching the setup formula (`2+7·1=9`) plus the runtime's
constant +1 DB baseline exactly — the sharpest confirmation that the steady
state really allocates nothing. `NUM_EDT_CREATE` deltas match `G·2·ΔT = 200`
exactly, and absolutes match `1+3G=4` setup plus `2GT` steady-state plus the
+1 EDT baseline (`4+1+200=205`, `4+1+400=405`). `NUM_EVENT_CREATE` deltas
match `G·(ΔT−Δ⌈T/50⌉) = 98` exactly, and absolutes match `12G=12` setup plus
`T−⌈T/50⌉` (`12+98=110`, `12+196=208`).

## Wiring

Setup mirrors `miniAMR_intel_bryan`: `mainEdt` creates every rendezvous event
once, as a labeled sticky event (`GUID_PROP_IS_LABELED | EVT_PROP_TAKES_ARG`),
before it forks the blocks; `blockInit` then derives, for each of six
directions, its outbound event from `range[dir]` at index `id` and its inbound
one from the neighbour's index (`ocrGuidFromIndex` alone, no create), makes
`connectEdt` depend on all six, and satisfies each outbound one with a
`connect_t` naming its own channel. A
boundary direction aliases receive to send, so an edge block satisfies and
consumes its own event. `connectEdt` folds the six `connect_t`s into
`block_t.comms`, destroys the `comm_t` datablock, and starts `blockEdt`.

Steady state has no wiring at all. `blockEdt` creates `stencilEdt` (1 dependence
slot, satisfied with `NULL_GUID`) and the successor `blockEdt` (1 slot), then
`ocrAddDependence(stencilOutEVT, blockGUID, 0, DB_MODE_RW)` — the successor waits
on a task that does nothing. The block state travels entirely in `paramv`, copied
by value at each create; the `comms` arrays it carries are never dereferenced
again.

DB concurrency: after setup there is none. `comm_t` and `connect_t` each have one
producer and one consumer and are explicitly destroyed. No datablock is ever RW
from more than one task, and no datablock outlives setup — so this row exercises
zero coherence traffic by construction, which makes it a clean isolation of task
creation and scheduling cost from the DB protocol.

## Flow

`mainEdt` (serial: parse, reserve 6 GUID ranges and create their `6G` labeled
events, `G`-iteration fork loop) → `G`
`blockInit`s → `G` `connectEdt`s (each blocking on six neighbour rendezvous, so
setup is a `G`-way barrier in effect) → `G` independent, strictly serial chains.

Parallel width is exactly the number of live chains. Within a chain there is zero
parallelism — `blockEdt` and `stencilEdt` alternate, each waiting on the other —
so `G = 1` occupies exactly one worker no matter how many nodes are configured.
The width then changes only at a refinement checkpoint, in one of two ways:
the die misses and the chain continues unrefined; or the die hits below
`maxRefLvl` and the chain is replaced by eight children. A hit *at*
`maxRefLvl` also prints `cannot refine more!` but is otherwise folded into the
miss case — `refineControlEdt` still creates one unrefined successor, so the
chain is never dropped. So live width can only rise (at a below-`maxRefLvl`
hit) or hold. Each id draws the same value at every checkpoint, so which
chains refine is a deterministic function of the arguments (the grid, which
fixes the ids, and `maxRefLvl`), though not a closed-form one.

Termination is a race, not a join: `blockEdt` calls `ocrShutdown()` when a chain
with `id == 0` reaches `numTsteps`. Because a refining block computes its
children's ids as `id·8·refLvl` plus a running offset, the first child of block 0
would also be id 0 — but block 0's draw is a miss, so it never refines and the
run ends when the original chain does, while any other chains are still running.
There is no finish EDT and no quiescence check, so anything those chains had left
to do is discarded.

## Placement (base)

No `OCR_APP_OPTIMIZED_PLACEMENT` guard exists in this port.

- The initial fork distributes: `forkSpmdEdts_Cart3D` (`SPMDappUtils.h:174`)
  queries `ocrAffinityCount(AFFINITY_PD)`, splits the nodes into a 3-D grid,
  partitions the `nx × ny × nz` block grid onto it contiguously, and sets
  `OCR_HINT_EDT_AFFINITY` on each `blockInit`.
- **Everything after `blockInit` passes `NULL_HINT`** — `connectEdt`, every
  `blockEdt`, every `stencilEdt`, every `refineControlEdt`, and every child chain
  created by a refinement. Under the shim that is `ARTS_HINT_ANY_RANK`, so the
  runtime round-robins them: a chain does not stay on the node its block started
  on; consecutive timesteps of the *same* block land on different ranks in
  rotation. This is the opposite choice from the bryan port, which pins the same
  chain to its creator.
- Datablocks all use `NULL_HINT` → home = creating rank. Only the setup blocks
  exist, so this barely matters.
- Consequence: the 640 B parameter block is the only thing that moves, and it
  moves with the EDT create rather than as a datablock acquire. The program
  therefore generates a steady stream of remote EDT creates with no coherence
  traffic behind them — a task-dispatch probe rather than a data-movement one.
- With the calibrated `--nx 1 --ny 1 --nz 1` the fork produces a single
  `blockInit` on one node; distribution only begins once refinement multiplies
  the chains, and then it is round-robin rather than spatial.

## Sizing

- `nx·ny·nz` is the initial width and the only *structured* distribution dial.
  For a machine of `W` total workers you want `nx·ny·nz ≥ W` at the start,
  otherwise most workers idle until a refinement happens.
- `num_tsteps` sets the chain length — the run's duration and total task count
  scale linearly with it, and it is the only way to lengthen the run because
  nothing per-timestep gets bigger.
- `num_refine` sets how far the width *can* grow (`×8` per level); whether it
  grows is decided by each id's fixed draw — deterministic and reproducible run
  to run, but not a closed-form task count, since it depends on which ids (the
  initial ones and every child's computed id) happen to draw a hit. A hit at
  the maximum level does not end a chain (it folds into the miss case — see
  Flow), so `--num_refine 0` drops no chain either. The catalog's
  `--nx 1 --ny 1 --nz 1` (one block, id 0) is the simplest configuration to
  reason about, since id 0 draws a miss.
- Grain is fixed and minimal: no argument makes a task do more work. This entry
  is a spawn-rate probe, and its only honest reading is tasks per second.

1 node × 15 workers: `--nx 4 --ny 2 --nz 2 --num_tsteps 20000` gives 16 chains,
one per worker, ~640k EDTs — every chain's die is fixed by its id, but the run
still ends (discarding whatever the other 15 chains have left to
do) as soon as the id-0 chain does. 8 nodes × 120 workers: `--nx 8 --ny 4 --nz 4` (128 chains)
at the same `num_tsteps`. The catalog's calibrated set (`--nx 1 --ny 1 --nz 1
--num_tsteps 1200000 --num_refine 2`) deliberately does the opposite — one very
long, fully deterministic, single-worker chain, which measures spawn latency on a
critical path rather than throughput, and is why the row is disabled by default
and why it cannot scale with node count.
