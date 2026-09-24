# CoMD_intel_chandra_tiled

*The same physics as a real MPI code: a rank grid you choose, one subdomain per
rank, and a face halo that travels as packed buffers over paired channel
events instead of shared datablocks.*
Source: `third_party/ocr-apps/apps/CoMD/refactored/ocr/intel-chandra-tiled/`
(12 C files, ~7.9k lines; `haloExchange.c` is the reason this port exists).

## Overview

Intel's second OCR port of the ExMatEx CoMD proxy, and structurally the
opposite of `CoMD_intel_chandra`: instead of promoting every link cell to a
rank, it keeps upstream CoMD's `xproc × yproc × zproc` domain decomposition and
runs one SPMD EDT chain per rank.  Each rank owns a brick of link cells plus one
shell of halo cells, holds its atoms in six flat arrays, and exchanges ghost
atoms with its **six face neighbours** in three sequential axis sweeps — the
classic CoMD communication pattern, expressed with OCR channel events in place
of `MPI_Sendrecv`.

The result scalar is `Final energy`, printed by `validateResult` at the last
step beside the initial energy, so a value off by more than the catalog's `1e-6`
means the integration went wrong.  What the program stresses is **structured
point-to-point data movement**: no datablock is ever shared between ranks, the
per-rank arrays are single-writer, and the only cross-rank objects are the
packed halo buffers.  It is the reference point against which the other CoMD
ports' shared-halo coherence traffic should be read — and, because the
per-rank chain is strictly sequential, the one CoMD entry whose parallel width
is a *command-line choice* rather than a consequence of the box size.

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|------------------|
| `-x/--nx`, `-y/--ny`, `-z/--nz` | unit cells per dimension (global) | 20 | ✓ parsed in `mainEdt`, copied into the global-parameter DB and then into every rank's `rankH_t` — multinode-safe |
| `-i/--xproc`, `-j/--yproc`, `-k/--zproc` | **rank grid** — this is the parallel width | 1, 1, 1 | ✓ same path.  Product = number of SPMD EDT chains; defaults give **one** rank, i.e. a fully serial run.  `mainEdt` validates the grid before the fork: a non-positive extent is fatal (an extent of 0 forks no rank at all, so nothing would be left to reach `sanityChecks`), while a grid whose split across the policy domains is uneven — or so narrow on an axis that a policy domain owns no rank at all, which is what the defaults do on more than one domain — runs correctly and prints a warning naming the load spread |
| `-N/--nSteps` | time steps | 100 | ✓ exact; the last step always prints and validates regardless of `-n`.  `0` is legal — the initial state is printed and validated and the run joins and shuts down without integrating |
| `-n/--printRate` | steps between the kinetic-energy allreduce and a status row | 10 | ✓ pure diagnostic/sync interval; it does not change the trajectory and does not gate termination |
| `-D/--dt` | time step (fs) | 1.0 | ✓ applied to both half kicks and the drift |
| `-T/--temp` | initial temperature (K) | 600.0 | ✓ |
| `-r/--delta` | initial random displacement (Å) | 0.0 | ✓ |
| `-l/--lat` | lattice constant (Å); `<0` = the potential's 3.615 | -1.0 | ✓ — moves the cell grid, see Sizing |
| `-e/--doeam` | use EAM instead of Lennard-Jones | 0 | ⚠ reachable — `-e -d <dir>` with a `funcfl` table works (`setfl` aborts), and a table ships in the app tree — but excluded from the campaign: `doeam = 0` is the published default, no potential fixture is staged, and EAM adds a second (force) halo the memory formula below does not count |
| `-d/-p/-t` (`potDir`, `potName`, `potType`) | EAM table selection | `pots`, auto, `funcfl` | ✓ `-d` takes any directory; all three are dead without `-e` |
| `-h/--help` | print the option table | — | ✓ prints and calls `ocrShutdown` |

