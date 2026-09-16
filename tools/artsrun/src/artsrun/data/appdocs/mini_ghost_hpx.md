# mini_ghost_hpx

*Mantevo's MiniGhost: a 3D halo exchange on an `npx × npy × npz` process grid,
one rank per position, several variables per rank, each advanced by a 27-point
stencil with a global sum per step.*
Origin: the HPX port of MiniGhost
(`third_party/miniapps/MiniGhost/src/`, STE||AR-GROUP/miniapps, pin
`0cf9c1c1`, 2015-10-22), copied to `benchmarks/hpx/mini_ghost_hpx/` and built
as `mini_ghost_hpx_hpx`. It is the first row of this section whose origin is
not the vendored HPX tree, and the first that needed a 2014-to-1.11 API
modernisation — the patch is large and that is the disclosure.

## Overview

`hpx_main` runs on every locality. Each rank owns one `stepper` component
holding `num_vars` partitions of the grid; a partition is two grids of
`(nx+2)(ny+2)(nz+2)` doubles (the local block and its halo), filled with
draws from the rank's own `mt19937`, plus one spike injected at the middle of
the global grid. Per step and per variable the rank unpacks the six faces its
neighbours pushed into its halo, computes its interior in
`nx_block × ny_block × nz_block` chunks, and packs the six faces its
neighbours will read at the next step; where a variable is *summed*
(`percent_sum`), the rank's grid sum plus its accumulated flux is broadcast to
every rank and each rank's and-gate waits for `nranks` partials. Rank 0 then
compares the total against the total it injected.

