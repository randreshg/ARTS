# The retired rows' sections of `benchmarks/hpx/README.md`

Cut verbatim on 2026-09-16 when the seven rows were retired; each section is the
row's disclosure as it stood (origin, edits, wait sites, placement, oracle, the
measured trend and anchor).  Paths inside refer to the tree at that date: the
programs now live under `hpx/<row>/` and `mirrors/<row>.c` next to this file,
and the two origin submodules (`third_party/miniapps`, `third_party/NBody`)
are gone -- their pins are in each `ORIGIN.md`.

## pi_hpx

Origin: HPX's `distributed_pi` collectives example
(`third_party/hpx/libs/full/collectives/examples/distributed_pi.cpp`, pin
`v1.11.0`). Edits: `hpx::init`/`hpx_main` in place of `hpx_main.hpp`'s
implicit everywhere-main (the runtime defaults run `hpx_main` on every
locality the same way), the `[HPX]`/`[E2E]` markers around the
broadcast/reduce with the `[PARCELS]` line after them, and the `pi:` line
formatted with 15 significant digits
and written by one `arts_hpx::write_stdout_line` call, so no other
stream's write can land inside it.

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::collectives::broadcast(...)`, called on every locality | start wait — every locality blocks until it has `N` | no wait needed: `N` and every rank's index travel directly in each task's paramv at creation |
| `hpx::collectives::reduce(...)`, called on every locality | end wait — join of every partial sum | a fixed `nl`-slot dependence join hinted at rank 0 |

Mirror mapping: one EDT per rank computing its block into an 8-byte
`double` DB, wired directly into a fixed-arity reduce EDT
(`benchmarks/apps/hpx_origin/pi_hpx.c`). The `[APP_E2E]` span opens right
after the broadcast that seeds every locality with `N` and closes right
after the reduce that produces the final value; the runtime's own `[E2E]`
is still printed on both sides as a separate observation, and its
single-argument `print_e2e` call makes `[APP_E2E] == [E2E]` on the HPX
side.

The pinned scalar, `3.14159266359028` at `N = 1e8`, is the algorithm's own
left-endpoint Riemann-sum value (`O(1/N)` truncation error, not
`O(1/N^2)`) — not fifteen digits of pi — and is not to be "corrected"
toward pi.

Gate arguments: `100000000`.
Calibrated arguments: `70000000000` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 60 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/pi_hpx_hpx --hpx:threads=2 100000000
```

Per-iteration cost differs by about 2.2× between the two sides of this row
at one locality (measured with the same flags: 3.1 ns against 1.4 ns per
term, no constant term — the E2E is linear in N on both), and the reason is
code generation, not a runtime: in the origin's `hpx_main` GCC keeps the
loop's accumulator in a general-purpose register and moves it through
`vmovq` on every iteration of the loop-carried add, while the mirror's
accumulator stays in an `xmm` register. The arithmetic is identical (both
sides print the same 15 digits) and the loop is the origin's, so it stays;
the number is the toolchain's answer to that function, reported as such.

## random_mem_access_hpx

Origin: HPX's `random_mem_access` example
(`third_party/hpx/examples/random_mem_access/`, pin `v1.11.0`). Edits: the
per-action stdout line removed from every action (`init`/`add`/`query`/
`print`); `--seed` exposed (the origin seeds from `random_device`); the
init loop now waits for its posts before the update phase starts (the
origin's fire-and-forget init races its adds on the same component; the
oracle needs init first); the print phase becomes a query gather on
locality 0 printing `COUNT_SUM <sum of final counts minus sum of initial
counts>`; the `[HPX]`/`[E2E]` markers around the whole program body, with
the `[PARCELS]` line after the end stamp.

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::wait_all(inits)` | end wait — join of every init | an `n`-count `inited` latch |
| `hpx::wait_all(barrier)`, the `add_async` posts | end wait — join of every update | a `k`-count `done` latch |
| `hpx::wait_all(counts)`, the `query_async` gather | end wait — join of every element's final count | the flat `n`-slot summer fed by the `n` query tasks |

Mirror mapping: one 32-byte DB per element homed at its `hpx::default_layout`
owner, one task per init/update/query hinted to that owner, and a single
flat summer over every element's final count producing the `COUNT_SUM`
oracle (`benchmarks/apps/hpx_origin/random_mem_access_hpx.c`).
The origin's component is a `hpx::components::locking_hook<…>`, so every
`add()` on one element runs under that element's own mutex; the mirror
carries that exclusion inside the element's own DB rather than as a
task-level edge — an `atomic_uint` spinlock field that every init, update
and query task on that element takes and drops around its own read or
increment, so independent update tasks, created and dispatched in draw
order, still land one at a time. The mirror's bounded draw reproduces
libstdc++'s `uniform_int_distribution` algorithm for a full-range 32-bit
engine exactly, so at the same seed the two sides draw the identical index
sequence; the oracle (`COUNT_SUM == iterations`) does not depend on this —
it counts delivered updates, not which element received them.

Gate arguments: `--array-size=4096 --iterations=200000 --seed=1`.
Calibrated arguments: `--array-size=4096 --iterations=2000000 --seed=1` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 120 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/random_mem_access_hpx_hpx --hpx:threads=4 \
  --array-size=4096 --iterations=200000 --seed=1
```

## transpose_hpx