The target's four `EXTRA_DEFINES`: **`DOUBLE_BUFFERED_EVTS`** doubles the halo
channel and send-buffer arrays (`NB_SEND_CHANNELS` 6 → 12) and indexes them by
`phase = step % 2` — load-bearing, see Wiring.  **`CHANNEL_EVENTS_AT_RECEIVER`**
decides which side *owns* each halo channel: with it a rank creates its own
receive channels and publishes their GUIDs, so the peer's send is a remote
satisfy (push); without it the sender owns them and the receiver's
`ocrAddDependence` is remote (pull).  **`WITH_COUNTED_EVT`** is **dead in this
source** — nothing under `intel-chandra-tiled/` references it; counted events
are used unconditionally through `createEventHelper`.  **`OCR_APP_COUNTED_OEVT`**
makes every EDT output event a COUNTED event the program supplies itself
(`OEVT_COUNTED_PRE` + `EDT_PROP_OEVT_VALID`) instead of a runtime-minted
single-fire event: an undeclared output event must linger forever (a later
registration on it is always legal), so per-step output events otherwise
accumulate without bound in the step count.  Coverage is complete: every
create either supplies a COUNTED output, or passes NULL where nothing
consumed the event at all (the timestep-loop spawn and the sort step were
minting events nobody read).

`MAXATOMS` (64), `ARITY` (10, reduction fan-in) and `numberOfTimers` (24) are
genuine compile-time constants.  `MAXATOMS` is not a neutral one: it is the
published data structure's reservation per link cell and **both** memory terms
are linear in it (see Memory), so at the campaign's ~19 atoms per box roughly
70% of the footprint is reserved-and-empty.  That is a property of the
published decomposition the base tier exists to exhibit, which is why it stays
a `#define` and the box is the only lever.  `USE_STATIC_SCHEDULER` is in the
upstream Makefile but *not* in this target, so the plain `forkSpmdEdts_Cart3D`
fork is used (see Placement).

## Structure

With `R = i·j·k` ranks, `lat = 3.615`, `cutoff = 5.7875`,
`gs_a = floor(n_a·lat / (proc_a·cutoff))` local boxes per axis,
`L = gs_x·gs_y·gs_z`, `H = 2·((gs_x+2)(gs_y+gs_z+2) + gs_y·gs_z)`,
`T = L + H`, `A = 64·T` atom slots, `S` steps and `Q = ceil((R−1)/10)`:

| object | count | size |
|--------|-------|------|
| atom arrays (`gid`, `iSpecies`, `r`, `p`, `f`, `U`) | `6R` | `4A`, `4A`, `24A`, `24A`, `24A`, `8A` bytes |
| `nAtoms` | `R` | `4T` |
| halo send buffers | `12R` | `bufCapacity` each (largest of the three face areas × 2 × 64 × 56 B) |
| tag buffers / cell lists / exchange parms | `12R` / `6R` / `R` | 8 B / `4·nCells` / ~456 B |
| `rankH_t` / `SimFlat` / potential / 5 reduction privates | `R` each | 7232 B / 1432 B / ~112 B / 368 B |
| DBs | `52R + 4` app blocks + the reduction library's transients | **none created per timestep** |
| events | `R·(89 + 41S + 5·boundaries) + 3` + reduction setup | 24 halo channels + 12 labeled sticky per rank at init; 41 per rank per step |
| EDTs | `R·(30 + 23S + 2·boundaries) + 3` + reduction | 23 per rank per step |

The reduction library costs, per collective, one launch EDT and one launch block
per rank plus a tree pass (`2R+Q−1` EDTs and `3R−2` blocks for the three
ALLREDUCE objects; `R+Q` and `2R−1` for the two REDUCE ones), and the same tree
pass once more per object the first time it is used — `5(5(R−1)+Q)` events
across the five objects (see below).  Four collectives run at init
(centre-of-mass velocity, kinetic energy twice, max occupancy), one per print
boundary, and two in the epilogue (performance timers, SPMD join).  Total
reduction cost: `22R + 12Q − 10` EDTs, `24R − 17` blocks, `5(5(R−1)+Q)` events.

The 23 steady-state EDTs per rank per step are: `timestepLoop` + `timestep`
(2), the five body tasks (`advanceVelocity`, `advancePosition`,
`redistributeAtoms`, `computeForce`, `advanceVelocity`), `updateLinkCells` +
`haloExchange(x)` + `sortAtomsInCells` (3), two chained `haloExchange` EDTs for
the y and z axes, three `exchangeData` EDTs, six `loadAtomsBuffer`/
`unloadAtomsBuffer` EDTs, and the two-step force dispatch
(`ljForce_edt → ljForce1_edt`).

