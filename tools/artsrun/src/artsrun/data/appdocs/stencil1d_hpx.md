# stencil1d_hpx

*A periodic 1-D heat ring advanced in lock-step across localities — one
partition per task per generation, two boundary elements on the wire.*
Origin: HPX's `1d_stencil_8` example
(`third_party/hpx/examples/1d_stencil/1d_stencil_8.cpp`, pin `v1.11.0`),
copied to `benchmarks/hpx/stencil1d_hpx/` and built as `stencil1d_hpx_hpx`.

## Overview

The distributed solver the HPX manual builds its 1d_stencil series up to:
`np` partitions of `nx` points each form one periodic ring, and `nt`
generations advance it by `u' = u + (k·dt/dx²)·(u_left − 2u + u_right)`.
Each locality owns `np/nl` partitions, runs one `stepper_server` component,
finds its two neighbours by basename, and per generation runs one `dataflow`
per partition: the interior is computed from the partition's own previous
data, and the two edge elements from the neighbouring partitions — pulled
locally without a copy, or across a locality as one `double` by the
`get_data` action after the neighbour's `post` has pushed the partition
handle into a step-keyed `receive_buffer`. A sliding semaphore of depth
`nd` holds the driver loop to `nd` generations ahead of completion.
`gather_here` collects every final partition to locality 0.

