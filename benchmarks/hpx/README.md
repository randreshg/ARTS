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
  existing constant as a CLI argument, API modernisation of a program
  written against an older HPX, and a *bugfix* — a defect of the origin that
  would otherwise fail or distort every run, named with its evidence in the
  program's `ORIGIN.md`. Not allowed: changing the algorithm, the
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
driver onto every locality would run it once per locality. `fib_hpx` is of
that shape; the other three rows take all six:

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
unchanged; `fib_hpx`, the row that allocates the most continuations,
moved by at most the run-to-run spread. The ARTS
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
an external project does not inherit the outer tree's options. The vendored
FFTW is the one library held below that: both of its builds — the HPX side's
archive and the mirror's pinned translation — are compiled at the level the
pinned artifact's stamp names. What the
toolchain then does with identical arithmetic is taken as it comes and
disclosed per row where it shows, never patched (the retired `pi_hpx` row,
whose one integration loop the two toolchains compiled to a 2.2× per-iteration
difference, is the worked example under `archive/hpx-origin-excluded/`).

### The `[E2E]` span

Every row defines the span the same way, and it is the span every runtime
of the comparison reports: the whole application on locality 0, from the
first statement of its `hpx_main` to immediately before that locality's
`hpx::finalize()`. Runtime start-up (everything `hpx::init` does before
`hpx_main` runs) and teardown lie outside it; option handling, allocation,
the program's work, its result and its reports lie inside it. That is the
boundary the runtimes it is compared against use — the ARTS stamp opens once
initialization is complete, as the main task becomes eligible, and closes
when rank 0 recognises shutdown, before teardown; the two reference OCR
runtimes stamp the same two events on their rank 0 — so one definition holds
on all four, measured on one node's clock.

Where every locality runs `hpx_main`, any of them may be the first to call
`hpx::finalize()`; the stamp does not follow that call. It is locality 0's
own, taken on locality 0's own path, and each such row puts a step every
locality takes part in (a gather, a reduction) ahead of it, so locality 0's
end is the application's.

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

## ARTS versus HPX

The section's comparison is of **one program on four runtimes** (the admission rules
above): the OCR mirror keeps the origin's operations, their order and its state, both
sides time the same span (`[E2E]`, the whole application on rank/locality 0), and neither side is tuned to
the other. The roster is the four rows whose origin never suspends a started
parallel task (its waits are continuations, or a bounded number of driver-thread
phase joins — the discipline an event-driven program has by construction, so the two
sides pay for the same thing; the bounded exception is `fib_hpx`'s synchronous call at
its distribution boundary, counted in the table below) and whose parallel width is set by the problem, not
fixed by the origin below the machine's (one task per locality, a fixed cell count):
`fib_hpx`, `stencil1d_hpx`, `network_storage_hpx`, `fft_hpx`. Seven more programs
went through the same admission, mirroring and calibration and were retired under
that criterion; their sections, sources and measured facts are kept under
`archive/hpx-origin-excluded/`. Two measurements make the comparison, and they answer
different questions.

### Idiom and width of every row

Idiom is drawn from each row's own disclosure below (the wait-site table and
"Mid waits" count); width is the formula `numeric_width`/`width_at` in
`logs/adhoc/2026-09-11-hpx-origin/calibration/size.py` read for that row.

