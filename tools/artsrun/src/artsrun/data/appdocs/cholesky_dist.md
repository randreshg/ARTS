# cholesky_dist

*The restructured version of `cholesky_blas`: the same factorisation, the same
2-D block-cyclic ownership and the same BLAS3 kernels, with each place building
its own tasks where they run.*
Source: `third_party/ocr-apps/apps/cholesky/ocr-mkl/cholesky_dist.c` (~500
lines), with the generated instance in
`third_party/ocr-apps/apps/cholesky/ocr-mkl/cholesky_instance.h` (~85 lines,
shared with the base row).

## Overview

The row this re-implements builds its whole DAG in `mainEdt` -- about `t^3/6`
task creations on one rank -- and then relies on placement to keep each kernel
near the tile it writes.  The placement is already the canonical one and cannot
be improved on: it is ScaLAPACK's 2-D block-cyclic map, every kernel sits on
the coordinate of the tile it updates, and first touch carries the tile's home
with it.  Measured at eight nodes it cuts remote acquires sevenfold.  The row
still degrades 3.24x across the node sweep with that layer on.

What this version changes is where the graph is built.  `mainEdt`'s work is a
function of `places` and not of the problem size; each place creates the event names, the tile blocks
and the tasks for the tiles it owns, on itself, and each of the three
enumerates only the indices it owns rather than the whole space.  The ownership
map, the kernels and the answer are untouched: the task set, the dependence
edges and the finisher wiring are index-for-index what the one-rank
construction produced.

## Parameters

`cholesky_dist --ds <n> --ts <n> [--fib <tile-stream>] [--places <n>]`.
The catalog runs `--ts 500 --places 32 --ds 85000` and names no input; the
previous `90000` was sized against a window that included a serial preamble
and a file read this version no longer has, and the anchor below is retaken
at the new value (see Sizing).

Every token must be a known option carrying a value.  An unrecognised option, a
trailing option with no value, and a zero or non-dividing `ds` or `ts` are each
printed and refused.  A **supplied** `places` outside
`1..t^2` is likewise **rejected, not clamped**, because the partition is the
subject of the measurement and a run must not report a knob it did not use; the
**default** (32) was not requested by anyone, so at a size too small to hold it
(`t < 6`) it is clamped to `t^2` and the configuration line prints the clamped
value.  An unrequested default must not fail a run with an error about an
option that was never passed.

`ts` is the tile edge and `t = ds/ts` the tiles per dimension.  `--fib` is
optional and is the tile-stream binary, the same layout the base row reads --
lower-triangular tiles in `(i, j<=i)` order, `ts^2` doubles each.  With no
`--fib` there is no input file: the matrix is generated from its own indices by
the place that owns each tile, from the definition the base row shares (one
header, included by both programs), so the two rows factor the same matrix at
the same `ds` and `ts`.

`places` is the ownership decomposition and is an **argument, not the rank
count**: the task count, the datablock count and the order the factor is
computed in are identical in every geometry, and the machine enters only
through the map from a place onto a rank.  Verified by running it: the answer
does not move over places 1, 2, 4, 8, 16, 32 and 64, nor over one, two, four
and eight nodes -- checked on the built binary, not inferred, on the
construction that preceded the split of startup across the places (see
Correctness for why the two build the same graph).

`ts` is also what decides whether a shrunk version of this row is the same
program.  A tile costs `ts^3` of arithmetic and `ts^2` of movement, so compute
per byte moved IS `ts`; a trend taken at a fifth the tile is a fifth the
arithmetic intensity and reverses the verdict.  The trend below therefore holds
`ts` at the catalog value and shrinks `ds` alone.

## Structure

`mainEdt` creates the finisher, a block holding the input path, one publication
event per place, and two tasks per place: a `placeName` and a `placeInit`.
That is everything it does: `places` events, `2*places` task creations and
`places^2` dependence registrations, none of them a function of `t`.

`placeName` runs on its own place.  It creates the version names for the tiles
that place owns, creates those tiles and fills them -- from the generator by
default, from the input file when one is named -- satisfies each tile's version
0, and publishes its names as one block.  `placeInit` runs on the
same place once **every** place has published, and creates the tasks for the
tiles it owns.  Both loops step by the partition, so each costs what the place
owns and neither walks the whole index space.