Origin: HPX's `transpose_block` example
(`third_party/hpx/examples/transpose/`, pin `v1.11.0`). Edits: *build* — the
program registers its own component module, whose factory and registry
plugin lists its component registration refers to (the app project gives
each executable its own `HPX_COMPONENT_NAME`, so the runtime's default
module is not the one it names); *cfg* — the run-everywhere cfg vector
becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line;
*markers* — `[HPX]`/`[E2E]` around `hpx_main`'s body, with `[PARCELS]` after
the end stamp; and *scalar* — the squared error the origin already
accumulates is printed as `ERRSQ %.6e` by one write, unconditionally, where
the origin shows it only under `--verbose`. The origin's own
`high_resolution_timer` is per iteration and stays where it is: it never
covered the block creation, the fill or the basename rendezvous, which the
marker window holds.

| HPX wait site | classification | mirror |
|---|---|---|
| `transpose()`'s two `.get()`s under `dataflow` | start wait — every input, already made ready | the transpose task's two block dependences |
| `A[p].get_sub_block(...)` for a remote `p` | start wait — the payload fetch | the same dependence, acquired from the square's home |
| the fill `for_each(par, …)` | driver wait — the parallel fill | the per-rank setup latch over one fill task per column block |
| `wait_all(A_ids)` / `wait_all(B_ids)` | driver wait — the basename rendezvous | the spawner's `nl` table-point dependences |
| `wait_all(block_futures)` | driver wait — per locality, once per iteration | the per-rank latch of `nlb·nb` output events |
| `test_results`'s `transform_reduce` | end wait — the root's result in hand | the check task's read-only squares |
| `hpx::finalize()` | end — collective, every locality present | the last check's `nl − 1` completion points |

Mirror mapping (`benchmarks/apps/hpx_origin/transpose_hpx.c`): one block per
`block_order²` square rather than one per column block, since a square is
what a task reads and writes and an OCR dependence is taken on a whole
block; one fill task per own column block, as the origin's `for_each(par, …)`
has it — that phase is inside the measured window on both sides, so a serial
fill would be a different decomposition of a measured phase; one transpose
task per (own block, phase) per iteration, hinted at the block's rank; and
`2 · matrix_size² · 8` bytes in total, the origin's own.

Three of the four block handovers need no point, and each earns it the same
way — the block is complete *and released* when its consumer's slot is
filled: an `A` square is written and released by its fill task before the
publisher that names it runs; a `B` square's next writer is created only
after both its previous writer and its reader have finished, through their
output events; and a rank's name map is released by each writer before it is
added as the next reader's dependence, which is what makes a plain
dependence an edge rather than a race. The rail therefore carries the name
exchange: each publisher publishes its own squares' GUIDs, on one labeled
STICKY point per consumer rank, in place of `find_all_from_basename`. A
within-rank publish is no message; a crossing one is about four (the remote
labeled create, the satisfy, and the consumer's RO acquire of a table homed
at the producer — the destroy is local here, because the point's home is the
consumer, one message cheaper than stencil1d's shape). Those counts hold
where a labeled range is homed by index, which is what ARTS and ocr-vx do
(`index % nranks`); xsocr homes a whole reserved range at the PD that
reserved it, so there every point lives at rank 0, no publish is free, and
each satisfy costs a message there and a forward.

Two things must not be forks. The iteration chain: the origin's root
accumulates `errsq` between `wait_all` and the next iteration on the same
thread, so the mirror chains latch(`i`) → check(`i`) → spawner(`i+1`) on
rank 0 and latch(`i`) → spawner(`i+1`) elsewhere; running the check and the
next iteration off the same latch would put an `RO` read and an `RW` write on
one `B` square at once, and no value oracle could see it — the transpose is
idempotent and the printed error is exactly zero either way. And the end:
the origin's localities all reach the collective `hpx::finalize()`, so the
mirror's last check waits on one zero-byte completion point per other rank,
each satisfied behind that rank's last iteration join, before it shuts down.
Rank 0 has a margin without it — one extra validation pass per iteration
that no other rank runs — but a margin is not a barrier, and a straggler's
remaining transposes would otherwise never run, invisibly. The joins between
iterations stay per rank, because the origin's are.

The error is exactly zero at every geometry and on every runtime: the fill
writes `1000·x + 0.001·y` for integers `x, y`, the transpose copies those
doubles, and the check recomputes the same expression with the same
integers. `xsocr`'s own `printf` replacement, which costs `stencil1d_hpx` a
last digit, renders zero exactly, so all six entries print
`ERRSQ 0.000000e+00`. The origin validates the root's own column blocks only
(`test_results(…, blocks_start, blocks_end)`), and that limitation of its
checking is disclosed in the appdoc rather than widened here.

Gate arguments: `--matrix_size=1024 --iterations=2` with `--num_blocks` per
node from the catalog's ladder (8 at one node, 4 at two, 2 at four, 1 at
eight) — the ladder holds the *total* block count at 8, which is what keeps
the square size and the phase count the same job at every geometry.
Calibrated arguments: `--matrix_size=71680 --iterations=10 --num_blocks=64` (per rung: 1 node: --num_blocks=64; 2 nodes: --num_blocks=32; 4 nodes: --num_blocks=16; 8 nodes: --num_blocks=8; 16 nodes: --num_blocks=4; 32 nodes: --num_blocks=2) — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/transpose_hpx_hpx --hpx:threads=4 \
  --matrix_size=1024 --iterations=2 --num_blocks=4
