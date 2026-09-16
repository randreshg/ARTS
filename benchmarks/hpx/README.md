# HPX-origin programs

## What this directory is

Six ports once started from an ARTS-native OCR application and mirrored
its algorithm and placement in HPX by hand; that direction is retired
(`archive/hpx-ports-ocr-origin/`). This directory reverses it: each row
starts from an unmodified program taken from HPX's own tree (or another
disclosed provenance), and an OCR program mirrors it — same argument list,
same result, same placement, expressed as EDTs and DBs instead of HPX
tasks and components. Design intent, the per-program dossiers and the full
admission process are `research/superpowers/specs/2026-09-11-hpx-origin-apps-design.md`.

## One row, four runtimes

A row's name (`<row>`) always ends in `_hpx` and is the directory under
`benchmarks/hpx/<row>/`. `<row>_hpx` is the HPX program, built by this
project into `build/benchmarks/hpx/`. `<row>_arts_<variant>`,
`<row>_xsocr` and `<row>_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/<row>.c`), built by the app factory into
`build/benchmarks/apps/`. One row in the catalog carries all four
binaries: `tools/artsrun/src/artsrun/data/hpx_apps.yaml`.

## Admission rules

A program enters the roster only if all five hold.

- **A1 — HPX distributed model only.** Every inter-locality interaction is
  an HPX primitive: action, component, channel, collective. The program
  contains no `MPI_` call (checked by grep at review). A program whose
  on-node part is HPX and whose inter-node part is MPI measures MPI, not
  HPX, and is inadmissible; a fair comparison cannot be made from it.
- **A2 — Wait sites classified.** An HPX task may suspend; an EDT cannot.
  Every `.get()`/`wait_all`/`receive().get()` inside a spawned task is
  classified as a *start wait* (inputs gathered before any compute — an
  EDT's pre-slots), an *end wait* (join of children — a finish/join
  event), or a *mid wait* (compute, then wait, then compute inside one
  task). Only a mid wait forces the mirror to split a task, and the count
  and location of every mid wait is disclosed in the row's own section
  below. Driver-level waits (`hpx_main`, an SPMD run loop) map to the main
  EDT and per-step joins.
- **A3 — Provenance.** Vendored HPX v1.11.0 tree (`third_party/hpx`,
  commit `c9b81b40`) first; STE||AR organisation repositories second;
  published third-party code last, and only with an archived, checksummed
  copy.
- **A4 — Disclosed edits only.** Allowed: the `[E2E]` window and `[HPX]`
  geometry line (`common/e2e.hpp`), the runtime cfg lines
  (`common/runtime_defaults.hpp`), a result scalar print, exposing an
  existing constant as a CLI argument, and API modernisation of a program
  written against an older HPX. Not allowed: changing the algorithm, the
  decomposition, the communication pattern, or the arithmetic — a dubious
  kernel is mirrored as written, not repaired. Every edit is a hunk in the
  program's `origin.patch` (below), regenerated and compared by a test.