A tile is rewritten at every step below its column, so the events carrying it
are **versioned**: version k of tile (i,j) is that tile after step k.  One event
per tile would be satisfied once per step, which is not a thing an event does.
Tile (i,j) exists in versions `0..j+1` and no further -- numbering every tile up
to the last step instead would spend two thirds of the names on versions nothing
ever satisfies or reads (1.94 M of 2.95 M at the previous anchor `t=180`).

The names are **not** gathered.  A tile's versions live contiguously in its
owner's block, in the owner's own enumeration order, and that layout is a pure
function of `(t, places)`: any place recomputes the offset of any tile from the
partition, indexes the owner's block and has the name.  What crosses a place
boundary at startup is therefore the input path (empty when the matrix is
generated) and one name block per place --
`1,004,550` names in total at the previous anchor `t=180`, the same names the
one-rank construction created, divided `places` ways and created where they are
owned.

The events are sticky, and that is the flavour the dependence pattern needs
rather than a fallback: a place satisfies a tile's first version before it has
created the tasks that read it, and the producer of a later version is built by
one place while its consumers are built by another, concurrently. Binding after
the satisfy is therefore normal here, which a once-event does not survive. A
counted event would fit too -- the consumer count is exact and derivable -- and
would reclaim each name as the factorisation walks past it, but the flavour is
not portable across the runtimes this row is compared on, and what it saves is
small against a working set that is the matrix. Binding before satisfying
everywhere would need a startup barrier gating every one of the `t^3/6` tasks,
to save names that are well under one percent of the residency.

Per step k, on each place, for the tiles it owns:

- **POTRF** on (k,k): version k becomes k+1.
- **TRSM** on (j,k) for j>k: reads the factored (k,k), produces the panel.
- **the trailing update**, one task per tile -- SYRK on (j,j), GEMM on (j,i) --
  each reading its own two panel tiles.

Each place produces **its own tiles**.  By default nothing is read at all: an
element's value is a function of its two global indices, so a place fills what
it owns with no file, no stream and no ordering against any other place, and
the matrix never appears anywhere as a whole.  When a file IS named the same
loop reads instead: the stream is fixed-size records in a regular order, so a
tile's offset is arithmetic and a place seeks to the ones it owns -- parallel
per place, but bytes inside the measured window, which Sizing quantifies.

There is no per-place panel gather.  One was built and measured: it loses at
every geometry that scales, because the copy costs more than the coalescing
saves and because the gather is a barrier, making all of a place's updates at a
step wait on the whole panel instead of on the two tiles they read.

## Wiring

A tile is one block of `ts*ts` doubles throughout; nothing is packed, gathered
or copied between places, so there is no transfer format to describe. What a
task receives is the tile it writes at slot 0 `DB_MODE_RW` and the one or two
panel tiles it reads at slots 1 and 2 `DB_MODE_RO`, and its `paramv` carries the
name of the event to satisfy with the block it wrote -- release first, then
satisfy, so nothing is exposed while the writer still holds it.

The input path is the only object `mainEdt` hands out.  Each place's `placeInit`
reads every place's name block `DB_MODE_RO`, which is what its publication event
carries.

Reclamation splits by object kind.  The **events** are sticky and none is
destroyed: `1,004,550` name objects are resident for the whole run at the
previous anchor `t=180`, and what the version numbering saves is the `1.94 M`
names nothing would ever have read, not a reclamation.  A counted event would
be reclaimed once its declared consumers had bound and the count is derivable
here, but the flavour is not portable across the runtimes this row is compared
on, and what it saves is small against a working set that is the matrix itself.
The **blocks** cannot be treated the same way either: the finisher destroys the
diagonal tiles, which it is the last reader of by construction, but the
off-diagonal tiles are left.  A tile's final version is
what the trailing updates of its own column read, so a reaper hung on it would
be a destroy with no ordering edge against those readers; freeing them needs a
per-tile last-reader count the DAG does not carry, and one block per tile is the
program's own working set either way.  What is left at shutdown is the matrix,
not an accumulation.

