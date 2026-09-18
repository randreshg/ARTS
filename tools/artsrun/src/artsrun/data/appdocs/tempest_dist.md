# tempest_dist

*The restructured version of `tempest`: the same cube-sphere exchange, the same
neighbour topology and the same cross-check, with the halo batched per rank
pair instead of one datablock per patch edge, and a round delivered by the
rank's tiles in parallel.*
Source: `third_party/ocr-apps/apps/tempest/refactored/ocr/intel-bryan/tempest_dist.c`
— a separate program that reuses the base port's geometry (the 363-line
cube-sphere neighbour finder) unchanged.  Selected as `tempest:restructured`.

## Overview

The base program anti-scales harder than anything else in the roster, and the
placement layer cannot reach the reason.  Hints already do everything a hint
can: they take remote acquires from **74.93% to 1.32%** and what crosses from
**14.4 GB to 265 MB** at four nodes, leaving under half a percent of the
442,368 directed patch edges crossing a rank line.  The row still degrades
**2.75x across the first node boundary**.

What is left is not the size of the cut but the price of crossing it.  A patch
edge owns one datablock that the two patches bounce: a timestep acquires it RW
on one side, stamps eight bytes into it and hands it back, so a crossing edge
migrates exclusive ownership twice per timestep.  The payload is eight bytes --
`nbData_t` is a single `s64`, and the source says "there will be other stuff
here later" -- while a remote acquire moves **349 bytes** of protocol on
average.  Ninety-eight percent of what crosses is not data.

This tier changes that, and it keeps the change measurable.  A rank keeps its
own patches' inbound stamps in memory and batches everything bound for one peer
into a single block per round; the same stamps cross, the same number of times,
and they cross together.  Nothing else in a round is per-rank serial either: a
rank's ownership is a box of the patch strip, the box is cut into tiles, and a
tile computes its own geometry once, opens its own tasks every round and
delivers every stamp that stays inside it.  What reaches the rank's single
apply is only what left a tile.

Why placement cannot do this: the dependence graph is a mesh, not a tree.  A
divide-and-conquer program (`nqueens`, `fib`) has independent subtrees, so
placing the top levels and letting the rest follow its creator removes cross-
rank traffic entirely -- an edge is traversed once.  Here every patch talks to
eight neighbours every timestep forever, so any partition leaves a cut and the
cut is paid `duration` times.  Placement minimises the cut; only a change of
what a cut edge costs can remove what remains.

## Parameters

Three arguments, all optional, positive-integer validated and range-checked
(`strtol` overflow and a bound on `patchRange` that keeps every size derived
from `6k^2` inside `u64` are both rejected with the usage message):

| argv | name | role | default |
|---|---|---|---|
| 1 | `patchRange` (k) | size: `6k^2` patches, `6k^2` tasks per round | 2 |
| 2 | `duration` | height: `duration` rounds, `duration - 1` exchanges | 100 (`DURATION`) |
| 3 | `groups` | grain: the target number of tiles a rank cuts its box into | 512 (`TEMPEST_GROUPS`) |

`patchRange` and `duration` are the same two dials as `tempest`.  `groups` is
this row's own knob and it sets two things at once: the width of a rank's spawn
and delivery level, and — because a tile delivers every stamp that stays inside
it — how much of a round's exchange escapes a tile and has to pass through the
rank's one serial point.  Bigger tiles mean less escapes and less width.  The
realised tile grid is the factorisation of the box closest to square, so the
actual group count is near `groups` rather than equal to it.

`duration` is at the shipped default and the exchange matches the base program
exactly: the base forwards on generations `0 .. duration-2` and its final
generation only reads, so both programs perform `duration` rounds, `duration-1`
sends and `duration-1` receipts, and no generation is left unconsumed.

## Structure

Per rank per round, four kinds of task:

- one **patch** task per owned patch, exactly as the base program has, each
  doing the same eight stores of its own patch number into that patch's own
  persistent slice.  Where each of those stamps has to land was decided once,
  at setup, so a round evaluates no geometry;
