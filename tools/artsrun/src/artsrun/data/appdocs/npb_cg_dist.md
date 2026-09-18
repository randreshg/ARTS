# npb_cg_dist

*The restructured version of `npb_cg`: the same NPB CG power iteration on a
**row-band SPMD decomposition** — one persistent chain per rank, a
channel-event fragment exchange in place of the whole-vector broadcast, and
library allreduces in place of single-EDT dot products.*
Source: `third_party/ocr-apps/apps/npb-cg/sdsc-ocr/` — a separate program
(`cg_dist_edt.c` + `cg_dist.h` + `cg_dist_makea.c`) sharing the base port's
random stream, matrix construction and utilities (`cg_gen.h`, `makea_ocr.c`,
`util_ocr.c`) but not its decomposition.  Selected as `npb_cg:restructured`.

## Overview

The base program anti-scales for four structural reasons no hint reaches:
every matvec broadcasts a *fresh* whole vector to `na/blk` readers, gathers
`na/blk` result fragments back into one EDT, runs its dot products and
axpys as single whole-vector EDTs on the critical path, and pays task
overhead per a few thousand flops of grain.  The rewrite dissolves all
four at once.  Rank `i` owns rows `[i·na/R, (i+1)·na/R)`: its band of the
matrix, its fragments of every vector (`x z r p q` packed in one per-rank
datablock), and a persistent chain of EDTs pinned to its own rank that
never hands a vector datablock to anyone.  Per matvec, each rank publishes
one immutable `nloc`-long fragment per peer over persistent channel events
— written once, read once, destroyed — and the two CG reductions travel as
3-double scalars through the reduction library's allreduce.  The chain
fuses the vector arithmetic into the same EDTs that consume the reduction
results, so an inner iteration's control plane is four EDTs per rank
(bcast → spmv → join → alpha, plus beta on the continuing path) regardless
of problem size.

**Within** a rank the product is a fan-out, not a single task: the band is
cut into `-c` row slices, one task per slice writes its own rows of `q` and
returns the partial dots those rows contribute, and a join sums them before
launching the allreduce.  Without it a rank's chain occupies exactly one
worker — the whole point of a 108-worker node is lost, and the run gets
*slower* as the node gets wider.  The slices are also where the band lives:
each slice datablock is created by the task that builds it, so the band is
first touched across the rank's workers instead of being written by one
worker and then read by all of them through that worker's memory
controller.

**The matrix is built where it lives, in parallel.**  The published
generator is one serial sweep: for every source row it scatters a row of
contributions into whatever destination rows that row's entries name,
inserting each into a sorted-by-column list.  It cannot be split by source
row, because two sources write the same destination — which is why the
serial form looks inherent, and why it was this program's largest floor.
Inverting the pairs removes it.  For every DESTINATION row, list the
`(source row, entry)` pairs that name it; building that index is one pass
over the `na·(nonzer+1)` generated entries, not over the
`na·(nonzer+1)²` contributions.  Afterwards each destination row is built
alone, by exactly the insertions the serial sweep would have performed on
it and in the same order, so there is no exchange, no bucketing across
ranks and no sort, and the matrix comes out **bit identical** to the
published one.  Each row slice is therefore constructed by the task that
will multiply it: the band is never assembled anywhere else and nothing is
copied to a rank.  The same implementation builds the base program's row
blocks (`cg_gen.h`), so both tiers pay the same construction.

**The index pass is divided, not replayed by one worker.**  Scanners — one
per contiguous range of source rows, `clamp(np/(2·nchunk), 1, nchunk)` of
them, so the fan-out widens only while a scanner's per-bucket histogram
stays cheaper than the scan it divides — each count their own pairs per
destination slice, prefix inside their own reserved region of the index, and
scatter.  No global merge is needed: a scanner writes only inside its own
region, whose bound is its own entry count.  Because the ranges are walked in
source order and each scanner's segment is in source order, a row's pairs come
out in exactly the order the serial sweep visited them, which is what makes the
result bit identical.  A cross-rank division of the same pass was weighed and
rejected: on top of this it would take a rank's scan from `na·k/nchunk` to
`na·k/(R·nchunk)` at the cost of an init-time all-to-all of the whole pair
array and a second bucketing pass on the receive side — at `nchunk = 216` the
intra-rank division is already narrower than a 32-way rank split would be.