- **A5 — A computation.** The program computes a result from arguments and
  reports it (or can, by A4's scalar print). Demos, interactive programs,
  tests of a component, and libraries are out.

**P — Placement.** The OCR mirror carries the HPX program's placement: an
EDT runs where the HPX task ran (the locality the program named), a DB is
homed where the HPX component or buffer lived. One tier, no `_hinted`, no
`restructured`. Dropping the placement would make the mirror a different
program (one that moves data the HPX program never moved).

## Layout and disclosure

```
benchmarks/hpx/
  CMakeLists.txt            # the standalone app project (ExternalProject)
  apps.cmake                # the one list of app targets, included by both
                            # projects
  common/                   # e2e.hpp, runtime_defaults.hpp
  probes/action_rtt.cpp     # not a program of the roster — measures one runtime primitive
  tests/static_smoke.cpp
  tests/check_origin.py     # regenerates each row's diff against its origin
  <row>/                    # e.g. fib_hpx/
    ORIGIN.md               # origin path/URL, commit/tag/DOI, licence, date
    origin.patch            # the complete disclosed diff origin -> ours
    <sources>               # the edited copy under its origin file names,
                            # built as <row>_hpx
```

The pristine origin is always present in the tree (the vendored HPX
submodule for every row here). `check_origin.py` runs in the app
project's ctest step as `check_origin_<row>`: for each row it diffs the
origin files named in `ORIGIN.md`'s `files:` table against ours and fails
unless the result equals `origin.patch` byte for byte. A reviewer reads
`origin.patch`; a change to a row is a change to its patch. Regenerate a
stale patch with:

```bash
python3 benchmarks/hpx/tests/check_origin.py --write benchmarks/hpx/<row> third_party/hpx
```

## Threading and pinning

One locality is one process (one MPI rank). Every program here bakes in
the same five runtime defaults, plus a sixth (`hpx.run_hpx_main`) that a
program whose `hpx_main` is locality 0's driver leaves out — forcing that
driver onto every locality would run it once per locality. `fib_hpx` and
`random_mem_access_hpx` are of that shape; `pi_hpx` takes all six:

- `hpx.use_process_mask=1` — an externally installed CPU affinity mask is
  authoritative: HPX sizes and pins its worker threads inside it instead of
  rebinding from the machine topology (which is HPX's stock behavior).
- `hpx.os_threads=cores` — the default worker count is one per *physical
  core* in the mask, so SMT siblings never carry a second worker. An
  explicit `--hpx:threads=N` overrides.
- `hpx.parcel.mpi.sendimm=1` — sends go out immediately instead of waiting
  for one of the few cached connections; a fire-and-forget delivery that
  waits parks its thread, and a completion burst parks thousands at once
  (each parked HPX thread holds a stack for as long as it is parked).
- `hpx.run_hpx_main=1` — *the optional sixth*: every locality runs
  `hpx_main` and stays inside it until the global completion edge, because
  `hpx::finalize()` on *any* locality begins global shutdown; a locality that
  returned early would tear the run down under its peers. A program written
  for a single `hpx_main` keeps that shape instead, and its other localities
  host its components until locality 0 finalizes.
- `hpx.thread_queue.max_thread_count=100` — the per-queue ceiling on
  materialising staged work ahead of demand. Every thread object that runs
  takes a 64 KiB stack, and the queue keeps the stack in its own cache for
  the rest of the run, so a process's stack memory follows how many thread
  objects it ever materialised rather than how many are live. The ceiling
  is soft — a queue that runs dry raises its own limit — so it trims a
  burst rather than bounding one.
- `hpx.scheduler=local-priority-lifo` — the pending queue pops most-recent
  first. A worker then runs its own newest child before its older siblings,
  so an eagerly spawned tree is walked depth-first and the materialised
  frontier is bounded by the tree's *depth*; the stock `local-priority-fifo`
  walks it breadth-first and materialises the whole frontier as started
  thread objects, each holding a stack. This is the one entry spelled
  without HPX's forcing modifier (`key=value`, not `key!=value`): a forcing
  entry outranks the command line, and a scheduling policy has to stay
  selectable with `--hpx:queuing`.

So a bare run uses every physical core of the machine, a run under a
narrowed mask uses exactly the cores of that mask, and one locality per
host — the remote launcher shape — needs no flags at all.
`--hpx:print-bind` prints the realized per-worker binding for verification.

Colocated localities (several per host) additionally depend on the one
backported upstream fix this build carries: stock v1.11.0 shifts each
newly registering locality's binding by the core footprints its peers
reported (AGAS `first_used_core`), computed against the machine topology
with no regard for the process mask, so colocated localities land outside
the disjoint per-rank masks the launcher set up. Upstream fixed this on
master (`6df14adf09`, "Don't apply PU offset for localities that
explicitly use core bindings"); the configure step applies that commit
verbatim onto the pristine submodule tree from `third_party/patches/hpx/`
— idempotently, so the submodule working tree stays at v1.11.0 plus
exactly that one patch.

### Thread-count sensitivity

A reference rank gets the same CPU block as an ARTS rank and divides it
its own way: ARTS spends one core of a 16-core block on a dedicated
progress thread and computes on 15, HPX polls the parcelport from its
workers and computes on all 16. The linear bound on what that is worth is
`16/15 = 1.067` — what a run whose whole cost is worker-parallel loses on
one worker fewer, and the most a run can lose. The CPU-budget asymmetry
between the two runtimes is therefore a few per cent at most, and it is
reported as a band rather than corrected for.

### MPI transport on one host

A local run simulates a multi-node job, so the localities of every runtime
reach each other through the network stack, never through shared memory:
a shared-memory transport would carry the parcels past the very layer the
simulation exists to exercise. ARTS's socket provider already runs over
127.0.0.1. Where the MPI layer is UCX (MPICH `ch4:ucx`, the usual build on
a workstation-class host), its defaults select shared memory and
cross-memory attach between colocated ranks, so it is restricted to TCP on
the loopback device:

```bash
UCX_TLS=tcp,self UCX_NET_DEVICES=lo mpiexec -bind-to none -n 8 ...
```

The experiment driver exports this for every HPX, xsocr and OCR-vx cell it
launches under its local launcher, and only there; remote launchers keep
their site's MPI defaults, where the fabric is the right transport. The
restriction matters: on the same program at two ranks, xsocr's
per-iteration increment was 2.2 ms on UCX's shared memory and 7.0 ms on
loopback TCP, while ARTS's was 3.7 ms on the loopback it always used, so
a local comparison made before the restriction put the runtimes on
different transports. It also removes the one failure mode shared memory
had here: UCX's SysV variant grows its receive-descriptor pool one System V
segment at a time and never returns them, so a run whose parcels ran into
tens of millions used to exhaust `kernel.shmmni`. Local numbers remain for
correctness and a rough trend; the fair comparison is made on Dane.

### The configuration every table was measured on

HPX runs here as HPX configures itself: 128-bit atomics on — by flags,
because this version's own feature test cannot enable them on any
toolchain (`hpx_check_for_cxx11_std_atomic_128bit()` never forwards its
arguments, and GCC reports a 16-byte `std::atomic` as not lock-free
regardless of hardware) — `local-priority-lifo` selected on the last of the
five configuration lines above, coroutine stacks taken from the heap
(`HPX_WITH_THREAD_STACK_MMAP=OFF`, an option of the build), and two source
patches, neither of which touches a scheduling structure. The stack option
is what lets a program with a wide flat burst of waiting tasks run at all:
a task that has started and waits keeps its stack, the runtime bounds how
many such tasks exist by nothing, and with one `mmap` per stack (two
mappings with the guard page) such a burst reaches the kernel's per-process
mapping limit — 65,530 by default, far below the node's memory — and dies
with an exception rather than slowing down. From the heap, the live-task
bound is the memory bound, which is the bound every other runtime here has.
What the option gives up is the guard page: an overflowing task stack
corrupts its neighbour instead of faulting. The first is upstream's own `6df14adf09`,
backported because a released HPX cannot place colocated localities inside
their masks without it. The second is local and fixes a crash:
`thread_local_caching_allocator::cache()` is marked out of line. An HPX
thread that suspends can resume on another worker, and an inlined lookup
may reuse a thread-local base computed before the suspension, so the
allocator cache — and the destructor registered for it — belong to the
previous worker, whose storage is read after that worker exits. Upstream
master had not fixed it when this build was pinned. `network_storage_hpx`
exposed it: on the default scheduler, work stealing untouched, its
unmodified origin crashed at exit in 27 of 36 runs at 2, 4 and 8
localities, and in none of 36 with the patch, with its measured window
unchanged; `fib_hpx` and `jacobi_hpx`, the rows that allocate the most
continuations, moved by at most the run-to-run spread. The ARTS
arms compared against it are compiled with `configs/counters_off.cfg` —
every counter compiled out — so no arm pays for instrumentation the other
does not have. One asymmetry survives that claim and is named rather than
folded into it: the HPX library is built with
`HPX_WITH_PARCELPORT_COUNTERS=ON`, which takes a mutex per parcel in the
parcelport's `gatherer::add_data`, so every parcel a timing run sends pays
a counter the ARTS side does not — an unquantified cost against HPX in
parcel-heavy cells that cannot simply be turned off, since `[PARCELS]`
reads exactly those counters.

Both sides compile for the same instruction set. The outer Release tree
adds `-march=native -mtune=native` to everything it compiles — the ARTS
runtime, the mirrors, XSOCR, OCR-vx — and the two HPX external projects
(the runtime and this app project) receive the same pair explicitly, since
an external project does not inherit the outer tree's options. What the
toolchain then does with identical arithmetic is taken as it comes and
disclosed per row (see `pi_hpx`), never patched.

### The `[E2E]` span

Every row defines the span the same way: it opens after the collective
that makes every locality ready and closes at the program's completion
edge, ahead of any collective that only releases the other localities.
That is the boundary the runtime it is compared against uses — the ARTS
stamp opens once initialization is complete, after the start-up
rendezvous and time sync, and closes when shutdown is recognised, before
teardown — so neither side's span contains its own start-up collective.
Each row's own section below states exactly where its span opens and
closes.

### The markers

`ARTS_E2E_MARKER` turns on the two measurement lines: `[HPX] locality=…
localities=… threads=…` from every locality, and `[E2E] <ns>` from locality
0 at the end of the span. `ARTS_STRUCT_MARKER` adds `[PARCELS] sent=…
bytes=… wire=…`, also from locality 0, printed immediately after the end
stamp and therefore outside the span. That line is the runtime's own
parcelport counters summed over every locality — locality 0 names each
locality's `locality#<i>/total` instance outright and reads it (the
`locality#*` wildcard resolves to an empty set in-process in this HPX
version), so a program whose `hpx_main` runs on locality 0 alone can print
it; those reads are parcels themselves, so the count carries a small fixed
self-overcount on top of the program's traffic — and a single-locality run
instantiates no parcelport, so it prints zeros. The line follows the end
stamp only when `ARTS_E2E_MARKER` is set too; on its own it is bare. There is
no `[STRUCT]` line in this section: that carried the retired ports' own
structural counters and none of these programs has any.

Both sides also print `[APP_E2E] <ns>` once, from the row's logical root —
under the same `ARTS_E2E_MARKER` gate as `[E2E]` on the HPX side, and
unconditionally on the OCR side, where the mirror's helper prints it — on
the application interval every row's own section below states: it opens after that row's option/seed/geometry work,
before the origin's first allocation or setup, and it closes after the
origin's own result, before any added checksum, report or teardown. The HPX
side's helper is `benchmarks/hpx/common/e2e.hpp`'s `print_e2e(clock,
app_clock)` — its single-clock form makes `[APP_E2E]` equal to `[E2E]` where
a row's two spans coincide, and the two-clock form is what lets them differ
where they do not. The catalog selects this span as the row's own timing
metric (`timing_metric: app_s`, `timing_contract:
hpx-origin/<row>/source-interval-v1`); `[E2E]` keeps printing on both sides
and remains a separate observation, never the selected metric.

## ARTS versus HPX

The section's comparison is of **one program on four runtimes** (the admission rules
above): the OCR mirror keeps the origin's operations, their order and its state, both
sides time the same application interval (`[APP_E2E]`), and neither side is tuned to
the other. Of the eleven rows in the roster, four are compared this way: `fib_hpx`,
`stencil1d_hpx`, `network_storage_hpx`, `fft_hpx`. The other seven carry
`comparison_excluded` in the catalog
(`tools/artsrun/src/artsrun/data/hpx_apps.yaml`) under the same two-part structural
criterion stated in the root `CLAUDE.md`'s "No bug-based masks" bullet: the origin
suspends started parallel tasks (blocking on futures, collectives or locks inside a
task — "line B"), or the origin fixes its parallel width below the machine's (one
task per locality, a fixed cell count — "width"). Two measurements make the
comparison, and they answer different questions.

