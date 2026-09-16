# transpose_hpx

*A blocked PRK matrix transpose, `B = A^T`: each locality owns column
blocks, fetches one square from every source block for each of its outputs,
and joins its own iteration. Root validates its own column blocks in
parallel after every turn.*
Origin: HPX's `transpose_block` example
(`third_party/hpx/examples/transpose/transpose_block.cpp`, pin `v1.11.0`),
copied to `benchmarks/hpx/transpose_hpx/` and built as `transpose_hpx_hpx`.

## Overview

`hpx_main` runs on every locality. Each creates its local A and B column
blocks, fills them using `for_each(par, ...)`, registers their basenames,
and discovers every block's handle. A is initialized to
`1000*column + 0.001*row`; B starts at `-1`.

Every iteration has another `for_each(par, ...)` over local column blocks.
Each block's lambda serially issues one transpose dataflow per source block,
with an A square and local B square as inputs, and records its phase join.
The locality waits for all its block joins. Root then calls
`transform_reduce(par, ...)` over its own column blocks, accumulating the
squared error into the run's total before starting the next iteration.
Other localities continue their own chains without a global iteration
barrier.

The OCR mirror (`benchmarks/apps/hpx_origin/transpose_hpx.c`) retains those
parallel column-block bodies during fill, transpose-task creation, and
validation. Each numerical transpose remains one square; each validation
remains one whole column block. The mirror runs as
`transpose_hpx_arts_<variant>`, `transpose_hpx_xsocr`, and
`transpose_hpx_ocrvx`, alongside HPX in one catalog row.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--matrix_size` | square matrix order | 1024 | reachable |
| `--iterations` | transpose/validation turns | 10 | reachable |
| `--num_blocks` | column blocks per locality | 1 | reachable |
| `--tile_size` | tile edge within one square transpose | matrix order | reachable |

The origin's `--verbose` only adds diagnostic output; the mirror always
prints its scalar. The mirror requires positive sizes, block count and
iteration count, and a matrix order divisible by `nl*num_blocks`. It checks
the block-count product before multiplication. A tile at least as large as
a square selects the origin's untiled loop; a smaller positive tile clips
its last row/column tile in the same way as the origin.

The mirror's usage domain is a superset of the origin's *usable* domain, not
an identical one: the origin accepts a non-dividing `order`/`num_blocks`
pair by silently truncating `block_order = order/num_blocks` (running a
smaller transpose than asked for) and accepts `--tile_size=0` by looping
forever (`i += tile_size` never advances). The mirror rejects all three as
usage instead, so nothing is lost from the comparison — a malformed or
out-of-domain argument prints usage and calls `ocrShutdown()` with status 0,
where the origin's own equivalent either runs silently wrong or hangs; the
missing `Solution validates`/`[APP_E2E]` marker fails the cell either way.

`num_blocks` is per locality. Strong-scaling arguments must hold
`nl*num_blocks` constant; holding the CLI value constant changes the block
count and each task's grain.

## Structure

Let `nl` be ranks, `nlb = num_blocks`, `nb = nl*nlb`,
`bo = matrix_size/nb`, and `it = iterations`.

| object | count | size or purpose |
|---|---|---|
| rank drivers | `nl` | one owner-local setup |
| fill tasks | `nb` | one column block per task |
| publishers | `nl` | publish local A names and retain local B names |
| iteration spawners | `nl*it` | one turn of one rank |
| block spawners | `nb*it` | each serially issues `nb` transpose tasks |
| block completion tails | `nb*it` | join the block's phase outputs |
| transpose tasks | `nb²*it` | one `bo²` square, two DB dependences |
| validation tasks | `nlb*it` | root only, one column block, `nb` B square inputs |
| validation combines | `it` | root only, `nlb` scalar results and the accumulator |
| final reports | `nl-1` | non-root final completion |
| A / B squares | `nb²` each | `bo²` doubles per DB |
| fill name slices | `nb` | `2*nb` GUIDs each, transient |
| A name tables | `nl` | `nlb*nb` GUIDs each |
| owner name maps | `nl` | `2*nlb*nb` GUIDs each |
| validation scalars | `nlb*it` over the run | one double, destroyed by its combine |
| accumulated error | 1 | one double on root |
| block joins / rank joins | `nb*it` / `nl*it` | count phase outputs / block completions |
| exchange / completion points | `nl²` / `nl-1` | one-shot name delivery / final completion |

The block-spawner layer is the origin's parallel block loop, not added
numerical parallelism. Inside a block, phase creation remains sequential.
The rank join waits for all block completions; each block completion follows
its transpose releases. HPX's parallel algorithms may schedule several
column-block lambda invocations in one runtime chunk, so one logical block
body is not a claim of one physical HPX thread.

Validation also preserves the origin's logical grain. A checker walks all
`nb` B squares of one root-owned column block in row/column order, producing
one scalar error. A local combine adds those scalars and updates the run
accumulator. The vendored HPX `transform_reduce` does chunk-local reductions
and a sequential combine; there is no cross-rank or invented reduction tree
here. In a correct run every squared error is exactly zero, so reduction
association does not change the scalar.

A/B numerical DBs persist throughout all iterations. The origin uses
column-block components and sub-block views; OCR splits them into square
DBs because that is the transferred and independently written unit. The
matrix payload remains `16*matrix_size²` bytes globally.

## Wiring

A fill task creates, initializes and releases its own A/B squares, then
writes their GUIDs into its name slice. Its output joins the local setup
latch, and the publisher reads the slices after that latch. The publisher
writes and releases its A name table, satisfies one labeled sticky point
per consumer rank, and retains the B names in the owner-local map. The first
iteration waits for every incoming table and installs the relevant A names
in its map. Each `(producer, consumer, kind)` point has one lifetime; none
is reused across iterations.

A is never modified after fill. Each transpose reads its source A square RO
and writes its owner's B square RW. Before enabling block spawners, the
rank spawner releases its initialized name map. Every block spawner creates
its phase latch and registers the block tail before enabling a transpose;
each transpose output is counted into the latch before its input slots are
filled. The block tail's output counts into the rank join.

Root creates every checker and the combine before enabling any block
spawner. All checkers depend on the **local rank join**, so no checker reads
B before the transposes of that iteration release it. The combine waits for
all checker results; the next root iteration waits for the combine's output.
Thus the check reads and next turn's writes never overlap, even though an
idempotent transpose could conceal that ordering error in the scalar.

| rank | per-iteration chain |
|---|---|
| root | block joins → local rank join → parallel column checks → combine → next iteration |
| non-root, before last turn | block joins → local rank join → next iteration |
| non-root, last turn | block joins → local rank join → report → completion point |

Root's final combine also waits for all non-root completion points before
printing and shutting down. That final join does not become a barrier on
earlier iterations. Ready outputs are registered before tasks can fire;
result DBs are released before being returned through output events.

## Flow

`mainEdt` validates arguments, creates the twelve templates, reserves the
`2*nl²` point range, and forks the rank drivers. Each driver creates its
map, A name table, and fill slices, then starts its parallel fills. Its
publisher installs B names locally and exchanges A names with every rank.

Each iteration spawner prepares its successors, then launches one local
block spawner per owned column block. Those bodies independently create
their phase dataflows. Root's local completion enables one checker per
owned column block, followed by a scalar combine. Other ranks independently
begin their next turn after local completion. The final root combine prints
`Solution validates` and `ERRSQ %.6e` only when accumulated error is below
`1e-8`; otherwise it prints the failure diagnostic without the success
marker or result scalar. The campaign therefore fails a non-validating
run even though the mirror's shutdown path does not encode that as a
nonzero exit status.

| HPX wait site | classification | mirror |
|---|---|---|
| parallel fill's `get_ptr().get()` | setup/start wait | owner-local fill inputs and setup join |
| basename discovery waits | driver setup wait | incoming table points |
| transpose's two ready `.get()`s | start waits | A RO and B RW dependences |
| per-block `when_all(phase_futures)` | block join | phase latch and block completion tail |
| local `wait_all(block_futures)` | driver iteration wait | rank join over block completions |
| parallel validation's `get_sub_block().get()` | checker start wait | that column's B square dependences |
| validation reduction | local end wait | scalar combine |
| collective finalize | program end | non-root completion points at final combine |

Numerical tasks have no mid waits. `[APP_E2E]` on the HPX side opens after
option parsing and the derived geometry, before the block vectors,
creation, fill and basename rendezvous, and closes after the root's
validation and its rate/avg/min/max report, before `ERRSQ` and before the
unregister/finalize work. The mirror opens at the head of rank 0's driver —
before the name blocks, the square creation and fill, and the table
exchange — carried in `paramv[P_START_NS]`, and closes on the last
iteration, right after `Solution validates`/the rate report and before
`ERRSQ`; exactly one line prints, and nothing prints on the error branch
(matching HPX's `terminate` before its own stamp). This is the
best-aligned of the section's three timing rows: the only asymmetry is that
the OCR side's `ocrGuidRangeCreate` and its twelve template creations precede
the driver and so sit outside its interval, where HPX excludes only option
parsing and geometry — sub-microsecond, but disclosed rather than moved.
The runtime's own `[E2E]` is still printed on both sides and kept as a
separate observation; on the OCR side it still ends at shutdown recognition
after all final reports, so a straggler's cost is excluded from `[APP_E2E]`
but not from it.

Inside the window on the HPX side only: five informational `std::cout`
lines the origin prints between its two stamps (block/tile geometry and
basename-exchange progress); the mirror's root prints none of them. This is
source output inside the measured span on one side only, immaterial in
magnitude but disclosed here rather than silently absorbed.

The oracle recomputes the same `1000*column + 0.001*row` expression used by
fill, and transposition copies those double values bit for bit. Correct
output is `ERRSQ 0.000000e+00`. Only root-owned B column blocks are checked,
as in the origin: remote A payloads used by root are covered, but non-root
B writes are not themselves validated. The final completion join proves
those ranks finished their stated work, not that every unchecked value is
correct.

## Placement

Column block `b` belongs to rank `b/nlb`. Every driver, fill, publisher,
iteration spawner, block spawner, block tail, transpose and report is hinted
to that block/rank's owner. Checkers, combines and the error accumulator are
on rank 0. Numerical squares, name slices/tables/maps and scalar results
all carry explicit DB home hints.

A transpose requests a remote A square when source and destination block
owners differ. The logical requested volume is
`8*matrix_size²*(nl-1)/nl` bytes per iteration, independent of `num_blocks`;
actual transfers may differ because the runtime can retain immutable A
copies. B always remains on the locality that writes it. Setup exchanges
A handle tables once; completion points carry no payload. Where labeled
range indices determine event home, a point is indexed to its consumer's
rank; a runtime that homes the entire range at the reserving PD may forward
those signals through root.

## Sizing

`matrix_size` changes the work and payload quadratically while a fixed
`nb` holds message/task count and decomposition fixed. `iterations` changes
chain length; `tile_size` changes the serial loop inside each transpose.
The validation width is only `nlb` column blocks on root, independently of
the `nb²` global transpose width. Both layers must be included when
interpreting the scaling curve.

Global numerical memory is `16*matrix_size²` bytes. The gate's
`matrix_size=1024`, `iterations=2`, and `nl*num_blocks=8` use 16 MiB of
matrix payload and 128 transpose tasks. Matrices in a node sweep must
remain divisible by the fixed total block count. Timings from earlier mirrors do not characterize this graph; the trend,
parity and sizing below were measured on this version.

**Calibrated arguments.** `--matrix_size=71680 --iterations=10` with
`--num_blocks = 64 / nl` per rung (the block total `B = 64` is the width,
`B²` tasks per iteration). `iterations` keeps the published value; the order
was derived to the 100 s window through the quadratic law and then held below
it by the registered pool's resident set. The pool maps a ladder of slabs per
NUMA node, so the resident set is a staircase over the live matrices, and
71 680 (= 70 × 1024 = 64 × 1120) is the largest measured order on the lower
step — 126–138 GB across the three arms in the sizing probes, against
187–201 GB at 81 920, over the 180 GB ceiling — where the sizing model's
77 248 would land on the upper step. The anchor measured 24.0–27.3 s on the
ARTS arms (EXCL 26.4 s, about a quarter of the window) at 145 GB resident and
59.8 s on HPX at 82 GB.