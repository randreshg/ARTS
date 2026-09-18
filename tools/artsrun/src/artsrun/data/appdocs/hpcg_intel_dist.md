# hpcg_intel_dist

*The same preconditioned conjugate gradient, the same 4-level V-cycle and the
same tile grid as `hpcg_intel`, with the exchange's CONTAINER replaced: one
persistent, double-buffered datablock per (tile, destination policy domain) per
multigrid level instead of one fresh block per direction per exchange — so a
halo payload crosses the fabric once per reading domain rather than once per
reading tile, and the steady state creates and destroys nothing.*
Source: `third_party/ocr-apps/apps/hpcg/refactored/ocr/intel-dist/hpcg.c`
(~2500 lines) + `timers.c`, linked against the `reductionTwoLevel` library
(`apps/libs/src/reductionTwoLevel/reductionTwoLevel.c`, `ARITY = 10`).

## Overview

This is the restructured tier of `hpcg_intel`. The arithmetic, the tile grid,
the recursive-bisection placement, the command line and the printed lines are
that row's; nothing about the physics or the decomposition of *work* changes.
What changes is what a domain boundary costs.

The base row's exchange creates one datablock per outgoing direction, satisfies
that direction's channel event with it, and lets the consumer destroy it. With
11 exchanges per CG iteration and up to 26 directions per tile that is **286
creates and 286 destroys per tile per iteration**, 220 of them (77 %) carrying
92 B. The consequence is not bytes — it is that the number of blocks a node
sends is set by the tile ADJACENCY, which does not shrink when the node count
grows, while the compute per node falls as 1/N. Measured: 25 % of the
achievable `15N/16` at 8 local nodes, a 4.03× core-second inflation, and a
cross-node block count per node per iteration that peaks in the middle of the
sweep and is flat at its ends. Neither placement (measured from four angles on
the base row; lost 1.12–1.65× in every arm) nor grain (`m = 32` is the last
multiple of 16 inside the node memory budget) can move it, because a home is
where a block lives and the grain does not change how many blocks there are.

This program keeps **one block per (tile, destination domain) per level**, for
the whole run, in two copies alternated by round. Each of a tile's outgoing
directions packs its own slice of the block belonging to its neighbour's
domain; each neighbour tile on that domain reads its own slice out of that one
block. The 26-way join is untouched — a consumer's 26 slots are still fed by 26
*distinct* source tiles, so no two slots ever name one block — and the sharing
is across CONSUMERS, which is precisely what turns "once per reading tile" into
"once per reading domain". The bytes on the wire are identical: a domain's
block is the concatenation of exactly the slices the per-direction blocks
carried, in direction order.

Second, the reduction. The base builds its 10-ary tree on the tile id, which is
unrelated to where a tile runs, so `(N−1)/N` of its blocks cross a domain
boundary. This row links `reductionTwoLevel` and numbers the participants
domain-major, so a domain reduces among its own tiles and only the domains'
roots reduce with each other — the same substitution `nekbone_dist` makes, and
for the same reason: renumbering *alone* was tried on the base row and lost
(1.00 / 0.92 / 0.84× at 2/4/8 nodes), because a flat tree's internal nodes are
its low indices and domain-major numbering piles every one of them onto the
first domain. The two-level construction is what makes the numbering pay.

## Parameters

Identical to `hpcg_intel` — same parser, same validation, same defaults, same
`bomb()` on a malformed argument list.

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `argv[1..3]` = `npx,npy,npz` | tile grid; `N = npx·npy·npz` | 3,4,5 (`N=60`) | ✓ parsed in `mainEdt`, copied into the shared DB every tile reads RO |
| `argv[4]` = `m` | tile cube edge, rounded up to a multiple of 16 | 16 | ✓ same path |
| `argv[5]` = `maxIter` | CG iterations; capped at `HPCGMAXITER` (50) | 50 | ✓ same path |
| `argv[6]` = `debug` | print level 0/1/2 | 0 | ✓ same path |
| `HPCG_NO_TWO_LEVEL_REDUCTION` | leaves the participant numbering at the identity and `nrankPerPlace` at 0, so the library builds the flat tree over tile ids — the base row's tree exactly | off (two-level on) | ✗ compile-time, for attribution |
| `PRECONDITIONER`, `COMPUTE`, `AFFINITY`, `TIMER`; multigrid depth; `ARITY` | as the base row | on,on,on,off; 4; 10 | ✗ compile-time |

The base row's disclosed conformance adaptations are inherited verbatim
(loud argument validation, `bomb()` exiting, the reduction library's accumulator
free, the per-iteration convergence lines behind `debug > 0`). The
`OCR_APP_OPTIMIZED_PLACEMENT` halo-home layer is **removed**, not disabled: it
targeted a per-exchange block that no longer exists.

## Structure

Write `mt_l = (m/2^l)³`, `L` for the number of directed neighbour links, and
`D = Σ_tiles |{domains a tile's neighbours live on}|` — the number of
(tile, destination domain) pairs, which is what this row counts instead of `L`.