Two constants needed a fix past the first pass, both invisible in a same-`S`
delta (which is why Pair A's deltas already checked out) and both explained by
the same mechanism as the sibling port: the runtime counts *every*
`ocrEventCreate` call, the one that parks behind the installed event included
(`arts_event_create` counts before it installs or parks, `event.c`).

- **The event constant is `89`, not `80`** (and the EDT constant needs a flat
  `+3`, not `+2`).  `initEdt`'s halo rendezvous (`CoMD.c`'s `ocrGuidFromIndex`
  sticky-GUID derivation) creates each of the `6R` labeled sticky GUIDs from
  *both* endpoints — a rank's own "send" create and the matching neighbour's
  "receive-mapping" create land on the same index — so it's 12 sticky creates
  per rank, not 6 (+6 events per rank).  The reduction library's own send/recv
  channel pairing
  (`reductionEdt`'s parent-side `recvEVT` and the child's
  `reductionSendChannelEdt`'s `sendEVT`, same file as the sibling port) has the
  identical shape, so the setup term is `5(5(R−1)+Q)` across the five
  reduction objects (Vcm, Ke, max-occupancy, perf-timer, SPMD-join), not
  `5(4(R−1)+Q)` (+5(R−1) events).
- **The epilogue was missing entirely from the compact per-step formula**,
  because it fires exactly once per run (at whichever boundary has
  `itimestep == nSteps`), not once per boundary: `finalizeEdt` (`timestep.c`,
  in that boundary's branch) runs on every rank (+1 EDT, +3 events: output +
  finish + `createEventHelper`), and `printPerformanceResultsEdt`
  (`timestep.c`, same branch) runs on rank 0 only (+1 EDT, +2 events) plus one
  `createEventHelper` shared by both branches (+1 event, every rank).  Summed
  over `R` ranks that is `+(R+1)` EDTs and `+(4R+2)` events.  A separate,
  opposite-sign effect offsets part of it: the *last* `timestepLoopEdt`
  iteration never creates a successor (it's the terminal step), so it's
  missing that create's own output event — `−R` EDTs and `−R` events.  For EDTs
  the two exactly cancel to a rank-independent `+1`, which is why only the
  flat constant moves (`+2` → `+3`, `30` stays `30`).  For events the
  coefficients don't match (`−1` vs `+4`), so the net `+3R` folds into the
  per-rank coefficient instead (`80` → `89`, with the halo-doubling fix above
  supplying the other `+6`) and the epilogue's own flat remainder (rank 0's
  extra `printPerformanceResultsEdt`, `+2`) lands in the constant (`1` → `3`).

Worked numbers, `-i 24 -j 12 -k 12` → `R = 3456`, at the shapes the sizing
section discusses (`gs_a` uses the *truncating* division above, which is why a
proportional box gives cubic `gs`); the last row is the catalog's own box —
`c = 15` truncates into the same `(9,9,9)` bucket as `c = 16` below, so the
two share every object count, and only the real volume (hence atom count and
occupancy) differs:

| box | `gs` | `L` | `T` | atoms | per rank | boxSize |
|---|---|---|---|---|---|---|
| `-x 240 -y 240 -z 240` | (6,12,12) | 864 | 1568 | 55.3 M | 16.0k, 18.5 of 64 slots per box | 6.025 Å |
| `-x 384 -y 192 -z 192` | (9,9,9) | 729 | 1331 | 56.6 M | 16.4k, 22.5 of 64 slots per box | 6.427 Å |
| `-x 408 -y 204 -z 204` | (10,10,10) | 1000 | 1728 | 67.9 M | 19.7k, 19.7 of 64 slots per box | 6.146 Å |
| `-x 360 -y 180 -z 180` | (9,9,9) | 729 | 1331 | 46.7 M | 13.5k, 18.5 of 64 slots per box | 6.025 Å |

Per-step EDT cost is ~22 per rank (the chains loop atoms inside EDTs), so a
100-step run is **~7.6M EDTs** whatever the box.  The footprint — atom arrays
plus worst-face halo buffers, set by the rank grid and the box, not by the
machine — is **node-count-independent**, so the whole of it lands on the single
node of the 1-node cell; the formula and the budget are under Memory.  ⚠ The
event formula verified below predates the output-event conversion: two
per-step events that nothing consumed (the timestep-loop spawn's and the sort
step's) are no longer created at all (−2 per rank per step), so the event term
now overcounts by exactly that much until the counter verification is re-run.

Counter cross-check: verified (1 node, `-x 16 -y 16 -z 16 -N 2 -i 2 -j 2 -k 1`
vs `-x 16 -y 16 -z 16 -N 4 -i 2 -j 2 -k 1`): measured absolutes EDT 406/590, DB
292/292, EVT 787/1115; subtracting the runtime's constant baseline (+1 EDT, +1
DB, +0 EVT per run) against the formulas gives an exact match in both absolute
value and delta — this app has no data-dependent tail (redistribution always
runs the same fixed EDT/event chain, unlike the sibling port's `move_edt`).

## Wiring

**The halo is the point of this port, so start there.**  Each rank keeps, per
face and per phase, a *pair* of channel events: one carrying the packed
`AtomMsg` buffer and one carrying an 8-byte count — 6 faces × 2 phases × 2 =
**24 channel events per rank**, all with `nbSat = nbDeps = 1`.  They are matched
at startup through a rendezvous on labeled sticky GUIDs: `initEdt` publishes its
four channel GUIDs for face `f` into event `6·rank + f` of a `6R`-wide range and
registers on the neighbour's event `6·nbr + opposite(f)`; `channelSetupEdt`
unpacks the six replies into `haloSendEVTs`.  From then on the halo is pure
message passing — `loadAtomsBufferEdt` packs a face into its own send buffer and
`ocrEventSatisfy`s the channel with it; the neighbour's `unloadAtomsBufferEdt`
takes it **RO** off the channel and calls `putAtomInBox`.

`redistributeAtomsEdt` drives one step of that: `updateLinkCellsEdt` (empties
halo cells, moves atoms that crossed a cell boundary) → `haloExchangeEdt`, a
self-chaining FINISH EDT that runs the x, y and z sweeps **strictly in order**
(so corner and edge cells are filled by data that already arrived on the
previous axis) → `sortAtomsInCellsEdt`.  Each sweep's `exchangeDataEdt` is a
FINISH EDT holding exactly one load and one unload.

The second idiom is `createEventHelper`, used for **every** intermediate join in
the program: an `OCR_EVENT_COUNTED_T` with `nbDeps = 1`, fed by an EDT's output
event and consumed as a `DB_MODE_NULL` control edge.  The declared count is
what makes the event reclaimable — the runtime frees it once its one consumer
has been delivered, where an undeclared single-fire event has to linger for
the rest of the run — so the whole per-rank DAG is a chain of one-shot,
self-reclaiming latches rather than an ever-growing web of lingering events.

Why double buffering is a **correctness** requirement, not a throughput tweak: a
rank reuses `sendBuf[face][phase]` every second step, and the pairwise handshake
supplies the missing edge.  Rank B's load at step `t+2` follows B's unload at
`t+1`, which follows A's load at `t+1`, which follows A's whole step `t`
finishing — and A's unload of B's step-`t` buffer is inside that scope.  With a
single buffer set the reuse would be at `t+1`, and no path in the DAG orders it
after A's unload at `t`; the OCR channel's `maxGen` would be the only thing
holding it back, and ARTS does not enforce `maxGen`.

DB concurrency, in one line: **there is none.**  Every per-rank array is written
only by that rank's own chain; every send buffer has exactly one writer and, one
step later, exactly one remote RO reader.  No datablock in this port is written
from more than one rank, and none has more than one concurrent reader.  The
reduction private blocks and the 24-byte reduction payloads are the only other
cross-rank objects.

## Flow

Per rank per step the DAG is a **single chain** about 15 EDTs deep:
`timestepLoop → timestep → velocity → position → redistribute →
{updateLinkCells → haloExchange(x → y → z), each exchangeData → load → unload}
→ sortAtoms → computeForce → ljForce → ljForce1 → velocity`.  Nothing inside a
rank runs concurrently with anything else in the same rank — even the one pair
that could overlap does not, because `unloadAtomsBuffer` takes the load's
output event as a `DB_MODE_NULL` edge — so

    max concurrent EDTs = R = xproc · yproc · zproc

exactly, and that is the whole parallelism story.  With the default `-i 1 -j 1 -k 1` the
program is *serial* whatever the machine — a single chain of ~23 EDTs per step,
each doing a full subdomain's work (this is why an uncalibrated run of this
entry looks like a few hundred EDTs of ~1 s grain).

Ranks are coupled only pairwise, through the face channels, so there is no
whole-grid barrier during the timestep loop; neighbours drift apart by at most
the one step the double-buffered channels allow.  The exceptions are the
diagnostic points: every `printRate` steps (and always on the last step) a
kinetic-energy **allreduce over all `R` ranks** gates `printThingsEdt`, which
sits inside the step's finish scope.  Initialisation adds three more collectives
(centre-of-mass velocity, kinetic energy, max occupancy) and the epilogue two
(performance timers, and an SPMD-join reduction whose root satisfies the
shutdown event) — so termination is a proper barrier, not a race.  The epilogue
hangs off the last step's `printThingsEdt`; with `-N 0` there is no such step,
so `FNC_initSimulation` builds the same epilogue behind the step-0 print
instead, and that print is then created the way the last step's is (finish
scope, timer-reduction block RW) because it is the one launching the timer
reduction the epilogue consumes and destroys.