| row | origin's parallel idiom | where the width comes from |
|---|---|---|
| `fib_hpx` | continuation (fire-and-forget `async`, a `when_all().then()` join); its one mid wait is the synchronous `n-2` call where the distribution boundary routes it off the caller's locality — at most 13 per run at the calibrated arguments (the eight calls at `distribute-at` and the five one above it), none at one node | `2·I` tasks, `I` the internal-call count below the serial threshold |
| `stencil1d_hpx` | continuation (`dataflow` over already-posted futures; 0 mid waits) | `np`, the partition total |
| `network_storage_hpx` | continuation (0 mid waits; the driver's own collectives bound each pass) | `globalMB·1024/transferKB` slots in flight per pass |
| `fft_hpx` | continuation (0 mid waits; two driver-level collective waits between phases) | `min(nx, ny/2+1)`, the row count of the narrower phase, which the origin's default chunking cuts into at most `4·W` tasks per locality — on both sides |

**Both tables below predate two changes to what they measure** and stand
only until the next campaign replaces them: the HPX `[E2E]` was then a
narrower in-program window where it is now the whole of `hpx_main`, and the
`fft_hpx` mirror then ran one task per row where it now runs the origin's
one task per loop chunk. A number recorded after either change is not
comparable with these.

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
feasibility the same way (a cell every runtime must fit) but never sets a time. Two
rows sit in their window; two are held below it by a ceiling and say so:
`stencil1d_hpx` by the resident set the registered pool's ladder maps over the
`nd + 2` generations of partition blocks both sides keep alive (the row's catalog
sizing note), and `network_storage_hpx` by the origin's 4096 MB store. CPU budget: ARTS computes on
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
  `fib_hpx`'s ratio is not a statement about either runtime: at the
  calibrated arguments the span is the serial recursion below the threshold
  (about `1.3·10¹³` calls against about 21 900 tasks), compiled as C++ on
  one side and as C on the other, and at one node nothing is spawned
  remotely.

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
pin `v1.11.0`). Edits (from `ORIGIN.md`): the `[HPX]` marker, the `[E2E]`
marker around the whole of `hpx_main` with the `[PARCELS]` line after it, and the
runtime cfg lines without `run_hpx_main` (the program's `hpx_main` is
locality 0's driver by design).

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::async(fib, loc1, n-1)` | none — fire-and-forget spawn | an EDT wired to the sum's slot 0, always created regardless of locality |
| `fib(loc2, n-2)`, a synchronous action call | mid wait when `loc2` is remote; none when `loc2` is `here` | remote: an EDT wired to the sum's slot 1 (the remainder becomes the dependence); local: the recursion continues inline in the same EDT |
| `hpx::when_all(f, r).then(when_all_wrapper())` | end wait — join of the two children | a two-slot `sum` EDT, created before either child |
| `fibonacci_future(n).get()` in `hpx_main`'s `n-runs` loop | end wait — driver root wait | one EDT with a single RO dependence on the run's root value |

Mirror mapping: one EDT per call the origin spawns (the `n-1` branch always,
the `n-2` branch where its synchronous call leaves the caller's locality — a
local `n-2` recurses inline, as in the origin), an 8-byte DB carrying each
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

## stencil1d_hpx

Origin: HPX's `1d_stencil_8` example
(`third_party/hpx/examples/1d_stencil/`, pin `v1.11.0`). Edits: *build* —
the program registers its own component module, whose factory and registry
plugin lists its two component registrations refer to (the app project gives
each executable its own `HPX_COMPONENT_NAME`, so the runtime's default
module is not the one they name); *cfg* — the run-everywhere cfg vector
becomes the runtime defaults, which carry the same `hpx.run_hpx_main` line;
*markers* — `[HPX]` at the top of `do_all_work`, `[E2E]` around the whole of
`hpx_main`, with `[PARCELS]` after the end stamp; and *scalar* — the gather loop folds each partition
it already pulls into a sum while it is in hand and prints `CHECKSUM %.14g`,
the sum of every final partition element, by one write.

| HPX wait site | classification | mirror |
|---|---|---|
| `heat_part`'s `dataflow` over `middle_data` and the two `get_data` futures | start wait — every input | the step task's three point dependences |
| `receive_left(t)` / `receive_right(t)` | start wait — a future handed to `dataflow`, never waited on | the two boundary points, the only ones that cross a rank |
| `sem->wait(t)` | flow control — a creation-depth limit, not a dependence | the spawner chain: signal from a rank's first partition, waited on `nd` generations later |
| `overall_result.get()` and the `get_data` gather loop on locality 0 | end wait — the result in hand | the rank gather tasks feeding the collect task's merge, then the serial read chain that folds the checksum |

Mirror mapping (`benchmarks/apps/hpx_origin/stencil1d_hpx.c`): one block per
partition per generation homed at `i / (np/nl)`, one task per partition per
generation hinted there, reading its own partition and the two neighbouring
edge elements and producing the next generation's partition and its two
edges. Every block a step task or a driver produces travels on a labeled
STICKY point from one reserved range — a data-block
dependence in OCR is satisfied when it is added and so carries no ordering,
and the point is both the ordering edge and the name service. The rule
states its own exception — a block written and released *before* its
consumer's edge is created is ordered by construction and needs no point —
and three kinds of block are in it, all at the program's end: the name table
a gather hands the collect task, the merged table the collect task hands the
read chain, and the final partitions the read chain takes by name. The
point's index names its *consumer's* rank, which decides the home only where
the runtime homes a labeled range by index — ARTS and ocr-vx do (`index %
nranks`), while xsocr homes a whole reserved range at the PD that reserved it,
so there every satisfy is a message to that PD and a forward. Where the index
decides, a within-rank publish is no message at all; a crossing edge is
seven messages — five on the value path (the remote opener's labeled create,
the satisfy, the consumer's RO acquire request and reply for a block homed at
the producer, and the consumer's destroy) and two for the reader's
acknowledgement, whose point is homed at the block owner's rank — against
the origin's three parcels on the same edge plus its handle's reference
counting, all header-sized but the one double: the price of one structure at
both distances rather than a separate local path. A fourth point kind carries the
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
coefficients and the gate's size — past that the agreement rests on the one
expression and the one summation order the two sides share — so all six entries agree bit for bit — but the answer is a
function of the locality count, because the origin initialises partition `i`
to its index *within its own locality* (`partition(here, nx, double(i))`).
Hence no cross-geometry pin: the oracle is the entries agreeing at each
geometry. One caveat on reading the lines: `xsocr` replaces `printf` with
its own formatter, which converts a double to about a part in `1e15`, so its
line can differ in the last printed digit for a value whose bit pattern is
identical to every other entry's.

Gate arguments: `--nx=64 --nt=16 --np=64 --nd=10` (`CHECKSUM 8386560` at one
locality and `4192256` at two on the HPX entry's `%.14g`; the OCR entries
print the same values as `%.14e`, `8.38656000000000e+06` and
`4.19225600000000e+06`).
Calibrated arguments: `--nx=270000 --nt=45 --np=4096 --nd=10` — the one-node anchor sizes under `## ARTS versus HPX`.

```bash
cd scratch && ARTS_E2E_MARKER=1 UCX_TLS=tcp,self UCX_NET_DEVICES=lo timeout -k 1 180 mpirun -n 2 -bind-to none \
  ../build_release/benchmarks/hpx/stencil1d_hpx_hpx --hpx:threads=4 \
  --nx=64 --nt=16 --np=64 --nd=10
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
middle of one), `[E2E]` around the whole of `hpx_main` on locality 0, the
only one that finalizes, with `[PARCELS]` after the end stamp; *scalar* — two per-locality atomics count the transfers the existing
continuations complete (the write continuation that passes on
`CopyToStorage`'s `TEST_SUCCESS`, the read continuation that returns
`TEST_SUCCESS` once the bytes are copied), an `all_reduce` after the read
test sums them, and locality 0 prints `TRANSFERS_OK <writes> <reads>` by
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
| `transfer_data`'s `f.get()` and the counting continuation | start wait — the reply, already arrived | the landing task's read-only dependence on the get task's reply block |
| the tally `all_reduce(...).get()` | end collective, before the end stamp | a tally task per rank feeding the final task on rank 0 |

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
copy; one completion task per transfer at the requester — the origin's
continuation — which counts the transfer and records its result, a put's
result reaching it as a 4-byte block where the origin's action returns its
`TEST_SUCCESS` through the future; one turn task per rank per pass, drawing the
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
than one transfer runs on both sides and prints `TRANSFERS_OK 0 0`. A rank's
tally task destroys its slot blocks where the origin's
`delete_local_storage()` frees its array — before the end stamp on both
sides.

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
therefore outside DB-WRF at block granularity: the counts survive only
because every completion runs on the counters' home rank — an
implementation accident, not program ordering. It is also a row
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
`transferKB` divides `(G/nranks) · 1024`. The mirror counts each transfer
where the origin does — at the requester, in the transfer's completion task.
The two sides draw the same destination and offset sequence — the mirror's
generator is `mt19937` at the origin's default seed and its bounded draw is
libstdc++'s `uniform_int_distribution` algorithm (multiply-high with
rejection) — and the counts do not depend on it.

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

## fft_hpx

Origin: the hpx-fft benchmark's `fft_hpx_loop` (`third_party/hpx-fft/src/`,
DaRUS doi:10.18419/darus-4520, BSL-1.0) — the third origin outside the
vendored HPX tree, reached through `ORIGIN_ROOT`, and the first program here
that needs a numerical library (the vendored FFTW, named in its `LINK` list).
The dataset's download endpoint builds its archive on request, so the archive
hash is not reproducible and `PROVENANCE.md` pins the two files by SHA256
instead. Edits: *cfg* — the run-everywhere vector becomes the runtime
defaults; *markers* — `[HPX]` once the locality count is known, `[E2E]`
around the whole of `hpx_main` on locality 0, `[PARCELS]` after that; *scalar* — the program computes no printable quantity
of its own, so each locality sums its rows and one `all_reduce` makes the
total, written once as `CHECKSUM %.14g`; and one
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
its source, and one task per loop chunk of each of the origin's eight
`for_loop(par, …)` calls: the transforms, the splits (each row writing a
disjoint slice into every destination's chunk, exactly as the origin's
row-parallel `split_vec`/`split_trans_vec`) and both levels of the nested
transposes. The chunk is HPX's own default — the smallest power of two that
leaves at most four chunks per core, the whole range on one core
(`default_parameters::get_chunk_size`) — applied by each runtime to its own
worker count. These are the origin's own parallel units,
carried unchanged: many split tasks hold one phase's chunk blocks
`DB_MODE_RW` at once and many transpose tasks hold the phase's destination
buffer `RW` at once, all writing disjoint bytes. The row is therefore outside
DB-WRF by design — the same-DB write-write conflicts the code does not
event-order are exactly what that model requires to be ordered — and no
coherence-plane cell runs it. The copies, the bytes and the
one message per (source, destination) are unchanged.

Each `scatter_to` is `nl` labeled STICKY points from the program's one
reserved range, indexed `(phase·nl + source)·nl + destination` — `2·nl²`
names, `2·nl(nl−1)` of them crossing a rank. Those counts hold where a
labeled range is homed by index, which is what ARTS and ocr-vx do
(`index % nranks`); xsocr homes a whole reserved range at the PD that
reserved it, so there every point lives at rank 0 and no publish is free.
A chunk has several readers — every transpose task of the destination reads
all `nl` arrivals — so a reap task depending on the first exchange's points
destroys those blocks and points where the origin's second gather assignment
frees the first exchange's receive vectors. Of what the origin allocates,
nothing else is destroyed but the two plans: the origin leaks its arrays (`vector_2d` allocates with `new[]`
under a defaulted destructor) and holds the last arrivals past its end stamp,
so the mirror leaves the rows, the transposed rows and the second exchange's
blocks to the runtime's teardown (above one rank the origin's collective
does release the send buffers it serialised; the mirror's one block per
chunk is also the arrival, which is held, so it stays — the appdoc's
teardown paragraph). The end
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
twice at one `-march` level (the pinned artifact's, `x86-64-v3`, which the
HPX side's FFTW build reads from the artifact's stamp) — GCC for the HPX
side, clang-14 for the mirror's translated library, which the tree consumes
as a pinned assembly artifact rather than compiling on the host —
leaving its own mark in the last bits of rounding and FMA contraction.

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

## The retired rows and ports

Seven HPX-origin rows that went through admission, mirroring and calibration
but fail the comparison's criterion — the origin suspends started parallel
tasks (`random_mem_access_hpx`, `transpose_hpx`, `jacobi_hpx`,
`mini_ghost_hpx`, `sheneos_hpx`) or fixes its parallel width below the
machine's (`pi_hpx`, `nbody_hpx`) — are archived under
`archive/hpx-origin-excluded/`: each row's HPX program with its disclosure
and patch, its OCR mirror, its appdoc, its catalog and gate-roster rows, its
section of this README with the measured trend and anchor, and the build
registrations that carried it (the vendored HDF5 among them).

The six earlier ports that mirrored an ARTS-native OCR application in HPX
(`nqueens`, `p2p`, `smithwaterman`, `Stencil2D_intel_channelEVTs`,
`tempest`, `triangle`) are archived, with their measured facts and
fair-geometry tables, under `archive/hpx-ports-ocr-origin/`.