What stays serial is what cannot be anything else: the draw stream
(`sprnvc` rejects on both range and duplicate, so a row's draw count is
data-dependent and no jump-ahead exists) and the running scale factor (a
product accumulated in row order).  Nothing is generated centrally and
shipped, though — as in the reference MPI implementation, **every rank
replays the same stream itself** and indexes only the rows it owns.  So the
replay costs one rank's time however many ranks there are, the construction
inputs never cross the wire, and they are released as soon as the build
that consumed them finishes.

Built-in run discipline: an untimed warm-up pass precedes the measured
passes; the reporting rank's shutdown EDT is gated on the final EDT's
output event (releases complete before shutdown); and a last collective
holds every rank in the run until the report, so no rank's chain is still
executing when the runtime comes down.

## Parameters

`-t` class, `-i` iteration override, `-c` row slices per rank (0 = derive).
Arguments are flag/value pairs walked forward and every flag must be recognised
and must carry a value; an unknown flag, a dangling flag or an unknown class
prints usage and shuts down rather than being skipped.  `-b` is **not**
accepted: nothing in this program blocks the matrix, so a blocking dial would
be a knob with no effect — and it is now rejected loudly rather than ignored.
Other loud failures: more than `CG_DIST_MAX_RANKS` (64) ranks, `na < nrank`,
and a `-c` above `CG_DIST_MAX_CHUNKS` (4096) or above the `nloc` rows a rank
owns.  That last one used to clamp silently, which would have measured a width
other than the one the run recorded.

## Wiring

Each rank's chain hands four datablocks RW from link to link — the private
state, the packed vector block, the timer (reporting rank only) and the
reduction-library private block — so they live on their creating rank
forever.  Channel GUIDs are exchanged once at setup through a labeled
sticky range (`nrank²` events, racing creators legal); fragments ride the
channels as fresh datablocks with `maxGen 8` headroom, and the per-
iteration allreduce bounds chain skew, so a generation can never be
overrun.  The final pass's residual matvec reuses the same bcast/spmv
links with the `residual` flag switching the operand from `p` to `z` and
the successor from `alpha` to the outer-iteration EDT.

Setup is a short chain of its own, because a slice builder cannot be wired
before the index it reads exists: rank-init draws the stream, reserves the
index and issues the scanners; a build-spawn EDT gated on all of them issues
the `nchunk` builders into the slice join; the slice join records the slices,
disposes of the construction arrays, and its output event releases the
channel-init EDT that starts the solve chain.  The private block travels RW
along exactly that order.

## Structure

Per rank per inner iteration: 1 bcast + 1 spmv + `nchunk` slice tasks +
1 join + 1 alpha (+1 beta except on the last); `R−1` fragment datablocks
created and destroyed; `nchunk` partial datablocks created and destroyed;
one allreduce of 3 doubles.  With `-c` unset, `nchunk = min(256,
ceil(nloc/16))`; with `-c n` it is `n`, and a rank always offers its workers
more tasks than they have hands.  Per outer iteration: 25 inner iterations,
one residual matvec, one outer EDT.  Setup, per rank: 1 rank-init, `nscan`
scanners, 1 build-spawn, `nchunk` row builders, 1 slice join, 1 channel init;
each scanner and builder carries an output event; a scanner creates one
scratch datablock and a builder two (its pair/pointer/capacity/cursor
workspace and its per-row column/value workspace) plus its slice and a
reference to it, i.e. four per builder.  Events:
persistent channels (`2·R·(R−1)` total) plus the one-time labeled
stickies; nothing grows with the iteration count.

