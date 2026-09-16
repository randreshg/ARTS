# fft_hpx

*A distributed 2D real-to-complex FFT: rows transformed in y, cut into one
chunk per locality, exchanged, transposed, transformed in x, and sent back
the same way — two collectives around a transpose.*
Origin: the hpx-fft benchmark's `fft_hpx_loop`
(`third_party/hpx-fft/src/fft_hpx_loop.cpp`, DaRUS doi:10.18419/darus-4520,
BSL-1.0), copied to `benchmarks/hpx/fft_hpx/` and built as `fft_hpx_hpx`.

## Overview

A `dim_c_x × 2·dim_c_y` array of reals — `dim_c_x = nx`,
`dim_c_y = ny/2 + 1`, the complex half-spectrum's real layout — is
block-distributed by rows, `n_x_local = dim_c_x/nl` rows per locality, every
row initialised to the same ramp `0, 1, …, dim_r_y − 1` where
`dim_r_y = 2·dim_c_y − 2`. `fft_2d_r2c()` is six phases:

1. a 1D real-to-complex FFTW plan applied to each local row in place;
2. `split_vec` cuts each row into `nl` outgoing chunks, one per locality;
3. under `--run=scatter`, `nl` `scatter_to`/`scatter_from` collectives at
   generation 1 — one communicator per *source* locality, so each locality
   receives one chunk from each source;
4. `transpose_y_to_x` writes the arrivals into the locality's share of the
   transposed array, `n_y_local = dim_c_y/nl` rows of `2·dim_c_x`;
5. a 1D complex-to-complex FFT per transposed row;
6. `split_trans_vec`, a second exchange at generation 2, and
   `transpose_x_to_y` back into the original layout.

`--run=all_to_all` replaces (3) and (6) with one `all_to_all`. This row fixes
`scatter`, the scheme the paper proposes; the other is a different collective
with a different message pattern and would be a second row, not a second
argument.