One program on four runtimes: `mini_ghost_hpx_hpx` is the HPX program,
`mini_ghost_hpx_arts_<variant>`, `mini_ghost_hpx_xsocr` and
`mini_ghost_hpx_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/mini_ghost_hpx.c`), one row in `hpx_apps.yaml`.
HPX primitives used: a component with seven actions (`set_global_sum`,
`set_{north,south,east,west,front,back}_zone`), `hpx::dataflow`,
`hpx::when_all(...).then(...)`, `hpx::post`, `hpx::lcos::broadcast_post`,
`hpx::lcos::local::receive_buffer`, `hpx::lcos::local::and_gate`,
`hpx::distributed::barrier`, `hpx::register_with_basename` /
`find_all_from_basename`, and `hpx::serialization::serialize_buffer` for the
zones on the wire. The mirror's mapping: one block per grid generation homed
at its rank, one task per origin task hinted there, each packed zone its own
block on a labeled point to the rank that unpacks it, and the all-reduce as
`nranks` publishes with no reduction tree, because the origin's
`broadcast_post` sends `nranks` parcels and builds none.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--scaling` | 1 strong (the dimensions are global), 2 weak (per rank) | 2 | reachable |
| `--nx` `--ny` `--nz` | grid dimensions | 100 each | reachable |
| `--ndim` | one dimension for all three, overriding `--nx`/`--ny`/`--nz` when given | unset | reachable |
| `--nx_block` `--ny_block` `--nz_block` | chunk size inside a rank | 100 / 100 / 10 | reachable |
| `--num_vars` | variables per rank | 5 | reachable |
| `--num_tsteps` | steps | 100 | reachable |
| `--stencil` | 20 none, 21 2D5, 22 2D9, 23 3D7, 24 3D27 | 21 | reachable |
| `--percent_sum` | how many variables are summed (100, or a tenth) | 0 | reachable |
| `--num_spikes` | injections | 1 | must be 1 (below) |
| `--npx` `--npy` `--npz` | process grid; the product must equal the rank count | 1 each | reachable |
| `--error_tol` | tolerance of the conservation check | 1e-5 | reachable |

Two of the origin's own rules govern the ladder. `--scaling` defaults to
**weak**, so a strong-scaling campaign must pass `--scaling=1`; and with
`npx == npy == npz == 1` and more than one rank the program silently sets
`npx = nranks`, which is its own design and is kept on both sides. The mirror
rejects as usage what neither side can compute from: a process grid whose
product is not the rank count, a scaling mode outside {1,2}, a stencil outside
20..24, `percent_sum > 100`, a zero block size, `num_tsteps < 1`, a local
dimension below one, and a name space at or above `2³²` (the OCR shim narrows
a GUID range count and every index derived from it to 32 bits). It also
rejects `--num_spikes` other than 1: in the origin that argument is not an
injection count but an outer loop that runs the **whole** `num_tsteps`
simulation once per spike, inserting the next spike into both grids before
each pass, and this program runs one pass — so an argument on which the two
sides would compute different programs is usage rather than a silent
difference. The draws of the other spikes are still consumed where the origin
consumes them, so the field is the origin's either way. The origin
checks the process grid and `percent_sum` and calls `hpx::terminate()`; it
does not check the rest.

Five more of the origin's options are refused rather than silently ignored:
`--report_diffusion`, `--checkpoint_interval`, `--checkpoint_file`,
`--report_perf` and `--debug_grid`. Each names work this program does not
implement — `--debug_grid` in particular turns the origin into a different
program, since its own setup forces every variable summed and a grid written
to file — so naming one is usage rather than a silent no-op that would let a
run differ from the origin with no diagnostic. `--npdim`, which the origin's
own option parser registers and never reads (only `--ndim` is ever read), is
accepted and likewise computes nothing: the mirror does exactly what the
origin does with it, not what its name might suggest.

A usage rejection prints the message above and calls `ocrShutdown()`, which —
unlike the origin's non-zero exit on a bad argument — leaves the process exit
status 0; the run is still caught because the required `ERRMAX` marker never
appears.

## Structure

Let `nl` = ranks, `V` = `num_vars`, `T` = `num_tsteps`, and `C` =
`⌈nx/nx_block⌉·⌈ny/ny_block⌉·⌈nz/nz_block⌉` chunks per step per variable
(7 at the gate arguments: one in x and y, seven in z). Per rank:

| object | count | size |
|---|---|---|
| driver task | 1 — the SPMD fork | — |
| init tasks | `V` — one per variable, behind that variable's own initial all-reduce (`SUM_READY`) | 1 dependence |
| spawner tasks | `V · T` — one per variable per step | 0 dependences (created by the spawner of the step before it, or by `start_edt` at step 1) |
| unpack tasks | `(T−1) · V · (neighbours)` | `(neighbours) + 2` each: the face point, every packed point of the step before, and `gsrc` (RW) |
| flux tasks | `V · T` | `(neighbours) + 1` |
| chunk tasks | `V · T · C` | `gsrc` (RO) + `gdst` (RW) + its boundary unpacks (0–6), plus, for `t > 1`: the previous step's completion point on either arm, and, where the variable is not summed, itself and up to six face-adjacent previous-step chunks as well |
| pack tasks | `V · T · (neighbours)` — every step, the last included; a last-step pack's zone is retired by a reaper rather than unpacked | (gating chunks) + 1 |
| sum tasks | `V · T`, for a summed variable | `C` (chunk completions) + 1 (flux) + 1 (`gdst`) + 1 (cumulative flux), and `(neighbours)` pack completions on the last step only |
| arrive + reduced tasks | `nl` and `1`, per summed variable per generation (`T+1` generations: the initial one plus one per step) | 3 (4 at the initial generation) and 2 |
| check tasks | `V · T`, for a summed variable | 1 (`SUM_READY`), +1 (`ERRMAX`, RW) on rank 0 |
| advance tasks | `V · T`, for an unsummed variable | 1 (flux); on the last step `C` more and `(neighbours)` pack completions — the chunks' and packs' own output events, so the retirement below never races their release; the step's sticky chunk points, which no later step will read, are then freed by this same task's body |
| reaper tasks | `V · Σ_r(neighbours)` overall — one per posted last-step face, run on the rank the face is addressed to | 1 (the posted face, RO) |
| done task | 1; final task 1 on rank 0; close task 1 on rank 0 | 1; `nl + 1`; and 1 (the reaper latch) |
| grid blocks | `2 · V` | `(nx+2)(ny+2)(nz+2)` doubles |
| face blocks | `V · T · (neighbours)` | `ny_·nz_`, `nx_·nz_` or `nx_·ny_` doubles |
| partial blocks | `(T+1) · V · nl` | one double |
| face points | `V · T · (neighbours)`, every one of them crossing | — |
| partial points | `(T+1) · V · nl`, of which `(nl−1)/nl` cross | — |
| step points | `(T−1) · V`, on either arm, never crossing | — |
| packed points | `(T−1) · V · (neighbours)`, never crossing | — |
| completion points | `nl`, at the very end | — |

At the gate arguments (64³, `V=4`, `T=10`, one neighbour per rank at two
ranks) the block counts are 8 grid blocks of 2.30 MB (one rank) or 1.18 MB
(two); 40 face blocks of 34.8 kB; and 88 partials of 8 B — none of that
changed by the ordering fix below. The task graph now also carries, per rank,
`nl · (T+1) · V` arrival tasks and `(T+1) · V` reduced tasks (the collective's
own machinery), and, at every rank with a neighbour, `V` reaper tasks per
neighbour and one close task at the very end. The point reservation is
`(T+2) · V · max(6, nl, C) · KIND_COUNT · nl` names, with `KIND_COUNT = 6`
now that the step's own completion and a pack's are kinds of point of their own
— 2016·`nl` at the gate (`C = 7` there) — which is a reservation, not an
allocation.

The task counts are the origin's own: one unpack per valid receive buffer per
step (`receive_boundaries`), one flux continuation
(`when_all(recv_futures).then(...)`), one chunk per block range
(`when_all(dependencies).then(get_stencil_call_op(...))`), one send per
direction whose buffer has a destination, and one `sum_grid` dataflow per
summed variable per step — split in two, for the reason under *Flow*.

## Wiring

**An OCR data-block dependence is satisfied when it is *added*, not when the
block is written**, so every block a task produces for a task that already
exists travels on a labeled STICKY point from the program's single
`ocrGuidRangeCreate(…, GUID_USER_EVENT_STICKY)` range, indexed through
`mirror_edge` as `ordinal · nl + consumer` — an index that gives each point one
producer and one consumer, and, where a labeled range is homed by index (ARTS
and ocr-vx spread it `index % nranks`), makes **a point's home its consumer's
rank**; xsocr homes a whole reserved range at the PD that reserved it, so there
every satisfy is a message to rank 0 and a forward from it. This row has six
kinds:

* **A packed face** is the payload and therefore the block (O3): the pack task
  creates it, fills it from the grid this step wrote, releases it, and
  satisfies `face(step+1, variable, opposite direction)` at the neighbour,
  which is exactly what the origin's `send_buffer` does with
  `hpx::post(SetOppositeZoneAction(), dest, buffer, step+1, var)`. The unpack
  task at the far end consumes it, destroys the point and destroys the block —
  except the face posted for the step *after* the last one, which the origin
  also sends (`send_boundaries` runs on every step, the last included) but no
  step ever unpacks: a reaper task, created by the same pack and placed on the
  rank the face is addressed to, consumes it instead, destroying the point and
  the block exactly as an unpack would. Every face crosses a rank here — one
  grid block per rank, and the process grid equals the rank count — so there
  is no within-rank case.
* **A partial** is one double per (step, variable) per *destination*: the sum
  task creates `nl` of them and satisfies `nl` points, because the origin's
  `broadcast_post` sends one parcel per target and builds no reduction tree
  (O5, flat where the origin is flat). Each has one consumer, an *arrival*
  task — one per sender, mirroring the origin's per-parcel `set_data` — which
  reads the point RO, destroys the point and the block, then adds its value
  into the destination's own accumulator DB under the origin's own two locks
  (`gate_mutex` then `value_mutex`, in that nesting) and releases both before
  it returns. The arrival's part in the generation's participant latch is its
  own **output** event, registered at the arrival's own creation and raised
  only once its body has released the accumulator — never a decrement from
  inside the body — so a *reduced* task, gated on that same latch, can only be
  admitted once every arrival has let go of its hold; that is what makes
  destroying the two accumulator DBs at the end of the variable's life safe.
  `reduced_edt` then reads the accumulator once, clears it and forwards the
  total. Sums land in **arrival order**, exactly as the origin's
  `value_ += value` does under its gate's mutex — not rank order, so this is
  the origin's own arithmetic rather than an approximation of it.
* **A packed zone's completion** is a data-free signal from a pack to the
  unpacks of the next step on its own rank, one per (step, variable,
  direction). A pack reads a **full padded plane** of the generation those
  unpacks write, so the two meet on the box's edge lines wherever a rank has
  neighbours on more than one axis: the pack releases the generation it read,
  then raises this point, and every unpack of the next step waits on all of the
  step's before it copies — which is exactly the edge the origin's fixed
  `receive_boundaries` takes on the send futures its step kept. It is raised
  only when a step follows; the last step packs and posts like every other one
  and nothing waits for what it packed. Each point has one consumer per
  receiving direction of the next step, all on its own rank, and the unpack of
  its **own** direction destroys it: a consumer, and one that cannot run before
  every unpack of its step has been registered, because the spawner adds their
  block dependences last.
* **A chunk's completion** carries the ordering the next step's chunks read,
  and only a variable that is **not** summed needs it: a summed variable's
  chunks get the previous generation together with the step's completion
  (below), because the sum that completion follows waits for every chunk of the
  step. The chunk task releases both grid generations it held —
  the source RO and the destination RW — before it raises this signal, so the
  point never outlives the hold that produced it. It is a data-free signal with
  more than one consumer — a chunk and its up-to-six face-adjacent neighbours,
  all on its own rank — and the consumer at the **same** chunk index destroys
  it: a task that waited on it, and so one that runs after it was raised and
  after every other consumer was registered. Nothing that merely ran later may
  free it, because nothing orders the spawner chain behind the chunks.
* **A step's completion** carries what the next step's chunks wait for beyond
  their own halo and neighbourhood, which is the one ordering the origin gives
  them there (`sum_future[src_]`): the reported all-reduce where the variable is
  summed, and the step's flux where it is not — on that arm the origin's
  `sum_grid` hands back the flux future itself. It is raised by the check task
  once the step's error has been folded into `ERRMAX`, or by the advance task
  once the flux is in hand; it is read RO by every chunk of the next step; and
  it is destroyed by a task that waited on it once every one of those chunks
  has been registered — the next step's own join on the summed arm, whose
  dependences are all of them, and the next step's chunk at index zero on the
  unsummed arm, which has no such join. The last step raises none: nothing
  reads it.
* **A rank's completion** is one data-free point into the final task.

Three ordering obligations the OCR contracts impose, and where they are met.
A once-type event is destroyed when it fires on XSOCR, so every consumer of a
task's output event is registered **before that task can run**: the spawner
creates the sum (or advance), the flux, the chunks and the packs first,
registers each output event everywhere it is needed — a pack's only on the last
step, the one step whose join names the packs — and only then adds each task's
own block and point dependences, the immediately-satisfied ones last.
A latch is decremented through an output event and never from a body: the
per-rank done latch counts the output events of each variable's last check (or
advance), registered by that step's spawner, and a generation's participant
latch counts the output event of every arrival task, registered by
`prepare_sum` at each arrival's own creation, never a decrement from inside
the arrival's body. And a block is released before it is handed on — the sum
task releases its cumulative-flux hold and its destination-generation hold
before it satisfies anything, so the next task to acquire either, the next
step's own sum and the next step's unpacks, can only be admitted once this one
has let go; a pack releases the generation it read before it raises its own
completion point, and an unsummed variable's chunk task releases both
generations it held before it raises its own, for the same reason.

**What is not ordered, because the origin does not order it.** The six unpack
tasks write disjoint halo *planes* of one grid block, but the planes meet at
the halo's edges and the origin's six `hpx::async` unpacks overlap there
exactly as the mirror's six tasks do. That overlap moves no value: an edge
cell's two writers carry the same diagonal rank's same cell, reaching this rank
by two different face hops, so whichever lands last writes what the other one
would have written — and the first two generations are zeroed, so it holds from
the first step. The chunk tasks of one step likewise write disjoint regions of
one block, as in the origin. One pair crosses a step, now that both sides issue
every step up front: a step's sum holds its destination generation RO, over the
interior, while the next step's unpacks hold that same block RW, over the halo
planes — disjoint regions, and the same unordered pair the fixed origin has,
where the sum task's `sum_all_grid` and the next step's unpack tasks are gated
on nothing in common.

**The pair that did move the answer, and how it is closed.** It crosses a
step: a pack of step `t` reads the grid block that the unpack of step `t+1`
writes, and the origin **as published** ordered neither of them — it launched
each send through `when_all(...).then(...)`, discarded the future, and its
post-loop `wait_all` names the sums, the flux, the receives and the
calculations but not the sends. That is a defect of the program rather than a
property to preserve, and it is fixed in the origin copy as a disclosed edit
(the last bullet of `ORIGIN.md`, carried in `origin.patch`): each step keeps
the futures of its sends, and every unpack of the next step waits on all of its
own rank's sends of the step before, ahead of its copy — the receive itself
still inside the task. The mirror carries the fixed program: each pack raises a
point of its own, one per (step, variable, direction), and every unpack of the
next step waits on all of the step's, so the mirror adds no ordering the origin
does not have. Measured **before that fix**, the difference was not academic:
the HPX program gave ten different values in ten runs at `2×2×2`
(3.566363e-01 … 3.637996e-01) and ten different values in ten at `2×2×1`
(7.857e-02 … 8.793e-02), while the mirror — which was ordered first, through
the step's own join — gave one value on every entry: ten runs each of
`arts_ocr_val_wb`, `arts_ocr_inv_wb`, `arts_ocr_excl_retain` and `ocrvx` at
`3.596480e-01`, and `xsocr` at `3.596484e-01`, its formatter's offset, a single
value sitting inside the HPX spread. Both sides close the pair by construction
now, and the measurement agrees: at `2×2×1` on four ranks, the gate's arguments,
five runs of the HPX program and five of the INV/WB mirror all print
`8.334530e-02` — the fixed origin repeats itself where it did not, and agrees
with the mirror bit for bit. Where a rank has neighbours on one axis only the
plane a pack reads and the plane an unpack writes are disjoint, the pair cannot
arise at all, and every entry agrees with the HPX program bit for bit:
`9.664469e-02` at `8×1×1` (ten runs) and `7.872000e-02` at `4×1×1` (five). The
ladder therefore runs a one-axis grid at every rung — `1×1×1`, `2×1×1`,
`4×1×1`, `8×1×1` — which is what the program chooses for itself when no grid is
named (`npx = nranks`), so the choice restores the origin's own default rather
than imposing one. A cubic grid stays reachable; it is not a rung of the ladder.
What the one-axis ladder costs is disclosed: two neighbours per rank instead of
six, a thinner halo than a cubic decomposition exercises.

**One more thing that *is* ordered, because leaving it unordered was a
defect.** The origin gated each direction's send on the chunk at the opposite end of that
axis from the plane it packs — `WEST` packs `g(1,y,z)`, which the first
x-chunk writes, and waited for the last — and all six directions were crossed
the same way. Since the chunks of a step are mutually unordered, the pack could
read a plane this step had not yet written and ship the previous generation's
values. With one chunk on an axis the gate lands on the writer and nothing
happens, which is why it stays hidden until an axis is cut: at `npz > 1` with
`nz_block` below the local depth, twenty eight-locality runs produced twenty
different answers, on the HPX program and on the mirror alike. Both sides now
wait for the chunks that write the plane they pack, and the mirror's twenty
runs then produce one answer. The HPX program's did not repeat even then, which
is what led to the cross-step pair above; that one is closed on both sides too.

## Flow

`mainEdt` validates the arguments, reserves the point range, creates the
reaper latch and the close task behind it, creates the final task on rank 0
with one completion point per rank, and forks one driver per rank. A driver
allocates its rank's grids, draws the origin's stream in the origin's order —
for every variable a spikes object (its values, then its locations) and then
that variable's grid — opens the initial collective and publishes its share of
each variable's initial total to every rank, and creates one init task per
variable, gated on that variable's own initial all-reduce (`SUM_READY`) and
counted into a run-wide init-barrier latch of `nvars · nl`. An init task
itself does no more than retire that sticky and forward the reduced initial
total: once every rank's every variable has cleared the barrier, each rank's
single `start_edt` — which holds one RO reader of every variable's reduced
total and RW of both its grids — inserts each variable's spike into both
grids in variable order, adds it to `source_total` on rank 0 (whether or not
the spike landed on this rank), releases the grids and opens that variable's
step 1. Each step's spawner builds that step's tasks — including, on every
step, that step's packs — and, at its end, creates the spawner of the step
after it: no join stands between two steps, which is where the origin's own
loop stands when it opens its next iteration, and every task of the new step is
gated by its dependences alone (its unpacks on the face point and the previous
step's packed points, its flux on its unpacks, its chunks on the step's
completion, their own previous generation and their neighbouring chunks or
faces, its packs on the chunks that write the plane they pack). The step's join
— the sum task for a summed variable, the advance task otherwise — therefore
joins what the origin's own join joins, this step's chunks and its flux, and
names that step's packs only on the **last** step, where what runs behind it
retires the grids those packs read. The check task that follows the sum raises
the step's completion for the next step's chunks to wait on, or, on the last
step, retires the variable's state; the advance task does the same on the
unsummed arm, where what it raises carries the flux. A rank's last-step packs are matched by reaper
tasks on the neighbouring rank, one per posted face, and a run-wide latch of
`nvars · (twice the grid's adjacent pairs) + 1` — decremented by every reaper
and once by the completion report — gates the one close task that ends the
run.

| HPX wait site | classification | mirror |
|---|---|---|
| `recv_buffer::operator()`'s `buffer_.receive(step).get()` | start wait — this step's zone | the unpack task's read-only dependence on the face point, alongside its dependence on every packed point of the step before (the gate the origin's fixed `receive_boundaries` takes on its kept send futures) |
| `when_all(recv_futures).then(flux_accumulate)` | continuation | the flux task on the unpacks' output events |
| the chunk's `when_all(dependencies).then(stencil)` | continuation — every input | the chunk task's dependences: its boundary unpacks, the previous step's completion point on either arm (the reduction where the variable is summed, the flux where it is not), and, where it is not summed, the previous step's chunks as well |
| `when_all(send_futures[dir]).then(send_buffer)` | continuation | the pack task on the chunks the origin gates that direction with |
| `sum_grid`'s dataflow: the local sum, then `sum_allreduce_[dst].add(...).get()`, then the profiling, the error check and the report | **mid wait** — one per summed variable per step | the task is split exactly there: the sum task releases its holds and publishes its partial; the check task then runs on the reduced total, prints the step's line and raises the step's own completion |
| `stepper->init(p).get()` and `hpx::wait_all(partition_initialized)` | driver wait — setup | the run-wide init-barrier latch of `nvars · nl`, decremented by every rank's every init task |
| `hpx::wait_all(run_futures)` per spike | driver wait — the variables' runs | the per-rank done latch of `num_vars` |
| `mini_ghost::barrier_wait()`, twice | driver wait — collective | the init-barrier latch above, and the completion points into the final task |

