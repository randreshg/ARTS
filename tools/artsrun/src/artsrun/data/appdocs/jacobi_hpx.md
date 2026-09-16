# jacobi_hpx

*A five-point Jacobi sweep over a two-generation grid held one row per
component — each row spawns line-block updates, and one global barrier per
iteration joins the rows before the next turn.*
Origin: HPX's `jacobi` example (`third_party/hpx/examples/jacobi/`, pin
`v1.11.0`), copied to `benchmarks/hpx/jacobi_hpx/` and built as
`jacobi_hpx_hpx`.

## Overview

`hpx_main` and the one solver run on locality 0. The grid bulk-creates `ny`
row components with `default_layout(find_all_localities())`, then root
initializes each row with `nx` ones and joins those initializations. The
solver bulk-creates `ny` iterators with the same layout. Each iterator's
initialization creates and initializes a second row with zeros on its own
locality. After an iterator and its neighbors are initialized, root requests
its boundary setup. That setup stores four futures for its two neighbors'
two generations, without waiting for their values.

`solver::run(max_iterations)` issues one `step()` per interior row from
locality 0 and waits for every step, on every iteration. Each step reads its
neighbor handles, spawns `update(dst.get(x,x_end), src.get(x-1,x_end+1),
top.get(x,x_end), bottom.get(x,x_end))` for each line block, waits for the
updates, and swaps source and destination. An update computes
`(left + right + top + bottom) * 0.25` in that order. Its four operand waits
precede all numerical work.

One catalog row runs on four runtimes: HPX runs the source program;
`jacobi_hpx_arts_<variant>`, `jacobi_hpx_xsocr`, and `jacobi_hpx_ocrvx` run
`benchmarks/apps/hpx_origin/jacobi_hpx.c`. The mirror carries the same
root-driven iteration loop, owner-local rows and iterators, line ranges,
row joins, and global barriers. Setup uses owner-local factories and row
initializers; it does not issue all numerical DB creations from root or
replicate every grid handle on every rank.