- one **group** task per tile, which reads its patches' slices and delivers
  every stamp landing on one of its own patches straight into its own view,
  copying only the rest into the tile's escaping block;
- one **apply** per rank, which routes what escaped: into the view of another
  tile of the same rank, or into one outgoing block per peer;
- one **spawn** task per tile, which opens the next round for that tile: it
  creates the tile's patch tasks and its delivery task, and wires the delivery
  task into the next apply.

Two things follow.  The round boundary is not a serial spine: the apply creates
only the next apply and the `groups` spawn tasks, a count bounded by the knob
rather than by the problem, and each spawn task then creates its own tile's
patch tasks in parallel with the others.  And the apply's own work is the
perimeter of the rank's tiles, not the number of patches it owns — at `k = 240`
on one node with the default `groups`, 5.86% of the round's stamps reach it;
at 32 ranks, where a rank owns 32x fewer patches, 30.5%.

Every hand-off is an output event -- a patch task passes its slice on through
its output event, a delivery task passes its escaping block -- so a consumer
acquires a block only after its producer has released it.  Sharing a datablock
guid orders nothing on its own.  The one buffer reuse that is not carried by an
event is a round's own: an apply creates the producers of round `t+1` only
after its last read of every block round `t+1` overwrites, so the writer of a
buffer is ordered behind that reader's accesses by the creation itself.

## Wiring

Setup runs in two phases so no rank ever registers a dependence on a name that
has not been created: `mainEdt` creates every labeled name and per-rank block,
phase one fills them, phase two reads them.  A channel is created by the rank
that RECEIVES on it and published at the label `sender*nranks + receiver`;
labeled channel ranges are not among this OCR's labeled kinds, so the guid
rides a labeled sticky, as the other rank-persistent ports here do.  Each rank
seeds its own incoming channels once: without a seed generation every rank's
first apply waits on peers whose first apply is waiting on it.

Within a rank, `rankStartEdt` inverts the ownership map, counts what it owes
each peer, creates one state block per tile and one **build** task per tile.  A
build task is what makes the tile's patch numbers, the destination of each of
their eight stamps, its `n` slices, its view and its escaping block, so the
geometry is evaluated once per patch and the tiles of a rank build in parallel.

A datablock dependence is satisfied the moment it is added, so `rankStartEdt`
releases each block before anything is wired to it — a task must never become
runnable while its producer still holds the block it will read.  The rank's
state block is written only by `rankStartEdt`; every later task takes it
read-only, so no round boundary is a rank-wide exclusive turn.

## Flow

A patch task stamps its own number into the eight slots of its slice, one per
direction that has a reverse link.  The reverse direction is derived from the
topology rather than assumed: the cube sphere reverses orientation across some
face seams, so a patch's north neighbour does not always have it to the south,
which is why the base port learns each reverse link by exchange instead of
computing it.

The tile's delivery task turns each stamp into either a write into its own view
or an entry of its escaping block, by a table lookup — the destination, its
tile and its element were all resolved at setup.  The rank's apply then routes
the escaping entries: one that names another tile of this rank becomes a write
into that tile's view, one that names a peer is appended to that peer's block,
and a peer's whole round crosses as a single block carrying `(to, dir, from)`
triples.  The triple travels rather than an agreed ordering, so the two sides
need agree on nothing beyond the geometry they both compute.

The apply consumes every incoming block before it publishes any outgoing block,
so a peer that is one round ahead can never overtake a reader of what it sent.
The last round publishes nothing — its stamps would have no reader — and the
rank that owns `TEST_PATCH` prints the cross-check and announces the rank done.

## Placement (base)

Patches have spatial homes on the minimum-cut `Pn x Q` map the hinted tier
offers as a hint, here structural.  The six faces unroll into a `k x 6k` strip,
and the strip is cut into a `Pn x Q` grid of boxes whose total cut length is
minimal over the factorisations of the rank count; the same cut, applied again
inside a box, makes the tiles.  Cutting an axis of length `L` into `m` parts at
`ceil(i*L/m)` puts `x` in part `floor(x*m/L)`, so routing (`homeOf`) and
indexing (the box, the tile, the element) are exact inverses of one another; a
disagreement would deliver a stamp to a rank with no slot for it.  A rank
therefore enumerates its own patches without touching the sphere.