**Mid waits: 1 per summed variable per step** — `T · V` at the gate
arguments, and the only ones in the program.

`[APP_E2E]` covers the same phases on both sides, and is the metric this row's
campaigns select (`timing_metric: app_s`). On the HPX side the origin's own
`run_clock` opens at `mini_ghost.cpp:90`, after setup and component creation
but before `stepper->init(p)`, and closes at `print_e2e(clock)` (`:97`), after
the second `barrier_wait()` and before the `ERRMAX` line; `print_e2e(clock)`
forwards to `print_e2e(clock, clock)`, so on this row `[APP_E2E]` and `[E2E]`
are the same span and rank 0 prints one of each. On the OCR side the mirror
stamps `mirror_now_ns()` in `mainEdt`, after the templates and the `ERRMAX` DB
and immediately before the final task and the SPMD fork, carries it through to
the final task, which prints `[APP_E2E]` and then `ERRMAX`. Inside the
mirror's window and not the origin's: the SPMD fork itself. Inside both: the
grids, the draws, the initial all-reduce, the init barrier, every step, and
the closing join. Outside both: option parsing, the `ERRMAX` report, and
profiling. The origin's own `timer_all`, printed only when `report_perf` is
off, starts earlier still, before the locality queries — a third, wider span
neither side's campaign reads. The runtime's own `[E2E]` is still printed as a
separate observation (`ARTS_E2E_MARKER`; runtime init and teardown excluded),
never the selected metric.