| object | count | size |
|--------|-------|------|
| private block | `N` | `≈ 429·m³` B, plus ~3.4 kB of tables: the 26-entry destination map, the per-level slice offsets for both ends, and `4·2·D/N` block GUIDs |
| shared / reduction-private / scalar blocks | 1 / `N` / `N` | 56 B / struct / 8 B |
| **exchange blocks** | `8·D`, **created once** | per (tile, domain, level): the sum of that domain's directions' slice lengths, ×8 B, ×2 copies |
| EDT creates | `55N + 3(2N+I−1)` per iteration | unchanged from the base row: the pack loop stays inside `haloExchangeEdt`, so the aggregation adds no task |
| Event creates | `37N` per iteration | unchanged |
| **DB creates** | `9N − 6` per iteration | the three reductions only. The base row's `11L` halo creates and `11L` destroys are **gone** |
| one-time | as the base row, plus `8·D` exchange blocks | the exchange blocks are created in each tile's own `initEdt`, released immediately, and held only by an exchange that is writing them |

`haloExchangeEdt` grows from 1 dependence slot to 27: the private block RW plus
this round's exchange blocks RW, one per destination domain, with the slots past
the domain count empty (as the boundary directions' channel events already are).
The RW dependence is what makes the write legal — a task may write only a block
it holds — and not what makes it safe; see Wiring.

At the campaign `24 12 12 32 50` (`N = 3456`, `m = 32`, `L = 77,464`):

| | base | this row |
|---|---|---|
| DB creates per iteration, cluster-wide | 883,202 | 31,098 |
| over 50 iterations | 44.2 M | 1.55 M |
| halo creates + destroys removed | — | **42.6 M + 42.6 M** |
| exchange blocks held, 1 → 32 nodes | 0 | 27,648 → 59,584 |
| exchange-block memory | 0 | **435 MB cluster-wide, 129 KB per tile** — 0.9 % of the 48.6 GB of private payload |

Cross-node halo blocks per node per iteration (a block counted once per reading
domain), replayed over `getMyPDc`:

| nodes | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|
| campaign `24 12 12`, base | 12,716 | 19,074 | 15,521 | 10,576 | 8,104 |
| campaign, this row | 1,584 | 2,376 | 2,178 | 1,666 | **1,372** |
| ratio | 8.03× | 8.03× | 7.13× | 6.35× | **5.91×** |
| trend `10 6 6`, base | 2,816 | 3,696 | 2,849 | 1,964 | 1,405 |
| trend, this row | 396 | 594 | 561 | 456 | 393 |

The bytes are unchanged at every entry. The budget this buys: at 32 campaign
nodes the base needs 5.4 µs per remote send to reach linear speedup, against a
26.0 µs measured remote read-acquire round trip; at 1,372 blocks the same
requirement is 21.2 µs — inside the round trip rather than 5× beyond it.

## Wiring

The tile spine is the base row's and unchanged: `hpcgEdt` → `mgEdt` →
`haloExchangeEdt` → `unpackEdt` → `smoothEdt`/`spmvEdt`, each hop a fresh
`OCR_EVENT_ONCE_T` carrying the private block RW, exactly one holder at a time.
The neighbour rendezvous is unchanged too: `initEdt` reserves `26N` labeled
STICKY GUIDs, ships a CHANNEL event GUID through the sticky at
`26·myrank+ind`, collects the mirror at `26·neighbour + (25−ind)`, and
`channelInitEdt` installs the arrivals as `haloRecvEVT[]`.

What changed is what a channel event carries and who owns it.

**The slice map is geometry, published nowhere.** A tile packs direction `d` at
the offset given by the lengths of every lower-numbered direction it sends to
the same domain. A receiver reading on direction `i` is receiving what its
neighbour packed on direction `25 − i` — the 26 directions are enumerated `k`,
then `j`, then `i` over −1..1 with the centre skipped, so a direction and its
opposite always sum to 25 — and it replays that same sum over the *sender's*
geometry. Both ends evaluate it once, in `initEdt`, from `npx/npy/npz` and the
same `getMyPD` bisection that places the tiles. No offset, no length and no
block identity is ever exchanged to set the layout up.

**A block is released once and satisfied several times.** The pack loop fills
every slice of every block; only then are the blocks released, and only then are
the channel events satisfied — so a reader never observes one direction's values
under a sibling direction's write.

**Two copies, and why two are necessary and sufficient.** A persistent buffer
needs the producer's next write ordered after the consumer's read, and the
program's own chain does not give that for one copy: a tile's pack in round e+1
is ordered after its neighbours' PACKS in round e (through `unpackEdt`, which
waits on all 26 channel events) but **not** after their UNPACKS, which are what
read the buffer. So one copy lets a producer overtake a reader by one round.
With two copies the first write that could reach an unread generation is in
round e+2, and

    pack(e+2) ← unpack(e+1) ← neighbour pack(e+1) ← neighbour unpack(e)