### Rows outside the comparison

Excluded, with the catalog's reason for each; every one stays selectable and
runnable everywhere else — a campaign, a consensus check, its own row's section
below.

- `pi_hpx` — the origin runs one integration block per locality and spawns no
  task, so on one node the whole computation is one thread; the parallel width is
  fixed by the locality count, below the machine.
- `random_mem_access_hpx` — the origin serializes the updates of one element with
  a component lock whose contention yields the running thread; the mirror
  serializes them through data-block ownership, a different mechanism.
- `transpose_hpx` — the origin's validation tasks start and then block on
  sub-block futures inside a parallel reduce, one per block per iteration (the
  numerical phase itself is continuation style).
- `jacobi_hpx` — the origin's update tasks start and then block on their
  neighbours' futures (async, then get), one suspended thread per line block per
  row per iteration — a frontier that grows with the grid.
- `mini_ghost_hpx` — the origin's sum tasks wait mid-task on an all-reduce and its
  unpack tasks block on a neighbour's parcel — a bounded count, but started tasks
  that suspend.
- `nbody_hpx` — the origin fixes its octree level at eight cells below nine
  localities (64 above), so on one node at most eight cell chains run at once on
  112 cores; the parallel width is fixed by the origin, below the machine.
- `sheneos_hpx` — the origin's worker tasks start and then block on their own
  bulk query (unwrap) and on the partition sends (wait_all), two suspended
  threads per worker — a frontier set by the width knob.

### Idiom and width of every row

Idiom is drawn from each row's own disclosure above (the `comparison_excluded`
string for an excluded row, the wait-site table and "Mid waits" count for a
compared one); width is the formula `numeric_width`/`width_at` in
`logs/adhoc/2026-09-11-hpx-origin/calibration/size.py` read for that row.