```

## jacobi_hpx

Origin: HPX's `jacobi` example (`third_party/hpx/examples/jacobi/`, pin
`v1.11.0`) — the first row whose origin is a whole component tree (sixteen
sources, all listed in `ORIGIN.md`) and the first whose `hpx_main` runs on
locality 0 alone. Edits: *cfg* — the runtime defaults are installed with
`hpx_main` left on locality 0, which is the shape the program is written
for; *markers* — `[HPX]` from every locality through a startup function
(`hpx_main` reaches only one), `[E2E]` from the first statement of
`hpx_main`'s body to where `run` returns, with `[PARCELS]` after the end
stamp; and *scalar* — the program computes no printable quantity of its own
(it prints a rate), so an iterator gains a `sum` action over its current row
and the solver a `checksum` that totals them, printed as `CHECKSUM %.14g` by
one write. The gather runs after the origin's timer and after the end stamp,
so it is outside the measured window on both sides. The origin's own
`high_resolution_timer` is inside `solver::run` and stays there; it never
covered the grid or solver construction, which the marker window holds. No
*build* edit: `jacobi_component.cpp` already registers a component module,
so the component name the app project gives each executable needs nothing
added. The component's own build file declares a dependency on the runtime's
iostreams component, which `add_hpx_origin_app`'s `LINK` list names rather
than the sources being edited.

| HPX wait site | classification | mirror |
|---|---|---|
| `update`'s four `.get()`s | start wait — every input | the line-block task's six block dependences |
| `top`/`bottom` `get(x,x_end)` for a remote neighbour row | start wait — the payload pull | the same two dependences, from the block's home |
| `wait_all(fs)` inside `step()` | end wait — the row's line blocks | the row join and the tail behind it |
| `wait_all(run_futures)` inside `run()` | driver wait — the global barrier | the barrier latch of `ny−2` tails |
| the grid's and solver's setup `wait_all`s | driver wait — setup | the setup latch of `2·ny` init tasks |
| `solver.run(...).get()` | end — every locality's last turn behind it | the finish task behind the last barrier |

Mirror mapping (`benchmarks/apps/hpx_origin/jacobi_hpx.c`): one block per
line block of one row of one generation, `2·ny·(R+2)` in all with
`R = ceil((nx-2)/line_block)`, because `R` tasks write one row's destination
generation between them and a block with two writers is what the
decomposition has to avoid; the two end columns are blocks of their own
because nothing ever writes them. The origin's source pull spans one column
further on each side, which the mirror takes as three read-only
dependences — all of them the task's own row, so it costs no wire traffic.
One task per row per iteration hinted where the row lives (the origin's
`step()` action), one per line block under it (its `update`), a per-row join
whose tail is the single event the global barrier counts, and one global
barrier per iteration on rank 0 (its `wait_all`).

**No rendezvous point anywhere, and the global barrier is why.** A turn's
source generation was last written by the previous turn, its destination was
last read by the previous turn, and the barrier — one tail per row, each
behind its row's line blocks' output events — stands between them, so every
dependence is added to a block that is already complete and released. Setup
earns the same exception with a latch: the driver creates every block with
`DB_PROP_NO_ACQUIRE` and a home hint and one init task per (generation, row)
fills it where it lives, exactly as the origin's `new_` is issued from
locality 0 and its `init` action runs at the row's locality. The name
tables (one replicated copy per rank, standing in for the iterator state
`setup_boundary` installs) are released before the first turn is handed one,
and a row sum is released before it is added to the total task.

**The kernel is mirrored as written.** The destination generation is created
with value `0.0` on *every* row, including the two that are never stepped,
and an iterator indexes its neighbours by its own source parity — so on
every other turn the first and last interior rows average against a row of
zeros and the field oscillates instead of settling at `1.0`. That makes the
checksum a function of the whole trajectory and of the iteration count,
which is what makes it an oracle; it also makes the odd iteration count part
of the workload. Every value is a dyadic rational a double holds outright,
so the arithmetic is exact and the pin is geometry-independent: every entry
prints the same number at one, two and four ranks, except `xsocr`'s last
digit (its own formatter, about a part in `1e15`, covered by the row's
tolerance).

Gate arguments: `--nx=256 --ny=256 --max_iterations=9 --line_block=32`,
pinned at `CHECKSUM 64487.150146484`.
Calibrated arguments: `--nx=32000 --ny=32000 --max_iterations=10 --line_block=700` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/jacobi_hpx_hpx --hpx:threads=4 \
  --nx=256 --ny=256 --max_iterations=9 --line_block=32
```

## mini_ghost_hpx