One program on four runtimes: `fft_hpx_hpx` is the HPX program,
`fft_hpx_arts_<variant>`, `fft_hpx_xsocr` and `fft_hpx_ocrvx` are the OCR
mirror (`benchmarks/apps/hpx_origin/fft_hpx.c`), one row in `hpx_apps.yaml`.
HPX primitives used: `hpx::collectives::create_communicator`, `scatter_to` /
`scatter_from` with `generation_arg`, `all_reduce` (the added tally), and
`hpx::experimental::for_loop(par, …)` for every phase. There are no
components and no actions — the whole distributed structure is collectives,
which makes this the section's purest exchange row. The mirror's mapping: two
blocks per rank — its local rows and its local transposed rows — one block
per (phase, source, destination) chunk, one task per row per transform, one
task per row per split (writing into every destination's chunk, exactly as
the origin's row-parallel `split_vec` does), one task per (source, row) per
transpose, and each `scatter_to` as `nl` labeled points — one logical payload
per (source, destination). HPX additionally routes its collective support objects
through rank 0; that runtime implementation can add physical messages.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--nx` | x dimension `dim_c_x` (rows) | 8 | reachable |
| `--ny` | y dimension `dim_r_y` (points per row) | 14 | reachable |
| `--plan` | FFTW planner rigour | `estimate` | reachable |
| `--run` | exchange scheme | `scatter` | reachable, `scatter` only in the mirror |

**`--nx` and `--ny` are the dimensions, not their logarithms.** The paper's
own script passes `--nx=16384 --ny=16384`; a reading of them as `log2` sizes
would be off by four orders of magnitude.

The pair must divide the geometry exactly. `n_x_local = nx/nl` and
`n_y_local = (ny/2 + 1)/nl` are truncating divisions in the origin, so a
non-dividing pair silently drops the trailing rows and transforms a smaller
array than the one asked for — no error, a different job. At `--nx=256
--ny=254` the two counts are 256 and 128, which divide every node count up to
sixteen. The mirror rejects a non-dividing pair as usage rather than
truncating, which is the one place it is stricter than the origin.

Two further mirror-side rejections, both of arguments neither side can
compute the stated job from: a `--run` that is not `scatter`, and a pair for
which the origin's second transpose would index outside the array (it derives
its row count from the received chunk's size divided by an input stride the
first phase set, and the mirror creates one task per row, so the two must be
the same number). `--plan` follows the origin's own else-less chain: an
unrecognised name keeps `estimate` rather than being rejected.

`--result` and `--header` are output knobs of the origin — printing the whole
array, and a header line in its own runtime CSV. Neither has a mirror and
neither is part of the row.

A rejected argument prints usage and calls `ocrShutdown()` with status 0,
where the origin's own option parsing exits non-zero; the missing
`CHECKSUM`/`[APP_E2E]` marker fails the cell either way.

## Structure

Let `nl` = ranks, `cx = nx`, `cy = ny/2 + 1`, `nxl = cx/nl`, `nyl = cy/nl`,
`cypart = 2·cy/nl`, `cxpart = 2·cx/nl`. Per run:

| object | count (per rank, unless noted) | dependences |
|---|---|---|
| driver task | 1 | 0 |
| `fft1_edt` (r2c transform) | `nxl` | 2 — rows buffer RW, plan image RO |
| `split1_edt` | `nxl` — one per row | `2 + nl` — phase-transform join NULL, rows buffer RO, `nl` outgoing chunk blocks RW |
| `publish_edt`, phase 1 | 1 | `1 + nl` — split join NULL, `nl` chunk blocks RO |
| `xpose_scope_edt`, phase 1 | 1 | `1 + nl` — phase gate NULL, `nl` arriving chunks RO |
| `xpose_outer_edt`, phase 1 | `nl` — one per source | 1 — one arriving chunk RO |
| `xpose1_edt` | `nl·nyl` — one per (source, row) | 2 — arriving chunk RO, transposed-rows buffer RW |
| `fft2_edt` (c2c transform) | `nyl` | 3 — scope output NULL, transposed-rows buffer RW, plan image RO |
| `split2_edt` | `nyl` — one per row | `2 + nl` — phase-transform join NULL, transposed-rows buffer RO, `nl` return chunk blocks RW |
| `publish_edt`, phase 2 | 1 | `1 + nl` — split join NULL, `nl` chunk blocks RO |
| `xpose_scope_edt`, phase 2 | 1 | `1 + nl` — phase gate NULL, `nl` arriving chunks RO |
| `xpose_outer_edt`, phase 2 | `nl` — one per source | 1 — one arriving chunk RO |
| `xpose2_edt` | `nl·nyl` — one per (source, row) | 2 — arriving chunk RO, rows buffer RW |
| `reap1_edt` | 1 | `1 + 2·nl` — join NULL, `2·nl` phase-0/1 chunk points RO |
| `finish_edt` | 1 | `5 + nl` — scope output NULL, reap output NULL, `nl` phase-1 chunks RO, rows RO, transposed rows RO, plan image RW |
| `sum_edt` (rank 0 only, once) | 1 | `nl` — one share per rank RO |
| rows buffer `varr` | 1 | `nxl·2·cy` doubles |
| transposed-rows buffer `warr` | 1 | `nyl·2·cx` doubles |
| chunk blocks | `2·nl` (`nl` per phase) | `nxl·cypart` doubles (phase 1) / `nyl·cxpart` doubles (phase 2) |
| plan image | 1 | see Sizing, below (varies with `--plan`) |
| rank share | 1 | one double |
| exchange points (global) | `2·nl²`, of which `2·nl(nl−1)` cross a rank | — |
| phase joins | 4 latches + 2 `EDT_PROP_FINISH` scopes per rank | — |

Per-rank dependence-slot total:

```
S = nxl·(4 + nl) + nyl·(5 + nl) + 4·nl·nyl + 9·nl + 10        (+ nl on rank 0)
```

and per-rank block count is `4 + 2·nl` (the two buffers, `2·nl` chunk blocks,
the plan image, one share). The global reservation is `2·nl²` names — two
phases × one point per (source, destination) pair — which is the whole
rendezvous this program needs.

An OCR block has exactly one writer, and the mirror's split and transpose
tasks are the origin's own parallel units, unchanged: `split1_edt`/
`split2_edt` run one task per **row**, exactly as the origin's
`split_vec`/`split_trans_vec`, each task writing a disjoint byte range into
**every** destination's chunk block; `xpose1_edt`/`xpose2_edt` run one task
per (source, row), exactly as `transpose_y_to_x`/`transpose_x_to_y`, each
writing disjoint rows or a disjoint column pair of the phase's destination
buffer. So `nxl` (or `nyl`) split tasks of a rank hold each of that phase's
`nl` chunk blocks `DB_MODE_RW` at once, and `nxl` `fft1_edt` tasks hold `varr`
RW at once, and `nl·nyl` transposes hold the phase's destination buffer RW at
once — all disjoint writes, all the origin's own concurrency, carried
unchanged. **`DB_WRF` is undefined for this row by design**: the program has
same-DB write-write conflicts the code does not event-order (disjoint rows
and columns at whole-DB granularity), which is exactly what
`ARTS_MEMORY_MODEL=DB_WRF` declares undefined, since many tasks write one
buffer, as the origin's `for_loop(par)` does. The coherence plane artsrun
selects from (`EXCL`/`INV`/`VAL` × release × write) carries no `DB_WRF`
entry, and this row has none either.

Where the origin has a barrier between phases (`for_loop` is blocking), the
mirror preserves the whole local phase boundary. Each split writes into all
`nl` local chunk blocks of its phase; only after all `nxl` (or `nyl`) local
splits are complete does the phase's publication task satisfy the exchange
points. Every second FFT waits for the first transpose phase join. The
second split latch also gates cleanup of the transposed rows; receipt of all
chunks aimed at this rank alone would not establish that its own outgoing
readers have finished.

Nothing is allocated per iteration — there are no iterations. The arrays occupy
`4·cx·cy·8` bytes per run, and the chunk blocks of both exchanges —
`2·cx·cy·8/nl` bytes per rank each — are created up front, so both exchanges
are resident for the whole run: the logical peak is `64·cx·cy` bytes per run
before runtime and allocator overhead.

## Wiring

**An OCR data-block dependence is satisfied when it is *added*, not when the
block is written**, so ordering has to ride on an event wherever a consumer's
edge can exist before its producer has written. This program has two block
handovers that need the rail and three that do not:

* **The chunks need it.** A rank cannot guess the GUID another rank's split
  task chose for the chunk bound for it — this is the origin's collective —
  so the producer writes and releases the chunk, then the publication task
  satisfies its labeled STICKY point after every local split is complete; the consumers register on the same point in `DB_MODE_RO`.
  The points come from the program's single
  `ocrGuidRangeCreate(…, GUID_USER_EVENT_STICKY)` range, indexed through
  `mirror_edge` as `(phase·nl + source)·nl + destination`, which gives each
  point exactly one producing task, one consuming rank and one destroyer, and
  never aliases two units of work onto one — several tasks of that rank read
  it, which is why the destroyer is a task of its own. Both sides open the point and `OCR_EGUIDEXISTS` is the
  expected second arrival.
* **The rows do not.** `varr` and `warr` are created, filled and released by
  their rank's driver before any task that reads or writes them exists, and
  every later writer is ordered behind the previous phase by a join. All
  first transposes complete before any second transform starts.
* **The rank share does not.** The finish task writes it, releases it, and
  only then wires it into the sum task's slot.

**Where a point lives is the runtime's choice and only the hop count depends
on it.** A labeled range is homed by index on ARTS and ocr-vx
(`index % nranks`), so a point's home is its consumer's rank and a
within-rank publish is no message; xsocr stamps the reserving PD's own
location into every GUID of a reserved range, so there all `2·nl²` points
live at rank 0 and every publish costs a message there and a forward. The
answer is the same on all four runtimes; the traffic is not, and that is
disclosed rather than equalised.

**A chunk has one producer and several readers** — every transpose task of
the destination rank reads all `nl` arrivals — so no reader can be its
destroyer. Each rank has one reap task per phase that depends on the same
points and destroys both the blocks and the points once the phase's transpose
join has fired; the second phase's reap is folded into the finish task, which
also destroys the rank's rows, its transposed rows and its two FFTW plans, in
that order and after the tally is taken.

**The joins are per rank, because the origin's `for_loop`s are per locality.**
Four latches per rank — one per transform phase and one per split phase —
each with its successors created and registered before any task can complete
it, since a latch is once-type and is destroyed when it reaches zero. The two
transposes are gated by their own `EDT_PROP_FINISH` scope instead, one per
phase, whose output event fires only once its whole spawned subtree — the
outer per-source task and its per-row children — completes. The first
transform's latch alone carries one extra slot beyond its `nxl` producers:
the driver's own last statement is one explicit decrement of it, issued only
after every task and dependence in the whole function has been created.
Every other join is gated, directly or transitively, behind that one firing,
so this single decrement releases the rank's entire graph at once rather
than needing a guard on each join individually. The two publication tasks
additionally join their split phases through all chunk result dependences.

**The end is collective, because the origin's is.** The origin's added tally
is an `all_reduce` every locality reaches; the mirror's sum task is created
before the fork with one slot per rank, each filled by that rank's finish
task, so `ocrShutdown()` sits structurally behind every rank's last work
rather than behind a timing margin.

## Flow

`mainEdt` derives the origin's sizes (including its recomputation of the real
length from the complex one, so an odd `--ny` transforms one point fewer),
validates them, reserves the `2·nl²` point names, creates the eleven templates
and the sum task, and forks one driver per rank.

A driver creates its rank's rows and fills each with the ramp, creates its
transposed rows zeroed, and builds the two FFTW plans — once per rank, on the
first row of each array, exactly where the origin builds them, so a planner
that writes (every flag but `estimate`) overwrites what it overwrites there.
It then creates every task of the rank in one order that two OCR contracts fix:
the two phase `EDT_PROP_FINISH` scopes first, empty of their own
dependences; then the reap and finish tasks, fully wired; then each phase's
publication task and its row-parallel splits, which also wire that phase's
`FINISH` scope; then the second-transform tasks; then the first-transform
tasks last, followed by the one explicit decrement (Wiring, above) that
releases the whole graph.

The plans are executed by tasks that did not build them, through
`fftw_execute_dft_r2c` / `fftw_execute_dft` — the thread-safe new-array form
the origin's own parallel `for_loop` already relies on. The plans are built
with the origin's own flag only; the mirror adds no `FFTW_UNALIGNED`. What
resolves the new-array contract's alignment requirement — the execute buffer
must have the plan buffer's alignment, and a block's payload address is not
fixed when the plan is built — is the translated library itself: every
alignment predicate FFTW evaluates is rewritten by the LLVM pass to test an
address's offset relative to its own allocation rather than its absolute
value, at the mask the configured FFTW build actually uses (16 bytes for the
vendored double-precision SSE2/AVX/AVX2/FMA build), so plan selection and
codelet applicability stay address-independent for every row, not only the
first (`fftw_reloc/README.md`, "Alignment and compilation"). That coupling
is a build-time one: it holds as long as the configured SIMD set keeps
FFTW's alignment constants at that value.

Executing a row also costs representation work that runs inside the measured
interval, on the OCR side only: the plan image is published once per rank —
built, written and made read-only before any row task runs — and each row's
open against it is O(1), a handful of header checks and pointer computations
against the image's own identity-keyed and offset-ordered arrays, with no
per-row allocation. The translated library's own memory accesses, wherever
the alignment rewrite touches a load, store or inline-asm move, are
unaligned-safe rather than assuming the plan-time base. Neither changes the
codelet arithmetic.

The measured disagreement between the HPX and OCR checksums is not a
difference in kernel arithmetic: both sides plan and execute the same FFTW
3.3.10 source at the same configuration, and the `ESTIMATE` plan text at the
catalog geometry is byte-identical between the two builds. What differs is
the compiler — the HPX program links `arts::fftw3`, compiled by GCC; the OCR
mirror links the translated `arts::fftw_reloc`, compiled by clang-14 — so the
same source, planner and selected plan end up as different machine code,
with different FMA contraction and rounding in the last bits. That is
consistent with the measured `~1e-15..1e-16` relative disagreement (below)
and is covered by the row's `1e-09` tolerance with seven orders to spare.

| HPX wait site | classification | mirror |
|---|---|---|
| `for_loop(par, …)` over the r2c transforms | driver wait — a phase barrier | the per-rank transform join |
| `for_loop(par, …)` over `split_vec` | driver wait — a phase barrier | the first publication task waits for all local split outputs |
| `communication_futures_[i].get()`, phase 1 | driver wait — the exchange (O4, one of two) | each transpose task's `nl` point dependences |
| `for_loop(par, …)` over `transpose_y_to_x` | driver wait — a phase barrier | the transpose join |
| `for_loop(par, …)` over the c2c transforms | driver wait — a phase barrier | the second transform join |
| `for_loop(par, …)` over `split_trans_vec` | driver wait — a phase barrier | the second publication task waits for all local split outputs; the split latch also gates cleanup |
| `communication_futures_[i].get()`, phase 2 | driver wait — the exchange (O4, two of two) | each final transpose's `nl` point dependences |
| `all_reduce(…).get()` (the added tally) | end — collective, every locality present | the sum task's `nl` rank shares |

Mid waits: 0. The two driver waits marked O4 are the origin's own driver
loop gathering the collectives' futures between phases.

Both sides mark the same logical interval with `[APP_E2E]`, matched to the
source's own timing scope (the catalog's `timing_contract:
hpx-origin/fft_hpx/source-interval-v1`, reported as `app_s`). On HPX,
`run_clock` starts immediately before the array allocation and ramp, so
allocation, fill, `initialize()` (both plans and the communicators) and
`fft_2d_r2c()` are inside; `print_e2e` runs right after `stop_total`, before
the added local sum, the `all_reduce` and the `CHECKSUM` print, and before
`~fft()` destroys the plans. HPX's `[E2E]` is the same stamp here, since
`hpx_main` calls the single-clock form of `print_e2e`. On OCR, `start_ns` is
taken at the top of `driver_edt` for rank 0, before the row buffers are
created and filled, and travels to `finish_edt` in `paramv`; `finish_edt`
prints it, for rank 0, before the rank's own fold, before every
`ocrDbDestroy`, and before `fftw_reloc_destroy_pair`. Both sides therefore
include buffer allocation and fill, both plans, both transforms and both
exchanges, and exclude the checksum reduction and the plan/buffer teardown.
The runtime's own `[E2E]` — the span to `ocrShutdown()` on the OCR side — is
still printed as a separate observation and is not the metric this row
reports.

**The scalar is the plain sum of the final array**, because the origin
computes no printable quantity of its own — it prints a phase-by-phase timing
table and appends a CSV row. `CHECKSUM %.14g` on the HPX side, `%.14e` on the
OCR entries (the xsocr `printf` replacement has no `%g`).

**It is a function of the locality count**, and the reason is the origin's own
arithmetic — but not the one it first looks like. It is *not* the first
transpose's `nl·j + i` column ordering: every row starts as the same ramp, so
after the real-to-complex pass every row is identical, the vector the second
transform sees is constant in x at every `nl` (the value is `32131 + 0j`
throughout), and permuting a constant vector changes nothing. What moves the
answer is the **second** transpose, which reads its chunk with the first
phase's input stride `dim_c_y_part` where that chunk was cut with
`dim_c_x_part`: both strides follow `nl`, so which chunk elements reach the
result follows `nl` too. The row's pinlessness is a symptom of the kept kernel
below, not of the block distribution. Measured at the gate arguments:

| ranks | HPX | every OCR entry |
|---|---|---|
| 1 | `28434214.354769` | `2.84342143547686e+07` |
| 2 | `26606318.203792` | `2.66063182037921e+07` |
| 4 | `24780157.170575` | `2.47801571705749e+07` |
| 8 | `22952333.813996` | `2.29523338139958e+07` |

The four OCR entries agree to the last printed digit at every geometry with
one exception: xsocr renders the four-node value `...48e+07` where the others
render `...49e+07`, that runtime's own printf replacement being good to about
a part in `1e15`. The HPX entry agrees with all of them to about `1e-16`
relative, which is two reduction orders (an `all_reduce` against the mirror's
rank-order sum) plus the same FFTW source compiled twice — GCC on the HPX
side, clang on the OCR side (Flow, above) — into two different codelet
binaries. The row's `1e-09` tolerance
covers that with seven orders to spare, and there is no cross-geometry pin: the oracle is the four runtimes
agreeing at each geometry, which is what consensus votes.

**One kernel is doubtful and is kept as written.** `transpose_x_to_y` reads
its chunk with input stride `dim_c_y_part` where that chunk was cut with
`dim_c_x_part` — the phase-1 stride in a phase-2 routine. For the shapes this
row runs the indices stay inside the array and every element of the result is
written exactly once, so it is a defined, deterministic answer and not a
memory error; whether it is the transpose the authors meant is not the
mirror's question. It is carried unchanged on both sides, and a mirror-side
usage check is what keeps it in bounds — enforced, not assumed: the highest
element that arithmetic reads is `cypart·(nyl − 1) + 2·nxl − 1` against a
chunk of `nyl·cxpart` doubles, which is in bounds exactly when
`2(nyl − 1)(nyl − nxl) < 1` — over the integers, `nyl ≤ nxl` or `nyl == 1`,
i.e. `ny/2 + 1 ≤ nx` unless `ny/2 + 1` is the rank count. This row's pair
satisfies it (128 ≤ 256) and so does `16384 × 16382`; a tall, narrow pair such
as `--nx=128 --ny=510` does not, and is refused as usage rather than
overreading the chunk block by 65280 elements. What follows for the oracle is worth
stating: the checksum sees the whole final array, so a mis-indexed exchange
*would* move it — but it cannot tell this arithmetic from a corrected one,
because both sides run the same arithmetic.

## Placement

Row `i` of the array belongs to rank `i/n_x_local` and transposed row `k` to
rank `k/n_y_local`, which is where the origin's block distribution puts them
— each locality allocates its own `vector_2d` and never moves it. The mirror
states that placement on every object: every driver, transform, split,
transpose, reap and finish task carries an explicit
`ocrAffinityGetAt(AFFINITY_PD, …)` EDT hint for its rank, and every rows
buffer, transposed-rows buffer, chunk and rank share an explicit DB hint.
**A chunk is homed at its source**, which is where the origin's
`values_prep_[j]` sits before the collective sends it. Nothing is left to
the build's no-hint policy.

The application defines `2·nl²` logical chunk deliveries per run, of which
`2·nl(nl−1)` cross ranks, each carrying `2·cx·cy·8/nl²` bytes. Their logical
remote payload is `4·cx·cy·8·(nl−1)/nl` bytes. Physical messages and wire bytes
also depend on event routing, acquisition and the collective implementation;
HPX's support-object relay through rank 0 is not counted by this formula. The
row and transposed-row buffers never leave their rank, and the `nl` rank
shares are one double each.

## Sizing

The width knobs are `nx` and `ny` together: work is `cx` transforms of length
`dim_r_y` plus `cy` transforms of length `cx`, so it grows as
`cx·cy·log(cx·cy)`, while the crossing volume grows as `cx·cy` and the
*message count* stays at `2·nl(nl−1)` whatever the size. That is the property
that makes this row the section's exchange row: the message count is fixed by
the geometry alone, so growing the size moves bytes per message and nothing
else.

Both dimensions must stay divisible by every node count in the sweep —
`nx % nl == 0` and `(ny/2 + 1) % nl == 0` — or the origin silently transforms
a smaller array. `ny` even keeps `dim_r_y == ny`; an odd `ny` transforms
`ny − 1` points, which is the origin's own recomputation and not an error.
A calibrated pair should keep `ny/2 + 1` a multiple of 8, which is what makes
the eight-node cell exact; `--ny=254` gives 128 and satisfies it, as would
`--ny=16382` (8192).

A non-dividing pair is worse than a smaller job, which is the second reason to
settle it first. `dim_c_y_part = 2·(ny/2 + 1)/nl` truncates as well, so where
that quotient comes out odd a chunk boundary falls between the real and the
imaginary half of one complex number and the arrivals are **mis-paired**, not
merely fewer. Alignment does not enter: the vendored FFTW's alignment for
doubles is 16 bytes on every x86 SIMD branch, and a row stride of `16·cy` bytes
is 16-aligned for any `cy`. What does is that an odd `ny/2 + 1` transforms
`ny − 1` points (the origin's own recomputation) and cannot divide an even node
count — two independent reasons never to calibrate on the paper's own square.

Memory is `4·cx·cy·8` bytes of array in total — `2·cx·cy·8/nl` per rank for
the rows and the same again for the transposed rows — plus both exchanges'
chunks, created up front, at `4·cx·cy·8/nl` per rank. At the gate arguments that is 1 MB of
array over the whole run, half rows and half transposed rows. The paper's
own size, `--nx=16384 --ny=16384`, is 4.0 GB of array and 2.0 GB of chunks
spread over the ranks, which is what a calibration has to size against; the
growth is `cx·cy`, quadratic in a square size. That pair is also a caution
rather than a model: `ny/2 + 1 = 8193` is odd, so the paper's own square
drops trailing rows at every locality count above one. A calibrated pair
here is chosen for divisibility first.

The two FFTW plans add one more DB per rank, the retained-state image
(Flow, above), sized by the identity-keyed entry array and the bytes of
every allocation FFTW retains: about 640 KB at the catalog geometry under
the default `--plan=estimate`, growing to about 8 MB under
`--plan=exhaustive` because of the extra solver candidates FFTW keeps at
that rigor. It is why the per-rank block count above names the image
separately from the two numerical buffers.

The `hpx-gate` roster runs `--nx=256 --ny=254 --plan=estimate --run=scatter`,
which finishes in milliseconds on one rank. What limits the multi-rank cell at
that size is the exchange's latency and not its bandwidth — `2·nl(nl−1)`
messages of `512/nl²` KB — so the gate's timing says more about per-message
cost than about the transform, and `nx`/`ny` are the knobs that move it back
onto computation. The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--nx=72000 --ny=71998 --plan=estimate
--run=scatter` at every node count. `ny = nx − 2` because the second phase
runs over the half-spectrum's `ny/2 + 1` rows: with `ny = nx − 2` that count
is `nx/2 = 36 000`, divisible like `nx` by every node count to 32 and at most
`nx` (the constraints above), where `ny = 72 000` would give 36 001 rows that
no even node count divides. `nx = 72 000 = 2⁶·3²·5³` is FFTW-sized. Derived
to the 100 s window through each arm's measured exponent between 60 480 and
77 760 — INV/WB, the slowest arm there, binds — under the one-node memory
budget with a 10 % margin (172 GB resident on ARTS, 166 GB on HPX at this
size), and rounded from the model's 71 680 to 72 000 (65 536 would drop
INV/WB to about two thirds of the window). The anchor measured 40.5 s on
INV/WB (39.8–55.2), 59.1 s on VAL (36.7–74.3) and 41.5 s on EXCL (37.2–67.0)
— one slow repeat per arm in the near-ceiling regime — and 67.2 s on HPX; the
sizing pass, on the runtime before its data-block descriptor lost its inline
payload, had measured 53–84 s.