**The scalar is the program's own conservation check, and it is not small.**
`ERRMAX` is the maximum over steps and summed variables of
`|source_total − value| / source_total`, the quantity the origin already
computes and already terminates on. Two properties of the origin's accounting
make it grow, and both are carried as written (A4: a doubtful kernel is
mirrored, not repaired):

* the spike is **added** to `source_total` while its insertion **overwrites** a
  grid cell, so the identity is off by the overwritten value from the start
  (5.1e-05 relative at 8³, 1.3e-06 at 64³); and
* `flux_accumulate` reads the **halo** planes (`g(0,y,z)`, `g(nx_−1,y,z)`, …)
  and only on a rank that sits on a global boundary — where no neighbour ever
  writes them, so they stay zero. The flux is therefore identically zero at
  every geometry, and the 27-point stencil's boundary loss is never credited
  back: the interior sum decays about half a percent per step (389 159 at step
  1 to 374 222 at step 10 against a `source_total` of 393 230, one rank, gate
  arguments). Both statements come from this tree's own sources and runs; the
  Mantevo reference implementation, which sums interior layers instead, is not
  vendored here, so nothing above rests on it.

The error therefore exceeds the origin's own default `error_tol` at every
geometry, and the origin calls `hpx::terminate()` when it does — so the row
passes the origin's own `--error_tol` raised to 1, which changes no line of
either program and lets both run to completion. What the four runtimes vote on
is then the measured error itself.