Origin: the HPX port of Mantevo's MiniGhost
(`third_party/miniapps/MiniGhost/src/`, STE||AR-GROUP/miniapps, pin
`0cf9c1c1`) — the first row here whose origin is not the vendored HPX tree
(the checker is pointed at the `miniapps` submodule through `ORIGIN_ROOT`),
and the first written against the 2014 interfaces, so most of its patch is
*modernise*: module include paths for the old `hpx/lcos/…`, `hpx/util/…` and
`hpx/runtime/…` ones, `hpx::program_options` (the one Boost library this build
does not carry — the accumulators, date_time, random, serialization and
lexical_cast uses are untouched, so the draws and the report stay the
program's own), `hpx::chrono` timers, `hpx::spinlock` with `std::lock_guard`,
`hpx::counting_semaphore_var<>`, `hpx::promise`, `hpx::dataflow`, `hpx::bind`
with `std::ref`, `hpx::unwrapping_n<2>`, `hpx::post`,
`hpx::get_num_localities(hpx::launch::sync)`, `get_id()`, `std::shared_ptr`,
`HPX_REGISTER_COMPONENT`, `hpx::lcos::broadcast_post` with its registration
macros, deleted copy operations where `HPX_MOVABLE_BUT_NOT_COPYABLE` was,
`hpx::init_params`, a named `hpx::distributed::barrier` in place of the
barrier created at rank 0 and looked up through an AGAS symbol event, and an
all-reduce gate that takes its participant count at construction and advances
its generation explicitly, because the interface no longer takes the count
with every generation. The rest: *build* — the program registers its own
component module, since the app project gives each executable its own
`HPX_COMPONENT_NAME`; *cfg* — the run-everywhere vector becomes the runtime
defaults; *markers* — `[HPX]` after `p.setup(vm)`, `[E2E]` from immediately
before `stepper->init(p)` (so the initialisation, both barriers and the run
are inside) to after the second `barrier_wait()`, with `[PARCELS]` after the
end stamp; *scalar* — the maximum of the `error_iter` the program already
computes and already terminates on, printed once as `ERRMAX %.6e`; and the
*bugfix* hunks described below.

| HPX wait site | classification | mirror |
|---|---|---|
| `recv_buffer::operator()`'s `buffer_.receive(step).get()` | start wait — this step's zone | the unpack task's read-only dependence on the face point |
| `when_all(recv_futures).then(flux_accumulate)` | continuation | the flux task on the unpacks' output events |
| the chunk's `when_all(dependencies).then(stencil)` | continuation — every input | the chunk task's dependences (its boundary unpacks; the previous step's completion point on either arm — the reduction where the variable is summed, the flux where it is not; and the previous step's chunks where it is not summed) |
| `when_all(send_futures[dir]).then(send_buffer)` | continuation | the pack task on the chunks the origin gates that direction with |
| `sum_grid`'s dataflow: the local sum, then the all-reduce `.get()`, then the check | **mid wait**, one per summed variable per step | the task is split there: a sum task that publishes its partial, and a check task on the `nl` partials |
| `stepper->init(p).get()`, `wait_all(run_futures)`, `barrier_wait()` ×2 | driver waits — setup, the runs, the collectives | the init tasks behind the initial all-reduce, the per-rank done latch, and the completion points into the final task |

Mid waits: 1 per summed variable per step.

Mirror mapping (`benchmarks/apps/hpx_origin/mini_ghost_hpx.c`): one block per
grid generation per variable homed at its rank, one task per origin task
hinted there, each packed zone its own block travelling on a labeled STICKY
point to the rank that unpacks it, and the all-reduce as `nranks` publishes
with no reduction tree — the origin's `broadcast_post` sends `nranks` parcels
and builds none, each arrival folding its value into the destination's
accumulator under the origin's own two locks (a gate mutex, then a value
mutex) and landing in arrival order, exactly as the origin's `value_ +=
value` does. A step's spawner creates the next step's spawner at its end, so no
join stands between two steps and every task is gated by its dependences alone,
as in the origin's own non-blocking loop; each step's chunks take the previous
step's completion through a step point, which the check task raises once the
error has been folded in where the variable is summed and the advance task
raises from the flux where it is not, and a chunk's completion is a point of
its own kind, needed only on that unsummed arm, whose next step then reads the
previous step's chunks directly. A step's join names that step's packs only on
its last step, where what runs behind it retires the grids those packs read.
The six unpacks of one step write disjoint halo planes that meet at the
halo's edges, and the mirror leaves them overlapping exactly as the origin's
six `hpx::async` unpacks do.

**The origin's defects are fixed, minimally, and disclosed — four of them
here, the fifth in its own passage below.** Under
strong scaling the remainder of a global dimension over its process-grid
dimension was computed with `&` instead of `%` (`tmp_nx & npx`), which is not
a remainder at all; the three operators are corrected. The comparison beside
them (`if(rank < remainder)`) is *not* changed and *not* judged: whether the
remainder belongs against the linear rank or against the rank's coordinate
cannot be settled from anything in this tree, which carries the port and no
reference implementation. The stepper built each partition as a temporary and
moved it into its vector, while the partition's constructor had already
attached a continuation capturing the temporary's address to receive the
global sum of its initial grid: the arriving partials reached the object in
the vector while the continuation read the moved-from temporary and wrote
through a destroyed pointer, so `source_total` stayed zero and every summed
variable's check failed on its first step; the partition is constructed in
place instead. `source_total_` and `flux_out_` were left uninitialised by the
constructor and `flux_out_` is *read* on the first summed step, feeding the
quantity the row reports — undefined behaviour that at eight localities made
four runs in twenty report errors of 1e+19 and larger and terminate; both are
initialised to zero. And each direction's send was gated on the chunk at the
opposite end of its axis from the plane it packs, so with more than one chunk
on that axis the pack could ship a plane this step had not written: twenty
eight-locality runs gave twenty different answers on both sides; each
direction now waits for the chunks that write its plane, and the mirror is
reproducible there afterwards.

**The HPX program as published was not reproducible on a multi-axis grid, and
the cause was not the one the halo overlap suggests.** The program has two
unordered pairs there and only the second of them moved a value.

*The six unpacks of one step overlap.* Each writes a full padded plane, so
adjacent faces meet on the box's edge lines and such a cell holds whichever
landed last. That carries no ambiguity: both writers of an edge cell are
two-hop copies of the **same** cell of the diagonally adjacent rank. The value
that arrives through the east face was written into the east neighbour's north
halo, two steps earlier, by that neighbour's own north unpack — out of the
north-east rank's cell `(1,1,z)`; the value that arrives through the north face
was written into the north neighbour's east halo, in the same two steps, out of
the same north-east cell. Whichever unpack lands last writes what the other one
would have.

*A step's pack reads the generation the next step's unpack writes.* At step `t`
a pack reads `grids_[dst_]`; the loop swaps, and at step `t+1` the unpacks write
`grids_[src_]` — the same array. As published, nothing joined them:
`send_boundaries` launched each pack as a `when_all(send_futures[DIR]).then(…)`
continuation and **discarded the future**; the next iteration's
`receive_boundaries` waited only for the neighbour's bytes; and the `wait_all`
after the loop names the sums, the fluxes, the receives and the calculations —
every future except the sends'. A pack that lost that race packed the next
step's halo, shipped it into the neighbour's edge cell, and the 27-point stencil
read it. That needs neighbours on two axes — exactly the geometries that moved
(`2×2×1`, `2×2×2`) and not the ones that did not (`4×1×1`, `8×1×1`, bit for bit
across all six entries).

**That is a defect of the program, and it is fixed in the origin copy** (a
disclosed edit in `ORIGIN.md` and `origin.patch`): each step keeps the futures
of its sends, and every unpack of the next step waits on them before it copies —
the receive itself stays inside the task. The mirror carries the fixed program:
a pack raises a point the next step's unpacks wait on, and, like the origin's
loop, the mirror issues every step up front and gates step `t+1` only by those
dependences (its unpacks on the neighbour's face and its own step-`t` packs, its
flux on its unpacks, its chunks on the step's reduction, their own previous
generation and their neighbouring chunks or faces). The first pair is left as
written on both sides: it cannot move a value. The row's ladder runs one axis at
every rung, which is also what the program picks for itself when no grid is named
(`npx = nranks`), so every rung is unanimous; a two-axis check after the fix
(`2×2×1` on four ranks, the gate's arguments) gives one value on five runs of
each side, `8.334530e-02`, where the published program gave ten values in ten
runs; a cubic grid stays reachable and is not a rung, and what the one-axis
choice costs is two neighbours per rank instead of six, a thinner halo
than a cubic grid exercises.

**Three more mirror-side corrections, from the section's concurrency review.**
The mirror packed and published at its **last** step, where no step follows to
unpack: face points with a producer and no consumer, blocks nobody frees, and —
since no join counted a pack — a publish that could be issued after the
reporting rank had already called `ocrShutdown()`. The origin's own send side
runs on every step, the last included, and its receive side registers for
steps `1…nt` only, so its own last sends are simply never claimed; the mirror
keeps that shape — packing and posting on the last step too — and gives each
such zone a reaper task on the rank it was addressed to, the rank that would
have unpacked it, counted into a run-wide latch behind the close task rather
than the step's own join, so the retirement is not part of what the run
reports but is still complete before the end. The `adv` template declared a fixed arity of one and
was instantiated with `1 + nchunks` on a variable's last step; it is
`EDT_PARAM_UNK`, like every other variable-arity template in the file. And a
variable that is **not** summed has no sum to join its step, so beyond that
step's flux — which is what its step point carries — its next step's ordering
rides the previous step's chunk completions, and the set each chunk waits for
is the origin's own: itself and its six **face**-adjacent chunks
(`partition.hpp`'s `dependency_west` … `dependency_back`). The nine- and
27-point stencils read cells a **diagonally** adjacent chunk writes, and that
pair has no edge on either side. Completing the set in the mirror would be an
ordering the origin does not have, so the mirror refuses the combination
instead: an unsummed variable (`--percent_sum` below 100), a diagonal-reading
stencil (22 or 24) and a blocking that puts a diagonal chunk on the axes it
reads them from is a usage error naming all three. The roster never reaches it
— `--percent_sum=100` makes every variable summed, and a summed variable's step
is joined by its own sum — and a calibration cannot walk into it silently.

**One grid block per (variable, generation) has many writers — the second such
disclosure in this section.** Within one step the six unpacks take the source
generation `RW` and the `nchunks` chunk tasks take the destination generation
`RW`; and across steps, now that every step is issued up front as the origin
issues it, a step's sum holds its destination generation `RO` while the next
step's unpacks hold the same block `RW` — halo planes against interior, the
same unordered pair the fixed origin has. Under the OCR memory model that is defined: `DB_MODE_RW` is not
exclusive, so several tasks may hold one block `RW` at once, and what makes
the result well defined here is that their regions are disjoint apart from
the edge lines above, not that the runtime serialises them. Under
`ARTS_MEMORY_MODEL=DB_WRF`, whose contract covers disjoint-region
siblings, they are unordered write-write conflicts on one block and the result
is not defined. The row measures identical under the `DB_WRF` build at one and
two ranks, which is evidence and not proof; `DB_WRF` is not on artsrun's
coherence plane, so no campaign cell exercises it. `network_storage_hpx`'s
storage slots were the first exception to this section's one-writer-per-block
rule; these are the second.

**What is left as written, and what it costs the row.** The origin's
`flux_accumulate` reads the *halo* planes of its grid and only on a rank that
sits on a global boundary, where no neighbour ever writes them — so the flux
is identically zero and the 27-point stencil's boundary loss is never credited
back. The conservation error therefore grows about half
a percent per step and exceeds the origin's own `error_tol` at every geometry,
where the program calls `hpx::terminate()`. That is a kernel computation, so
it is mirrored rather than repaired, and the row passes the origin's own
`--error_tol=1` instead: both programs then run to completion and the error
they measure is what the entries vote on. There is no cross-geometry pin —
every rank draws its own initial field, so the answer is a function of the
rank count, as `stencil1d_hpx`'s checksum is. Every OCR entry computes the
same bit pattern at each geometry (`3fb0eaa3dd5335fc` at one rank,
`3fb219105b309b99` at two) and the HPX program prints the same values; `xsocr`
renders those identical bits as `6.608082e-02` and `7.069544e-02`, a fixed
`+5e-07` offset of its own formatter, which the row's `1e-05` tolerance covers
with less margin than this section's other rendering caveats.

Gate arguments: `--scaling=1 --nx=64 --ny=64 --nz=64 --num_vars=4
--num_tsteps=10 --stencil=24 --percent_sum=100 --num_spikes=1 --error_tol=1`
with the process grid per node from the catalog's ladder (`ERRMAX
6.608032e-02` at one locality, `7.069494e-02` at two).
Calibrated arguments: `--scaling=1 --nx=480 --ny=480 --nz=480 --num_vars=5 --num_tsteps=100 --stencil=24 --percent_sum=100 --num_spikes=1 --error_tol=1 --npx=1 --npy=1 --npz=1` (per rung: 1 node: --npx=1; 2 nodes: --npx=2; 4 nodes: --npx=4; 8 nodes: --npx=8; 16 nodes: --npx=16; 32 nodes: --npx=32) — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/mini_ghost_hpx_hpx --hpx:threads=4 \
  --scaling=1 --nx=64 --ny=64 --nz=64 --num_vars=4 --num_tsteps=10 \
  --stencil=24 --percent_sum=100 --num_spikes=1 --error_tol=1 \
  --npx=2 --npy=1 --npz=1
```