orders it after the read: the outer edges are the private block flowing through
each tile's own chain, the middle edge is a channel-event satisfy. Two copies is
also the bound the base row's channel events already carry as `maxGen = 2`.
The alternation is kept per level in the private block and chosen by the task
that CREATES the exchange, which is the task holding the private block.

The dependence modes are not what makes any of this safe. Under a
validation-family protocol a reader is never blocked by a writer, so the RW
dependence buys a legal hold and nothing more; the ordering is the event chain
above, which every arm of the plane honours identically.

**The reduction.** `reductionTwoLevel` has the same interface and the same
`reductionLaunch` call sites; `initEdt` additionally sets `nrankPerPlace` and
replaces `rpPTR->myrank` with the participant's domain-major index. Both are
derived on every rank from the grid and the bisection alone — `evenSplit` walks
the bisection itself rather than the ranks — so every rank builds the same tree.
Where the bisection does not give every domain the same tile count, or at one
domain, `nrankPerPlace` stays 0 and the library builds the flat tree over the
identity numbering, which is the base row's tree bit for bit.

## Flow

Identical to the base row: `mainEdt` spawns `N` `initEdt`s (each running its own
`matrixfill` preamble on its own node), the channel rendezvous, then per tile
`hpcg` p0 `rtr` → p1 convergence test → `mg` V-cycle (`mgStep` 0..6,
`level = 3−|mgStep−3|`, 17 `mgEdt` bodies, 10 exchanges) → p2 `rtz` → p3 halo +
SpMV → p4 `pAp` → p5 update → p1. Parallel width is `N` and only `N`. An
iteration is still 11 exchange barriers plus 3 reduction barriers — **the
restructure removes no synchronisation and adds none**; it changes only how many
blocks each barrier's data arrives in.

`initEdt` does three things more than the base row's, all `O(26)` or
`O(N)` per tile and all before the measured window's first iteration: the
destination-domain map, the per-level send and receive slice offsets, and the
`8·nsend` block creates. The domain-major participant index costs one pass over
the tiles below this one.

## Placement

There is no `_hinted` family, and the reason is stronger than "it was not
tried": the base row's hinted layer exists, was debugged (a `u32` neighbour-rank
wrap voided four campaigns before it was fixed) and was measured from four
angles, and it lost at every multinode geometry in both write policies. Its
counters say why — under write-back it changed nothing it targeted
(`NUM_DB_ACQUIRE_REMOTE` 1,237,628 → 1,237,747) and added two messages per moved
block; under write-through it removed 92 % of the remote acquires and still lost,
because it added three.

Placement in this row is the design instead. The tile→domain map is the
author's recursive bisection, balanced 1.000 with compact 3-D sub-boxes at every
campaign node count. Every EDT is pinned to its tile's domain. An exchange block
is created by, and written by, exactly one tile, so the creator-home default
puts it where its only writer is; under write-back it stays with that writer and
moves on demand, which is the behaviour the aggregation is built around. And the
reduction, the one plane the base row could not hint at all (partials are minted
inside the library), is placed by the two-level tree.

## Sizing

The catalog carries the base row's arguments and the base row's pin.

`expect` is `14248044.343658` at `24 12 12 32 50` — the base row's own value at
the base row's own arguments. That is not a coincidence to be checked once and
forgotten: at ONE domain the two-level tree *is* the flat tree over the identity
numbering, and the exchange change moves the same doubles into the same halo
positions in the same order, so the one-node cell must reproduce the base row's
digits **bit for bit**. It is the cross-tier identity gate.

Above one domain the exchange stays exact and the reduction does not: the
two-level tree reassociates the sum, so `final deviation` agrees to the row's
`1e-4` tolerance rather than bit for bit. This is the same disclosure
`nekbone_dist` carries, and `HPCG_NO_TWO_LEVEL_REDUCTION` recovers exactness at
every node count if a measurement needs to separate the two changes.

The scalar is a convergence residual, not a size-invariant checksum, so it moves
with the grid: `expect` is REGENERATED whenever the arguments change, never
edited, and `expect_args` is kept identical to `args` so the pin rides the
campaign cell.

Width, grain and memory are the base row's constraints unchanged — `N` must be
an integer multiple of the widest geometry's 3456 workers, `m` a multiple of 16
(four multigrid levels), and payload `≈429·m³·N`, which puts `24 12 12` at
`m = 32` at ~105 GB RSS and makes it the last rung that fits. The exchange
blocks add 0.9 % to that. The restructured tier's own 120 s window is set by
re-measuring the one-node anchor after the rewrite; until that is done the row
carries the base row's arguments so the two tiers are read at one instance.

## Family shape

`hpcg_intel` and `hpcg_intel_dist` are one row: the same program's answers to
the same problem, in increasing order of how much of the *wiring* was rewritten.
The pair is the exhibit for a specific claim — that where a program's cross-node
cost is a message COUNT set by its decomposition, no placement and no grain can
reach it, and only a change of container can. The base row is the evidence for
the first half (a hinted layer built, debugged and measured, and beaten by
doing nothing); this row is the evidence for the second.