**The rank-0 output, on both sides and inside the measured window.** The
origin prints three things there and the mirror prints the same three, from the
tasks that stand where the origin's statements stand. Its parameter report
(`params.hpp`'s `print_header`: the banner, the stencil, the global and local
dimensions, the variable count, the report interval and tolerance, the summed
count, the steps, the task grid, the scaling mode, the process count and the
date) goes out once from rank 0 after that rank's partitions are built and its
own initial all-reduce has landed, before the first barrier; the mirror prints
it from its rank-0 driver, once that rank's state is built and before any step
is opened. Its summed-sum task then prints `sum_grid at step …`
unconditionally at every step, which the mirror prints from the check task, the
half that holds the step's reduced value. And when the error is over the tolerance or the step falls on
`report_diffusion` — which defaults to the whole run, so that is one line at
the last step — it prints `Time step T for variable V the error is E; error
tolerance is X.`; the mirror prints that line under the same condition, with
the interval fixed at the step count because naming `--report_diffusion` is
usage here. None of the three is read by the driver's marker or scalar regex,
so at the roster's arguments this is `V·(T+1)` rank-0 stdout writes plus a
header on either side rather than a divergence the oracle can see.

**The abort path reports no scalar, on either side.** Where the error exceeds
`error_tol` the origin prints the report line above and calls
`hpx::terminate()`, so the run ends with no `ERRMAX` line at all; the mirror
prints the same line and stops, and prints no `ERRMAX` either. A run that
failed the program's own conservation check therefore yields no scalar on
either side, and the driver sees a missing marker rather than a number from a
run that failed. With `--error_tol=1` neither side reaches it.