## Flow

One chain per rank, and inside an inner iteration the links fire in the
order conjugate gradient asks for: a bcast publishes the rank's fragment of
the operand, an spmv fans out `nchunk` row-slice tasks over the rank's rows,
a join folds their partials, an alpha reduces `p·q` and forms the step, and
a beta reduces `r·r` and forms the direction — beta on every inner iteration
but the last.  An outer iteration is 25 of those plus one residual matvec,
which reuses the same bcast/spmv links with a flag switching the operand
from `p` to `z` and the successor from alpha to the outer EDT.  Setup runs
per-rank init, the scanner fan-out, the build spawn, `nchunk` row builders, a
slice join and a channel-init EDT before the first outer iteration.

## Placement (base)

The decomposition is a band of rows a rank, and the placement follows from
it rather than from a hint: each rank's chain hands its four datablocks --
the private state, the packed vector block, the timer on the reporting rank
and the reduction library's private block -- RW from link to link, so they
are created once on that rank and stay there for the whole run.  Every setup
EDT — scanners, build spawn, builders, slice join, channel init — carries an
explicit `AFFINITY_PD` hint at its own rank, which is also what makes the
scanners' disjoint `RW` writes into one index datablock a disjoint write
rather than a migration.  Fragments ride persistent channels as fresh
datablocks, so what crosses is the operand fragments and the two reductions,
never the matrix: every rank replays the draw stream itself, so the matrix is
built where it is used and construction never crosses the wire.

There is no separate `hinted` version.  The row band is the placement, and a
hint layer could not express or change it.

## Sizing

The catalog pins class D with `-c 216`.

**Width.**  The instantaneous frontier is `W = R·c`, and `c` is node-invariant
under strong scaling, so the widest geometry decides it:

    R·c = m·3456  at R = 32   ⇒   c = 108·m ;   m = 2  ⇒  c = 216,
    W = 32·216 = 6912 = exactly 2 × 3456.

216 is also exactly twice a node's 108 workers, so **every** node count runs two
balanced waves per rank.  The derived default (`nchunk = 256`, saturated at
every rung for class D) gives 8192 = 2.37× and leaves a 40-worker third wave —
above the 2× floor but neither a multiple nor balanced.  Constraints hold:
`216 ≤ 4096` and `216 ≤ nloc = na/R` (46 875 at class D over 32 nodes; the
bound only bites past `R = 6944`).  This is the SPMD carve-out of the
parameter-invariance rule: the per-rank width is fixed by the argument, while
the aggregate width and the replayed draw stream scale with `R` on purpose.

**Class.**  Class D is the largest class that runs; the next section says why E
is a cliff rather than a cost, and `-i` cannot bridge the gap because class D
reaches the benchmark's own 1e-8 bar only near iteration 85 of its published
100, so the published count stands.

**Memory (one node, `-t D`).**  Slices dominate:
`Σ cap ≤ na·(nonzer+1)²·12 B` = `1.5e6·484·12` = **8.71 GB** (8 B value +
4 B column index per reserved slot).  Transient construction inputs: `acol`
132 MB, `aelt` 264 MB, the pair index 264 MB, `arow`/`rowoff`/`scale` 30 MB,
the scanner bucket bounds `8·nscan·(c+1)` = 375 kB — all destroyed at the slice
join.  Packed vector `(5·nloc + na)·8` = 72 MB.  **≈ 9.5 GB**, inside the
190 GB one-node budget by a factor of twenty.  At 32 nodes each rank holds
272 MB of slices, ~690 MB of replicated construction inputs and 14 MB of
vector, ≈ 1.0 GB.  Note that the pair index is reserved at the whole entry
count on every rank rather than at the band's share, because a scanner reserves
its region before it knows how much of it lands in the band; the surplus is
never written, so away from one node it costs address space rather than pages.