## nbody_hpx

Origin: STE||AR-GROUP's `NBody`, `DistributedNBody/NBody Code/main.cpp` (pin
`6147a176`, 2016-02-17) — the second row here whose origin is another
submodule, reached through `ORIGIN_ROOT`, and the one whose origin path
contains a space. The repository states no licence. Most of its patch is
*modernise*, from the 2016 interfaces: `hpx::collectives::gather_here` /
`gather_there` with `num_sites_arg` for `hpx/lcos/gather.hpp`, module include
paths, `hpx::for_each` with `hpx::execution` and `hpx::util::counting_iterator`
for the Boost one, `hpx::program_options`, `std::uint64_t`, `hpx::util::format`
(whose conversion goes inside the placeholder), `hpx::spinlock` with
`std::lock_guard`, `component_base` / `component`, `hpx::dataflow` and
`hpx::unwrapping`, `hpx::post`, `find_from_basename` /
`register_with_basename`, `get_id()`,
`hpx::get_num_localities(hpx::launch::sync)`, `hpx::chrono`, a `const` deleter
for the serialized buffer, and `hpx::init_params`. Nothing of Boost remains.
One of those renames is not only a spelling: the collective now takes the local
**value** where it took a future, so a locality resolves its own result at the
call — one more wait in the driver body, none inside a task. The rest: *cli* —
the six file-scope constants and the cell count become `--n --nt --size
--theta --th --k-th --np`, and with them two usage checks the edit owes,
because the constants are what made the paths unreachable: `--size` must be at
least `--k-th`, and at least the widest owned cell's membership, or the program
indexes past its buffers; *cfg* — the run-everywhere vector becomes the
runtime defaults; *markers* — `[HPX]` once the locality count is known, `[E2E]`
from the top of `hpx_main` before `create_Octree()` (so the tree build is
inside) to after locality 0 has pulled every gathered cell, `[PARCELS]` after
the end stamp, and a newline flushed after the program's own report, which ends
without one; *scalar* — `CHECKSUM %.14g`, the sum of the gathered cells' body
positions; and four *bugfix* hunks described below.