## Placement (base)

Everything below is the program's own, compiled in unconditionally.  Every
object placement it makes is right; the one decision it gets wrong on a
non-cubic rank grid is which policy-domain grid the rank grid is blocked over,
and that is what the hinted tier re-chooses (see Placement (hinted)).
`mainEdt` calls `forkSpmdEdts_Cart3D`, which queries
`ocrAffinityCount(AFFINITY_PD)`, factors the policy-domain count into a 3-D
grid (`splitDimension_Cart3D`) and gives each `initEdt` the
`OCR_HINT_EDT_AFFINITY` of the policy domain that owns its block of the rank
grid (`getPolicyDomainID_Cart3D`, a block partition per axis, so a 3-D
sub-brick of ranks per node).  Every subsequent EDT takes its hint from
`getAffinityHintsForDBandEdt`, i.e. `ocrAffinityGetCurrent` — which reproduces
the index map rather than pinning on a creator, because the rank itself was
placed from its index, not from where it was created.

The factorisation is exact and the load imbalance is exactly 1.00 at every
node count in the sweep: with a 24×12×12 rank grid the policy-domain grid is
(1,1,1)/(2,1,1)/(2,2,1)/(2,2,2)/(4,2,2)/(4,4,2) at 1/2/4/8/16/32 nodes, i.e.
3456/1728/864/432/216/108 ranks per node with zero remainder, each node's
ranks a contiguous brick.  `mainEdt` checks that pairing before it forks and
says so out loud when a grid would idle or unbalance a domain.