## Flow

Startup is two waves over the places, separated by one global join: every place
names and fills its own tiles and publishes its name block, and every place's
`placeInit` depends on **all** `places` name blocks, so the build phase starts
at `max over places (name + fill)` and the first POTRF cannot fire until the
slowest place has finished.  That join is a real cost and it is new: on the
previous construction a place's `placeInit` needed only `mainEdt`'s single
gathered grid, so a place could build its graph as soon as its own tiles were
ready.  The net critical path is still shorter -- what the join replaces is a
serial `1.0 M`-event preamble on one rank.  What the join is exposed to is the
tail of the per-place fills, and that tail is now arithmetic on each place's own
memory rather than a shared filesystem's tail latency; naming a `--fib` file
puts a filesystem back under it.

A narrower dependence set would remove the join and is **not available**: a
trailing update on place `g` reads panel tile `(i,k)` whose owner is
`(i%P)*Q + (k%Q)`, and neither `i%P` (the loop strides `i` by `Q`) nor `k%Q`
(the step index is not gated in that loop) is fixed by `g`'s own row and column
classes, so a place's name references span the whole place grid rather than its
own row and column.  Any construction that lets a place start earlier has to
make the names addressable without a publication edge at all -- a labeled GUID
range would, and does not hold on this runtime, where creating a labeled GUID
replaces whatever stands at it.

Tiles are produced and satisfied by their owners.  Each step's POTRF fires on the
diagonal tile's current version, the TRSMs on that and their own, and the
updates on their tile and the two panel tiles they read.  The finisher takes
every diagonal tile at its final version, prints the trace and shuts down; its
`t` slots are wired by the places that own the diagonal tiles, each one wiring
the versions whose names it holds.

## Placement (base)

The tiles are owned by places on ScaLAPACK's two-dimensional block-cyclic map,
and a place is then mapped onto a rank by folding **both** axes of the place
grid onto a near-square rank grid.  That second step is the one that is easy to
get wrong: a linear `place * nranks / places` keeps only the axis the place
index varies slowest against, so the column coordinate never reaches the rank
and the distribution is one-dimensional exactly where messages are generated --
the whole property the two-dimensional map exists to provide, lost at the last
step.

What that costs is a question of how much data crosses, so it shows where the
data is.  At the previous anchor's arguments over eight nodes, folding both axes runs
**1.22x** faster (127.8 s against 104.7 s, two replicates each whose own spread
is 0.5%) and holds **a third less** resident memory -- 69 GB a rank against 47.
At the trend size the same comparison shows no difference in time at all (16.85
against 16.93 at four nodes, 13.48 against 13.73 at eight): a fifth the data
puts the run against this host's aggregate arithmetic ceiling rather than
against its network, and a placement map cannot be seen from there.  The
residency still separates -- 57 GB against 73 at four nodes -- because that is
set by how many ranks a panel reaches, not by how long it takes to get there.

Everything a place creates -- names, tasks and blocks alike, including the
tiles it fills -- is created on that place, and the blocks and tasks
carry its rank as a hint, so a tile is first-touched on the place that owns it.  That is the only thing the program asks the
machine, and it asks it for a hint: the tile partition above it is fixed by
argument, so where a place lands changes nothing about what the program
computes.

There is no separate `hinted` version.  The block-cyclic map is already the
canonical placement for this factorisation and the base row carries it as its
optimised layer; here it is structural rather than optional.

## Correctness

The instance is `A = D + v*v^T` with `v_i` in `[-1,1)` and `d_i` in `[1,2)`
pure functions of the global row index: positive definite by construction, with
no zero entry, so a tile's final version is what the trailing updates of its own
column read and **every** element of the factor feeds a diagonal tile.  The
trace is therefore a function of the whole factorisation and not only of its
diagonal -- which the identity input this row used to read was not: under that
one the factor was the identity, the trace was exactly `ds`, and no
off-diagonal error could move it.  The expected value is now measured rather
than analytic, and pinned at these arguments; the summation order is fixed by
the finisher's slots, so it is a determinism check as well as a value check.

