# nbody_hpx

*DistributedNBody: an octree built over randomly placed bodies, one level of it
spread over the localities, and bodies migrating between cells step by step.*
Origin: `third_party/NBody/DistributedNBody/NBody Code/main.cpp`
(STE||AR-GROUP/NBody, pin `6147a176`, 2016-02-17), copied to
`benchmarks/hpx/nbody_hpx/` and built as `nbody_hpx_hpx`. It is the second row
of this section whose origin is not the vendored HPX tree, and the second that
needed a modernisation — from the 2016 interfaces this time. The repository
states no licence.

## Overview

`hpx_main` runs on every locality. `create_Octree()` builds, on every rank
alike, the same tree: `n` bodies whose coordinates are `rand() % 100` from a
stream nobody seeds, a root cell over `[0,100]³`, and a recursive subdivision
that stops where a cell holds no more than `--th` bodies. A traversal then
gives each cell an interaction list. One `stepper` component per locality owns
`np / nranks` cells of a fixed level — `np` is 8 below nine localities and 64
from nine — and per step runs, per owned cell, the chain
`compute_position → copy_mem → changed_members → Non_Members`, fed by six
neighbour partitions the rank received (`New_Members`) and followed by six the
rank sends. `gather_here` collects every locality's cells at the end and
locality 0 pulls each cell's data.

One program on four runtimes: `nbody_hpx_hpx` is the HPX program,
`nbody_hpx_arts_<variant>`, `nbody_hpx_xsocr` and `nbody_hpx_ocrvx` are the OCR
mirror (`benchmarks/apps/hpx_origin/nbody_hpx.c`), one row in `hpx_apps.yaml`.
HPX primitives used: two components (`partition_server` with a direct action,
`stepper_server` with seven actions), `hpx::dataflow` with `hpx::unwrapping`,
`hpx::post`, `hpx::lcos::local::receive_buffer` keyed by step,
`hpx::register_with_basename` / `find_from_basename`,
`hpx::serialization::serialize_buffer` for a partition on the wire,
`hpx::collectives::gather_here`/`gather_there`, and one `hpx::for_each` over
the tree. The mirror's mapping: one block per stage value homed at the rank
that computes it, one task per origin task hinted there, a pushed partition's
identity — not a copy — linked to a labeled point at the rank that reads it,
and the gather as one block per rank pulled twice, in sequence, by rank 0's
own two-pass read chain.

**The one determinism the row rests on is `rand()`.** Both programs draw the
body coordinates from the C library's unseeded `rand()`, so both build the same
tree on the same libc — this is the only cross-language agreement the row
depends on, and it is what makes the two sides' answers comparable at all.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--n` | bodies | 1000000 | reachable |
| `--nt` | steps | 100 | reachable |
| `--size` | bodies one cell's partition holds | 150000 | reachable |
| `--theta` | opening ratio of the tree traversal | 0.3 | reachable |
| `--th` | bodies a cell holds before it is subdivided | 100 | reachable |
| `--k-th` | migration slots at the tail of a partition | 500 | reachable |
| `--np` | cells the localities share out | *(rule)* | reachable |

The first six were file-scope constants and are now arguments with those
defaults. `--np` has no single origin value: the program picks 8 below nine
localities and 64 from nine, and `--np=0` (the default) applies that rule —
including for nine to fifteen localities, where the origin left the variable
uninitialised (`ORIGIN.md`, bugfix). Both sides refuse what neither can compute
from: a level other than 8 or 64, more localities than cells, `--size` below
`--k-th` (a partition shorter than the migration tail a step splices into it is
indexed before its own start) and `--size` below a cell's membership (a
partition is filled with its cell's members — where the origin wrote past its
buffer instead). The two spell that last refusal from opposite ends and reject
the same arguments: the program measures the widest cell of the whole level, so
every locality reaches the same verdict rather than one refusing while the rest
wait for it, and the mirror measures the cells each rank owns, where any rank's
refusal ends the run. The mirror additionally refuses a rank count whose
neighbour table is not a permutation of the ranks in every direction, a name
space at or above `2³²`, and `--nt=0`: the origin accepts it and simply reports
the initial partitions unmoved, but the mirror requires `--nt >= 1`.

The origin's own `README.txt` — the third file of its directory, not carried as
a source — is its only statement about these knobs, and it sizes a run by
`SIZE = 15 · (n / localities)`, with `n` the particle count and `nt` the
iteration count. That is the relation a calibration should start from. Neither
program enforces it; both simply refuse a `--size` that cannot hold what a
partition must hold.