**Anchor at these arguments, one node of 108 workers: 298.2 s, 15 GB
resident; target 120 s (restructured).**  The construction moved from one
serial prologue per rank to a divided index plus a per-slice parallel build,
so the previously recorded anchor figures do not carry over; the node-count
curve above one node is what a fresh campaign sweep takes.  Class D remains
the pinned rung.

## What still bounds it

- **The draw stream — the benchmark's own Amdahl term, and worth reading off
  the curve rather than hiding.** `sprnvc` rejects both out-of-range draws
  and duplicates, so a row's draw count is data-dependent and no jump-ahead
  into the LCG exists; drawing in parallel would consume the stream in a
  different order and produce a different matrix, i.e. a different answer.
  It is therefore replayed whole on every rank and does not shrink with the
  node count.  It is now the *whole* flat floor: the capacity pass is gone
  (each builder derives its own rows' reserved counts while it groups its
  pairs) and the index pass is divided over the rank's own workers, so the
  0.93 s those two used to add at class D is no longer flat — only the draws
  and the `O(na)` scale recurrence are.  The last measurement of the draws
  alone was 0.756 s at class D and 0.072 s at class C; the fraction of a
  32-node cell they represent is `calibration pending` with the rest.
- **Two collectives per inner iteration.** That is conjugate gradient, not
  this port: `p·q` must be reduced before `α`, and `r·r` before `β`.
  Removing one needs a different CG variant (Chronopoulos–Gear), i.e. a
  change of numerics rather than of decomposition.
- **The product is gather-latency bound, not bandwidth bound** — which is
  what NPB CG is *for*.  Widening a node buys memory-level parallelism, not
  bandwidth: at class C one NUMA node's 15 workers **beat** all 108
  (12.7 s vs 16.8 s of solve for the same 40 iterations), and at class D
  7.2× the workers buy 1.76× (69.9 s vs 39.6 s at `-i 5`, ~44 GB/s of
  matrix stream).  Page
  placement is not the reason — `numactl --interleave=all` moves it 2–4%.
  Cache-blocking the gather would change the summation order, i.e. measure
  a different benchmark.
- **A `Θ(na)`-per-rank operand assembly, flat in the rank count.** Each rank
  publishes `nloc` doubles to each of `R−1` peers and assembles a full
  `na`-long operand before every product, so per-rank receive volume does not
  shrink with `R` and aggregate traffic grows as `Θ(R·na)` — ~12 MB into each
  rank per matvec at class D over 32 nodes.  NPB's own MPI reference avoids
  this with a 2-D `√p × √p` partition costing `Θ(na/√R)` per rank.  Whether it
  binds here is open: compare `[E2E]` at 16 vs 32 nodes, or the transport byte
  counters at 8 vs 32.  If it binds, the 2-D partition is the honest next
  version.
- **The class ladder has no rung at the anchor's budget.** Class E is a
  cliff rather than a cost — its operand vector is 72 MB, so every gather
  is a DRAM access plus a TLB page walk (class C's is 1.2 MB, class D's
  12 MB), and one outer iteration alone takes over 14 minutes.  `-i`
  cannot bridge the gap either: class D reaches the benchmark's own 1e-8
  verification bar only around iteration 85 of its 100, so the published
  count is barely padded.  Class D is therefore the largest class that
  runs, and it is what the catalog pins.
- **The row is outside DB-WRF.** The `nchunk` slice tasks, and
  the `nscan` scanners during setup, take one datablock `RW` concurrently and
  write disjoint ranges of it — legal under every campaign arm (all OCR memory
  model), but DB-WRF requires every same-DB write-write conflict to be
  event-ordered, at whole-DB granularity, and these are not.  No
  campaign cell runs it under DB-WRF; that arm would have to exclude this row
  or give the fan-outs per-slice datablocks.