The compiled fork is the **library** one, `apps/libs/src/ocrAppUtils`.  The
app-local `SPMDappUtils.h` carries a second, slightly different copy of the
same factorisation and is included by nothing in this target — read the
library, not the header.

Datablocks are placed two ways, and both land correctly: the halo send buffers
carry an explicit `OCR_HINT_DB_AFFINITY` of the current PD, and everything else
(the six atom arrays, `nAtoms`, `rankH_t`, `SimFlat`, the potential, the
reduction privates, the cell lists) passes `NULL_HINT` — which, with home =
creating rank, still homes them on the owning rank because they are created
*inside* the rank's own EDT.  The only rank-0-homed objects are the argv and
global-parameter blocks, read once per rank at init.

The resulting multinode traffic is therefore exactly the algorithm's: per rank
per step, six packed halo buffers out and six in (each sized for the rank's
worst face, whole-DB granularity regardless of how many atoms actually
travel), plus one 24-byte reduction payload per collective.  Locality that
exists in the algorithm is fully expressed — this is the port to compare the
others against, not a coherence stress, and it is why the family sweep is
arm-indifferent to a fraction of a percent.

`USE_STATIC_SCHEDULER` would replace the per-rank affinity hints with one fork
EDT per policy domain plus an `OCR_HINT_EDT_DISPERSE` hint; it is not defined
for this target, so the direct affinity mapping is what runs.

## Placement (hinted)

Audited 2026-09-08 (every create on the step path enumerated from source:
23 step EDTs and ~39 events per rank carry `PTR_rankH->myEdtAffinityHNT`, zero
DBs are created per step, every per-rank array is `NULL_HINT`-created inside
the rank's own EDT and so homed there, the halo channel events live at the
receiver, the labeled rendezvous/reduction ranges are homed `idx % nranks`
and unmovable, events take no hint).  Three freedoms were checked:

- **The policy-domain grid — a real freedom.**  `splitDimension_Cart3D`
  (`ocrAppUtils.c`) factors the *domain count* near-cubically and never looks
  at the rank grid.  On a torus rank grid an axis left unsplit contributes
  zero halo crossings, a discrete term a surface-to-volume argument cannot
  see; the exact per-step crossing count of a `p_x × p_y × p_z` split of an
  `N_x × N_y × N_z` rank grid is `Σ_{p_a>1} 2·p_a·(R/N_a)`.  Enumerated over
  every factorisation that divides each axis exactly (`factorization.py` in
  the audit unit), campaign grid `24 × 12 × 12`:

  | nodes | library | crossings | minimum (balanced) | crossings | library minimal? |
  |---|---|---|---|---|---|
  | 2 | 2×1×1 | 576 | 2×1×1 | 576 | yes |
  | 4 | 2×2×1 | 1728 | **4×1×1** | **1152** | **no (−33.3 %)** |
  | 8 | 2×2×2 | 2880 | **4×1×2** (ties 4×2×1, 8×1×1) | **2304** | **no (−20.0 %)** |
  | 16 | 4×2×2 | 3456 | tied minimum | 3456 | yes |
  | 32 | 4×4×2 | 4608 | tied minimum | 4608 | yes |

  On the local trend grid `10 × 6 × 4` the library's split is the balanced
  minimum at 1/2/4/8 (the lower-crossing 4×1×1 / 4×2×1 would cost a 1.5–2×
  rank-load spread on a strictly sequential SPMD chain).
- **The halo send-buffer home — a freedom the base has right.**  The buffer
  is persistent and RW-acquired by its producer every second step, so
  producer-home costs one payload crossing per directed link where
  consumer-home costs two (the RW acquire must fetch; there is no
  acquire-without-fetch), under both write policies.
- **Reduction payloads / rank-0 blocks — not movable or not worth moving.**
  `reduction.c` is compiled once as an OBJECT library, so the flavour define
  never reaches it, and its traffic is `1 / 2.1×10⁶` of the byte flow; one
  home cannot be local to `R` readers of the argv/global-parameter blocks.

**The layer** (`COMD_SURFACE_MINIMAL_PD_GRID` under `OCR_APP_OPTIMIZED_PLACEMENT`,
`CoMD.c`; the library fork is split into `forkSpmdEdts_onPdGrid_Cart3D`, the
body, and `forkSpmdEdts_Cart3D`, split-then-delegate, with no behaviour change
for other callers): among the factorisations that divide every axis exactly
it takes the minimum-crossing one, a tie keeps the library's default, and
balance is never traded.  Hint values only — same EDTs, DBs, events, wiring.

**Measured** (INV × WB, 15w+1p × 1/2/4/8, two repeats, scalars identical in
every cell):