A usage rejection prints the message above and calls `ocrShutdown()`, which —
unlike the origin's non-zero exit on a bad argument — leaves the process exit
status 0; the run is still caught because the required `CHECKSUM` marker never
appears.

## Structure

Let `nl` = ranks, `L` = `np / nl` cells per rank, `T` = `--nt`, `S` = `--size`.
A body is 88 bytes (three `int`, nine `double`), so a partition is `S · 88`
bytes — 352 kB at the gate arguments. Per rank:

| object | count | size |
|---|---|---|
| driver task | 1 — the SPMD fork | builds the tree |
| `New_Members` tasks | `6 · T` — five of each six are dropped; issued directly by `graph_edt`, not by a separate per-step head task | 2 dependences |
| `compute_position` tasks | `T · L` | 2 |
| `copy_mem` tasks | `T · L` | 1 |
| `changed_members` tasks | `6 · T · L` — five of each six dropped | 2 |
| `Non_Members` tasks | `4 · T · L` — three of each four dropped | 2 |
| reclamation tasks | `L + 1 + T · (6 + 12 · L)`, one per graph value — every stage output the run ever creates, discarded ones included | its own reader count + 3 (a value-ready edge, the run's own completion gate, and the pool RW) |
| gather publish | 1 per rank, folded into `graph_edt`'s own body once its step loop ends — not a separate task | `L` value identities, no copy |
| collect + pull + close tasks | 1; `2 · np` (two passes over every cell, chained one at a time); 1 | `nl + 1` (every rank's gather point RO, the start point RO); 2 each; `nl + 1` (every rank's cleanup point NULL, the checksum's own report NULL) |
| stage blocks | drawn from the pool, one per graph value, recycled as values are reaped — the pool's ceiling is the `L + 1 + T · (6 + 12 · L)` above | `S · 88` bytes |
| initial blocks | `L + 1` partitions | `S · 88` bytes |
| arrival points | `6 · T`, every one of them crossing or self-addressed | — |

At the gate arguments (`n=20000`, `T=2`, `S=4000`, one rank, eight cells:
`L=8`) that is 204 stage tasks (the origin's own operation count) and 12
arrival points. `graph_edt`'s pool is sized to the worst case if nothing were
ever reused — `L + 1 + T·(6+12·L)` = 213 partitions of 352 kB, one per graph
value the run can ever create at once — though the pool's own LIFO reuse can
in practice serve the run from far fewer live allocations than that ceiling.
The task counts are the origin's own, including the tasks whose results it
discards: of the six `New_Members` a step launches, the slot keeps only the
last, and of the six `changed_members` and four `Non_Members` a cell launches,
likewise — the origin overwrites the slot before anything reads it. The mirror
runs all of them, because they are the program's work, and every one of their
outputs — kept or discarded alike — is a graph value with its own reaper; none
of them is left to a plain heap buffer.

The tree is the same on every rank and is not a block: `cell` is an array of a
million cells the origin allocates whatever `n` is, and `b` the body array. At
the gate the tree reaches 585 cells and index 584.

## Wiring

**An OCR data-block dependence is satisfied when it is *added*, not when the
block is written**, so a block a task produces for a task that already exists
travels on a labeled STICKY point from the program's single
`ocrGuidRangeCreate(…, GUID_USER_EVENT_STICKY)` range, indexed through
`mirror_edge` as `ordinal · nl + consumer` — an index that gives every point
exactly one producer and one consumer and never aliases two units of work onto
one. Where that index also settles a point's *home* is the runtime's affair,
and only the hop count follows from it: ARTS and ocr-vx spread a labeled range
by index (`index % nranks`), so a point lands on its consumer; xsocr stamps the
reserving PD's own location into every GUID of the range, so a range reserved
in `mainEdt` homes all of its points at rank 0. This row has one kind of point
and one other mechanism:

* **An arrival** is the partition one rank pushes to one of its six neighbours
  for one step: `notify_value` links the value's own sticky event directly to
  `arrival(step, receiving direction)` at the neighbour — no copy; the value
  *is* the block its far-end reader will acquire — which is what the origin's
  `hpx::post(from_X_action(), neighbour, step, p)` does with a serialized
  partition: an identity crossing a rank, not a fresh one. The `New_Members`
  task at the far end reads the point RO; among the value's recorded readers,
  it is what eventually triggers the value's one reap task. The ordinal is
  `step · 6 + direction`: the origin pushes **one partition per direction per
  rank per step**, not one per cell, so the index counts directions and steps
  and nothing else. Each has exactly one producer — the neighbour tables are
  permutations at every rank count the row runs.
* **Everything else is wired directly by `graph_edt`**, which builds the whole
  `nt`-step chain — every direction's `New_Members`, and every owned cell's
  `compute_position → copy_mem → changed_members → Non_Members` — in one task,
  before any of it runs. Each logical operation becomes two "stage" EDTs, the
  origin's own two continuations (discarded outputs included), threaded
  through a small graph of values: a value is a sticky event plus the list of
  later-created tasks that will read it, appended as `graph_edt` wires each
  reader in turn. No point is needed for any of this — a value's consumer
  already exists, with its dependence added, before the value's own producer
  can run — so the intra-rank wiring is complete before `graph_edt` returns.
  Only the arrival above still needs a labeled point, because the far end's
  tasks do not exist yet when the value that will reach them is created.

**Reclamation.** Every value `graph_edt` creates — kept or discarded, with one
reader or several — gets exactly one reap task, mirroring the origin's own
`partition_allocator`: a per-rank pool hands out a retained block (LIFO) or
allocates a fresh one, and nothing is ever freed before the run ends, which is
what the pool's unbounded caching preserves. A reap task is gated on the
value's own readiness, on the output event of every task `graph_edt` recorded
as one of its readers — a counted list, not a single-reader shortcut, so a
value six `changed_members` tasks read waits for all six — and on a `building`
gate that closes only once `graph_edt` has finished appending every reaper's
own reader list, so a reaper's creation can never race one more reader being
added to the value it is about to reap. It returns the block to the pool
rather than destroying it: the same buffer a later allocation may hand
straight back out, the origin's own LIFO reuse rather than a fresh
`ocrDbCreate` every time.

Three ordering obligations and where they are met. A once-type event is
destroyed when it fires on XSOCR, so every consumer of one is registered
before it can fire: `graph_edt` appends a value's reader list as it wires each
reader, and only wires a reaper's own dependences — one per recorded reader —
after every one of them has been appended. A counted quantity is decremented
through a task's output event and never from a body, which every reaper's
reader-dependence is an instance of. And a block is released before it is
handed on: a stage task releases the block it produced before it returns, so
the tasks `graph_edt` already wired on that output event never observe it
still held.

## Flow

`mainEdt` validates the arguments, reserves the point range, creates the
collect task (behind every rank's gather point and the start point), the pull
chain's first task and the close task, and forks one driver per rank. A driver
builds the tree, opens every point it will ever read, and creates
`graph_edt`. `graph_edt` checks every rank's cells against the partition size,
stamps the run's start on rank 0, builds its cells' initial partitions and the
empty neighbour value out of the source pool, pushes its first cell's
partition to all six neighbours (identity only — see *Wiring*), then builds the
*entire* `nt`-step chain directly, one task at a time, before any of it runs:
each step's six `New_Members` fed by that step's arrival point, then for every
owned cell `compute_position → copy_mem → changed_members ×6 → Non_Members
×4` in the origin's own alias order, threading every result through its own
sticky value and reader list, and — before the last step — pushing the step's
outgoing neighbour value to all six neighbours. Once the chain is wired,
`graph_edt` publishes the rank's `L` final values to the gather point and
creates one reap task per value the run ever produced, kept or discarded
alike, then returns. Locality 0's collect task assembles every rank's values
into one array and starts the pull chain: the first pass acquires each cell's
retained partition RO and keeps nothing of it, as the origin's `.wait()` loop
does; the second pass, chained behind the first, acquires the same partitions
RO again, sums the positions and reports. The close task, behind every rank's
own cleanup and that report, ends the run.

| HPX wait site | classification | mirror |
|---|---|---|
| `Left_.get()` … in `send_*` | start wait — neighbour ids resolved at setup | the neighbour table, computed |
| `receive_*(time)` feeding a `dataflow` | start wait — this step's arrival | the `New_Members` task's read-only dependence on the arrival point |
| the chain's `.then(…)` and `dataflow(…)` stages | continuations — every input ready | one task per stage, all wired directly by `graph_edt` before any of the chain runs |
| `result.get()` before the gather | driver wait — added by the modernisation, because the collective now takes the value where it took a future | `graph_edt`'s own publish of the rank's `L` values, once its step loop ends |
| `overall_result.get()` | driver end wait — the gathered cells | the checksum print inside the pull chain's second pass, behind one RO acquisition of every gathered cell's partition |
| the `get_data(cell0).wait()` / `.get()` loops | driver end waits — locality 0 pulling each cell's whole partition, twice | the two-pass pull chain: one RO acquisition of every cell's retained partition per pass, no copy, the second pass chained behind the first's completion |

**Mid waits: 0.** No stage body blocks on a future and then computes: every
stage is a continuation on ready values, and the three `.get()`s are all in the
driver body, two of them the origin's own and one the modernisation's.

**The per-cell iterations are a strict chain, and that is the row's shape.**
In `do_work` the six `From_*`, the six `To_*` and `n_` are *aliases* — all six
`From_*` name `Parent_[(time+1)%2]` and index `[0]`; `To_B_ … To_D_` and
`next_members` all name `U_[(time+1)%2]`. So iteration `j`'s
`compute_position` takes the value iteration `j-1`'s last `Non_Members` wrote,
and no two cells' stages overlap. That is what makes the program's answer well
defined even though `compute_position` mutates the process-global body array,
and it is why the mirror is a chain rather than a fan.

`[APP_E2E]` covers the same phases on both sides, and is the metric this row's
campaigns select (`timing_metric: app_s`) — it is **not** the same span as
`[E2E]` on this row. On the HPX side the origin opens one clock at the top of
`hpx_main`, before `create_Octree()` (so the tree build is inside it), and a
second, `app_clock`, immediately before `step.do_work()` — after the tree, the
interaction lists and the stepper component — and stops **both** at one
`print_e2e(clock, app_clock)`, placed after the gather, the first pull pass,
the origin's own elapsed-time print and the checksum accumulation. So
`[APP_E2E]` covers `do_work`, the gather and both final read passes, while
`[E2E]` additionally contains the octree build; only rank 0 prints either. On
the OCR side the mirror stamps `mirror_now_ns()` at the top of `graph_edt` —
after the tree build, the interaction lists and the partition-size check,
before the initial partition fills — and carries it through a DB and a sticky
event to the pull chain, which prints `[APP_E2E]` after its second pass's
accumulation and before `CHECKSUM`. The boundary matches the spec's
prescription (follow `do_work` and the two final read passes) on both sides,
with two disclosed asymmetries: the origin's own elapsed-time print sits
inside its `[APP_E2E]` with no mirror counterpart, and the mirror's eager
graph-wiring cost — every `ocrAddDependence` call `graph_edt` makes for the
whole `nt`-step chain — sits inside the mirror's window, where the origin's
`dataflow` construction is far cheaper. The runtime's own `[E2E]` is still
printed as a separate observation, never the selected metric.

**What the scalar can and cannot see.** `CHECKSUM` is the sum of `r1[0]`,
`r1[1]` and `r1[2]` over every body of every gathered cell, in locality order,
then cell order, then entry order — the simplest digest of the final state the
program already holds, since the program computes no printable quantity of its
own. It follows the bodies as they migrate between cells and localities, which
is the row's distributed content. The mirror prints it `%.14e` where the
origin prints `%.14g`: deliberate, not a fidelity gap — xsocr's own `printf`
replacement has no `%g` conversion, so a form every OCR runtime can render was
chosen instead of the origin's. The catalog's `scalar_re` and tolerance accept
either form and the parsed values agree. It is **blind to the force kernel**: a
partition carries the positions its bodies had when it was built, because
`compute_position` writes only the force back into the partition it passes on,
while the positions it integrates go into the process-global body array and
reach nothing else. The kernel is therefore a closed loop with no observable
output — in the origin as much as in the mirror — and no digest of the
program's own final state can see it. This is disclosed rather than repaired
(A4), and it is also what makes the answer independent of the locality count:
`5862219` at 1, 2, 4 and 8 nodes on every entry.

Two consequences worth stating plainly. The mirror's force values and the HPX
program's *cannot* be expected to agree, because the origin builds its tree
with concurrent tasks that race on one field (below), so the interaction lists
— and every force computed from them — are not reproducible even between two
HPX runs. And a mirror whose kernel was wrong would not be caught by this
number; the row's fidelity is established by reading the mirror against the
origin, which is what the task review does.

**What is left as written.** The force kernel keeps the `1 +` each interaction
adds to the force and the `1 +` in its denominator, and the single-precision
acceleration. `changed_members` keeps the stray semicolon that makes its
positive-direction test a statement whose outcome is discarded. `new_tree`
keeps walking to the root and changing nothing, because no cell ever records a
neighbour (`neighbor()` exists and nothing calls it) — and the one statement
the mirror does not carry is the call that walk would make if a cell ever did
record one: `insert_node`, which counts the body into that neighbour's cell.
It is unreachable as the program stands (`neighbors[]` is `-1` at
initialisation and in every boundary assignment, and nothing writes it
afterwards), and it is named here rather than transcribed, because copying
unreachable code buys the appearance of fidelity and not fidelity. An edit
that ever populated `neighbors[]` would make the two sides diverge silently;
this paragraph is its warning. `Non_Members` keeps the
branch that copies nothing when the first free slot is the first slot. And the
tree build keeps its concurrency: `strc3` subdivides sibling subtrees in
parallel and a body that sits exactly on a split plane belongs to two cells, so
two tasks write that body's `parent` field at once — 628 of 20000 bodies at the
gate arguments. Every other cross-task write in that phase is to a cell of the
task's own subtree. The value it settles on reaches only the interaction lists,
and through them only the force, so it moves nothing this row reports; it is
named here because it is why the forces are not reproducible. The mirror
builds the tree the same way: `subtree_edt` recurses with `EDT_PROP_FINISH`,
spawning one child task per over-threshold child concurrently and sharing the
body DB RW across siblings, so the same race on a split-plane body's `parent`
field exists in the mirror too — it is the origin's own race, carried rather
than serialised away.

## Placement

One rank owns a contiguous run of the level's cells: `1 + i + (8/nl)·rank` for
the eight-cell level and `9 + i + (64/nl)·rank` for the sixty-four-cell one —
the origin's `get_cell_foreach_lc`. Its six neighbours come from the origin's
own step table (`±1`, `±2`, `±4` through `idx`), which is a permutation of the
ranks at 1, 2, 4, 8, 16, 32 and 64 and is undefined elsewhere; the mirror
refuses a rank count it does not name.

The mirror states that placement on every object: every driver, `graph_edt`,
`New_Members`, `compute_position`, `copy_mem`, `changed_members`,
`Non_Members`, reclamation, collect, pull and close task carries an explicit
`ocrAffinityGetAt(AFFINITY_PD, …)` EDT hint for its rank, and every partition
and gather block an explicit DB hint for the rank that writes it. Nothing is
left to the build's no-hint policy.

What crosses a rank is therefore what crosses in the origin: six partitions per
rank per step (352 kB each at the gate), and at the end every cell's whole
partition to rank 0 **twice** — `2 · L · S · 88` bytes per rank, which is what
the origin's two pull loops serialise out of `get_data`, a component action
that returns a partition by value. Where a labeled range is homed by index
(ARTS, ocr-vx) a push is one message when it has somewhere to go and none when
the neighbour is the rank itself — which four of the six directions are at two
ranks and all six are at one; where a whole reserved range is homed at the PD
that reserved it (xsocr) every satisfy is instead a message to rank 0 and a
forward from there, and no push is free.

Per-rank process state stands where the origin keeps file-scope objects: the
body array, the cell array, the rank's cell list, and the two slots the chain
passes its values through. Only the body array is written after the tree is
built, and only by `compute_position`, which the chain serialises.

## Sizing

**This row's width is its cell count — 8, or 64 from nine localities — whatever
the node count, and it does not change with any argument.** `--n`, `--size` and
`--nt` all deepen or lengthen the work inside those cells; none of them widens
it. So the row fails the width rule at every geometry: at the gate arguments
eight cells are spread over as many as eight ranks, and a rank with one cell
runs one chain on one worker of the sixteen it was given. It is an anti-scaler
by construction and is reported as such, never resized to look better (spec
§4.8, the base-tier rule).

Measured at the gate arguments, `[E2E]` (predating this row's `app_s` timing
contract, so not `[APP_E2E]`): the HPX program 0.341 s at one locality and
0.780 s at two — it grows, the per-step pushes costing more than the
shortened chain saves. `nbody_hpx_arts_ocr_val_wb` 0.331 s at one rank, 0.205 s
at two, 0.120 s at four, 0.094 s at eight — it falls while there are still
cells to hand out, and there is nothing beyond eight until the level changes.
Both are local numbers on one machine and are a rough trend only.

Memory has two terms and the first is fixed: the origin allocates a million
cells per rank whatever `n` is (~350 MB resident at one locality for the HPX
program, 288–296 MB per rank above it), and the mirror allocates the same array
— it merely does not touch the pages it never reaches. The second term is the
partitions, drawn from `graph_edt`'s own pool: its capacity is
`(L + 1 + T·(6+12·L)) · S · 88` bytes — ≈75 MB at the gate arguments — the
worst case if every allocation the run will ever make were live at once, not a
measurement; the pool's LIFO reuse is what lets the bytes actually resident at
any moment track however many partitions are live rather than that ceiling,
and there are no separate end-phase copies on top of it any more, since the
final passes now acquire the retained partitions directly. A calibrated size
must still watch `--size` and `--nt` together, since both feed the same
ceiling. `nbody_hpx_arts_ocr_val_wb` measured 726 MB resident at one rank,
most of it the runtime's own arenas.

The `hpx-gate` roster runs `--n=20000 --nt=2 --size=4000 --theta=0.3 --th=100
--k-th=500` at every node count, which finishes in a few hundred milliseconds
per cell. As with the other rows of this section, the argument for the paper's
measurement is the calibrated one at the end of this section.

**xsocr does not stand up cleanly, for two independent reasons — one now
repaired in the runtime tree, one still open.** At two ranks,
`nbody_hpx_xsocr` segfaults in about half its runs, always at the same place
and never in the mirror's own code: `hcPolicyDomainProcessMessage`
(`hc-policy.c:3598`, case `PD_MSG_DEP_SATISFY`) reached from
`processIncomingMsg`, on the line whose own comment reads *"TODO I think
there's a MD cloning issue here. The dstKind can be remote."* — an incoming
satisfy for a labeled event whose metadata the receiving policy domain does not
hold, dereferenced without the assertion a release build compiles out. It is
not the mirror's point discipline: opening every point in the driver, before
anything can publish to one, left the rate where it was (5 of 10 before, 6 of
10 after), and a run with a single step, where no step ever ends and the only
pushes are the drivers' own, passes 10 of 10 — as does `mini_ghost_hpx` on the
same runtime and geometry. The window is a rank pushing an arrival for a step
its neighbour has not reached, which this row leaves wide because its ranks are
coupled through nothing else. One fact about where the points live belongs
beside that, as a fact and not as a diagnosis: xsocr homes a whole reserved
range at the PD that reserved it, so on that runtime every one of this row's
`6 · nt · nl` labeled creates, and every satisfy of one, lands on rank 0's GUID
table while that table is also serving the forwards and destroys for the same
range. This defect's status is unchanged and unconfirmed here.

At **one** rank a second, unrelated defect used to reach every run: a reap
task learns which block to return to the pool by reading a GUID out of a slot
wired `DB_MODE_NULL` — spec-legal (a `DB_MODE_NULL` slot's GUID is still
passed to the task) and exactly what ARTS and ocr-vx do — but stock xsocr
zeroed that GUID on a `DB_MODE_NULL` slot instead of passing it through
(`hc-task.c`, in both `taskAllDepvSatisfied` and `satisfyTaskHc`), so the
reaper pushed `NULL_GUID` onto the pool, the next allocation handed it out
uncreated, and the next stage task faulted on its first write. That repair has
since landed in this tree: `hc-task.c` no longer zeroes a `DB_MODE_NULL`
slot's GUID, so the one-rank entry, which used to fail this way on every run,
is exact again. The two defects are independent — one in cross-rank
message-dispatch metadata, the other in single-rank NULL-mode dependency
handling — and only the second is confirmed repaired here.

**Calibrated arguments.** `--n=120000 --nt=100 --size=24000 --theta=0.3
--th=100 --k-th=500` at every node count. `nt` keeps the published value,
`size = n/5` the gate's relation, and the width — the eight cells of the top
split — is exempt from the width floor. Derived to the 100 s window through
the row's measured two-point work exponent and rounded to the body count; the
anchor measured 86.6–94.5 s on the ARTS arms (INV/WB 91.7 s) and 51.5 s on
HPX, 7.3 GB resident on ARTS and 9.0 GB on HPX.