The map divides evenly at every geometry the campaign uses: at
`k in {48, 96, 192, 216, 240, 288, 864}` (the last is the catalog's own
choice) and `nranks in {1, 2, 4, 8, 16, 32}` every rank owns exactly
`6k^2/nranks` patches (imbalance 1.000, derived arithmetically).

Every task and every block a rank owns carries that rank's affinity, so a
patch's slice, its tile's state, view and escaping block and the rank's
outgoing blocks are all born where they are used.

There is no separate `hinted` version: the decomposition is the placement.

## Sizing

`duration` keeps the shipped default -- the README calls the run "a few
timesteps" and `go.sh`'s recommended argument is the patch range -- so `k`
reaches the window, and the width follows: `6k^2` patch tasks are runnable at
once in every round.

`6k^2` is an exact integer multiple of the largest geometry's 3456 workers
exactly when `24 | k`, since `6(24j)^2 = 3456 j^2`.  The reachable points
include `k = 48` (4x), `96` (16x), `192` (64x), `216` (81x), `240` (100x),
`264` (121x), `288` (144x) — and the catalog's `k = 864` (1296x), landed on
by the memory ceiling below rather than the width floor.  The previous
calibration's `k = 248` was **not** one of them (106.78x).

Memory, per rank, with `nlocal = 6k^2 / nranks`, `ngroup` tiles and `nesc` the
stamps that leave a tile:

```
tile state    80 * nlocal + 64 * ngroup    bytes  (patch numbers, destinations, slice guids)
slices        64 * nlocal                  bytes  in nlocal datablocks
views         64 * nlocal                  bytes  in ngroup datablocks
escaping      32 * (nesc + ngroup)         bytes  in ngroup datablocks
outgoing      24 * (duration-1) * sum(peerCount+1)  bytes, never reclaimed
```

so about `208 * nlocal` bytes plus the escaping term — 77 MB at `k = 240` on
one node, 103 MB at `k = 288` — plus per-datablock runtime overhead on
`6k^2 + O(groups * nranks)` live objects, which is the term that actually
dominates.  The outgoing blocks are the one term linear in `duration`: they are
small (one per peer per round) but nothing destroys them, so "memory does not
grow with the run" is not true of this program -- it grows slowly.  Against a
190 GB budget the total never binds, so the size may be chosen from the width
window alone.

**The campaign now runs `k = 864`.**  Every timing number this section used to
carry -- the ladder at `k = 96/224/248/272`, the three-arm anchor, the
four-node counter comparison and the one-to-eight-node trend -- was taken on
an earlier structure whose measured time was dominated by per-rank serial
work this version removes, so none of them transfer.  The re-derived size is
memory-capped rather than time-capped: the per-datablock runtime overhead
term above puts one node at 127 GB resident at `k = 864`, against the 190 GB
budget, so the rung that would reach the 120 s restructured-tier window on
time exceeds the per-node memory cap first.  The one-node anchor is 42.3 s;
the node-count trend is still to be taken.

The cross-check is unchanged: the rank that owns `TEST_PATCH` prints the marker
`*CROSS-CHECKING NEIGHBOR DATA EXCHANGE*` and the 3x3 grid of the patch numbers
that reached its eight slots.  The grid is pure topology, so it is invariant in
node count, runtime and `duration`, and the driver's scalar -- the last number
of the third row, the SE neighbour of patch 0 -- is `5k^2 + 1`: **11521** at
`k = 48`, 288001 at `k = 240`, 307521 at `k = 248`, and **3,732,481** at the
catalog's `k = 864`.  That is the same value the
base row's oracle pins, so the two rows share one known answer.  The grid is
complete from `duration >= 2` on more than one rank (a stamp that crosses is
sent in one round and received in the next) and from `duration >= 1` on a
single rank, where nothing crosses; at `duration = 1` on several ranks the
remote slots print `-1`, which is the same nothing the base program's single
read-only generation produces.