| HPX wait site | classification | mirror |
|---|---|---|
| `Left_.get()` … in `send_*` | start wait — neighbour ids resolved at setup | the neighbour table, computed |
| `receive_*(time)` feeding a `dataflow` | start wait — this step's arrival | the `New_Members` task's dependence on the arrival point |
| the chain's `.then(…)` / `dataflow(…)` stages | continuations | one task per stage, created by the stage before it |
| `result.get()` before the gather | driver wait — the modernisation's | the gather task behind the rank's last stage |
| `overall_result.get()`, the `get_data(cell0)` loops | driver end waits | two rank-0 tasks, each taking one copy of every gathered cell's partition, the summing one behind the discarding one |

Mid waits: 0 — no stage body blocks and then computes.

Mirror mapping (`benchmarks/apps/hpx_origin/nbody_hpx.c`): the per-cell
iterations of `do_work` are a strict chain in the origin — its six `From_*`,
six `To_*` and `n_` are aliases of two slots, so cell `j` reads what cell `j-1`
last wrote — and the mirror is that chain, each stage creating the task that
reads its block. Only the six partitions a rank pushes per step travel on
labeled points, one per direction per step, indexed so that the reader is the
home wherever the runtime homes a labeled range by index. A stage value
with several readers carries a counted edge that destroys it when the last
reader finishes, which is the reference the origin's `serialize_buffer` holds.
The five of six `New_Members`, five of six `changed_members` and three of four
`Non_Members` whose results the origin overwrites before reading are run and
dropped, as they are there. Those dropped turns and the reclaims they carry down
are behind the end: each rank's reclaims count into a join of their own and the
rank reports done through one cleanup point apiece, which the close task —
created before the ranks fork, alongside the collect task it follows — holds
together with the report event the two-pass pull chain raises only once every
gathered cell has been read twice, so it cannot shut the runtime down while a
rank is still freeing a block or finishing a turn whose result nobody reads.