There is **no cross-geometry pin**: every rank draws its own initial field
from its own stream, so the field, `source_total` and the error are functions
of the rank count, exactly as `stencil1d_hpx`'s checksum is. The oracle is the
six entries agreeing at each geometry, which is what consensus votes. They do,
and bit for bit: at the gate arguments every OCR entry computes
`3fb0eaa3dd5335fc` (`6.608032e-02`) at one rank and `3fb219105b309b99`
(`7.069494e-02`) at two, and the HPX program prints those same values. The
ladder's grid is one axis at every rung, so that picture holds at all four:
every rank has neighbours on one axis only, the halo-edge race under Wiring
cannot arise, and the HPX entry repeats bit for bit as the OCR ones do. A cubic
grid is where the cross-step pair under Wiring lived; it is closed on both
sides now, and a two-axis check (`2×2×1`, four ranks, five runs of each side)
votes one value on both. One
caveat on reading the lines: `xsocr` replaces `printf` with its own formatter,
which renders those identical bit patterns as `6.608082e-02` and
`7.069544e-02` — a fixed `+5e-07` offset, 7.6e-06 relative, which the row's
`1e-05` tolerance covers with less margin than the other rows' rendering
caveats. The bit patterns were compared directly to establish that this is
rendering and not arithmetic.

