# nekbone

*Nek5000's spectral-element CG proxy as an SPMD program built out of EDTs: a
binary fork tree spawns `Rx·Ry·Rz` virtual MPI ranks, each running a
fixed-length conjugate-gradient solve.*
Source: `third_party/ocr-apps/apps/nekbone/refactored/ocr_src/` (~21k lines;
the DAG is the generated `z_nekbone_inOcr.c`, 3083 lines).

## Overview

Nekbone solves a Poisson problem on a 3-D brick of spectral elements with CG
and no real preconditioner (`solveM` is an identity copy). Each iteration is
a local sum-factorised matvec (`ax`), a nearest-neighbour gather-scatter over
the 26-neighbour cubic lattice, and three global dot products. The result
scalar (`CGloop_stop> rnormfinal`, printed by virtual rank 0 once the tail
recursion runs out) is the residual the CG loop ends on — the one printed
quantity that observes the matvec, the halo exchange and the reductions, and
the only one that depends on `CGcount`. The *initial* residual
(`CGstep0_stop> rnorminit`, printed before the loop) is kept as a secondary
scalar; the completion marker `NKTIME> FinalEDT=` comes from the terminal
`finalEDT`. The program stresses
three things at once: real double-precision FLOPs (O(pDOF⁴) per element), a
fixed-size neighbour exchange, and a global all-reduce on every iteration's
critical path — at scale the reduction latency, not the arithmetic, is what
a run pays for.

## Parameters

Exactly eight positional arguments are required; anything else prints
`ERROR: 8 cmd line arguments are needed` and shuts the runtime down
(`neko_globals.c`, `init_NEKOstatics`). Each must be a non-empty run of decimal
digits whose value fits an `unsigned int` — an empty token, a token with any
other character, and a value that would wrap the accumulation are each named in
an error that stops the run, rather than being folded into a number. There are
no defaults.

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `argv[1..3]` = `Rx Ry Rz` | virtual-rank lattice; `Rtotal = Rx·Ry·Rz` is the SPMD width | none (required) | ✓ parsed in `mainEdt`, stored in the `NEKOstatics` DB and copied down the fork tree — multinode-safe |
| `argv[4..6]` = `Ex Ey Ez` | elements per virtual rank; `Etotal = Ex·Ey·Ez` | none (required) | ✓ same path |
| `argv[7]` = `pDOF` | DOF per axis per element; polynomial order is `pDOF-1`; must be ≥ 2 | none (required) | ✓ same path |
| `argv[8]` = `CGcount` | CG iterations — a fixed count, never a convergence test | none (required) | ✓ same path |

Two ordering constraints abort the run if violated: `Rx ≥ Ry ≥ Rz` and
`Ex ≥ Ey ≥ Ez` (the id↔triplet hashing is not rotation invariant). Those, a
zero on any lattice or element axis, and `pDOF < 2` each print what was
received before stopping. All four are checked in the serial preamble
(`init_NEKOstatics`), so an ill-formed argument set stops the run before any
rank is forked; `pDOF < 2` is additionally re-checked per rank in
`init_NEKOglobals`, where it names the offending rank's `pDOF`. Nekbone's
polynomial-order *sweep* is collapsed (`pDOF_end = pDOF_begin + 1`) — one
order per run. Compile-time knobs, none
argv-reachable: `ARITY` = 10 (all-reduce fan-in,
`libs/src/reduction/reduction.h`), `Nfoliation` = 2 (binary fork tree),
`NEKbone_neighborCount` = 26 (hard-asserted), the four `REDUCTION_*` and
three `NKEBONE_USE_CHANNEL_FOR_HALO_*` switches (all on), and
`NEK_OCR_ENABLE_AFFINITIES` (on — see Placement).

Of the program's five per-rank diagnostic sites, four — the configuration line
(`print_NEKOglobals`), the three `NKTIME> rank=` stamps and the 18
`NEKO_CGtimings` lines, all under the hardwired `NEKO_USE_TIMING` /
`NEKO_PRINT_TIMING` / `NEKO_CG_TIMING` switches — are now emitted by virtual
rank 0 only; they used to be printed by every rank. The fifth, the tail
recursion's `TESTIO> TAILRECUR ELSE` trace, is compiled out with its four
sibling sites under `TAILRECURSION_VERBOSE` (defined nowhere in the tree) and
so prints on no rank: it had lost that guard to a comment while the siblings
kept theirs. Together the five were 23 lines per rank, 79.5k of them at the
campaign lattice, each one a `write()` on the measured path, with a volume set
by the rank count instead of the workload. The completion marker
`NKTIME> FinalEDT=` and both residual lines are unaffected: they were always
printed once.

