# sheneos_hpx

*A tabulated nuclear equation of state, cut into a cube of three-dimensional
partitions spread over the localities, and a set of workers that each
interpolate the same grid of query points out of it — one bulk request per
partition, one reply.*
Origin: HPX's `sheneos` example (`third_party/hpx/examples/sheneos/`,
BSL-1.0), copied to `benchmarks/hpx/sheneos_hpx/` and built as
`sheneos_hpx_hpx`.

## Overview

`hpx_main` runs on locality 0 alone. It builds one `sheneos::interpolator`,
which

1. reads the three axis ranges (`ye`, `logtemp`, `logrho`) from the HDF5
   table to learn each axis's first sample, last sample, step and length;
2. creates `num-partitions` `partition3d` components with
   `hpx::default_layout` over every locality;
3. cuts the cube into `p³` slabs, `p = floor(cbrt(localities))`, and
   `init_async`es each one with its own offsets and counts — a slab that is
   not the last along an axis takes two extra samples past its right edge, so
   a point in its last cell still has the neighbour the interpolation reads;
4. registers a symbolic name per partition.

Each partition then reads its own hyperslab of eight datasets (`logpress`,
`logenergy`, `entropy`, `munu`, `cs2`, `dedt`, `dpdrhoe`, `dpderho`) plus the
scalar `energy_shift`. That is the whole of the program's file input, and it
is where its opening phase goes.

Then, on every locality, `num-workers` `test_bulk_action`s are spawned. Each
builds the full `num-ye × num-temp × num-rho` grid of query points in a
shuffled order and issues one `interpolate_bulk_async`; the client groups the
points by the partition that answers for them, dispatches one bulk action per
partition, and reassembles the replies by original index. Every query returns
all eight quantities, trilinearly interpolated, with `pow(10, ·)` applied to
the two logarithmic ones and the table's shift subtracted from the energy.

One program on four runtimes: `sheneos_hpx_hpx` is the HPX program;
`sheneos_hpx_arts_<variant>`, `sheneos_hpx_xsocr` and `sheneos_hpx_ocrvx` are
the OCR mirror (`benchmarks/apps/hpx_origin/sheneos_hpx.c`), one row in
`hpx_apps.yaml`. HPX primitives used: `hpx::new_<partition3d[]>` with
`default_layout`, component actions (`init`, `interpolate_bulk`),
`register_as`/`connect_to` through AGAS, `hpx::async` of a plain action to a
named locality, `hpx::wait_all`, and `hpx::unwrap`. It is the section's only
row whose input is a **dataset** rather than its arguments, and the only one
that reads a file inside the measured window.

**One origin defect is fixed, and it is the reason the example could not run
at all.** `fill_partitions` called `partitions_[index].init_async(...)` while
`partitions_` was still the default-constructed empty vector — the assignment
from `hpx::new_<partition3d[]>`'s future sat *below* that loop — so every one
of those calls indexed a zero-length vector, guarded only by an `HPX_ASSERT`
a release build compiles out. **The defect is demonstrable from the source
alone**, and the repair does not rest on having watched it fail; it moves
`partitions_ = result.get();` above the loop, which is the precondition the
origin's own assert states, and changes nothing else. Reverting only that hunk
and rebuilding does crash the program at one locality (SIGSEGV), which is
recorded beside the row's logs, but the fix stands on the undefined behaviour
rather than on the crash. Upstream knows the example is broken: its
build file carries `# TODO: Fix example. Not added to unit tests until
fixed.`

