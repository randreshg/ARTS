# fib_hpx

*Distributed Fibonacci over HPX futures and plain actions — one EDT per
spawned call, an output dependence carrying each 8-byte result.*
Origin: HPX's `fibonacci_futures_distributed` quickstart example
(`third_party/hpx/examples/quickstart/fibonacci_futures_distributed.cpp`,
pin `v1.11.0`), copied to
`benchmarks/hpx/fib_hpx/fibonacci_futures_distributed.cpp` and built as
`fib_hpx_hpx`.

## Overview

Computes `fib(n)` by binary recursion over HPX plain actions and futures:
every `fibonacci_future(n)` with `n >= threshold` spawns an async call for
its `n-1` branch and a synchronous call for its `n-2` branch, then joins
both with `hpx::when_all(f, r).then(when_all_wrapper())`; below `threshold`
the call falls back to a serial C++ recursion, and `n < 2` returns
immediately. This is an HPX-origin catalog row: one program on four
runtimes — `fib_hpx_hpx` is the HPX program, `fib_hpx_arts_<variant>`,
`fib_hpx_xsocr` and `fib_hpx_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/fib_hpx.c`), one row in `hpx_apps.yaml`. HPX
primitives used: a plain action (`fibonacci_future`), `hpx::async`, a
synchronous action call, `hpx::when_all().then()`, `hpx_main`. The mirror's
mapping: one EDT per call the origin spawns — the `n-1` branch always, the
`n-2` branch only where the origin's synchronous action call leaves the
caller's locality; a local `n-2` recurses inline in the caller's task, as it
does in the origin — delivering its 8-byte result on an output dependence,
with a two-slot `sum` EDT standing in for `when_all().then()`.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--n-value` | `n`, the Fibonacci argument | 10 | reachable — parsed in `mainEdt`/`hpx_main` on every side |
| `--threshold` | value below which a call runs the serial C++ recursion instead of spawning | 2 | reachable |
| `--distribute-at` | the recursion level whose children are routed onto other localities | 2 | reachable |
| `--n-runs` | number of timed repetitions of the whole computation | 1 | reachable |
| `--loc-repeat` | how many times a locality other than the caller's repeats in the round-robin list | 1 | reachable — a non-positive value clamps to `0` (no remote repetitions beyond the caller); the origin instead assigns it to an unsigned `size_t`, so a negative value there builds a round-robin list of about 2^64 entries, and the two sides agree only at exactly `0` |
| `--test` | which test(s) to run: `0` = serial reference only (the mirror runs it and shuts down without the DAG), `1` = the future-based DAG only, `all` = both, serial reference first | `all` | reachable — the mirror validates and accepts the same three spellings `0`/`1`/`all`; the gate uses `--test=1` |

A malformed or out-of-range argument prints usage and calls `ocrShutdown()`
with status 0, where the origin's option parsing exits non-zero; the missing
result marker fails the cell either way.

## Structure

Let `T` = `threshold`. A call `fibonacci_future(n)` is *internal* when
`n >= T` (it recurses); every other call (including `n < 2`) terminates by
delivering a value directly. Per run, with `I` the internal-call count:

| object | count |
|---|---|
| `sum_edt` | `I` — one per internal call |
| `fib_edt`, the `n-1` branch | `I` — `spawn_fib` always creates this task, local or remote |
| `fib_edt`, the `n-2` branch | at most a handful per run — only when the branch is routed off the caller's locality at a `distribute_at` boundary; otherwise the call recurses inline in the same EDT |
| 8-byte DBs | `2I + 1` — one per leaf delivery (`I + 1`: a full binary tree cut at `threshold` has one more leaf than internal node) and one per `sum_edt` (`I`) |
| `run_edt` | `--n-runs` — one per repetition |

## Wiring

Reply-by-dependence into a fixed two-slot `sum` EDT stands in for
`when_all().then()`: `spawn_fib` always creates the `n-1` branch's task
wired to `sum`'s slot 0 (`DB_MODE_RO`); the `n-2` branch either recurses
inline on the caller's rank or is spawned and wired to slot 1. Either way
`sum_edt` fires once both slots are satisfied and delivers the sum on the
parent's own slot — the join needs no latch, since its arity is always
exactly 2. `run_edt` closes each repetition with a single dependence on the
tree's root value. A value block has exactly one consumer and is destroyed
there (`sum_edt` destroys the two it summed, `run_edt` the root), so the
live block count follows the frontier rather than the whole tree and
`--n-runs` does not multiply it.

## Flow

`mainEdt` validates the arguments, builds the six EDT templates (fib, sum,
run, start, query, count), creates the per-rank `locality_t` state DBs,
appends their GUIDs to every task's parameter vector — the origin's tasks
reach their locality's state as a global, with nothing to acquire, so the
mirror carries the names in the parameters rather than in a block every task
would depend on — and creates `start_edt` on rank 0. `start_edt`
runs the untimed serial reference when `--test` is `0` or `all` (shutting
down immediately for `--test=0`, without starting the DAG), then starts run
0: the round-robin counter (`next_locality`) resets and the root call
unfolds top-down (spawn) while `sum` joins fold bottom-up. `run_edt` either
starts the next repetition or, after the last run, prints the result line
and starts the per-rank serial-count gather that ends the program.

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::async(fib, loc1, n-1)` | none — fire-and-forget spawn | `spawn_fib`: an EDT wired to `sum` slot 0, always created regardless of locality |
| `fib(loc2, n-2)`, a synchronous action call | a mid wait when `loc2` is remote (build the call's arguments, then block for the reply); no wait when `loc2` is `here` (a plain recursive call) | remote: `spawn_fib` wired to `sum` slot 1 — the remainder (using the reply) becomes the dependence; local: `fibonacci_future` recurses inline in the same EDT |
| `hpx::when_all(f, r).then(when_all_wrapper())` | end wait — join of the two children | `sum_edt`, created before either child, fires on its two RO dependences |
| `fibonacci_future(n).get()` in `hpx_main`'s `n-runs` loop | end wait — driver root wait | `run_edt`, one RO dependence on the run's root value |

The measurement is `[E2E]` on both sides, stamped on rank/locality 0 alone:
the whole application, from its first statement to the point it asks the
runtime to stop, runtime start-up and teardown excluded. On the OCR side the
runtime stamps it (the main task becoming eligible, shutdown recognised on
rank 0); on the HPX side `run_clock` opens as the first statement of
`hpx_main` and `print_e2e` closes it immediately before `hpx::finalize()`.
The serial reference when `--test` asks for it, the timed loop, the result
line and the serial-count report are inside it on both sides. Option
validation and the per-locality setup are inside it on the OCR side only —
the origin does both in a startup function every locality runs before
`hpx_main` — which is the six templates and `nl` state blocks of one run.

**What the span holds at the calibrated arguments is the serial kernel.** A
run is about 21 900 tasks against about `1.3·10¹³` calls of the serial
recursion below the threshold, so the runtime's share of the span is under a
percent on either side. Left to each side's own build, the row's ratio
would be, to first order, how two compilations of one four-line function
compare — the origin's as C++, the mirror's as C, same statements, same
flags — and interleaved timings of that function alone differ by up to a
tenth between equally valid compilations of it. So the function is one
translation unit, `fib_serial_kernel.c`, compiled once and linked by all
four programs (the origin's *kernel* edit declares it `extern "C"` and
drops its own definition): the span's bulk is the same machine code on
every entry, starting on a 64-byte boundary in every program — the same
instructions placed differently against that boundary by four linkers
measured a tenth apart on their own — and what is left to differ is the
runtimes. And at
one rank every routed index resolves to the caller's own locality, so the
one-node cell spawns nothing remotely: the row's distributed content starts
at two ranks.

`run_edt`'s result line now also carries the round-robin counter's final
value, `fibonacci_future(n) == r,next_locality,<value>`. The mirror then keeps the origin's `serial_execution_count` — the
per-locality atomic incremented on every call below the threshold — and
gathers it: one query EDT per rank, in ascending rank order, each returning
its counter, printed as `serial-count,<rank>,<count/runs>`, exactly as the
origin's own per-locality report after its timed loop. The gather is part of
the program on both sides, and so inside the measured span on both.

**On ocr-vx the query's event is kept.** The one event per serial-count
query is a COUNTED output event with its one consumer stated, reclaimed after
it on ARTS and xsocr; ocr-vx does not implement COUNTED and keeps it to
teardown — `nl` events per run, a property of that reference
(`benchmarks/hpx/README.md`, "One row, four runtimes").

## Placement

The origin builds a per-locality `localities` list — itself (`here`) at
index 0, then every other locality repeated `loc-repeat` times — and routes
both children of a `distribute_at`-level call to the next unused index of a
per-locality round-robin counter (`next_locality`); every call below that
level stays on the caller's locality. The mirror carries the identical
index arithmetic: `rank_of_index` reproduces `get_next_locality`'s
modulo-into-the-repeated-list computation from a per-rank `next_locality`
counter — the origin's per-locality state (`next_locality` and
`serial_execution_count`) is a `locality_t` RW DB homed at each rank, not
process memory, so the mirror runs the same atomics the origin runs against
its own locality's fields, against that rank's DB under an ordinary
`DB_MODE_RW` acquisition — and every EDT create carries
`OCR_HINT_EDT_AFFINITY` for the routed rank via
`ocrAffinityGetAt`.

Each value block carries an explicit home hint: the rank that wrote it,
which is where the origin's future holds its value before the consumer
reads it. The placement is therefore stated by the program rather than
inherited from the build's no-hint DB policy (`ARTS_NOHINT_DB_HOME` /
`ARTS_SHIM_NOHINT_DB_HOME`), which under the default build resolves to the
same rank.

## Sizing

The width knob is `n-value`/`threshold` together — the object counts in
Structure scale with the internal-call count, not with `n` alone. The
`control-gate` experiment runs the small consensus arguments
(`--n-value=30 --threshold=12 --distribute-at=25 --n-runs=1 --test=1`),
which finish in well under a second and are never reported as a timing
number. The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--n-value=62 --threshold=44 --distribute-at=57
--n-runs=1 --test=1` at every node count. `n − threshold = 18` holds the task
count at the gate's relation (about 21 900 tasks), `distribute-at = n − 5`;
`n` is the size knob, the serial work below the threshold growing as φⁿ.
Derived to the 100 s window from the probe through that law; the anchor
measured 92.5 s on every ARTS arm and 108 s on HPX (107–109), 6.1 GB resident on
ARTS.
