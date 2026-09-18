# nekbone_dist

*The restructured tier of `nekbone`: the same spectral-element CG solve over
the same instance, with the all-reduce's own decomposition redesigned — the
participants are numbered place-major and the tree is built in two levels
around that numbering.*
Source: `third_party/ocr-apps/apps/nekbone/refactored/ocr_src/` with
`../ocr_dist/neko_reduction.c` substituted for `neko_reduction.c`, and the
`reductionTwoLevel` library in place of `reduction`
(`benchmarks/apps/CMakeLists.txt`).

## Overview

Read `appdocs/nekbone.md` first: the program, its parameters, its object
counts, its wiring and its placement map are that row's, unchanged. Everything
below is the delta.

`nekbone` puts three all-reduces on every CG iteration's critical path, and the
shared reduction library builds one flat `ARITY = 10` tree over participant
indices. The program hands that tree the geometric rank id, so the tree's edges
are unrelated to where a rank runs: with the halo made local by placement, the
tree is what is left. No placement hint can reach it — neither the participant
index (program data) nor the tree's shape (library wiring) is a hint — which is
why this is a separate program rather than a hinted flavour.

Two changes that are only worth anything together:

1. **Place-major participant numbering** (`ocr_dist/neko_reduction.c`,
   `calcReductionIndex`): a rank's tree index is
   `place·(bx·by·bz) + (lx + bx·(ly + by·lz))`, computed from the same box
   decomposition the placement map uses (`nekbone_placeGrid`, reused so the
   numbering and the placement cannot disagree about which ranks share a
   place). It is a bijection on `[0, Rtotal)`, so the all-reduce still runs
   over every rank; what changes is which partial sums form first. On its own
   it is *worse* than doing nothing — a flat tree's internal nodes are its low
   indices, and place-major numbering puts every one of them on the first
   place.
2. **A two-level tree** (`reductionTwoLevel.c`, `red_children`): with `L`
   participants per place, a participant's children are its place-local
   `local·ARITY + j + 1` while `< L`, and a place root (`local == 0`)
   additionally takes the roots of places `place·ARITY + j + 1`. That leaves
   exactly `P-1` edges crossing between places — the least a tree over `P`
   places can have — and the upper tree's internal nodes are roots of distinct
   places, so nothing piles onto one of them. `red_L` returns `nrank` when
   there is one place or no equal grouping, so the one-place tree is
   bit-identical to the flat one.

The residual is the same sum reassociated, so it agrees with the `nekbone` row
to the tolerance rather than bit-for-bit — and, because `red_L` keys on
`Rtotal / places`, the reassociation changes at every node count. The catalog
pin must therefore hold across the whole sweep, not only at one node.

## Parameters

Identical to `nekbone`: the same eight required positional arguments, the same
validation, the same compile-time knobs. The rank lattice (`18 16 12`) and
`pDOF`/`CGcount` (`8`/`100`) are the `nekbone` row's — the rank lattice is the
machine's, not the tier's — but the catalog's element block is **not** the
same: this row runs `5 5 5` (`E = 125`) against the base row's `3 3 3`
(`E = 27`). The rewrite is in the all-reduce, but with a different grain a
share of any measured delta is size, not only structure.

## Structure

App EDT, DB, halo-envelope and reduction-scalar counts are `nekbone`'s —
neither change adds or removes a participant or a round. The two counts that
carry the `I` term (participants with children) do differ, and unlike the flat
tree's they move with the place count: `nekbone.md` gives reduction EDTs as
`(1+3N)(2R+I-1) + (2R-1+I)` and events as `5D + 6R + I - 4`, with
`I = ⌈(R-1)/ARITY⌉ = 346` at `R = 3456` for every place count. Under the
two-level tree `I` is the number of participants `red_children` returns a
non-empty set for: with `L = R/P` consecutive participants per place, the
`⌈(L-1)/ARITY⌉` local parents of each place plus the upper tree's internal
nodes, which are place roots and so already counted — 11 per place, `11P` in
all, i.e. **352 at 32 places** (`L = 108`) against the flat 346, and a
different value at every node count. What else differs is the shape of the
reduction's own tasks:

| | `nekbone` | `nekbone_dist` |
|---|---|---|
| reduction EDT dependence count | `ARITY + 2` = 12 | `RED_FANIN + 2` = 22 |
| incoming channel arrays | `ARITY` | `RED_FANIN = 2·ARITY` |
| channel install | one dependence per child | one per slot, `NULL_GUID` for every slot above the child count |
| edges crossing between places | up to one per tree edge | exactly `P-1` |

The doubled fan-in is what lets a place root hold both its place-local children
and the place roots below it in one dependence array. It is a cost this row
pays and the `nekbone` row does not, and it runs against the tier — unlike the
accumulator frees, which used to run for it (see Correctness).

## Wiring

`nekbone`'s, with one substitution: `NEKO_ForkTransit_reduction` fills
`reductionPrivate.myrank` with `calcReductionIndex(...)` instead of the rank id
and sets `nrankPerPlace = calcReductionGroup(...)`, the two values the library's
grouping reads. Everything else — the channel install through labeled `STICKY`
events, the per-rank `returnEVT` channel, the `RW` `reducPrivate` DB threaded
through the chain, the halo — is unchanged.

## Flow

`nekbone`'s. The width is still `Rtotal` serial 12-EDT chains; what shortens is
the critical path through each all-reduce, from a flat `⌈log₁₀ R⌉` levels whose
every edge may cross a node to `⌈log₁₀ L⌉` local levels plus `⌈log₁₀ P⌉`
crossing ones.

## Placement (hinted)

**This row is built with `OCR_APP_OPTIMIZED_PLACEMENT` forced on**
(`EXTRA_DEFINES` in `benchmarks/apps/CMakeLists.txt`), so it carries the hinted
tier's contiguous-box rank→place map, not the base `r % P` map. The restructure
is therefore *hinted placement + the two-level tree*, and it is measured
against the hinted tier as much as against base. The reuse is deliberate: the
numbering reads the same `nekbone_placeGrid` decomposition, so a participant's
place-local run of indices is exactly the ranks that share its place. With the
base map the grouping would still be a valid tree, but its groups would not be
places and the `P-1` crossing edges would be an accident.

## Correctness

The library this row links used to skip accumulator frees that the shared
library performs: the per-call block was destroyed only under a macro nothing
defines, the `ALLREDUCE` leaf arm destroyed nothing at all, and the `BROADCAST`
arm had lost its destroy outright. With `ndata = 1` and `1 + 3·CGcount` calls
per rank that is ~1.04M destroys skipped and ~1.04M blocks leaked per run at
3456 ranks — a row doing less work than the tier it must beat. All four frees
are now unguarded, and the two libraries' `reductionEdt` bodies differ only in
`ARITY` → `RED_FANIN`, `red_L` and `red_children`. The one-place anchor, where
both rows run the identical tree, is the control that this is so.

## Sizing

`nekbone`'s formulas verbatim: width `Rtotal = Rx·Ry·Rz` (SPMD, 1.0× the widest
geometry's worker total), grain `E·(12·P⁴ + 22·P³)` flops per rank per
iteration, memory `7·(P³·E + 1)·8` bytes per rank times `Rtotal` plus a
transient generation and the runtime's metadata. The lattice (`18 16 12`) and
`pDOF`/`CGcount` (`8`/`100`) are the `nekbone` row's, but the **element block
is not**: this row runs `5 5 5` (`E = 125`) against the base row's `3 3 3`
(`E = 27`), a 4.63× larger per-rank grain, so neither the window nor the
memory carries over unchanged. The vector term alone scales with it linearly
— `7·(P³·E)·8` bytes per rank is ~500 KiB/vector at `E = 125` (up from
108 KiB at `E = 27`), ~12.4 GB of vectors across the 3456 ranks (up from
2.7 GB) — but the object-count overhead that made up most of the base row's
15.5 GB does not move with the element block, so the new one-node total is
measured at 43 GB, not 15.5 GB. This row's `expect` is pinned at these
arguments: it is not the `nekbone` row's, since the tree reassociates the sum,
and it moves again if the sizer changes the element block.