The second generation is zero even on the two rows that are never stepped.
Interior iterators choose neighbor generations by their own source parity,
so every other turn reads zeros from those boundary rows. The mirror keeps
that behavior. It does not replace the boundary values with a conventional
fixed boundary condition.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--nx` | columns | 10 | reachable |
| `--ny` | rows | 10 | reachable |
| `--max_iterations` | global turns | 10 | reachable |
| `--line_block` | columns per update task | 10 | reachable |

The origin declares but never reads `--output`; it is not part of this row.
The mirror rejects malformed values, `nx < 3`, `ny < 3`, `line_block == 0`,
and sizes exceeding its checked limits (`nx*ny < 2^30`,
`2*ny*(ranges+2) < 2^24`). A line block wider than the interior is clipped
to one update range. Zero iterations remain valid: both generations are
initialized, and the checksum reads the first generation.

A rejected argument prints usage and calls `ocrShutdown()` with status 0,
where the origin's own option parsing exits non-zero; the missing
`CHECKSUM`/`[APP_E2E]` marker fails the cell either way.

## Structure

Let `nl` be ranks, `R = ceil((nx-2)/line_block)`, and `it` be iterations.
A row descriptor stores `R+2` numerical DB handles in column order: the
first column, each interior range, and the last column. An iterator state
stores its two own row-descriptor handles and their numerical range handles,
immutable after initialization. Each interior row also has two immutable
generation views, each containing its own source/destination ranges and the
current generation's top/bottom ranges. Only root holds the global list of
row, iterator and two ready-event handles: `4*ny` GUIDs.

| object | count | purpose |
|---|---|---|
| owner factory tasks | `2*nl` | empty row descriptors, then iterator states |
| root setup continuations | 3 | gather rows, start iterator factories, gather iterators |
| row initialization tasks | `2*ny` | allocate and fill numerical range DBs on their owner |
| iterator initialization tasks | `ny` | create the second row and join its initialization |
| iterator publication tasks | `ny` | install both own-row handles and range names |
| boundary request tasks | `ny-2` | issue four neighbor handle requests, then return |
| neighbor handle tasks | `4*(ny-2)` | the two generations of each neighbor |
| neighbor installation tasks | `2*(ny-2)` | publish one immutable view per source generation |
| global-turn tasks | `it` | root's iteration dispatch |
| row-turn tasks | `it*(ny-2)` | owner-local step |
| row-tail tasks | `it*(ny-2)` | completion after the row's update releases |
| line-block tasks | `it*(ny-2)*R` | the numerical updates, six dependences each |
| finish task | 1 | begin the checksum after the final turn |
| checksum dispatch and sum tasks | `ny` each | acquire each row's current ranges and sum columns |
| total task | 1 | sum row results in ascending row order |
| numerical grid DBs | `2*ny*(R+2)` | `2*nx*ny*8` payload bytes |
| row descriptors | `2*ny` | `R+2` GUIDs each |
| iterator states | `ny` | two descriptor GUIDs plus `2*(R+2)` own range GUIDs each |
| generation views | `2*(ny-2)` | `4*(R+2)` source/destination/top/bottom range GUIDs each |
| root handle table | 1 | `4*ny` GUIDs |
| ready events | `2*(ny-2)` | one persistent generation-view publication each |
| global barriers / row joins | `it` / `it*(ny-2)` | count rows / line-block completions |

Factory result tables are transient and contain only that factory's local
row/iterator handles. They are destroyed by their root gather. Grid and
iterator data persist through the run; no numerical grid DB is allocated
per iteration. Checksum scalars are released before delivery and destroyed
by the final total task. Checksum dispatch waits for both generation views,
then destroys their ready events and view DBs after the last global turn.

The range split is an OCR representation requirement: line-block updates
write disjoint slices of a row, while dependences acquire whole DBs. Splitting
at the origin's transfer unit preserves one writer per range and makes the
same graph legal under DB_WRF. The wider source window uses three adjacent
local range DBs; top and bottom each use one range with exactly the origin's
remote payload extent. Local HPX row ranges alias the original vector;
remote ranges serialize only their requested elements.

HPX invokes four non-direct `row::get` actions plus one update per range,
so the application-level iteration count is `1+5*R` invocations per interior
row against `2+R` mirror EDTs. Start waits becoming dependences explains why
those counts differ. They are not equal-cost units, and dividing an elapsed
ratio by the count ratio does not establish a per-dispatch runtime ratio.
A runtime-cost claim needs phase timing and thread/task-state evidence;
cached stack mappings are not a count of currently suspended tasks.

## Wiring

Both allocation and first touch of a numerical range happen in its row's
owner-local initializer. Factories create empty descriptors with
`DB_PROP_NO_ACQUIRE`; each initializer acquires its descriptor RW, creates
and fills its numerical DBs, releases those DBs, and records their handles.
A global latch over generation-0 initializer output events gates solver
construction, preserving the origin's grid-before-solver order.

Each iterator initializer is a finish EDT. It creates its second row locally,
spawns its zero initializer, and installs both row descriptors and their
range names in a continuation behind that initializer's output. The finish
output is a completion signal only. Root registers every boundary consumer
before enabling any iterator initializer; a boundary request has completion
dependences on its own and both adjacent initializers.

The boundary request spawns four neighbor-handle tasks and returns. Its
output counts toward the solver's setup join, which therefore waits for
request issuance, as the origin does. Each of two installers waits for only
its source generation's top and bottom descriptors, and publishes a local
immutable view together with own source/destination handles. Step `it`
waits on `ready[it & 1]`, so a delayed next-generation response cannot block
the current step. The two installers never write the same DB. Both views
are reused until the final checksum drain; there is no global neighbor-data
readiness barrier.

Per iteration, root creates the global barrier and its successor before
launching any row turn. Each row registers its tail behind the row join
before launching its updates. An update's output event decrements the row
join after its numerical DB releases; the tail's output decrements the
global barrier. A global barrier therefore precedes every access in the
next generation. The final checksum is behind the last barrier; at zero
iterations its interior-row consumers still wait for both ready views, so
shutdown cannot strand outstanding setup work for an unused generation.

| HPX wait site | classification | mirror |
|---|---|---|
| grid `wait_all(init_futures)` | setup join | generation-0 initializer output latch |
| iterator create/init `.get()` | setup continuation | local second-row init and publication under one finish |
| solver's own/adjacent init waits | setup prerequisites | three completion slots of each boundary request |
| boundary setup's stored futures | ready at the consuming step | current generation's two handle results, installer and ready event |
| update's four `.get()`s | start waits | six numerical DB slots |
| step `wait_all(fs)` | end wait | row join and completion tail |
| run `wait_all(run_futures)` | driver wait each iteration | global row-completion barrier |
| checksum's row futures | final gather | row sums and root total |

The numerical tasks have no mid waits. The extra setup continuations only
express the origin's creation/init waits and descriptor indirection; they
add no numerical kernel or new iteration phase.

## Flow

`mainEdt` creates templates and the owner row factories. Root gathers their
handles, issues row initializers, and joins them before creating iterator
factories. After gathering iterator handles it prepares all iterator
initializers and their boundary consumers, then enables initialization.
Each iterator initializes its second row on its owner. Boundary requests
issue the four neighbor queries; the setup join enables the first global
turn while each row's first step independently awaits its neighbor results.

Each global turn issues one owner-hinted step per interior row. A step
spawns exactly `R` numerical updates, joins them, and reports once to root.
The global join opens the next turn or final checksum. The final sum reads
generation 0 on the two unstepped rows and `it mod 2` on interior rows,
sums each row's columns in order, then sums row results in row order and
prints `CHECKSUM %.14e` before shutdown.

`[APP_E2E]` on the HPX side opens at the first statement of `hpx_main`'s
body — so the four `vm` reads, the grid construction, the solver
construction and `run` are all inside — and closes right after
`solver.run(max_iterations)` returns, before `solver.checksum()`;
`[APP_E2E] == [E2E]`, and `hpx_main` runs on locality 0 only, so exactly one
line. The mirror opens after option parsing and the sanity fold, before the
eighteen template creations and the factory fork (which therefore sit
inside the mirror's interval only, with no HPX-side counterpart), and closes
at the first statement of `finish_edt`, which runs once the last iteration's
barrier fires and before the `ny` sum tasks are created. One print, on rank
0. The checksum is now outside both windows — `finish_edt` stamps before
dispatching the sum tasks on either side — so the directional bias the
runtime `[E2E]` still carries (which includes setup and closes at shutdown
recognition) is not present in `[APP_E2E]`; the runtime's own `[E2E]` stays
a separate observation.

Inside the window on the HPX side only: `solver::run` restarts a
`high_resolution_timer` and prints a flushed MLUPS line before returning,
inside the measured span; the mirror has no counterpart to that print. It is
disclosed here as a measurement asymmetry, not credited to either side as
work.

The checksum depends on the computed field and generation parity. It is
not a conserved constant: second-generation boundary zeros affect later
updates. The scalar comparison uses the catalog's numerical tolerance;
long trajectories need not remain exact merely because the initial values
and multiplier are dyadic. The reference runtimes' number formatting may
also differ in the last printed digit.

## Placement

Row `y` and its iterator use HPX's ceil-block default layout, represented by
`mirror_owner(y, ny, nl)`. Every factory, initializer, iterator continuation,
step, tail, update, and row-sum task is explicitly hinted to the owner it
represents. A neighbor handle task runs on the neighbor's owner. Root setup
continuations, global turns, finish, and total run on rank 0. Every DB has
an explicit home hint, and ready events are created by the owner factory.

Numerical traffic crosses only where a top/bottom row is remote. For a
populated ceil-block layout with interior rows across every rank boundary,
the requested halo volume is `16*(nl-1)*(nx-2)` bytes per iteration,
independent of `line_block`. This is a logical requested-volume count:
a coherence protocol may retain or suppress transfers. Root-to-row step
issuance and row completion cross too, exactly where the origin sends its
step actions and their futures back.

Setup uses the two origin bulk distributions, per-row initialization, local
second-row creation, and neighbor-only descriptor delivery. It does not
broadcast unrelated grid range names. OCR descriptor traffic carries range
GUID arrays where an HPX component handle identifies a sliceable row; the
array's extent is a consequence of the range representation. Numerical
DB allocation remains local even as the number of ranges increases.

## Sizing

The grid sets work: `(nx-2)*(ny-2)` updates per iteration. `line_block`
sets the `R`-way row split, and `max_iterations` lengthens the chain without
widening it. The available update width is `(ny-2)*R`; choose a populated
layout so every intended rank owns useful rows.

Grid payload memory is `16*nx*ny` bytes. Numerical DB count and descriptor
metadata grow with `ny*R`; the root's handle table grows only with `ny`.
There is no `nl`-fold full-grid name table. Smaller `line_block` increases
parallelism, metadata, and runtime task demand while leaving the logical
halo byte volume unchanged.

The gate arguments `--nx=256 --ny=256 --max_iterations=9 --line_block=32`
produce 8 line blocks per row, 254 interior rows, 9 global barriers,
18,288 update tasks, and 5,120 numerical grid DBs. The odd iteration count
is part of the workload; `expect` and `expect_args` must move together.
Windows measured on earlier mirrors do not describe this setup graph; the
trend, parity and sizing below were measured on this version.

**Calibrated arguments.** `--nx=32762 --ny=32765 --max_iterations=10
--line_block=730` at every node count. `max_iterations` keeps the published
value; `line_block` is derived per size as the smallest multiple of ten that
keeps the HPX origin's started-and-waiting frontier — one coroutine stack per
line-block task, 64 KiB each from the heap — under the one-node stack budget of
100 GB. The grid was derived to the 20 s window through each arm's measured
two-point exponent (coarser grain is cheaper per point than the cell-update
law assumes), which asks for about 45 000², and is held below it by this
mirror's own cell bound above (`nx·ny < 2³⁰`): the largest square under it.
The sizing pass measured 12.2–12.9 s on the ARTS arms (0.64 of the window) and
59 GB resident; HPX printed nothing in 600 s at this size and took 514 s at
29 172² (a started-and-waiting thread per line-block task), which is recorded
as measured, not sized for.