## Structure

Write `R = Rtotal`, `E = Etotal`, `P = pDOF`, `N = CGcount`,
`D = Σ_ranks neighbours`, and `I = ⌈(R-1)/ARITY⌉` (ranks with children in the
reduction tree). Per-rank DOF count is `P³·E`; `NBN_REAL` is `double`.

| object | count | size |
|--------|-------|------|
| app EDTs | `12·N·R + 18·R + 2` — 12 per iteration per rank, plus a `2R-1`-node fork tree with its join twin and a 10-EDT setup chain per rank | — |
| reduction EDTs | `(1+3N)·(2R+I-1)`, plus a one-time `2R-1+I` channel install | — |
| app DBs | `26·N·R + 57·R - 7` | see below |
| — solution vectors C, F, R, X, W, P, Z | 7 live per rank | `(P³·E + 1)·8` B each |
| — per-element scratch G1/G4/G6/UR/US/UT/temp | 7 per rank, reused for every element | `(P³ + 1)·8` B |
| — derivative matrices `dxm1`, `dxTm1` | 2 per rank | `(P² + 1)·8` B |
| halo envelopes | `D·(N+3)` | `(ddof+1)·16` B |
| reduction scalars | `(3R-2)·(1+3N)` | 8 B |
| events | `5·D + 6R + I - 4` — 3 CHANNEL creates plus 2 labeled-STICKY `ocrEventCreate` *attempts* per directed halo edge (both endpoints create the same rendezvous GUID; the second create parks at the label's home behind the first for the rest of the run and still increments the counter), plus the reduction's per-rank `returnEVT` channel, per-non-root-rank up/down channels, 2 labeled-STICKY attempts per tree edge, and output events on the channel-install EDTs and on `finalEDT` | — |

A rank's neighbour count is `n(rx,Rx)·n(ry,Ry)·n(rz,Rz) - 1`, with `n = 3`
interior, `2` on a boundary, `1` on a degenerate axis: 26 for an interior
rank, 7 for a corner. Summed over the lattice,
`D = (3Rx-2)(3Ry-2)(3Rz-2) - R`. Per-neighbour payload `ddof` is
`(Ex(P-1)+1)²`, `Ex(P-1)+1` or `1` DOF for a face, edge or corner contact.

Worked, at the catalog's `18 16 12 3 3 3 8 100` (`R=3456, E=27, P=8, N=100`,
`D=77,872`, `I=346`): 4,209,410 app + 2,191,614 reduction = **6,401,024 EDTs**
(+1 runtime baseline, which is the catalog's 6,401,025); 9,182,585 app +
3,120,166 reduction scalars + 8,020,816 halo envelopes ≈ **20.3M DBs**;
389,360 halo + 21,078 reduction = **410,438 events**. Each solution vector is
108 KiB, so the live set is 756 KiB per rank — **2.7 GB of vectors across the
3456 ranks**, and 15.5 GB measured at one node once runtime metadata for 6.4M
EDTs and 20.3M DBs is counted. A face envelope is 484 DOF (7,760 B), an edge
22 and a corner 1, so an interior rank exchanges ~50 KiB per gather-scatter
round — ~150 MB per round across the lattice, ~15 GB over the run. The
all-reduces move 8 bytes each and cost latency, not bandwidth.

Object counts do not move with the element block: at 144, 27 and 24 elements a
rank the EDT count is the same 6,401,025, because the block is pure grain and
the task population is the rank lattice's.

Counter cross-check: verified (1 node, three points — `2 2 2 2 2 2 4 2`,
`2 2 2 2 2 2 4 4`, `2 2 1 2 2 2 4 2`): NUM_EDT_CREATE (467/755/235) and
NUM_DB_CREATE (1,307/1,967/563) match the formulas above exactly (+1
runtime-baseline EDT/DB per run). NUM_EVENT_CREATE needed the labeled-GUID
correction above — the losing `ocrEventCreate` attempt counts, in both the
halo handshake and the reduction tree's channel install — to match 325/325/81
exactly; the old `4D+5R+I-3` predicted only 262/262/66.

## Wiring

The generated code calls `ocrXHookup(OCR_EVENT_ONCE_T, ...)` everywhere, but
that helper is a bare `ocrAddDependence` wrapper (`app_ocr_util.c:100`) —
**its event type and flags are ignored and no event is created**. Every edge
in the main DAG is a direct DB→EDT dependence; the only real events belong to
the halo and the reduction.

- **Fork/join**: `SetupBtForkJoin` seeds `BtForkIF` over `[1, R]`; a node with
  `low < hi` splits in two (`BtForkFOR` → two `BtForkIF`), a node with
  `low == hi` is one virtual rank and starts `BtForkTransition_Start`.
  `BtJoinIFTHEN` folds 16-byte checksums back up.
- **Per-rank setup**: `BtForkTransition_Start` → `channelExchange_start/stop`
  (installs the halo channels) → `nekMultiplicity_start/stop` →
  `nekSetF_start/stop` → `nekCGstep0_start/stop` → `setupTailRecursion`.
- **CG iteration**, a strictly serial 12-EDT chain per rank:
  `tailRecursionIFThen` → `tailRecurTransitBEGIN` → `nekCG_solveMi` →
  `beta_start/stop` → `axi_start/stop` → `alpha_start/stop` →
  `rtr_start/stop` → `tailRecurTransitEND` → next `tailRecursionIFThen`.
- **Halo**: `start_channelExchange` creates, once per directed neighbour edge,
  one labeled `STICKY` event from a `Rtotal`-wide range (27 ranges, one per
  lattice direction) and three `CHANNEL` events (`maxGen=2, nbSat=1,
  nbDeps=1` — single producer and consumer per generation), one each for the
  multiplicity, set-f and ax rounds. A round satisfies the neighbour's channel
  with a freshly created payload DB and hooks its own channel onto the `_stop`
  EDT at one of 27 slots (unused directions take a `NULL_GUID` dependence), so
  `nekCG_axi_stop` has 45 slots — 18 fixed, 27 halo.
- **Reduction**: `reductionLaunch` builds an `ARITY=10` tree with per-rank
  up/down `CHANNEL` events, installed once through labeled `STICKY` events and
  reused for every later all-reduce; each rank threads one `reducPrivate` DB,
  taken `RW`, through the whole chain.

DB concurrency is low by construction: every solution vector is created, read
and written only by its own rank's chain, so no DB has two concurrent writers
and the maximum reader fan-out is one. The contention point is not a DB at
all — it is the reduction tree's root, which every iteration passes through
three times. One irregularity: `nekCG_axi_start` acquires `nekW` as `RO` and
writes the whole matvec result into it, which is safe only because the block
never leaves its node (see the notes file).

## Flow

`mainEdt` does no heavy work — it parses argv, reserves the labeled-GUID
ranges and seeds the fork tree, which is `log₂R` deep, doubles in width per
level, costs `~5R` EDTs and is over quickly.

Steady-state parallel width is exactly **`Rtotal`**: a virtual rank is a
serial chain with no intra-rank parallelism whatsoever, so the runtime sees
`R` independent 12-EDT chains per iteration plus the reduction and halo EDTs
connecting them. The serial bottlenecks are the three all-reduces per
iteration — each a `⌈log₁₀R⌉`-deep tree through a single root rank that every
rank blocks on — and the halo `_stop` EDTs, which cannot fire until all of a
rank's up-to-26 neighbours have satisfied their channels. A rank is never
more than one iteration ahead of its neighbours, nor ahead of the root at all.

## Placement (base)

This application carries an `OCR_APP_OPTIMIZED_PLACEMENT` layer (built as
`nekbone_hinted`, `HINTED_PLACEMENT` in `benchmarks/apps/CMakeLists.txt`;
catalog `hinted: true`), described in the next section. Outside that guard the
affinity hints it carries are genuinely base:
`ENABLE_EXTENSION_AFFINITY` is defined for the benchmark build, so
`NEK_OCR_ENABLE_AFFINITIES` is on and the program places explicitly.
`BtForkIF` computes `pdID = rankID % ocrAffinityCount(AFFINITY_PD)`, i.e.
`rankID % nodes`, and creates that rank's `BtForkTransition_Start` with an EDT
affinity hint for it; everything downstream asks for `NEK_OCR_USE_CURRENT_PD`,
so the whole setup chain and CG chain stay on the node the rank landed on, and
DBs created there with `NULL_HINT` are homed on that same node. Only the
fork/join tree itself (`BtForkIF`, `BtForkFOR`, `BtJoinIFTHEN`) is hint-less
and scatters round-robin — and it carries only 16-byte checksum blocks.

The *only* inter-node traffic is therefore the physical one: halo envelopes
created on rank A and RO-acquired by rank B, plus 8-byte reduction scalars
climbing the tree. The algorithm's locality is expressed; the lattice is not.
`rankID` linearises `(rx,ry,rz)` as `rx + Rx·ry + Rx·Ry·rz`, so a neighbour
along an axis is node-local exactly when its id delta — `1`, `Rx` or `Rx·Ry` —
is a multiple of the place count. On the catalog's `18 16 12` the deltas are
1 / 18 / 288, so an x-neighbour is *always* remote and the local share of the
9,672 face adjacencies is 66.3% at 2 places and 32.8% at 4, 8, 16 and 32 —
while the load balance stays exactly 1.0, every place holding `R/P` ranks.
The map costs locality, not balance.

That remote fraction is a property of the lattice's factorisation as much as
of the map: the same `r % P` on a `24 12 12` lattice (also 3456) would keep
65.7% local at 4 and 8 places, because 24 and 288 are multiples of both. The
16- and 32-place figures are the same either way. So the base-vs-hinted gap at
4 and 8 nodes is an upper bound on what the map alone costs, and the 16/32-node
gap is the lattice-independent one.

## Placement (hinted)

The layer changes exactly one function: the rank-to-place map. Base ships
`calcPDid_S` = `rankID % places` (`neko_globals.c`), which puts a rank's
x-neighbours on other places by construction — consecutive rank ids are
x-neighbours, and consecutive ids land on consecutive places — so with more
than one place most of the 26-neighbour halo is remote. Under the guard,
`calcPDid_lattice` calls `nekbone_placeGrid`: it factors the place count into
`nx·ny·nz` boxes that divide the rank lattice, ranks the candidates by volume
per surface (the faces a place does not own are exactly the halo it exchanges),
and maps each rank to the box its lattice coordinate falls in. Every place still
holds the same number of ranks and every rank the same number of elements, so
balance is untouched; only which ranks share a place changes. One guard was
earned by measurement: a box one rank thick on an axis keeps none of that axis's
neighbours — it narrows the same exchange onto fewer peers rather than making it
local, and measured worse than spreading it (8 nodes, 120 ranks: 4.52 s against
4.45 s, the only legal factorisation there being 2×1×4) — so such a split falls
back to the shipped map. The restructured tier's participant numbering reuses
`nekbone_placeGrid` so both agree about which ranks share a place.

The search accepts a factorisation `nx·ny·nz = P` only when each factor
divides its own lattice axis exactly (`nx | Rx`, `ny | Ry`, `nz | Rz`) and no
box is thinner than 2 ranks on any axis. Exact divisibility is what keeps the
balance at 1.000 — every place holds precisely `R/P` ranks and every rank the
same elements — and it is why the map is a function of the lattice, not only of
the node count: an unequal split (5,5,4,4 in x at 32 places) would be more
cubic but would hand places 60 and 48 ranks. Balance ranks first, so such a
split is not taken. If no factorisation survives, the map falls back to the
shipped `r % P` rather than confining ranks.

On the catalog's `18 16 12` (`18 = 2·3²` is what limits `nx` to {1,2} at 16 and
32 places) the search is live at every node count of the sweep:

| places | grid `nx·ny·nz` | box | ranks per place | face adjacencies local |
|---|---|---|---|---|
| 2  | 2×1×1 | 9×16×12 | 1728 | 98.0% (base 66.3%) |
| 4  | 2×2×1 | 9×8×12  | 864  | 95.8% (base 32.8%) |
| 8  | 2×2×2 | 9×8×6   | 432  | 92.8% (base 32.8%) |
| 16 | 2×4×2 | 9×4×6   | 216  | 88.3% (base 32.8%) |
| 32 | 2×4×4 | 9×4×3   | 108  | 82.4% (base 32.8%) |

Crossing adjacencies are `(nx-1)·Ry·Rz + (ny-1)·Rx·Rz + (nz-1)·Rx·Ry` out of
9,672. Every box is at least 2 thick on every axis, so the thin-box fallback
never fires in the sweep, and the imbalance is 1.000 at 2, 8, 16 and 32 places
alike — the same balance the base map has. What the reduction costs is
untouched: its tree is built over geometric rank ids and no hint can reach its
shape, which is what the restructured program attacks.

Measured (15w+1p per node, `18 16 12 2 2 2 12 100`): base 134.46 / 280.75 /
209.76 / 127.14 s at 1/2/4/8 nodes, never beating its own one-node time; hinted
133.15 / 108.05 / 71.37 / 56.49 s, monotone, 2.36× over one node. At the anchor
the two tiers are identical (one place — the map is irrelevant), which is the
check that the change is placement and nothing else. That trend was taken at
`pDOF 12`, i.e. at the same 13,824 DOF per rank as the catalog's
`3 3 3 × pDOF 8`, but at a lower flop-per-halo ratio; hinted's scaling at the
campaign arguments is not yet measured.