## Placement

One rank is one position of the process grid: `myrank_xy = rank % (npx·npy)`,
`px = myrank_xy % npx`, `py = myrank_xy / npx`, `pz = rank / (npx·npy)` — the
origin's own arithmetic. A
face with no neighbour is never sent, which is the origin's
`send_buffer_*.dest_` test, and a rank receives through exactly the faces it
sends through, which is where the origin marks a receive buffer valid.

The mirror states that placement on every object: every driver, init, spawner,
unpack, flux, chunk, pack, sum, arrive, reduced, check, advance, reaper, done,
final and close task carries an explicit `ocrAffinityGetAt(AFFINITY_PD, …)`
EDT hint for its rank — a reaper's hint names the *destination* rank of the
face it retires, not its own producer's — and both grids of a variable and
every partial and packed face carry an explicit DB hint for the rank that
writes them. Nothing is left to the build's no-hint policy.

What crosses a rank is therefore what crosses in the origin: one packed zone
per neighbour per variable per step (34.8 kB at the gate), and one partial per
rank per summed variable per step (8 B). The point layer carries the placement
in the other direction — where a labeled range is homed by index (ARTS,
ocr-vx) a point's home is its consumer — so a publish is a message only when it
has somewhere to go, and a consumer's destroy is local; on xsocr, which homes
the whole reserved range at the PD that reserved it, each publish is instead a
message to that PD and a forward from it.
Per crossing face the mirror spends about four messages (the producer's remote
labeled create, the satisfy, the consumer's read-only acquire of a block homed
at the producer, and its destroy) against the origin's one parcel carrying the
serialized buffer; the set of crossing edges is identical.