**Four defects in the origin are fixed, minimally, and disclosed.** `np` is
read uninitialised at nine to fifteen localities, and above sixty-four; the
`--np` default applies the rule the assigned branches state and the usage check
refuses the rest. `idx()`, the neighbour table, reads an uninitialised `temp`
at every rank count it does not tabulate, and runs off its end without a
return; both are closed with the stay-put value the function itself gives for
the counts below its smallest tabulated one, which is a no-op wherever it does
assign. The same function guards two of its branches on `i == -2` and `i == +2`
where `i` is a `std::size_t` and every sibling branch spells the same guard as
`dir`: no locality id can equal either, so with the branches dead sixty-four
localities fall through to an index outside the locality set and then wait on a
name nobody registers. Both become `dir`, and the repaired table is a
permutation. And `cell_list()` appends to shared vectors under
`hpx::parallel::for_each(par, …)` where the targets are not distinct — 64
iterations write 41 cells and 18 of them are written by two or three at once —
an unsynchronised `push_back` on one `std::vector`; that one algorithm's policy
becomes `hpx::execution::seq`, which is also the order a per-iteration buffer
merged afterwards would produce.

**What is left as written.** The force kernel keeps its `1 +` per interaction
and its `float` distance; `changed_members` keeps the stray semicolon that
discards its positive-direction test; `new_tree` keeps walking to a root it
never changes; and `strc3` keeps subdividing sibling subtrees concurrently,
where a body on a split plane belongs to two cells and two tasks write its
`parent` field at once (628 of 20000 at the gate). The dropped `New_Members`
turns overlap the same way, on both sides: they run beside `compute_position`,
so their `InCube` test reads the body table while the force kernel is writing
positions into it. That test decides nothing — its verdict only re-states an
entry the copy before it already made. The racing `parent` store is why the two
sides' *forces* cannot be expected to agree — they are not reproducible between
two HPX runs either. It moves nothing the row reports: a partition carries the
positions its bodies had when it was built, `compute_position` writing only the
force back into it, so the checksum follows the bodies between cells and
localities and is blind to the kernel, which reaches nothing else. That is also
why the answer does not depend on the locality count.

Gate arguments: `--n=20000 --nt=2 --size=4000 --theta=0.3 --th=100 --k-th=500`
— `CHECKSUM 5862219` on the HPX program and `5.86221900000000e+06` on every
OCR entry (`%.14e`, because the xsocr `printf` replacement has no `%g`), the
same at one, two, four and eight. Width is the cell count, 8 below nine
localities and 64 from nine, whatever the node count: an anti-scaler by
construction.
Calibrated arguments: `--n=120000 --nt=100 --size=24000 --theta=0.3 --th=100 --k-th=500` — the one-node anchor sizes under `## ARTS versus HPX`.

**`nbody_hpx_xsocr` segfaults in about half its two-rank runs**, always in that
runtime's own incoming-message path — `hcPolicyDomainProcessMessage`
(`hc-policy.c:3598`, `PD_MSG_DEP_SATISFY`), on the line whose comment reads
"TODO I think there's a MD cloning issue here. The dstKind can be remote" — and
never in the mirror. A single-step run passes 10 of 10, as does
`mini_ghost_hpx` at the same geometry, and opening every point in the driver
before anything publishes left the rate unchanged; the window is a rank pushing
an arrival for a step its neighbour has not reached. At one rank the entry is
exact.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/nbody_hpx_hpx --hpx:threads=4 \
  --n=20000 --nt=2 --size=4000 --theta=0.3 --th=100 --k-th=500