## Sizing

`Rtotal` is the only dial that moves parallelism; `Etotal` and `pDOF` move
grain and memory; `CGcount` moves duration only.

- **Parallel width** is `Rtotal = Rx·Ry·Rz`, and the program is SPMD: the
  lattice is set to the total worker count of the widest geometry and held
  there for every node count (32 nodes × 108 workers = 3456 = 18·16·12), with
  `Rx ≥ Ry ≥ Rz`. A virtual rank is a strictly serial 12-EDT chain, so the
  instantaneous frontier is `Rtotal` app EDTs plus the reduction and halo EDTs
  connecting them: 1.0× the worker total, with no scheduling slack at 32 nodes.
  Grain buys no width — see the object counts above.
- **Task grain** is `E·(12·P⁴ + 22·P³)` flops per rank per iteration, dominated
  by the `12·E·P⁴` matvec. `pDOF` is the cheap dial (compute ~P⁴, halo ~P²,
  memory ~P³); `Etotal` buys grain and memory linearly, halo as `E^(2/3)`.
- **Memory** is `7·(P³·E + 1)·8` bytes per rank times `Rtotal` for the live
  vectors, plus a transient generation of four vectors per iteration and the
  runtime's own metadata for the object counts above. At `18 16 12 3 3 3 8 100`
  that is 2.7 GB of vectors and 15.5 GB measured on one node — the whole
  lattice runs on one node at every node count, so this is the figure the
  190 GB per-node budget is read against.
- **Duration** is reached with the element block, never with `CGcount`:
  `pDOF = 8` and `CGcount = 100` are the program's own published values (every
  one of the ten `x_edison_run_Rx*` scripts passes 8, and the original
  hardcodes 100 iterations in its driver), and the element block is exactly
  what the original's input file varies per processor. Keep the block as cubic
  as its count allows: the halo's share falls with the block's
  surface-to-volume.
- **The residual pins stand.** Nothing in this unit touches the arithmetic —
  the stdout reduction removes duplicate diagnostic lines and the two residual
  lines were always printed once, by virtual rank 0 — so the `nekbone` row
  keeps `expect: 1.30069594398318E-06` at its arguments. (`nekbone_dist` is
  re-pinned only because its *arguments* moved to this row's grain, and both
  rows must be re-pinned if the orchestrator changes the element block, since
  the residual is a function of the size.)
- **The one-node anchor is measured**: the element block is the
  orchestrator's dial, and the recorded 24.5 / 23.5 / 24.0 s predate this
  unit's cut of the per-rank diagnostic stdout (79.5k lines at the campaign
  lattice down to ~80); the anchor taken after it, at the shared base/hinted
  point, is 25.5 s and 15.6 GB resident against a 20 s target.