One program on four runtimes: `stencil1d_hpx_hpx` is the HPX program,
`stencil1d_hpx_arts_<variant>`, `stencil1d_hpx_xsocr` and
`stencil1d_hpx_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/stencil1d_hpx.c`), one row in `hpx_apps.yaml`.
HPX primitives used: two component types with actions
(`partition_server::get_data`, `stepper_server::from_left/from_right/
do_work`), `hpx::dataflow`, `hpx::post`, `hpx::find_from_basename`,
`hpx::lcos::local::receive_buffer`, `hpx::sliding_semaphore`, and
`hpx::collectives::gather_here/gather_there`. The mirror's mapping: a
per-partition ring of `K = min(nd + 2, nt + 1)` labeled blocks homed at
that partition's rank — the first `K` generations each create their ring
slot, every later generation reuses the slot a retirement frees for it —
one task pair per partition per generation hinted there, every produced
value pushed to its single consumer on a labeled point, and a per-rank
spawner chain in place of the semaphore.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--nx` | points per partition | 10 | reachable |
| `--nt` | generations | 45 | reachable |
| `--np` | partitions in total | 10 | reachable |
| `--nd` | how many generations the driver may run ahead | 10 | reachable |
| `--k` | heat transfer coefficient | 0.5 | reachable |
| `--dt` | time step | 1.0 | reachable |
| `--dx` | grid spacing | 1.0 | reachable |

`np` is a total, not a per-locality quantity: the origin divides it by the
locality count, so it is invariant across a node sweep and needs no
`args_by_nodes`. An `np` that does not divide evenly by `nl` is silently
truncated down to the nearest multiple — exactly as the origin's own
`local_np = np/nl` truncates — and only a truncated `np` still below `nl` is
usage. `nt == 0` is a valid run (zero time steps); when `nt > 0` the mirror
additionally rejects `nx < 2` and `nd == 0` as usage. The origin rejects only
`np < nl` (and prints a line), so the mirror is the stricter of the two on
the arguments it can compute with; `dx == 0` is accepted on both sides (it
divides into the coefficient, producing `inf`/`nan`, which the program then
runs with). One further rejection is the mirror's own: a `(nt, np)` pair
whose point reservation `11 · (nt + 1) · np · nl` would reach `2³²`. The OCR
shim narrows a range count and every index derived from it to 32 bits, so
that product is the program's whole addressable name space; the check folds
its factors one at a time against the ceiling, so an argument large enough
to wrap the product is rejected rather than wrapped past it.

A rejected argument prints usage and calls `ocrShutdown()` with status 0,
where the origin's own rejection prints a line and returns without ever
reaching its timer; the missing `CHECKSUM` marker fails the cell either way.

## Structure

Let `nl` = ranks, `local_np` = `np / nl`. Per run:

| object | count | size |
|---|---|---|
| driver tasks | `nl` — one per rank, the SPMD fork | — |
| interior tasks | `nt × np` — one per partition per generation | 1 dependence |
| boundary tasks | `nt × np` — one per partition per generation | 4 dependences each |
| retire tasks | `(nt + 1) × np` — one per generation per partition | 1–5 control dependences |
| signal tasks | `nl × ⌈nt/nd⌉` — a rank's first partition, every `nd`-th generation | 2 dependences — the boundary task's output, the rank's semaphore block RW |
| spawner tasks | at most `nl × max(nt − nd − 1, 0)` — one each time a rank's issue loop meets the depth limit, which depends on how far the signals have advanced | 2 dependences — its wake event, the semaphore block RW |
| gather tasks | `nl`, plus one collect task on rank 0 | `local_np + 1` (the rank's final-generation points, plus a control signal that every generation has been issued) |
| read tasks | `np` on rank 0, one at a time (`2 × np` with `--results`) | 2 dependences — one final partition RO, the merged name table RO |
| root, shutdown and drain tasks | one root and one shutdown task on rank 0, `nl` drain tasks | the shutdown task `nl`, a drain task 1 |
| partition blocks | `K × np` ring slots (`K = min(nd + 2, nt + 1)`) | `nx` doubles |
| edge blocks | `2 × nt × np` logical edges, of which `2 × nt × nl` cross a rank and allocate a fresh DB | one double |
| rank-share blocks | `nl`, plus the collect task's one merged table | `local_np` guids each; the merged table `np` |
| partition points | `(nt + 1) × np` | — |
| edge points | `2 × nt × np`, of which `2 × nt × nl` cross a rank | — |
| signal points | `nl × ⌈nt/nd⌉` (`SIGNAL_RELEASE`); a spawner's wake is an unlabeled COUNTED event of its own | — |
| lifetime/retire points | `3 × nt × np` (`USE_MIDDLE`/`USE_LEFT`/`USE_RIGHT`) plus `(nt − 1) × np` (`SLOT_RELEASE`) plus `np × max(0, nt + 1 − K)` (`KIND_RETIRED`, one per ring-slot reuse) plus `2 × nl − 1` (`FINAL` per rank, `STEPPER_RELEASE` per rank but rank 0) | — |

Every point is a labeled COUNTED event from one range reserved in `mainEdt`
(kind `GUID_USER_EVENT_COUNTED`) and named by `ordinal = (t · np + i) · KIND_COUNT + kind`, `KIND_COUNT` = 11:
the three the origin's own graph produces (partition, left edge, right edge),
plus eight more that exist only to sequence a generation's lifetime and its
block's reuse — three per-generation life-cycle acknowledgements
(`USE_MIDDLE`/`USE_LEFT`/`USE_RIGHT`, one per reader of that generation's
data), four retirement kinds (`SLOT_RELEASE`, `SIGNAL_RELEASE`, `FINAL`,
`STEPPER_RELEASE`) that tell a generation's slot when every reader has
released it, and `KIND_RETIRED`, which carries the freed block itself as
payload to the one task licensed to overwrite it. The reservation is
`11 × (nt + 1) × np × nl` names, which is a
reservation and not an allocation. The task and logical-generation counts are
the origin's own: one `dataflow` per partition per generation and the two
boundary elements each `heat_part` materialises. The block itself follows
the origin's own free list instead of the origin's per-`dataflow` allocation:
the first `K` generations of a slot each allocate their block where the
origin's `heat_part` allocates `partition_data next(size)` inside the
`middle_data` continuation — a labeled create, so the runtime's registered
pool places the block's pages once, at its first user — and every later
generation of that slot is handed the same block by the retirement that
frees it, exactly as the origin's `partition_allocator` pops an array off
its free list rather than calling `new`.

The depth limit is now the only bound on the live set: at most `nd`
generations exist beyond the last completed one, so live blocks are
`O(nd × np × nx)` rather than `O(nt × np × nx)`, which is the property the
origin's semaphore buys and the mirror's spawner chain reproduces — measured
at ≈ 4 generations at the anchor.

## Wiring

**Ordering rides on a point wherever a consumer's edge can exist before the
block is written — which is every block a step task or a driver produces, at
every distance.** An OCR data block added as a dependence is satisfied *at the moment it is added* — it
carries no ordering — so a consumer wired straight to a block GUID may run
before the producer has written it. Ordering therefore has to ride on an
event, and the mirror uses one rail for it: the producing task writes the
block, releases it, and satisfies the point; the consuming task registers on
the point in `DB_MODE_RO`, and destroys the block only where that is a
crossing edge's one-double block; a partition block is never destroyed
(below). A point has one producer and one consuming *rank*: a generation's
partition point is read by both of that partition's tasks, the interior and
the boundary one, where the origin hands one `shared_future` to both. A point
fires once and keeps its value until its last declared consumer has
registered, so registration and publication may happen in either order,
which is what lets a spawner create a generation's tasks long after its
inputs' producers were created; every consumer is known when the point is
made, so the point is reclaimed after the last of them and no task destroys
one.

**Every point has exactly one creator, and the create is ordered before
everything that names it** — the producer's satisfy and every consumer's
registration. OCR's contract is create-before-use, and xsocr holds a program
to it for events (an early satisfy finds no object), so the mirror does not
lean on a runtime that parks early messages. Three creators cover every
point. The step task that produces generation `g` of partition `i` is
created by `create_step(g − 1, i)`, which first creates every point of that
generation whose consumers run on `i`'s rank — the partition point, an edge
read by a same-rank neighbour, the three reader acknowledgements, the slot
release, the signal release, and the ring hand-over — so the create precedes
the producer, the task that will create the retirement, and every same-rank
reader, all of which are created after it on that rank (for generation `0`
the driver does the same before it publishes). An edge read across a rank has
no such place on the producer's side, so the reader's rank creates it, two
generations ahead: the step that produces a neighbour's edge of generation
`g` reads this partition's edge of generation `g − 1`, which is published by
the step `create_step(g − 2, reader)` creates, so creating it there orders it
before the producer too — the reader's driver creates generation `1` before
it publishes generation `0`, and the root creates generation `0` before the
fork. The rank's teardown points (`FINAL`, `STEPPER_RELEASE`) are created by
its driver first of all.

The rail's rule states its own exception: a block that is already complete
when its consumer's edge is created needs no point, because the edge is then
ordered by construction. Three kinds of block are in that position, all at
the program's end: the rank share the gather task writes, releases, and only
then wires into the collect task's slot; the merged table the collect task
hands the read chain the same way; and the final-generation partitions the
read chain takes by name, which were complete before their names were
gathered. Everything a step task produces is consumed by a task that already
exists when the block does not yet, and so goes on the rail.

The point's index is `ordinal · nl + consumer_rank`, which gives every point
one producer and one consumer; and where a range's names are spread
`index % nranks` — ARTS and ocr-vx both do — **a point's home is its
consumer's rank**, which is also where it is created — except generation 0's
crossing edges, which the root creates (Setup inside the span, below). Within a rank that makes
a publish no message at all, which is the case the great majority of points
are in. Across a rank the value path is four messages: the satisfy, the
consumer's RO acquire of a block homed at the producer (a request and its
reply), and the consumer's destroy of that block. The acknowledgement path
adds one: the point through which a reader tells a block's owner it is done
with that generation is homed at the *owner's* rank, and a crossing reader —
which holds only its own copy of the edge, never the owner's block —
satisfies it from its body once the copy is read, where a same-rank reader
does so through its completion, after its release of the block. The origin
spends three parcels on the same edge — the `post` that pushes the handle and
the `get_data` request and reply — plus whatever its own reference counting
of the pushed handle returns when the reader drops it, so the rail costs about
five messages against about three per crossing edge, all of them
header-sized but the one double, and buys one structure at both distances
for it: a handle
announced to the consumer and the element then pulled is the origin's own
mechanism, and here it is the only mechanism, with no second path for the
local case to drift away from. Those counts are the ones a runtime that homes
a labeled range by index gives; on xsocr, which stamps the reserving PD's own
location into every GUID of the range, every point of every generation lives
at rank 0 instead, so no create or publish is free and each costs a message
there.

The consumers, which are the ring the origin's wrapping index describes:

| block | consumer | rank of that consumer |
|---|---|---|
| `P(t, i)` — partition `i`'s generation `t` | `S(t, i)` | `owner(i)` |
| `R(t, j)` — element `nx−1` of `P(t, j)` | `S(t, j+1)` | `owner((j+1) mod np)` |
| `L(t, j)` — element `0` of `P(t, j)` | `S(t, j−1)` | `owner((j−1+np) mod np)` |

`S(t, i)` therefore has exactly four dependences — the interior task's
output in RW, plus three points in RO: its own partition, the left
neighbour's right edge, and the right neighbour's left edge — and produces
exactly three blocks, which is what the origin's `heat_part(left, middle,
right)` reads and what its `partition(middle.get_id(), next)` plus the two
`get_data` proxies present. Generation `0` is published by the drivers; the
last generation produces no edges, because the gather reads it whole.

**The semaphore's wait and wake need no point.** The flow-control semaphore
is per-rank state (a DB), and every task that touches it — the spawner that
creates generation `t + nd`, and the dedicated `signal_edt` a rank's first
partition's step spawns at every `nd`-th generation — runs on that same
rank, so the origin's `sem->signal(t)`/`sem->wait(t)` pair (creating step `u`
needs step `u − nd` complete) needs nothing across a rank: a blocked
`issue_generations` mints an unlabeled `OCR_EVENT_COUNTED_T` with its one
consumer, the spawner, and
stores it in the semaphore's own `waiter` field, and the `signal_edt` that
later raises `lower` past the blocking threshold satisfies that event
directly, under the semaphore's own spinlock. What *is* a point kind is the
signal's own retirement: `signal_edt`'s output event feeds a
`SIGNAL_RELEASE` point, one of the eight life-cycle/retirement kinds above,
so the generation whose checkpoint the signal represents is retired only
once the signal has fired.

**A generation's block is freed for reuse once every reader has
acknowledged it, and never destroyed.** A cross-rank edge block still has exactly one consumer, which destroys
it (the step task destroys the one or two 8-byte edge DBs it read that
crossed a rank, and recognises a same-rank neighbour's edge as an alias of
that neighbour's own partition block, which it must not destroy). A
partition block is different: once every reader of a generation has
signalled it (the three `USE_*` acknowledgements plus the retirement point
that follows from `SLOT_RELEASE`/`SIGNAL_RELEASE`/`FINAL`/`STEPPER_RELEASE`),
the dedicated `retire_edt` runs — it destroys nothing, neither the block nor
the points, which reclaim themselves. If a later generation still owes that
ring slot a write (`g + K ≤ nt`), `retire_edt` publishes `KIND_RETIRED` with the
block's own guid as payload, so the task licensed to reuse it
(`create_step` at generation `g + K`) receives that same block as a
`DB_MODE_RW` dependence and writes into it directly — no `pool_push`, no
`allocate_partition`, and no second create. A slot no later generation
reaches — the last `K` generations of each partition — publishes nothing,
and its block simply persists, still labeled, until the runtime's own
teardown. **The ring is never destroyed**, because the origin never returns
an array: its `partition_allocator` keeps every array it ever handed out on
its free list for the life of the process. `drain_edt`, behind the rank's
`RETIRED` latch of `(nt + 1) × local_np` retirements (every retire task,
whether or not it published, counts into it), destroys the semaphore block
and signals shutdown. The collect task destroys the `nl` per-rank gather
tables once it has copied their names into the merged table, and the last
read task destroys the merged table. Every output event the program asks
for is one it made itself, a COUNTED event with its consumers stated
(`EDT_PROP_OEVT_VALID`), so none outlives its last consumer; the rank's
retirement latch is the one event per rank that a runtime which keeps a
fired event keeps.

## Flow

`mainEdt` validates the arguments, reserves the point range, and hands the
rest to a root task on rank 0 — a runtime may run `mainEdt` on any rank, and
the root is where the origin's locality-0 driver starts its clock. The root
creates the `nl`-dependence collect and shutdown tasks on rank 0 and forks
one driver per rank (`mirror_spmd_fork`), after creating the points of the
edges that cross a rank in generation `0`. A driver creates its rank's
teardown points and its gather task, whose `local_np` final-generation
partition points are registered by the step creations that create them;
creates generation `0`'s points and the crossing points its partitions read
at generation `1`; writes and publishes
generation `0` — each partition initialised to `local_index · nx + j`, plus
its two edge elements, which is the origin's initial condition and its
initial `send_left`/`send_right`; creates the step tasks of generations
`0 … min(nd, nt) − 1` outright; and creates one spawner per remaining
generation, each waiting on the signal `nd` generations back.

The interior task computes the interior into its generation's block —
freshly created for the slot's first `K` generations, the ring's reused one
after — and releases it into the boundary task's dependence; the boundary
task then computes the two edges, in `heat_part`'s order, releases and
publishes the same block as the next generation's partition
and (unless it is the last) its two edges, raises the signal if it is its
rank's first partition, and destroys the edge blocks it consumed that
crossed a rank, acknowledging each to its owner as it does (a same-rank edge
is an alias of a still-live neighbour buffer and is left alone, and is
acknowledged by the task's completion — see Wiring). A spawner creates its generation's
step tasks for its rank and spends the signal it waited on.

The gather task concatenates its rank's final-generation partition names
into one table and wires it into the collect task's slot for its rank; the
collect task merges all `nl` rank tables into one `np`-entry table and
starts a serial chain of read tasks, one partition at a time in ascending
index order, that pulls every final partition once and folds it into the
checksum while it is in hand — the origin's own serial
`s[i].get_data(middle).get()` loop, which on the HPX side sums each
partition where it is pulled. The last read stamps the narrow
`Execution_Time` and prints `CHECKSUM`. With `--results`, it then starts a
second, independent serial read chain — the origin's own separate
`--results` print loop — that prints every partition's values before the
timing line and the shutdown release; without it, the timing line and the
shutdown release follow directly.

| HPX wait site | classification | mirror |
|---|---|---|
| `heat_part`'s `dataflow` over `middle_data` and the two `get_data` futures | start wait — every input | the boundary task's three point dependences (its fourth is the interior task's output) |
| `receive_left(t)` / `receive_right(t)` | start wait — a future handed to `dataflow`, never waited on | the two boundary points, the only ones that cross a rank |
| `sem->wait(t)` | flow control — a creation-depth limit, not a dependence | the spawner chain: signal from a rank's first partition, waited on `nd` generations later |
| `overall_result.get()` and the `get_data` gather loop on locality 0 | end wait — the result in hand | the rank gather tasks feeding the collect task's merge, then the serial read chain |

Mid waits: 0.

The measurement is `[E2E]` on both sides, stamped on rank/locality 0 alone:
the whole application, from its first statement to the point it asks the
runtime to stop, runtime start-up and teardown excluded. On the OCR side the
runtime stamps it (the main task becoming eligible, shutdown recognised on
rank 0, once every rank's drain has completed and the dedicated shutdown task
calls `ocrShutdown()`). On the HPX side every locality runs `hpx_main`;
`run_clock` opens as its first statement and `print_e2e` closes it, on
locality 0, immediately before `hpx::finalize()` — locality 0 gathers every
other locality's partitions before it returns from `do_all_work`, so its end
is the application's. Two things sit in the OCR span with no HPX
counterpart, both small: the final release that lets every rank's drain run
is published after the timing line, so rank 0's stamp waits for each rank's
last retirements and its shutdown signal, where locality 0 waits for no
other locality once it holds their partitions; and a final partition pulled
from another rank stays cached at rank 0 after the read task releases it
(how long is the coherence arm's choice), where the origin frees each pulled
`partition_data` as its loop moves on — the same bytes on the wire, a larger
resident set at rank 0.

The origin's own `Execution_Time_sec` is a different, narrower span and both
sides keep it where the origin has it: from just before the steppers start to
the final partitions in hand on locality 0. The mirror takes both of its ends
on rank 0 — the start in the rank-0 root task that creates the collect and
shutdown tasks and forks the drivers, never in the main task, which a runtime
may run on any rank and whose clock is then another node's.

`CHECKSUM` is the sum of every element of the final state — the simplest
digest of what the program already holds, since `1d_stencil_8` computes no
printable quantity of its own (it prints a timing table). **What that
digest can and cannot see is worth stating.** Over a periodic ring the
update adds `(k·dt/dx²)·Σ(u_left − 2u + u_right)`, which is zero for any
coefficient, so the sum is *conserved*: it equals the initial state's sum at
every generation, and it is therefore invariant under any permutation of the
state. A program that swapped a partition's two neighbours, or built the
ring the wrong way round, would print the same number. The four runtimes
agreeing on it is evidence that they agree, not that the ring is oriented as
the origin orients it; that is established by reading the mirror against the
origin, and it is what this row's review did. The limitation is the
quantity's, it is disclosed here, and nothing is added to the program to
work around it.

At `k = 0.5`, `dt = dx = 1` and the gate's size every value stays a dyadic
rational small enough to be exact in binary64, so all six entries agree bit
for bit — `8386560` at one locality and `4192256` at two for the gate
arguments, which the HPX entry prints that way (`%.14g`) and the OCR entries
as `8.38656000000000e+06` and `4.19225600000000e+06` (`%.14e`). At the
calibrated size the values outgrow 53 bits and the agreement rests instead
on what the two sides share: one expression, in the origin's operand order,
and one summation order — the final partitions in ascending index, each
element in order. The `xsocr` entry's rendering can differ in the last
printed digit: that runtime replaces `printf` with its own formatter, whose
double conversion is good to about a part in `1e15`. That is a rendering
limit of one reference runtime's output path, not a difference in the
answer; the row's `1e-09` relative tolerance covers it with four orders of
magnitude to spare.

**On ocr-vx the points are kept.** Every point and every output event here is
COUNTED, reclaimed after its last consumer on ARTS and xsocr; ocr-vx does not
implement COUNTED and keeps each one to teardown, so its event memory grows
with `nt · np` (about a dozen events per partition per generation — about
`2.2·10⁶` at the calibrated arguments, some 3.3 GB at an estimated 1.5 KB
each, divided among the ranks) where the other two stay at the live window — a property of that reference, disclosed
and not patched (`benchmarks/hpx/README.md`, "One row, four runtimes").

**Setup inside the span, as the origin's.** The root's `2 · nl` creates of
generation 0's crossing edges sit inside the span; the origin's counterparts
— the steppers' basename registration and lookup and each `receive_buffer`
entry, made when first touched — are inside its `hpx_main` too.

## Placement

`local_np = np / nl`, and partition `i` belongs to rank `i / local_np` —
which is where the origin created it (`hpx::find_here()` inside `do_work`,
on the locality whose `stepper_server` is running) and where every later
generation stays (`partition(middle.get_id(), next)` is colocated with its
predecessor). The mirror states that placement on every object: every step,
spawner and gather task carries an explicit
`ocrAffinityGetAt(AFFINITY_PD, ...)` EDT hint for its rank, and every block
an explicit DB hint for the rank that writes it. Nothing is left to the
build's no-hint policy.

The point layer carries the placement too, and in the other direction: where a
labeled range is homed by index (ARTS, ocr-vx) a
point's home is its *consumer's* rank, so a partition block that never
leaves its rank is announced locally, and the two boundary elements per rank
per generation — `2 × nt × nl` of the `2 × nt × np` edge points — are the
only ones a message is spent on. The origin spends three parcels on each of
those edges (`post`, then the `get_data` request and reply); the mirror
spends five (the four of the value path and the one of the reader's
acknowledgement, named under Wiring above). The extra messages per crossing
edge are what one structure at both distances costs, and the set of edges
that cross is identical.

A within-rank edge costs nothing beyond the partition block already in
flight: `publish_edges` hands that *same* block guid to a same-rank
neighbour's edge point, exactly as HPX's `get_data(left_partition)` returns
a proxy over the neighbour's array and copies nothing when the neighbour is
local — one structure serves both distances, and only a crossing edge pays
for a second one. A crossing edge allocates a fresh one-double block and a
labeled point per side (created by the reader's rank, satisfied and
registered once each), where the origin's `post` plus its `get_data`
request/reply pay for the same handoff in three parcels; the mirror pays five
messages for it (named under Wiring above). That cost scales with `2 · nt · nl`, not
with `np`: at the gate's two ranks it is 64 blocks and 64 points, and it
does not grow with how many partitions each rank runs. The alternatives are
worse under this rail: letting a step read its neighbours' whole partition
blocks gives a partition point consumers on three ranks, which breaks the
rule that a point has one consuming rank; and
materialising every edge, local ones included, would give the program a
second structure the rail does not need.

## Sizing

The width knob is `np`, a total that stays invariant across a node sweep
(the origin divides it by the locality count itself), with `nx` and `nt`
setting the work per partition and the length of the chain. `nd` is flow
control, not a size: it bounds how far ahead the graph is built and with it
the live block set.

One ceiling binds a calibrated size: `11 · (nt + 1) · np · nl` must stay
below `2³²`, because the OCR shim narrows a range count and every index
derived from it to 32 bits and that product is the program's whole point
space. At the gate arguments it is 23,936 names at two ranks; at `nt = 45`,
`np = 4096` over eight ranks it is about 16.6 M, so the ceiling is far
away — but it is a usage rejection, not a degradation, so a size is checked
against it rather than discovered to exceed it.

The `controls-gate` roster runs `--nx=64 --nt=16 --np=64 --nd=10`
(`CHECKSUM 8386560` at one locality, `4192256` at two), which
finishes in milliseconds on one rank. At two ranks that cell is
latency-bound by construction — 4096 points in total, and one `post` plus
one `get_data` round trip on each boundary's critical path every generation
— so its two-locality time says more about per-message cost than about the
solver; `nx` is the knob that moves it back onto computation. The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--nx=270000 --nt=45 --np=4096 --nd=10` at every
node count (`np` is a total the origin divides by the locality count). `np =
4096`, a power of two above the width floor (32 × 108 = 3456) and aligned to
neither runtime's thread count; `nt` and `nd` keep the origin's published
values; `nx` was derived to the one-node window of 100 s through the linear
work law and then held below it by memory. Both sides keep `nd + 2 = 12`
generations of partition arrays alive — the origin's allocator keeps every
array it ever made and a partition component retires two steps after it is
produced; the mirror keeps a ring of the same depth per partition — so
`8 · 12 · np · nx` bytes are live on either side, and the ARTS resident set,
the registered pool's per-node ladder mapped over that live set, reaches the
one-node budget of 200 GB with a 10 % margin first: 156–176 GB measured at
`nx = 280 000` across the three arms and 174–197 GB at 300 000, with a
15–20 GB scatter from where first touch lands each partition. The size stops
one round step below the measured 280 000 (from the sizing model's 324 000 at
the width floor, `np · nx` kept), where the anchor measured 21.9–22.2 s on the
three ARTS arms at 170–176 GB and 19.4 s on HPX at 96 GB — about a fifth of
the window, held there by the memory ceiling; see the section's anchor table.