| row | origin's parallel idiom | where the width comes from | compared? |
|---|---|---|---|
| `fib_hpx` | continuation (fire-and-forget `async`, a `when_all().then()` join) | `2·I` tasks, `I` the internal-call count below the serial threshold | yes |
| `pi_hpx` | width fixed by the origin (one integration block per locality, no task spawned) | `nl` tasks — one per locality (D4-exempt) | no — width |
| `random_mem_access_hpx` | started-then-waits (a component lock's contention yields the running thread) | `array-size`, the element count | no — line B |
| `stencil1d_hpx` | continuation (`dataflow` over already-posted futures; 0 mid waits) | `np`, the partition total | yes |
| `transpose_hpx` | started-then-waits (validation tasks block on sub-block futures inside a parallel reduce) | `B²`, `B` the total block count | no — line B |
| `jacobi_hpx` | started-then-waits (`update` tasks `async`, then block on four neighbour `get`s) | `(ny−2)·R`, `R = ceil((nx−2)/line_block)` | no — line B |
| `network_storage_hpx` | continuation (0 mid waits; the driver's own collectives bound each pass) | `globalMB·1024/transferKB` slots in flight per pass | yes |
| `mini_ghost_hpx` | started-then-waits (a summed variable's step blocks mid-task on an all-reduce; unpacks block on a neighbour's parcel) | `num_vars·C·nl`, `C` the chunk count per rank | no — line B |
| `nbody_hpx` | width fixed by the origin (octree level 8 cells below nine localities, 64 above) | `8` cells (D4-exempt) | no — width |
| `fft_hpx` | continuation (0 mid waits; two driver-level collective waits between phases) | `min(nx, ny/2+1)`, the row count of the narrower phase | yes |
| `sheneos_hpx` | started-then-waits (worker tasks block on their own bulk query and on the partition sends) | `W·L`, `W` the worker total, `L` the live-partition count | no — line B |

**The trend** (the development host, 1/2/4/8 colocated ranks of 15 workers + 1
progress thread, small arguments, three interleaved repeats) reads each row's *shape*:
the scaling verdict that chooses its measurement window (`scales` → 100 s;
otherwise `min(100, 1800 / (1.2 · f_worst))` s, floored at 5 s, with `f_worst` the
largest `c_n/c_1` over the sweep), and the ARTS/HPX ratio `r` at every
geometry, the median of the three per-repeat ratios. `r` in [0.77, 1.30] at every
geometry is *parity*; within [0.5, 2.0] *recorded*; beyond it *investigate* — and
every such row was investigated, with the cause named under the row above.

| row | ARTS 1n / 2n / 4n / 8n (s) | HPX 1n / 2n / 4n / 8n (s) | verdict ARTS / HPX | r = HPX/ARTS at 1n · 2n · 4n · 8n | class |
|---|---|---|---|---|---|
| `stencil1d_hpx` | 6.83 / 3.83 / 2.37 / 1.61 | 5.68 / 3.06 / 1.81 / 1.36 | scales / scales | 0.83 · 0.80 · 0.76 · 0.78 | **recorded** |
| `fib_hpx` | 13.62 / 8.48 / 5.57 / 4.45 | 15.23 / 8.90 / 5.92 / 4.73 | scales / scales | 1.12 · 1.05 · 1.03 · 1.10 | **parity** |
| `network_storage_hpx` | 5.05 / 33.95 / 30.56 / 26.00 | 3.52 / 35.64 / 27.49 / 17.99 | anti / anti | 0.69 · 1.03 · 0.90 · 0.69 | **recorded** |
| `fft_hpx` | 6.89 / 5.24 / 3.54 / 1.87 | 6.60 / 7.58 / 6.46 / 8.25 | scales / anti | 0.96 · 1.25 · 1.75 · 4.03 | **investigate** |

The row the trend flags is the runtime's, not the program's: `fft_hpx`'s HPX side
anti-scales because its scatter collectives route through locality zero. The two
rows inside the recorded band are ARTS's: `network_storage_hpx` at one rank is the
scheduler's locality (the store's slots are served where they were created, with no
NUMA-aware stealing, so one node carries most of the traffic); `stencil1d_hpx` at
these small partitions is the registered pool's population of the ring's footprint
at first touch — the mirror materialises `nd + 2` generations of blocks per partition
as the origin's allocator does, but the pool populates every slab it maps for them
(the teardown report's grow waits), a warm-up that is a fifth of a 7 s cell and is
amortised at the anchor, where the row is at parity.

**The anchor** (one node, 112 cores — 108 ARTS workers + 4 progress threads, HPX
112 workers — at the catalog's calibrated arguments, three interleaved repeats) is
the measurement the paper's node ladder starts from. Sizes were derived, never tuned:
the trend's window sets a row's target time `T`; the row's work law, one refit and
one interpolation through the measured two-point exponents place the slowest ARTS
arm in 0.8–1.2 `T` under the width floor (≥ 3456, the roster's rounded width), the frontier law and a 200 GB
one-node memory budget taken with a 10 % margin — memory first, then time, because a
cell that runs out of memory is lost for good — and the HPX resident set bounds
feasibility the same way (a cell every runtime must fit) but never sets a time. Six
rows sit in their window; five are held below it by a ceiling and say so:
`jacobi_hpx` by the mirror's own 32-bit cell range, `sheneos_hpx` by a boundary in
the ARTS time at this width (cubic on either side of it, four times higher above: a
data block above half the registered pool's base slab takes the pool's per-allocation
direct path, and the row's reply block crosses that line at `n = 81`),
`stencil1d_hpx` by the HPX origin's resident set (twelve generations of partition
arrays at `nd = 10`), `transpose_hpx` by the registered pool's slab step, and
`network_storage_hpx` by the origin's 4096 MB store. CPU budget: ARTS computes on
108 of 112 threads and HPX on all 112, a linear bound of 1.037 on `r`, reported as a
band and not corrected for.

| row | T (s) | ARTS INV/WB (min–max) | VAL/WB | EXCL/RETAIN | HPX (min–max) | peak RSS GB ARTS / HPX | r = HPX / ARTS INV/WB (min–max) | class |
|---|---|---|---|---|---|---|---|---|
| `stencil1d_hpx` | 100 | 22.0 (21.8–22.0) | 22.2 | 21.9 | 19.4 (17.2–20.3) | 175.7 / 96.2 | 0.89 (0.78–0.92) | **parity** |
| `fib_hpx` | 100 | 92.5 (92.5–92.5) | 92.5 | 92.5 | 108.2 (107.0–109.2) | 6.1 / 0.2 | 1.17 (1.16–1.18) | **parity** |
| `network_storage_hpx` | 100 | 7.0 (6.8–7.1) | 6.7 | 7.2 | 4.8 (4.8–4.8) | 15.1 / 4.5 | 0.69 (0.68–0.71) | **recorded** |
| `fft_hpx` | 100 | 40.5 (39.8–55.2) | 59.1 | 41.5 | 67.2 (66.9–67.7) | 171.7 / 166.1 | 1.67 (1.22–1.68) | **recorded** |

Rows outside parity at the anchor, and what each one is:

- `fft_hpx` (1.67, recorded): every ARTS arm carries one slow repeat at this size —
  INV/WB 39.8 / 40.5 / 55.2 s, VAL 36.7 / 59.1 / 74.3 s, EXCL 37.2 / 41.5 / 67.0 s at
  172 GB resident, the near-ceiling regime the sizing found above 60 480 — against HPX's
  66.9–67.7 s; the ratio is reported on the median.
- `network_storage_hpx` (0.69, recorded): the toy's one-node ratio matches the trend's
  1n column (0.69 at small arguments); the cause is the scheduler's locality named
  above, a runtime property disclosed, not sized around.
- `stencil1d_hpx` (0.89) and `fib_hpx` (1.17) are inside the parity band.

The seven rows outside the comparison keep the anchors of their own sections, measured
before the runtime's data-block descriptor lost its unused inline payload
(`logs/exp/20260916-104712` for `pi_hpx` and `nbody_hpx`, `logs/exp/20260915-054055`
and `-225942` for the rest): their times stand, their ARTS resident sets are upper
bounds on today's, and none of them enters a comparison table.

D14 (`present/D14.md`): from the trend's curve and these anchors, the worst cell of the
1..32-node sweep is `network_storage_hpx` at 2 nodes, ~48 s projected (58 s with the
1.2 margin) against Dane's 1800 s cell timeout — the store's cross-node traffic peaks
there and eases with more nodes; every other row's worst cell is its one-node one.
Every row fits with the 1.2 margin.

The two halves together: where a row scales on both runtimes it is at parity or
within the recorded band at every geometry of the trend and at the anchor; where HPX
is slower by more than the band the cause is a property of its runtime that the
mirror does not share (thread-per-task frontiers, collectives routed through one
locality, an allocator that never releases) or of the compiler, and where ARTS is
slower the cause is on its side and named (the scheduler's locality in
`network_storage_hpx` at one node; the registered pool's population of a footprint at
first touch in `stencil1d_hpx` at small partitions, and the ladder it maps over the
live data — 1.3–1.6× at the anchor — which is what bounds that row's size). The calibration record — the laws, every probe, the
refit, the dispositions — is kept with the experiment logs, and each row's sizing
sentence is in its appdoc.

## fib_hpx

Origin: HPX's `fibonacci_futures_distributed` quickstart example
(`third_party/hpx/examples/quickstart/fibonacci_futures_distributed.cpp`,
pin `v1.11.0`). Edits (from `ORIGIN.md`): the `[E2E]`/`[HPX]` markers
around the timed `n-runs` loop and the `[PARCELS]` line after it, and the
runtime cfg lines without `run_hpx_main` (the program's `hpx_main` is
locality 0's driver by design).

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::async(fib, loc1, n-1)` | none — fire-and-forget spawn | an EDT wired to the sum's slot 0, always created regardless of locality |
| `fib(loc2, n-2)`, a synchronous action call | mid wait when `loc2` is remote; none when `loc2` is `here` | remote: an EDT wired to the sum's slot 1 (the remainder becomes the dependence); local: the recursion continues inline in the same EDT |
| `hpx::when_all(f, r).then(when_all_wrapper())` | end wait — join of the two children | a two-slot `sum` EDT, created before either child |
| `fibonacci_future(n).get()` in `hpx_main`'s `n-runs` loop | end wait — driver root wait | one EDT with a single RO dependence on the run's root value |

Mirror mapping: one EDT per recursive call, an 8-byte DB carrying each
delivered value, a two-slot `sum` EDT standing in for
`when_all().then()`, and the origin's per-locality round-robin counter and
serial-execution counter mirrored as a per-rank `locality_t` RW DB — not
process memory — homed at each rank (`benchmarks/apps/hpx_origin/fib_hpx.c`).

Gate arguments: `--n-value=30 --threshold=12 --distribute-at=25 --n-runs=1 --test=1`.
Calibrated arguments: `--n-value=62 --threshold=44 --distribute-at=57 --n-runs=1 --test=1` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 120 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/fib_hpx_hpx --hpx:threads=4 \
  --n-value=30 --threshold=12 --distribute-at=25 --n-runs=1 --test=1
```

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

## stencil1d_hpx

Origin: HPX's `1d_stencil_8` example
(`third_party/hpx/examples/1d_stencil/`, pin `v1.11.0`). Edits: *build* —
the program registers its own component module, whose factory and registry
plugin lists its two component registrations refer to (the app project gives
each executable its own `HPX_COMPONENT_NAME`, so the runtime's default
module is not the one they name); *cfg* — the run-everywhere cfg vector
becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line;
*markers* — `[HPX]`/`[E2E]` around `do_all_work`'s body, with `[PARCELS]`
after the end stamp; and *scalar* — the gather loop keeps the partition data
it already pulls and prints `CHECKSUM %.14g`, the sum of every final
partition element, by one write.

| HPX wait site | classification | mirror |
|---|---|---|
| `heat_part`'s `dataflow` over `middle_data` and the two `get_data` futures | start wait — every input | the step task's three point dependences |
| `receive_left(t)` / `receive_right(t)` | start wait — a future handed to `dataflow`, never waited on | the two boundary points, the only ones that cross a rank |
| `sem->wait(t)` | flow control — a creation-depth limit, not a dependence | the spawner chain: signal from a rank's first partition, waited on `nd` generations later |
| `overall_result.get()` and the `get_data` gather loop on locality 0 | end wait — the result in hand | the rank gather tasks feeding the checksum task |

Mirror mapping (`benchmarks/apps/hpx_origin/stencil1d_hpx.c`): one block per
partition per generation homed at `i / (np/nl)`, one task per partition per
generation hinted there, reading its own partition and the two neighbouring
edge elements and producing the next generation's partition and its two
edges. Every block a step task or a driver produces travels on a labeled
STICKY point from one reserved range — a data-block
dependence in OCR is satisfied when it is added and so carries no ordering,
and the point is both the ordering edge and the name service. The rule
states its own exception, and exactly one block is in it: the rank share the
gather hands to the checksum task is written and released *before* its edge
is created, so that edge is ordered by construction and needs no point. The
point's index names its *consumer's* rank, which decides the home only where
the runtime homes a labeled range by index — ARTS and ocr-vx do (`index %
nranks`), while xsocr homes a whole reserved range at the PD that reserved it,
so there every satisfy is a message to that PD and a forward. Where the index
decides, a within-rank publish is no message at all; a crossing one is four
(the remote opener's labeled create, the satisfy, the consumer's RO acquire of
a block homed at the producer, and the consumer's destroy) against the
origin's three parcels on the same edge — one more per crossing edge, for one structure at both
distances rather than a separate local path. A fourth point kind carries the
semaphore: a rank's first partition signals its generation done and the
spawner that creates generation `t + nd` waits on it.

The digest is the plain sum of the final state — the origin computes no
printable quantity of its own. It is *conserved* by the periodic update for
any coefficient, hence invariant under any permutation of the state: a
swapped neighbour or a reversed ring would print the same number, so the
entries agreeing shows that they agree and not that the ring is oriented as
the origin orients it, which reading the mirror against the origin
establishes. That limitation is the quantity's and is disclosed in the
appdoc rather than engineered around. It is exact in binary64 at the default
coefficients, so all six entries agree bit for bit — but the answer is a
function of the locality count, because the origin initialises partition `i`
to its index *within its own locality* (`partition(here, nx, double(i))`).
Hence no cross-geometry pin: the oracle is the entries agreeing at each
geometry. One caveat on reading the lines: `xsocr` replaces `printf` with
its own formatter, which converts a double to about a part in `1e15`, so its
two-locality line reads `4.19225599999999e+06` for a value whose bit pattern
is identical to every other entry's.

Gate arguments: `--nx=64 --nt=16 --np=64 --nd=10` (`CHECKSUM 8386560` at one
locality, `CHECKSUM 4192256` at two).
Calibrated arguments: `--nx=270000 --nt=45 --np=4096 --nd=10` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 180 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/stencil1d_hpx_hpx --hpx:threads=4 \
  --nx=64 --nt=16 --np=64 --nd=10
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

## network_storage_hpx

Origin: HPX's `network_storage` performance test
(`third_party/hpx/tests/performance/network/network_storage/`, pin
`v1.11.0`) — `network_storage.cpp` and the `simple_profiler.hpp` it
includes, which contributes no hunk. Edits: *cfg* — the run-everywhere cfg
vector becomes the runtime defaults, which carry the same `hpx.run_hpx_main`
line; *markers* — `[HPX]` from every locality through a startup function,
which every locality runs before `hpx_main` starts on any of them, so the
geometry lines precede the test's own output (it writes its lines in
pieces, and a launcher merging the streams can drop a marker line into the
middle of one), `[E2E]` from immediately after the storage allocation (an
uninitialised `new char[]`) to the return of the read test on locality 0
(its closing barrier and its statistics output), with `[PARCELS]` after the
end stamp; *scalar* — two per-locality atomics count the transfers the existing
continuations complete (the write continuation that passes on
`CopyToStorage`'s `TEST_SUCCESS`, the read continuation that returns
`TEST_SUCCESS` once the bytes are copied), an `all_reduce` after the end
stamp sums them, and locality 0 prints `TRANSFERS_OK <writes> <reads>` by
one write; and *bugfix* — the pointer allocator's `deallocate` checks
`HPX_TEST_EQ(p == pointer_ && n, size_)`, which compares the `bool`
`p == pointer_ && n` with the transfer size in bytes and so can never pass
(it failed and printed for every read that crossed a locality); the line is
commented out. The warm-up pass runs through the same write continuation, so
it is counted. The origin's own `high_resolution_timer`s time each test
separately — the warm-up's time is printed too; only its CSV record and its
profile table are suppressed — and the marker window holds all three tests,
because the mirror performs the warm-up too. The program's build
file names the runtime's iostreams component, which `add_hpx_origin_app`'s
`LINK` list names in turn.

Three facts of this version govern the row. `--semaphore` is dead:
`USE_CLEANING_THREAD` and `USE_PARCELPORT_THREAD` are commented out, so the
sliding semaphore and the two helper threads are compiled out and the option
is parsed and never read, on either side. Nothing is verified: the two copy
functions return `TEST_SUCCESS` unconditionally, so the scalar counts
completed transfers. And the storage has concurrent writers by construction
(below). This program exposed the defect the build's local allocator patch
fixes — an HPX thread that suspends can resume on another worker, and HPX's
inlinable thread-local cache lookup, reached across that suspension, left a
continuation cache and its destructor with the previous worker — as "The
configuration every table was measured on" above describes.

| HPX wait site | classification | mirror |
|---|---|---|
| `distributed::barrier::synchronize()` at the start and the end of each test | driver wait — collective, six per locality | one barrier task per call on rank 0, entered and left through one point per rank |
| `when_all(final_list).then(reduce)` and `result.get()`, once per pass | driver wait — the pass's join, per locality | a latch per rank per pass counting its transfers' output events, gating the next turn |
| the write continuation's `fut.get()` | start wait — a ready future inside its continuation | the put task's output event |
| `transfer_data`'s `f.get()` and the counting continuation | start wait — the reply, already arrived | the get task's read-only dependence on the destination's slot |
| the tally `all_reduce(...).get()` | teardown collective, after the end stamp | a tally task per rank feeding the final task on rank 0 |

Mid waits: 0.

Mirror mapping (`benchmarks/apps/hpx_origin/network_storage_hpx.c`): the
storage cut at the transfer unit — one block of `transferKB` KiB per slot,
homed at its rank — since every transfer reads or writes exactly one slot
and an OCR dependence is taken on a whole block; one put task per
`CopyToStorage`, hinted at the destination rank, reading the sender's slot
`i` (the origin sends `&local_storage[i * transfer]` by reference) and
writing the destination's slot at the drawn offset; two tasks per
`CopyFromStorage` — a get task at the destination rank that copies its slot
into a fresh reply block, and a land task at the asker that copies that
reply into the asker's own slot at the same offset and destroys it, exactly
as the origin's pointer allocator lands the reply through its own second
copy; one turn task per rank per pass, drawing the
pass's destinations and offsets from the rank's `mt19937` at the origin's
default seed in the origin's order; a latch per pass, the origin's
`when_all`; and a barrier task on rank 0 per `synchronize()`, with a relay
task per rank between the two barriers that separate consecutive tests. A put
whose source and destination slot name the same block — a put from slot `i`
onto the same rank's offset `i` — copies inside the one RW acquisition of
that block it already holds, one buffer and one `memmove`, rather than
adding the source a second time in a different mode: exactly the origin's
`std::copy(src, src+len, dest)` with `src == dest`. A get never aliases this
way: its destination-rank read and its requester-rank write are two separate
tasks acquiring two separate blocks even when the asker is also the
destination — a legitimate concurrent access to two different blocks, as in
the origin. A pass over zero slots is an
empty join (a latch of one and one explicit decrement), so a storage smaller
than one transfer runs on both sides and prints `TRANSFERS_OK 0 0`.

The origin addresses another locality's storage by offset, which an OCR
program cannot: each driver publishes its slot names in a table on one
labeled STICKY point per rank, and each rank's first turn copies every table
into its own state block, which then rides from turn to turn, released
before each handover, together with the random stream the origin's driver
keeps across its three tests. That exchange has no counterpart in the origin
and sits inside the ARTS stamp only.

**Nothing orders two transfers that land on one slot.** Two puts can draw
the same destination slot, two gets on one rank the same offset, and a
transfer can read a slot another is writing. The origin's storage array has
no lock, and the mirror adds no chain of write turns: a chain would need a
cross-rank sequencer the origin does not have, a bandwidth test whose
storage is deliberately unsynchronised would stop being the same program,
and the counts do not read the racing bytes. This row's *payload* is
therefore not write-race-free at block granularity: under
`ARTS_MEMORY_MODEL=DB_WRF` its bytes are undefined while its counts are
not, and the `wrf_val_wt` check asserts the counts only. It is also a row
in which a slot is read from another rank while its home writes it, so each
coherence protocol's treatment of a reader under a writer is part of what
the row exercises; none of the OCR runtimes can wedge on the crossing pairs
it makes, because each takes a task's blocks that can wait on another
rank's release in GUID order.

The end is collective, as the origin's is: rank 0's final task waits on
every rank's tally, each behind the barrier that closes the read test, which
no rank enters before its last pass has joined. The counts are what the
program performs — with `--all-to-all`, `nranks · (iterations + 1) · slots`
puts and `nranks · iterations · slots` gets, `slots = localMB MiB /
transferKB KiB`. The row runs the origin's own `--globalMB=G`, which gives
each locality `G/nranks` MB, so the totals are `(iterations + 1) · G · 1024 /
transferKB` puts and `iterations · G · 1024 / transferKB` gets at every node
count, provided `--all-to-all` is true, the node count divides `G`, and
`transferKB` divides `(G/nranks) · 1024`. Each mirror task counts itself where it runs (a put at its
destination, a get at its asker) where the origin counts both at the
requester; only the sums are printed. The two sides' destination and offset
sequences differ — the mirror reduces the stream with `%`, the origin
through `uniform_int_distribution`, whose algorithm is
implementation-defined — and the counts do not depend on them.

Gate arguments: `--globalMB=16 --transferKB=64 --iterations=2 --semaphore=16
--all-to-all=true --no-local=false --distribution=1` — 16 MB in total,
`TRANSFERS_OK 768 512` at one, two, four and eight localities, pinned on the
put count.
Calibrated arguments: `--globalMB=4096 --transferKB=64 --iterations=5 --semaphore=16 --all-to-all=true --no-local=false --distribution=1` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/network_storage_hpx_hpx --hpx:threads=4 \
  --globalMB=16 --transferKB=64 --iterations=2 --semaphore=16 \
  --all-to-all=true --no-local=false --distribution=1
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

## fft_hpx

Origin: the hpx-fft benchmark's `fft_hpx_loop` (`third_party/hpx-fft/src/`,
DaRUS doi:10.18419/darus-4520, BSL-1.0) — the third origin outside the
vendored HPX tree, reached through `ORIGIN_ROOT`, and the first program here
that needs a numerical library (the vendored FFTW, named in its `LINK` list).
The dataset's download endpoint builds its archive on request, so the archive
hash is not reproducible and `PROVENANCE.md` pins the two files by SHA256
instead. Edits: *cfg* — the run-everywhere vector becomes the runtime
defaults; *markers* — `[HPX]` once the locality count is known, `[E2E]` from
just before the array is allocated and filled (so the fill, the plan build
and both phases are inside) to immediately after the program's own end stamp,
`[PARCELS]` after that; *scalar* — the program computes no printable quantity
of its own, so each locality sums its rows and one `all_reduce` makes the
total, written once as `CHECKSUM %.14g` after the end stamp; and one
*bugfix*.

**The bugfix.** `basenames_` held `const char*` and was filled with
`std::move(std::to_string(i).c_str())` — a pointer into a temporary that dies
at the end of that statement, read by `create_communicator` on the next one,
for every one of the program's `num_localities` scatter communicators. The
names are held in a `std::vector<std::string>` instead and the two call sites
pass `.c_str()`: local, structure-preserving, and it changes no communicator
and no collective. It is made unconditionally, not because it misbehaved —
the unmodified copy built here ran to completion and exited 0 at one, two,
four and eight localities, which is recorded as evidence and not as the
trigger. A dangling read is undefined however benign one toolchain's output
looks.

Two facts about the arguments, both as written. **`--nx` and `--ny` are the
dimensions, not their logarithms** (the paper's own script passes
`--nx=16384 --ny=16384`). And the pair must divide the geometry: the origin's
`n_x_local = nx/nl` and `n_y_local = (ny/2 + 1)/nl` truncate, so a
non-dividing pair silently transforms a smaller array — the mirror rejects
that as usage instead, the one place it is stricter.

| HPX wait site | classification | mirror |
|---|---|---|
| each `for_loop(par, …)` phase | driver wait — a phase barrier | that phase's per-rank join |
| `communication_futures_[i].get()`, generation 1 | driver wait — the exchange (O4) | each transpose task's `nl` point dependences |
| `communication_futures_[i].get()`, generation 2 | driver wait — the exchange (O4) | each final transpose's `nl` point dependences |
| `all_reduce(…).get()` (the added tally) | end — collective, every locality present | the sum task's `nl` rank shares |

Mid waits: 0; the two O4 waits are the origin's own driver loop gathering the
collectives between phases.

Mirror mapping (`benchmarks/apps/hpx_origin/fft_hpx.c`): two blocks per rank —
its local rows and its local transposed rows, one object each, exactly as the
origin holds them — one block per (phase, source, destination) chunk homed at
its source, one task per row per transform, one task per row per split
(writing a disjoint slice into every destination's chunk, exactly as the
origin's row-parallel `split_vec`/`split_trans_vec`), and one task per
(source, row) per transpose. These are the origin's own parallel units,
carried unchanged: many split tasks hold one phase's chunk blocks
`DB_MODE_RW` at once and many transpose tasks hold the phase's destination
buffer `RW` at once, all writing disjoint bytes. `ARTS_MEMORY_MODEL=DB_WRF` is
therefore undefined for this row by design — the same-DB write-write
conflicts the code does not event-order are exactly what that model declares
undefined — and no coherence-plane cell runs it. The copies, the bytes and the
one message per (source, destination) are unchanged.

Each `scatter_to` is `nl` labeled STICKY points from the program's one
reserved range, indexed `(phase·nl + source)·nl + destination` — `2·nl²`
names, `2·nl(nl−1)` of them crossing a rank. Those counts hold where a
labeled range is homed by index, which is what ARTS and ocr-vx do
(`index % nranks`); xsocr homes a whole reserved range at the PD that
reserved it, so there every point lives at rank 0 and no publish is free.
A chunk has several readers — every transpose task of the destination reads
all `nl` arrivals — so a per-phase reap task depending on the same points
destroys the blocks and the points once the phase's join has fired. The end
is the origin's: the sum task is created before the fork with one slot per
rank, so shutdown sits behind every rank's last work.

The mirror plans FFTW with the origin's own flag only — it adds no
`FFTW_UNALIGNED`. What resolves the new-array execute contract's alignment
requirement (the execute buffer must carry the plan buffer's alignment, and a
block's payload address is not fixed when the plan is built) is the plan
library itself: the two plans are built once per rank into a retained,
serialized image — an LLVM translation of the same FFTW source, opened by
each row's task through pointer arithmetic — whose every alignment predicate
is rewritten to test an address's offset relative to its own allocation
rather than its absolute value, at the mask the configured FFTW build
actually uses (16 bytes for the vendored double-precision SSE2/AVX/AVX2/FMA
build), so plan selection and codelet applicability stay address-independent
for every row. Its effect on the answer is under `1e-16` relative; the
remaining cross-runtime disagreement is the same FFTW source compiled
twice — GCC for the HPX side, clang-14 for the mirror's translated
library — leaving its own mark in the last bits of rounding and FMA
contraction.

**The checksum is a function of the locality count**, like `stencil1d_hpx`'s,
and for the origin's own reason — though not the obvious one. The first
transpose's `nl·j + i` column ordering cannot move this answer: every row
starts as the same ramp, so the vector the second transform sees is constant
in x at every `nl`, and permuting a constant vector changes nothing. What
moves it is the second transpose reading its chunk with the first phase's
stride (below), both strides following `nl`. At the gate arguments the HPX entry
prints `28434214.354769`, `26606318.203792`, `24780157.170575` and
`22952333.813996` at one, two, four and eight, and every OCR entry prints the
same values as `%.14e` (`2.84342143547686e+07` and so on). The four agree to
the last printed digit everywhere but one place: xsocr renders the four-node
value `...48e+07` where the others render `...49e+07`, its own printf
replacement's double conversion being good to about a part in `1e15`. There is no cross-geometry pin; the oracle is the
entries agreeing at each geometry.

One kernel is doubtful and kept: `transpose_x_to_y` reads its chunk with the
phase-1 input stride `dim_c_y_part` where the chunk was cut with
`dim_c_x_part`. For the shapes this row runs every element of the result is
still written exactly once and no index leaves the array — which a mirror-side
usage check enforces rather than assumes: the arithmetic stays in bounds
exactly when `ny/2 + 1 <= nx` or `ny/2 + 1` equals the rank count, so a tall,
narrow pair such as `--nx=128 --ny=510` is refused instead of overreading the
chunk. It is a defined deterministic answer; whether it is the transpose the
authors meant is not the mirror's question, and both sides run it unchanged.

Gate arguments: `--nx=256 --ny=254 --plan=estimate --run=scatter`, one list at
every geometry — 256 and `254/2 + 1 = 128` divide every rung.
Calibrated arguments: `--nx=72000 --ny=71998 --plan=estimate --run=scatter` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && mkdir -p result && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 300 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/fft_hpx_hpx --hpx:threads=4 \
  --nx=256 --ny=254 --plan=estimate --run=scatter
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

## The retired ports

The six ports that mirrored an ARTS-native OCR application in HPX
(`nqueens`, `p2p`, `smithwaterman`, `Stencil2D_intel_channelEVTs`,
`tempest`, `triangle`) are archived, with their measured facts and
fair-geometry tables, under `archive/hpx-ports-ocr-origin/`.