| rank grid | tier | 1n | 2n | 4n | 8n |
|---|---|---|---|---|---|
| `10 6 4` (trend; maps identical at every count — the layer's self-check) | base | 10.90 | 6.13 | 4.34 | 3.43 |
| | hinted | 11.03 | 6.19 | 4.45 | 3.51 |
| `4 6 10` (long axis last — the campaign grid's asymmetry; maps differ at 2n/4n) | base | 10.73 | 6.89 | 5.05 | 3.13 |
| | hinted | 10.83 | **6.11** | **4.26** | 3.22 |

Where the map is the same the two tiers agree to noise (the ~1–2 % offset is
the box's drift); where it differs the layer is 11 % faster at 2 nodes and 16 %
at 4.  On the campaign grid the map differs only at 4 and 8 nodes (−33 % and
−20 % crossings, `bufCapacity = 867 328 B` per face → ~500 MB less halo per
step); at 1/2/16/32 nodes the two tiers lay out identically and must match.

## Sizing

Two dials that do different jobs.  **`-i/-j/-k` set parallelism** — width *is*
`R = i·j·k`, and because the per-rank chain is strictly sequential (`unload` is
ordered after `load`, `DB_MODE_NULL`, so there is no intra-rank overlap at all)
the instantaneous frontier is exactly `R`.  This is an SPMD row: `R` is set to
the total worker count at the widest geometry and left there, node-invariant.
"A few times the worker count" is not available here — the next legal multiple,
`-i 24 -j 24 -k 12` = 6912, is structurally fine but costs ~+15% payload on top
of an already-binding memory budget.  **`-x/-y/-z` set work and memory.**
Shrinking the rank grid does not shrink the total footprint, it concentrates
it.

Structural floors, and they are the only ones the code imposes:
`sanityChecks` demands `n_a·lat ≥ 2·cutoff·proc_a`, i.e. **`n_a ≥ 3.203 ·
proc_a`** on every axis, and `initLinkCells` asserts **`gs_a ≥ 2`**.  There is
*no* commensurability requirement: `createFccLattice` places atoms from
real-space bounds with a half-open filter, so a box that does not divide the
grid is correct, merely slightly imbalanced.  Commensurability is a
load-balance preference, and the shape rule that follows from it is the one
upstream's own run scripts use: **make the box proportional to the rank grid**
(`-x 24c -y 12c -z 12c` for this grid), which gives cubic subdomains, cubic
`gs`, and the smallest worst-face halo buffer for a given atom count.

- The campaign cell is `-i 24 -j 12 -k 12` (`R = 3456`) — one persistent
  rank-chain per worker at the largest campaign geometry (32 nodes × 108
  workers), node-invariant; smaller node counts pack more chains per node.
  Every axis of the grid divides exactly at every node count in the sweep.
- **Box: `-x 360 -y 180 -z 180` (`c = 15`).**  It was chosen by growing `c` in
  the proportional shape until the 1-node cell reached the anchor band without
  breaking the memory budget below — the size knob, never the step count.
- **Height knobs stay at the published defaults**: `-N 100` and `-n 10` are
  the parser's defaults *and* what every upstream run script uses.  `-n` need
  not divide `-N` in this port (the epilogue fires on
  `itimestep%printRate==0 || itimestep==nSteps`), unlike the sibling
  `CoMD_intel_chandra`.  `-D/-l/-T/-r` are not passed.
- Do **not** leave `-i/-j/-k` at their defaults for a measurement: the run is
  correct but single-chain, and its numbers describe one core.

## Memory

The footprint is set by the box and the rank grid at startup, is
node-count-independent, and does not move with the step count — so the 1-node
cell carries all of it and is the binding constraint on the box.

With `lat = 3.615`, `cutoff = 5.7875`, `MAXATOMS = 64`, `sizeof(AtomMsg) = 56`,
and `gs`, `L`, `H`, `T` as in Structure, let
`F = { (gs_y+2)(gs_z+2), (gs_x+2)(gs_z+2), (gs_x+2)(gs_y+2) }` be the three face
areas and `maxF = max F`:

    atom arrays   88 · MAXATOMS · T          bytes   (gid 4, iSpecies 4, r/p/f 24 each, U 8)
    nAtoms        4 · T
    halo buffers  12 · (maxF · 2 · MAXATOMS · 56)    (all twelve sized by the LARGEST face)
    cell lists    16 · ΣF
    fixed         ~9.8 KB  (rankH_t 7232, SimFlat 1432, exchange parms 456, tag buffers, potential, 5 reduction privates, 6 rendezvous event blocks; every term below ~1 KB per rank is folded in here)

    per rank  =  5632·T + 4·T + 86016·maxF + 16·ΣF + ~9.8 KB
    payload   =  R × per rank

Two consequences worth stating plainly.  **`MAXATOMS` multiplies both leading
terms**, and mean occupancy at a campaign box is ~19 of its 64 slots, so ~70%
of the payload is reserved-and-empty — the published data structure's cost,
which the base tier exhibits rather than optimises.  **`maxF` is the whole
anisotropy penalty**: all twelve buffers take the largest face, so a subdomain
of aspect ratio 1:2:2 pays ~37% more halo bytes per unit of local volume than a
cube.  That is why the box is chosen proportional to the rank grid.

Worked payloads at `R = 3456`, `-i 24 -j 12 -k 12`:

| box | `gs` | `maxF` | per rank | payload | atoms |
|---|---|---|---|---|---|
| `-x 240 -y 240 -z 240` | (6,12,12) | 196 | 25.7 MB | 88.9 GB | 55.3 M |
| `-x 336 -y 168 -z 168` | (8,8,8) | 100 | 14.3 MB | 49.3 GB | 37.9 M |
| `-x 384 -y 192 -z 192` | (9,9,9) | 121 | 17.9 MB | 61.9 GB | 56.6 M |
| `-x 408 -y 204 -z 204` | (10,10,10) | 144 | 22.1 MB | 76.5 GB | 67.9 M |
| `-x 432 -y 216 -z 216` | (11,11,11) | 169 | 26.9 MB | 93.1 GB | 80.6 M |
| `-x 360 -y 180 -z 180` | (9,9,9) | 121 | 17.9 MB | 61.9 GB | 46.7 M |

Resident set is larger than payload by a runtime/allocator factor that is
**measured, not derived**.  The two recorded points (88.9 GB payload → 201.6 GB
resident, 102.0 GB → 225.6 GB) fit `resident ≈ 40 GB + 1.82 × payload`; a
pessimistic reading, in which the whole ~113 GB gap is fixed per-DB metadata
(the DB *count* per rank is the same at every box), gives `resident ≈ 113 GB +
payload`.  Under both models a 190 GB budget admits `-x 384` (153–175 GB) and
puts `-x 408` at the edge (179–189 GB); `-x 432` is over.  The catalog settled
on `-x 360` — the same `(9,9,9)` bucket as `-x 384`, hence the same payload and
the same admitted budget, but fewer real atoms and so less compute per rank at
no extra memory cost, which is what let it clear the 1-node anchor band.
**A 1-node cell with peak-RSS capture confirmed it at preflight** — the
projection alone was not strong enough to choose, and no figure in the tree
records the host, arm or `route_table_size` behind 201.6 GB.

## Inside the `[E2E]` window

Nothing recomputes the answer: `validateResult` prints `eTot0`, recorded once
at step 0, beside the final energy that `printThingsEdt` lifted straight out of
the kinetic-energy allreduce.  No second force evaluation, no reference pass,
no file output at all (the app writes none, and the EAM tables are the only
file it would ever read).  Data generation is index-seeded and parallel:
`createFccLattice` walks only the rank's own real-space bounds, and momenta
come from `mkSeed(gid, callSite)` — a pure hash of the atom's global id, so the
initial state is identical at every node count and no RNG state is shared.

What *is* in the window besides physics, all of it published structure, and all
of it `O(R)` rather than `O(R × steps)`:

- the serial fork of `R` `initEdt`s on rank 0 (`forkSpmdEdts_Cart3D`);
- the halo rendezvous — `6R` labeled sticky events, each created from **both**
  endpoints, plus `5R` reduction-range events, homed round-robin by index, so
  roughly `12R` of those creates are remote messages;
- the per-rank lattice build and link-cell zeroing (`MAXATOMS · T` slots);
- `finalizeEdt`'s teardown of 18 templates and 14 datablocks per rank, plus the
  SPMD-join reduction that satisfies the shutdown event.

Init is therefore a real share of the window and grows with the box, while the
step loop grows with box × steps; record the split alongside the anchor seconds
(`calibration pending`).  A per-arm difference at small `-N` is fan-out and
teardown, not step-loop behaviour.

The rendezvous is also the row's one undisclosed runtime dependency: both
endpoints create the *same* labeled sticky GUID with `GUID_PROP_CHECK`, and the
program is correct only if the second create leaves the first's (possibly
already satisfied) event standing.  ARTS does: the second create parks at the
label's home behind the first and, since the label is never destroyed, stays
parked for the rest of the run.  Two creates of one live label are outside
ARTS's labeled-GUID contract (`docs/programming_model/guids.rst`); this shape
costs one parked create per label and changes no result.

**Per-step wire law.**  Each face DB is sized for the worst case and travels
whole: `6 × bufCapacity` bytes per rank per step regardless of how many atoms
actually move (twelve buffers exist, two per face by parity, but one phase
travels per step: six packed faces out, six in).  At ~19 atoms per box against the `2·MAXATOMS` slots per cell the
buffer reserves, that is ~3.5× over-send on the largest face and more on the
other two, which are packed into a buffer sized for the largest.  It is legal (published
structure, and ARTS moves whole DBs), but it sets the per-step inter-node byte
count every arm is compared on — read the arms' network sensitivity against
that denominator, not against the atoms that moved.

## Family shape (measured, 15w+1p x 1/2/4/8 nodes -- retired geometry, re-take pending)

`calibration pending` — the recorded ferrari ladder (501 / 274 / 164 / 86 s over
1/2/4/8 nodes at 15w+1p, 5.8× at 8 nodes, all four arms within 500.2–501.1 s at
1n) was taken at a retired geometry and before the halo-buffer sizing fix, so it
is not comparable to a current run and must be re-taken at the adopted args.
The shape it showed is what the row is for, and the structure predicts it:
persistent chains, pairwise face coupling, no per-step global barrier, and a
balanced static map.  Set against its fork-join sibling (`CoMD_sdsc`, which
anti-scales on every arm), this port is the family's demonstration that the
persistent-chain SPMD rewrite — wire the DAG once, exchange halos by message —
is what removes the wiring-plane wall; its cost profile is the algorithm's own
traffic, which no coherence arm can distinguish.