```

## sheneos_hpx

Origin: HPX's `sheneos` example (`third_party/hpx/examples/sheneos/`,
BSL-1.0) — twelve files across six translation units, and the only program
of this section
whose input is a **dataset** rather than its arguments: a tabulated nuclear
equation of state (`datasets/sheneos/HShenEOS_rho220_temp180_ye65_…h5`,
staged by hand and checksum-gated, axes 65 x 180 x 220). It is also the only
one that reads a file inside the measured window. Edits: *cfg* — the runtime
defaults with `hpx_main` left on locality 0 alone, the shape the program is
written for; *markers* — `[HPX]` geometry from every locality through a
startup function, `[E2E]` from the first statement of `hpx_main`'s body (so
the interpolator's construction, which reads the whole table off disk, is
inside the window) to the completion edge at `wait_all(bulk_tests)`,
`[PARCELS]` after the end stamp; *scalar* — the program assigns its results
and casts them to void, so the edit keeps them: each worker returns the sum
of the values it received and `hpx_main` prints `CHECKSUM %.14g` by one
write, after the end stamp; and one *bugfix*.

**The bugfix, and why this example could not run at all.** `fill_partitions`
called `partitions_[index].init_async(...)` inside its triple loop while
`partitions_` was still the default-constructed empty vector — the assignment
from `hpx::new_<partition3d[]>`'s future sat *below* the loop — so every one
of those calls indexed a zero-length vector, guarded only by an
`HPX_ASSERT(index < partitions_.size())` that a release build compiles out.
The defect is demonstrable from the source alone, and the repair does not rest
on having watched it fail. The assert states the intended precondition and the only way to satisfy it is to have the vector before the
loop, so `partitions_ = result.get();` moves above it — the same partitions,
on the same localities, with the same slabs and the same names. Upstream
knows: `examples/sheneos/CMakeLists.txt` carries `# TODO: Fix example. Not
added to unit tests until fixed.`

One defect is **kept**, and it is worse than it first looks: `~interpolator`
advances its index twice per turn (`std::to_string(i++)` inside a
`for (…; ++i)`), so it visits every other index — and it asks for the wrong
names anyway, because `fill_partitions` appends `'/'` to a by-value copy of
the base while the stored `config_data::symbolic_name_` never sees the slash.
Between the two, no partition name is unregistered at all. It is teardown-only
and defined all the same: the destructor runs after the end stamp and after
`CHECKSUM`, and AGAS answers a missing name with an invalid id rather than an
error, so nothing escapes the implicitly `noexcept` destructor — the program,
not a bug to repair.

Two facts about the arguments, both as written. **`hpx_main` reads
`num-ye-points` three times** — `num_temp_points` and `num_rho_points` are
both `vm["num-ye-points"]` — so the grid is a cube of that one number and the
other two options are dead; the mirror parses all three and overwrites the
latter two the same way, so the two are the same program at any arguments.
And **`--num-partitions` must equal the node count**, because the program
derives the partitions-per-dimension twice from two different inputs:
`fill_partitions` uses `cbrt(localities)`, `connect()` uses
`exp(log(num_instances)/3)`. They agree only where
`floor(cbrt(partitions)) == floor(cbrt(localities))`; anywhere else the
client routes a query to a partition that was never initialised and the
interpolation throws. The mirror rejects an unequal pair as usage, the one
place it is stricter.

| HPX wait site | classification | mirror |
|---|---|---|
| `result.get()` / `wait_all(lazy_sync)` in `fill_partitions` | driver wait — the partitions exist and hold their slabs | the init join, a latch of `p^3` |
| `unwrap(bulk_tests)` in a worker | end wait | the collect task's reply points |
| `wait_all(bulk_tests)` in `hpx_main` | driver wait — the completion edge | the sum task's one slot per worker |

Mid waits: 0.

Mirror mapping (`benchmarks/apps/hpx_origin/sheneos_hpx.c`): one block per
partition holding its three axis slices and eight value arrays, read by one
init task on the rank that owns it through the HDF5 **C** API (the same
vendored library the HPX program reads with through the C++ one); one request
block and one query task per (worker, partition) group, which is the origin's
one bulk action per partition; one reply block per group carrying the
origin's full eight values per coordinate; one collect task per worker and
one sum task on rank 0, created before any worker task exists — this row has
no per-rank SPMD fork, since `hpx_main` runs on locality 0 alone — so
shutdown sits behind every rank's last work. Only the replies need the
ordering rail — a worker cannot
know the GUID the query task will choose — so each is published on a labeled
STICKY point indexed `(worker·p^3 + partition)·nl + worker_rank`; everything
else is written and released before its consumer's slot is filled. The
mirror reproduces the origin's per-locality query shuffle exactly: a
per-rank block carries a glibc `random_r`-style RNG state seeded with
`seed + rank`, as the origin seeds its process-wide generator, and each
axis's index array is shuffled by the same forward algorithm the origin's
vendored `random_shuffle` runs, so at the same seed the two sides draw the
identical per-axis order. The query *set* is a function of the axis ranges
and the point counts alone and does not depend on this order; only the
summation order does, which the tolerance covers. It does carry the per-worker re-read of the three axis ranges that
`connect()` performs, because that is file I/O inside the window; it does not
carry `connect()`'s AGAS name resolution, because the mirror's table block is
the name service.

**The hot spot is the point of the row.** The cube is cut by
`floor(cbrt(localities))`, so there is exactly **one live partition at one,
two and four nodes, and it sits on locality 0** — every query from every
worker on every rank goes there, and the whole 165 MB table lives there.
Eight nodes is the first rung where all eight partitions carry data. That is
what a cube-root decomposition does on a node count that is not a cube; it is
reported, never engineered away.

**The checksum is geometry-independent**, unlike `stencil1d_hpx`,
`mini_ghost_hpx`, `nbody_hpx` and `fft_hpx` — though the harness applies the pin
only at the one-node cell, where the arguments equal `expect_args`, and the other
rungs are judged by agreement among the entries. The query set does not depend on
the partitioning, and the ladder holds the worker total at 8 (`--num-workers`
is per locality, so it falls as the node count rises). The HPX entry prints
`1.8609856691588e+40` and every OCR entry `1.86098566915881e+40`. The `1e-09`
tolerance covers summation order — the origin sums each worker's replies in
its shuffled query order, the mirror group by group in axis order, a relative
difference of order `1e-13` — and the xsocr formatter's part in `1e15`.

Gate arguments: `--num-ye-points=8` with `--num-partitions` equal to the node
count and `--num-workers` making the total 8.
Calibrated arguments: `--num-ye-points=95 --num-temp-points=95 --num-rho-points=95 --num-partitions=1 --num-workers=4096 --seed=1` (per rung: 1 node: --num-partitions=1 --num-workers=4096; 2 nodes: --num-partitions=2 --num-workers=2048; 4 nodes: --num-partitions=4 --num-workers=1024; 8 nodes: --num-partitions=8 --num-workers=512; 16 nodes: --num-partitions=16 --num-workers=256; 32 nodes: --num-partitions=32 --num-workers=128) — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 1800 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/sheneos_hpx_hpx --hpx:threads=4 \
  --file=../datasets/sheneos/HShenEOS_rho220_temp180_ye65_version_1.1_20120817.h5 \
  --num-ye-points=8 --num-temp-points=8 --num-rho-points=8 \
  --num-partitions=2 --num-workers=4 --seed=1
```