The two rows factor the SAME matrix: the entry is one definition in one header
that both programs include, evaluated as one multiply and one add, so the tiles
are bit-identical at equal `ds` and `ts` however they are decomposed.  Whether
the two rows' traces then agree bit-for-bit depends on the tile size and
nothing else -- see the base row's document; the catalog runs the two at
different `ts`, so they agree to round-off, not to the last digit.

The factor itself was checked against an independent sequential Cholesky on
three non-trivial SPD matrices (`1/(1+|i-j|)`, a cosine band and a modular
pattern, each diagonally dominant), fed as tile streams to both rows: **all
500,500 elements agree to 1.5e-15** on every one of the three, and this row
reproduced their traces (`31622.766542 / 44719.846738 / 31748.460760`) at
places 1, 4 and 16 and at one, two and four nodes.  Those runs used the file
input and predate both the division of startup across the places and the
generated instance.  What they establish carries over regardless: the kernels
and the task graph are unchanged, and the graph is index-for-index the same at
every `(t, places)`, so what those runs pinned was the arithmetic, which is not
what either change touched.

## Sizing

Two knobs, and they divide cleanly -- neither is chosen for speed.

`ts` is the WIDTH knob: peak width is `t(t-1)/2` with `t = ds/ts`, and the class
rule asks for four times the largest geometry's 3456 workers, so `t >= 167`.
`ds` is the SIZE knob and the window fixes it.  At `ds = 90000` with `t = 180`
that makes `ts = 500` and a peak of 16,110 -- 4.66x the workers.

The base row derives the same way and lands on the same `t`: `ds = 16700`,
`ts = 100`, `t = 167`, peak 13,861.  The tile sizes differ by a factor of five
only because the two rows sit in different windows -- the base anti-scales and
is calibrated against 10-30 s, this one scales and gets ~150 s.  `ts` is not a
value anyone picks; it falls out of `ds/t`.

A larger tile does run faster here, and that is exactly why it is not used: it
would give a peak width of 4,005, which is 1.16x the workers rather than 4x,
and narrowing the width for a timing gain is the one move the width rule
forbids.  The experiment's subject is what the decomposition costs.

Counts and residency follow from `(t, places)` alone.  The concrete column is
`at the previous anchor` (`ds = 90000`, `t = 180`) and moves with `ds` when the
anchor is retaken; the formula column does not.

| quantity | formula | at the previous anchor (`t=180, places=32`) |
|---|---|---|
| tasks | `t + t(t-1)/2 + t(t^2-1)/6` | 988,260 |
| peak width | `t(t-1)/2` | 16,110 |
| tile blocks | `t(t+1)/2` | 16,290 |
| version names | `sum over j<=i of (j+2)` | 1,004,550 |
| name block, largest place | -- | 34,017 names = 272 KB |
| **tile payload** | `t(t+1)/2 * ts^2 * 8 B = ds(ds+ts)/2 * 8 B` | **32.58 GB** |
| name blocks, all | `1,004,550 * 8 B`, held once per place that reads them | 8 MB each |
| per-place index | `t(t+1)/2 * 8 B` transient in `placeInit` | 130 KB |

Whole-run residency at one node is the tile payload plus the runtime's own
per-object structures for `988,260` tasks and `1,004,550` events; the payload
is the term that grows with the size knob.

### There is no input read in the window any more

The tiles are generated where they are owned, so the run's in-window input term
is **zero bytes**: no file is opened, nothing is staged before the run, and
nothing about the instance depends on the node count.  What replaces the read
is `t(t+1)/2 * ts^2` element evaluations of one hash, one multiply and one add,
divided `places` ways like every other per-tile cost, on each place's own
memory.

What that removed was not small.  Reading the lower triangle costs

`t(t+1)/2 * ts^2 * 8 B = ds(ds+ts)/2 * 8 B` -- **32.58 GB** at the previous
anchor, the same bytes as the tile payload,

divided `places` ways; with one rank a node and `places = 32` the term a NODE
carried was `ds(ds+ts)/2 * 8 B / nodes`:

| nodes | 1 | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|---|
| input bytes a node (previous anchor) | 32.58 GB | 16.29 | 8.15 | 4.07 | 2.04 | 1.02 |

That was a node-count-dividing non-workload term inside the measured window,
loading the 1-node cell -- the denominator of every speedup this row reports --
most heavily, so it inflated the strong-scaling factor of the row the campaign
presents as its one strong scaler by an amount set by per-node read bandwidth.
It is gone, in this row and in the base row together, so the two tiers' windows
still span the same work.  `--fib` still reads, unchanged, for a run that
supplies a real matrix; a campaign cell does not name it.

`places` is also the startup width: it sets how many tasks divide the naming,
the tile loading and the task creation.  At `places = 32` the widest geometry
gets exactly one `placeName` and one `placeInit` per rank, so a rank's share of
the preamble runs on one worker while the rest idle.  A larger node-invariant
value divides it further at every geometry; the cost of raising it is that
`mainEdt` registers `places^2` dependences (1,024 at 32, 16,384 at 128), so the
knob is bounded well below `t^2` in practice even though the validator accepts
anything up to it.

**At the previous anchor's arguments** (one node, 108 workers + 4 progress) the
row ran 145.5 s / 89 GB under `val_wb`, **159.7 s / 83 GB** under
`inv_wb` and 151.8 s / 85 GB under `excl_retain`.  Those figures were taken
before the startup was divided across the places and before the input read left
the window -- the factorisation they measure is the same one, but the preamble
they include is not, so this table predates the current anchor and does not
carry over.  The current one-node anchor, under `inv_wb`, is 139.1 s and 68 GB
resident at `--ds 85000 --ts 500 --places 32`, against the restructured
tier's 120 s target.  The tightest family in that earlier table was `inv_wb`
at 160 s, the window a strong scaler asked for; the largest residency any
family there held was 89 GB, a third of what a node has.  The catalog
arguments are run at the anchor and nowhere else here: a node sweep at that
size is what a campaign produces, not what calibrates one.  The families
spread 1.10x in time, and the same binary in the same family moves about 5%
between runs on this host, so the row is stated as a range rather than one
figure.  Memory is not what bounds this row: the
largest geometry measured holds **45 GB a node**, less than one node does,
because the tiles divide while the runtime's own structures do not grow with
the node count.  Resident figures here are per RANK; the runner sums VmRSS over
the processes it started, which on a host that carries every rank itself is a
whole-machine total and not what a node has to hold.

**The trend** was taken at `ds = 40000`, the same `ts = 500`, at 15 workers and
one progress thread a node -- also before the startup was divided and before
the input read left the window.  The removed terms only ever added time, so
**the times below are an upper bound on what the row now takes**; the 2.38x
itself is NOT bounded either way, because both removed terms fell with the node
count -- the read divided `places` ways over the ranks, the preamble was never
measured -- so the shape of what was taken out is unknown.

| nodes | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| time | 32.76 s | 22.26 | 16.67 | **13.77** |

**2.38x from one node to eight**, where the base row degrades 4.21x over its
own sweep.

The trend size is a shrink of the catalog cell rather than a smaller problem:
`ts` is held and only `ds` moves.  Taken at `ds = 10000, ts = 100` instead --
a fifth the tile -- the same binary anti-scales, 2.30 s at one node against
6.07 s at eight, because arithmetic per byte moved goes with `ts` and that
version is a fifth as intense.  The verdict follows the tile, so the trend has
to keep it.

The design's live set is one block per tile and nothing else.  A serial
emulator running this program's real task graph at `ds = 40000, ts = 500`
reports 88,593 tasks (every one of them fired), 3,243 datablocks, 91,800 events
and a peak live set of 6,180 MB against a 6.03 GiB matrix -- the data, not a
multiple of it.  Those counts are what the decomposition derives on paper, so
what the runtime holds beyond them is its own coherence copies.

About half the reads in that emulation are of a block homed on another rank,
and that is structural rather than a placement failure: a trailing update reads
one panel tile from its own row and one from its own column, and no assignment
of tiles to ranks makes both local.  What the ownership map decides is not
whether a panel crosses a rank but how many ranks it must cross to.