**One origin defect is kept and disclosed, and it is worse than it looks.**
`~interpolator` unregisters the partitions' symbolic names in a loop that
advances the index twice — `std::to_string(i++)` inside a `for (…; ++i)` — so
it visits only every other index. It also asks for the wrong names:
`fill_partitions` takes `symbolic_name_base` **by value**, appends `'/'` to
that local copy and registers `base + "/" + i`, while the stored
`config_data::symbolic_name_` never sees the slash, so the destructor asks for
`…interpolator_test0`, `…2`, `…4`. Between the two, **no partition name is
unregistered at all.** It stays teardown-only and defined all the same: the
destructor runs at the closing brace of `hpx_main`'s block, after the end
stamp and after `CHECKSUM`, and AGAS answers a missing name with an invalid id
rather than an error (`symbol_namespace::unbind` returns `invalid_gid` and
`addressing_service::unregister_name` catches), so nothing escapes an
implicitly `noexcept` destructor. No undefined behaviour and no check that
cannot pass, so by this section's rule it is the program rather than a bug to
repair.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--file` | path to the HDF5 EOS table | a 2010 file name | reachable; required in the mirror |
| `--num-ye-points` | query points on the ye axis — **and on the other two** | 40 | reachable |
| `--num-temp-points` | declared for the temp axis; never read | 40 | parsed, then overwritten |
| `--num-rho-points` | declared for the rho axis; never read | 40 | parsed, then overwritten |
| `--num-partitions` | partition components to create | 32 | reachable; must equal the node count |
| `--num-workers` | workers **per locality** | 1 | reachable |
| `--seed` | seeds each locality's query shuffle | 0 (wall clock) | reachable |

**`hpx_main` reads `num-ye-points` three times.** `num_temp_points` and
`num_rho_points` are both `vm["num-ye-points"]`, so the grid is a cube of
that one number and the other two options are dead. Carried as written on
both sides — the mirror parses all three (a malformed value is still usage)
and then overwrites the latter two — so the two programs are the same
program at any arguments, not only at the catalog's.

**`--num-partitions` must equal the node count.** The program derives the
partitions-per-dimension **twice, by two different numerical methods, for
what is meant to be the same value**: `fill_partitions` cuts the cube with
`floor(cbrt(localities))` while `connect()` routes a query with
`floor(exp(log(num_instances)/3))` — mathematically identical expressions
that truncate to different integers at some inputs, because the two library
calls round differently (checked over 1..4096: they diverge first at 343,
then 512, 1000, 2744, 3375, 4096). Requiring `num-partitions == nodes` alone
is necessary but not sufficient — it only makes the two calls evaluate the
same underlying real number, not the same truncated integer — so the mirror
carries both derivations as the origin has them and additionally rejects,
as usage, a `(num-partitions, nodes)` pair for which
`floor(cbrt(nodes)) != floor(exp(log(num-partitions)/3))`. Anywhere the two
would disagree, the client routes a query to a partition index that was
never initialised — whose `min_value_`, `max_value_` and `delta_` are all
zero — and the interpolation throws `argument out of range`. The catalog's
`args_by_nodes` keeps `num-partitions` equal to the node count at every
rung, none of which reaches a disagreeing pair.

A rejected argument, an unusable axis, or a mismatched partition/node pair
prints usage and calls `ocrShutdown()` with status 0, where the origin's own
equivalent throws or exits non-zero; the missing `CHECKSUM`/`[APP_E2E]`
marker fails the cell either way.

The seed changes query order, table locality and floating-point summation
order, while preserving the point set. Both programs build identity index
arrays, seed the process-wide generator with `seed + locality_id`, and
shuffle each axis using the forward `random_shuffle` algorithm of the
vendored build's standard library. A zero seed selects wall-clock seconds.
Concurrent workers retain the source's shared `srand`/`rand` behavior; their
interleaving is not promised to produce an identical permutation across runs.

## Structure

Let `nl` = ranks, `w` = `--num-workers`, `W = w·nl` the worker total,
`p = floor(cbrt(nl))`, `L = p³` the live partitions, and `N` the query points
per worker (`n³` for `n = --num-ye-points`). Per run:

| object | count | size |
|---|---|---|
| partition blocks `Q(k)` | `L` | three axis slices + eight value arrays of the slab |
| init tasks | `L` — one per partition | 2 dependences |
| request blocks | one per (worker, non-empty partition) | `3·points` doubles |
| query tasks | same | 2 dependences |
| reply blocks | same | `8·points` doubles |
| plan blocks | `W` — one per worker | `2·(groups + N)` u64: group counts and query-order mapping |
| collect tasks | `W` | `1 + groups` dependences |
| worker tasks | `W` | table and completed axis ranges |
| axis-read tasks | `W`, chained per rank | table and previous read completion |
| reply-copy tasks/blocks | one per nonempty worker group | one reply / `8·points` doubles |
| share blocks | `W` | one double |
| start task | 1, on rank 0 | 2 |
| sum task | 1, on rank 0 | `W` |
| reply points | `W·L` reserved as `W·L·nl` names | — |
| init join | one latch of `L` | — |

At the gate's arguments `n = 8`, so `N = 512` points per worker and `W = 8`
at every rung. **`L = 1` at one, two and four nodes** and `8` at eight, which
is the placement section's subject.

Two counts are the origin's units carried across unchanged: one task per
(worker, partition) pair is the origin's one bulk action per partition, and
its reply is that action's return value. What the mirror does *not* carry is
the origin's AGAS step — `connect()` resolves `num_instances` symbolic names
per worker — because the mirror's table block **is** the name service; that
asymmetry is stated here rather than papered over. The per-worker re-read of
the three axis ranges, which `connect()` also performs, **is** carried: it is
file I/O inside the measured window on both sides. The origin's process-wide
mutex covers all three axis reads. The mirror chains only these read stages
per rank, releases the next reader when those reads end, and lets coordinate
creation and query work proceed independently. HDF5's thread-safe build also
has an internal API mutex; thread safety does not imply concurrent H5Dread.

Each reply continuation copies its group's values into a local output block,
corresponding to the origin's copies into query-indexed result slots. The
plan maps every original query ordinal to its group and offset. The final
fold follows those ordinals across the output blocks, preserving result
reassembly without concurrent writes to one large DB. The flattened wire
encoding retains one request and reply per group.

The mirror creates only the `L` live partitions. The origin creates
`num_instances` components and initialises `p³` of them, so at every rung the
catalog runs, the components beyond `L` hold no data and are never queried.
What the mirror therefore also omits is their **setup traffic**, and it is not
nothing: the origin creates every one of the `num_instances` components,
registers a symbolic name for each, and resolves all of them in every worker's
`connect()` — at two and four nodes that is one and three extra components
created, named and resolved per worker, inside the measured window.

## Wiring

**An OCR data-block dependence is satisfied when it is *added*, not when the
block is written**, so ordering rides on an event wherever a consumer's edge
can exist before its producer has written. Read-stage results and reply-copy
results use producer output events; inter-rank replies use labeled points.

* **The replies need the rail.** A worker cannot know the block GUID the
  query task will choose for the reply bound for it, so the producer writes
  the reply, releases it, and satisfies one labeled STICKY point; the
  worker's reply-copy task registers on that point in `DB_MODE_RO`; its output
  event delivers the completed local copy to the collect task. The
  points come from the program's single
  `ocrGuidRangeCreate(…, GUID_USER_EVENT_STICKY)` range, indexed through
  `mirror_edge` as `(worker_global·L + partition)·nl + worker_rank`, which
  gives each point exactly one producer and one consumer and never aliases
  two units of work onto one. Both sides open the point and
  `OCR_EGUIDEXISTS` is the expected second arrival.
* **The partitions do not.** `Q(k)` is created before any task exists,
  written by its init task, and read only by query tasks that are created
  *after* the init join has fired — so a plain dependence is already an edge.
* **The requests do not.** The worker creates each request block, fills it,
  releases it, and only then wires it into the query task it created itself.
* **The plan and the shares do not.** Each is written, released, and then
  handed to a task whose slot the writer fills.

**Destruction follows the consumer.** A request has exactly one consumer and
its query task destroys it; a reply and its point have exactly one consumer
and its reply-copy task destroys both. The collect task destroys the copied
outputs and plan; the sum task
destroys the `W` share blocks. The table and the partition blocks are left to
the runtime's teardown, which is what the origin does with the components
themselves — they outlive every query and die with the runtime.

**The end is collective, because the origin's is.** The sum task is created
in `mainEdt`, before any worker exists, with one slot per worker, each filled
by that worker's collect task. `ocrShutdown()` therefore sits structurally
behind every rank's last work rather than behind a timing margin.

**Where a point lives is the runtime's choice and only the hop count depends
on it.** A labeled range is homed by index on ARTS and ocr-vx
(`index % nranks`), so a reply point's home is its consuming worker's rank
and a within-rank publish costs no message; xsocr homes a whole reserved
range at the PD that reserved it, so there every point lives at rank 0 and no
publish is free. The answer is the same on all four runtimes; the traffic is
not, and that is disclosed rather than equalised.

## Flow

`mainEdt` parses the arguments, reads the three axis ranges itself (the reads
`fill_partitions` performs on the locality that builds the interpolator),
derives `p` and the slab geometry, reserves the point names, creates the
table block and the `L` partition blocks, creates the sum task and the init
join, and creates one init task per partition. The start task — behind the
join — creates `W` worker tasks, one per (rank, worker) with an explicit rank
hint, which is the origin's `hpx::async<test_bulk_action>(id, …)` from
locality 0.

A read stage re-reads the three axis ranges in one serialized section. Its
worker continuation builds sample arrays by repeated addition, shuffles
index arrays, and enumerates their Cartesian product. It groups those points
by partition, retaining each query ordinal, then issues a request and reply
continuation for each nonempty group. The collect task folds copied results
in original query order after every continuation completes.

| HPX wait site | classification | mirror |
|---|---|---|
| `result.get()` in `fill_partitions` (component creation) | driver wait — setup | the partition blocks exist before any task |
| `wait_all(lazy_sync)` (partition init + name registration) | driver wait | the init join, a latch of `L` |
| `connect_to` / `config_data` action in each worker | driver wait — setup | the table block, read-only |
| `unwrap(bulk_tests)` — the worker's one bulk future | end wait | the collect task's `groups` reply-copy outputs |
| `wait_all(lazy_results)` inside `bulk_context` | end wait — the worker's per-partition gather | the same dependences |
| `wait_all(bulk_tests)` in `hpx_main` | driver wait — the completion edge | the sum task's `W` share slots |

**Mid waits: 0** — verified while implementing. Every task here either sets
up, or issues all of its work and waits once at its end; nothing blocks in
the middle of its own computation on a value produced later.

`[APP_E2E]` covers the same span on both sides. On the HPX side it opens as
the **first statement of `hpx_main`'s block**, i.e. after the argument
reads, the seed fallback and the seed print but before the interpolator is
constructed, so the whole table read is inside the window; it closes
immediately after `wait_all(bulk_tests)`, before the added per-worker fold
and `CHECKSUM`. `print_e2e` returns early off locality 0, and `hpx_main`
runs on locality 0 alone, so exactly one line prints. The mirror's
`start_ns` is taken in `mainEdt` after option/seed/geometry handling and
before the three axis reads that stand for the interpolator's own reads,
and travels in `paramv[P_START_NS]`, never a global; `sum_edt` fires on the
`ntotal` worker shares and calls `mirror_app_e2e` as its first statement,
before the root fold, the print and the destroys. The runtime's own `[E2E]`
is still printed on both sides and kept as a separate observation. Two
asymmetries stay inside the window on one side each: the origin's per-worker
AGAS `connect()` work (see Structure) is HPX-side-only, and the mirror's
`reply_edt` landing copy (see Wiring) is OCR-side-only.

**The scalar is the sum of every value every worker received**, because the
origin computes nothing printable — it assigns `results` and casts it to
void. `CHECKSUM %.14g` on the HPX side, `%.14e` on the OCR entries (the xsocr
`printf` replacement has no `%g`). Each worker's result fold is inside both timing windows. HPX's final sum of
worker scalars is after its stamp; OCR's final sum is before shutdown.

**The answer is geometry-independent**, which makes this row unlike
`stencil1d`, `mini_ghost`, `nbody` and `fft`. The query set depends only on
the axis ranges and the point counts; the partitioning decides *who* computes
each value, not which values exist. So with the worker total held at 8 the
answer is the same job — and the same number — at every rung:

| ranks | live partitions | HPX (`%.14g`) | OCR entries (`%.14e`) |
|---|---|---|---|
| 1 | 1 | `1.8609856691588e+40` | `1.86098566915881e+40` (all ten) |
| 2 | 1 | `1.8609856691588e+40` | `1.86098566915881e+40` (all ten) |
| 4 | 1 | `1.8609856691588e+40` | `1.86098566915881e+40` (all ten) |
| 8 | 8 | `1.8609856691588e+40` | `1.86098566915882e+40` (nine), xsocr `…81e+40` |

These are historical gate measurements from before the query-order and
read-stage corrections. They establish the point-set checksum, not timings
or structural validation of the corrected graph.

**The pin, however, is applied at the one-node cell only.** The harness
compares against `expect` where the cell's arguments equal `expect_args`, and
this ladder changes `--num-partitions` and `--num-workers` at every rung — so
1n is judged against the pinned value while 2n, 4n and 8n are judged by
agreement among the entries at that rung. `transpose_hpx` carries the same
caveat for the same reason. A regression at 8n is caught by consensus, not by
the pin.

The `1e-09` tolerance allows floating-point summation-order differences
caused by concurrent shared-generator interleavings, partition arithmetic
and the output format. Both programs now fold in their own generated query
order. Exact shuffle-sequence comparison uses a fixed seed and one worker;
the sum alone cannot validate that workload-order property.

What the scalar can see: every interpolated value of every query, so a wrong
slab, a wrong ghost plane, a mis-routed point or a lost reply all move it. It
cannot see the *order* the queries were issued in, which is exactly the
degree of freedom the shuffle occupies.

## Placement

Partition `k` is homed at `mirror_owner(k, num_partitions, nl)`, which with
the required `num_partitions == nl` is rank `k` — where
`hpx::default_layout` puts the `k`-th component. Its init task carries the
same rank hint, so **each slab is read by the rank that owns it**, never
pulled through rank 0. A request block is homed at its worker's rank, where
the origin's coordinate vector sits before the action carries it; a reply
block is homed where it is written, which is the partition's rank, so the
payload travels on demand to the worker that consumes it. Every task and
every block states its placement explicitly; nothing is left to the build's
no-hint policy.

**The hot spot is the row's subject, and it is the program's own.** Because
the cube is cut by `floor(cbrt(localities))`, there is exactly **one live
partition at one, two and four nodes, and it sits on locality 0** — so every
query from every worker on every rank goes to rank 0, and the whole table
lives there. Eight nodes is the first rung where `p = 2` and all eight
partitions carry data, one per rank. That is a consequence of a cube-root
decomposition over a node count that is not a cube, it is what the published
program does, and it is reported rather than engineered away: the row exists
to show what such a decomposition costs a runtime.

What crosses a rank per run, at the catalog's arguments: `W·L` request/reply
pairs, of which the ones whose worker is not on the partition's rank are
messages — `8·(nl−1)/nl` of the eight pairs at the `L = 1` rungs, i.e. 4 of 8
at two nodes and 6 of 8 at four. A request carries `3·512·8 = 12.3 KB` and
its reply `8·512·8 = 32.8 KB`. The table itself never moves, and the `W`
shares are one double each. This row's traffic is therefore small and
one-sided by construction — its interest is the serialisation at one home,
not bandwidth.

## Sizing

The staged table (`datasets/sheneos/HShenEOS_rho220_temp180_ye65_…h5`,
checksum-gated, 391 MB) has axes `ye = 65`, `logtemp = 180`, `logrho = 220`,
so a full cube of one quantity is `65·180·220 = 2,574,000` doubles (20.6 MB)
and the eight together are **164.7 MB**. That is the resident set of the one
live partition at one, two and four nodes, held in a single data block on
rank 0. At eight nodes each rank holds one slab of about `34·92·112` cells
with its ghost planes — roughly 21 MB per rank, a little over the cube in
total because the ghosts are read twice.

The knobs are independent of the table: work is `W·n³` interpolations of
eight quantities each, so it grows as the **cube of `--num-ye-points`** times
the worker total, while the table read is fixed by the file. Memory also grows with these arguments. Requests, ordinal maps, replies and
copied results occupy up to `24 + 16 + 64 + 64 = 168` bytes per point before
allocator overhead, in addition to table and task metadata. Their actual
live population depends on scheduling and cannot be bounded by worker
thread count alone; calibration records peak RSS and a conservative model.

Two invariants a campaign must keep, both the origin's:
`--num-partitions == nodes` (above), and the **worker total held constant**
(`--num-workers` is per locality, so it must fall as the node count rises) if
the answer is to stay pinned. The catalog's ladder holds the total at 8:
`8, 4, 2, 1` workers at `1, 2, 4, 8` nodes.

The `hpx-gate` roster runs `n = 8` — 512 points per worker, 4096 in all —
which finishes in well under a second of query work; at one locality the
measured `[E2E]` is about 64 ms and the origin's own timer attributes 60 ms
of it to building the interpolator, i.e. **the gate cell is dominated by the
table read**. The knob that moves it back onto computation and onto the hot
spot is `--num-ye-points`, cubed; the measurement argument is the calibrated one at the end of this
section.

**Calibrated arguments.** `--num-ye-points=95 --num-temp-points=95
--num-rho-points=95 --seed=1` with `--num-partitions = nl` and
`--num-workers = 4096 / nl` per rung (the worker total `W = 4096` is the width,
a power of two above the floor of the widest geometry, held at every rung by
the invariant above). The points per axis, cubed, carry the duration, and the
row sits below its 20 s window because the HPX origin's resident set binds:
it grows faster than cubic with the points per axis — 127 GB at `n = 90`,
193 GB at `n = 100` at this width, over the 180 GB one-node ceiling taken with
its margin — so `n = 95` is the round size under it (about 158 GB predicted
through the measured exponent). The anchor measured 12.9–13.3 s on the ARTS
arms (0.65 of the window) at 24 GB resident and 105.9 s on HPX at 170 GB.