Per-rank process state stands where the origin keeps per-partition members
(O7): the grid names, `source_total`, the accumulated flux, the done latch,
and the running `ERRMAX` maximum, which is atomic because the summed
variables' checks run concurrently on rank 0.

## Sizing

The width knobs are `--nx --ny --nz`: under `--scaling=1` they are the global
dimensions and each rank gets its share, so the job is fixed across a node
sweep and only the per-rank block shrinks. `--num_vars` multiplies the whole
graph (its variables are independent chains); `--num_tsteps` lengthens it
without widening it — and lengthens the measured error, since the conservation
error grows every step. `--nz_block` is what cuts a step into chunks: at the
gate arguments `nz_block=10` over 64 planes gives seven chunks per step per
variable, so the chunk decomposition is exercised rather than collapsed, and
plan 3 can turn that knob without touching either program.

Memory is `2 · num_vars · (nx+2)(ny+2)(nz+2) · 8` bytes per rank — 18.4 MB at
one rank and 9.4 MB per rank at two, at the gate arguments — plus the faces in
flight. One ceiling binds a calibrated size: `(T+2) · V · max(6, nl, C) ·
KIND_COUNT · nl` (`KIND_COUNT = 6`) must stay below `2³²`, because the OCR
shim narrows a range count and every index derived from it to 32 bits; at the
gate that is 4 032 names at two ranks, so the ceiling is far away, but it is a
usage rejection rather than a degradation.

The `hpx-gate` roster runs `--scaling=1 --nx=64 --ny=64 --nz=64 --num_vars=4
--num_tsteps=10 --stencil=24 --percent_sum=100 --num_spikes=1 --error_tol=1`
with the process grid per node from the catalog's ladder (1·1·1, 2·1·1, 4·1·1,
8·1·1 — one axis at every rung, as under Wiring), which finishes in tens of
milliseconds per cell. The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--scaling=1 --nx=448 --ny=448 --nz=448
--num_vars=5 --num_tsteps=100 --stencil=24 --percent_sum=100 --num_spikes=1
--error_tol=1` with `--npx = nl --npy=1 --npz=1` per rung (one axis at every
rung; `nx` a multiple of 32 so the slab divides at every node count).
`num_vars` and `num_tsteps` keep the published values; the cube was derived to
the 100 s window through the cubic law and one refit; the sizing pass measured
70–90 s on the ARTS arms (EXCL the slowest, in the window) and 46 s on HPX,
12 GB resident.
