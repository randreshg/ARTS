# stream

*The four McCalpin STREAM kernels (copy/scale/add/triad) run over `numThreads`
independent per-chain arrays, `nTimes` times each, with every kernel's output
array freshly created and the previous one destroyed.*
Source: `third_party/ocr-apps/apps/stream/ocr/stream.c` (701 lines; from line
579 to the end — the last ~18% — is a `#if 0`-disabled serial reference
checker, dead code).

## Overview

Computes the classic STREAM bandwidth kernels — `copy: c=a`, `scale:
b=scalar*a`, `add: c=a+b`, `triad: a=b+scalar*c` — but unlike McCalpin's
canonical STREAM, `scale` reads directly from `a`, not from `copy`'s output
`c`; `copy`'s result is computed and then discarded (its `RW` dependence into
`add` exists only to authorize destroying the DB, never to read it). The array
is split into `numThreads` disjoint, mutually independent chains (no data or
control dependence between them), each iterated `nTimes` times with a fresh
set of `a`/`b`/`c` DataBlocks churned every iteration. That per-kernel churn is
the published inefficiency this tier exists to exhibit: the array on every
dependence edge is created where its producer ran and read wherever the
consumer lands.

The result scalar the harness reads is the validation line, and it is now a
*reduction*: every chain verifies its own final slice against the closed form
of the kernel recurrence, returns the number of elements it verified, and
`finalize` sums those counts and prints
`Solution Validates: all <n> elements match expected value`. `n` equals
`perThreadSize * numThreads` exactly when every element of every chain is
within `1e-13` relative, so the pin is still "the whole array validated" — but
no task ever holds more than one chain's slice, and the join carries 16 bytes
per chain instead of the whole result. `STREAM_RESULT a[0] = ...` rides along
as an extra scalar (chain 0's final first element, a pure function of
`nTimes`).

`finalize` also prints an `MB/s` line built from per-sweep samples; those
samples are per-process (see Placement), so the line reports only what the
reporting process actually timed and says so when it timed nothing. It is a
diagnostic, never what the harness validates.

The catalog lists a restructured twin, `stream_dist` — upstream's *other*
shipped variant (persistent per-chain arrays) plus an index affinity map,
registered as its own row with the same argument triple. With near-zero
arithmetic per element and DBs churned on every step, the program is a
data-movement (memory-bandwidth) probe locally — and, once EDTs and DBs spread
across ranks, a data-*migration* probe instead (Placement).

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `streamArraySize` | total element count, split evenly across chains | source `#define` 9000000; the build's `EXTRA_DEFINES` lowers it to 1000 for an argument-free smoke run | ✓ parsed in `mainEdt` from the argument DB, forwarded through paramv to `mainLet`/`loop`/every kernel — multinode-safe |
| `argv[2]` = `numThreads` | number of independent chains (DAG width) | source 32; build 2 | ✓ same path |
| `argv[3]` = `nTimes` | sweeps per chain (DAG depth) | source 1000 (McCalpin's own canonical value is 10; the OCR port raised it) | ✓ same path; the `NTIMES` macro is `#ifndef`-guarded, so a build define can lower the argument-free default |

If `streamArraySize % numThreads != 0` the run truncates to
`perThreadSize = streamArraySize / numThreads` elements/chain and prints a
warning to stderr (the tail elements are simply not represented in any DB).
Zero arguments are replaced by the compile-time defaults, loudly in the sense
that the truncation warning still fires. Compile-time only: `SCALAR` (0.42,
the recurrence multiplier — McCalpin's own MPI-STREAM value, not the 3.0 of the
2016 OCR port, which overflows long before 1000 sweeps), `STREAM_TYPE`
(`double`). `mainEdt` also builds an `OCR_HINT_EDT_DISPERSE`/`NEAR` hint and
attaches it to every `mainLet` create; it is not a CLI argument and, per
Placement below, the shim does not translate it.

## Structure

Let `T = numThreads`, `K = nTimes`, `pts = perThreadSize`. Per chain:
1 `mainLet` EDT, `K` `loop` EDTs (the first `FINISH`, the rest plain), `4·K`
kernel EDTs (`copy`/`scale`/`add`/`triad`, one set per sweep) and 1 `check`
EDT.

| object | count | size |
|--------|-------|------|
| EDTs total | `T·(2 + 5K) + 2` (per-chain chain + `check` + `mainEdt` + `finalize`) | — |
| DBs | `T·(2 + 4K)` — one initial `a`, then `copy`/`scale`/`add`/`triad` each create one fresh DB per sweep, plus one 16-byte result record per chain | `pts·8` bytes each; the record is 16 B |
| Events | `T·(4 + 4K)` — per chain: the ONCE completion event, the first `loop`'s output event, its finish event (it is the `FINISH` EDT), one output event per kernel EDT per sweep, and `check`'s output event | — |
| EDT templates | 8 (`mainLet`, `loop`, `copy`, `scale`, `add`, `triad`, `check`, `finalize`), created once, reused for every instance | — |

Worked numbers: calibration pending — the orchestrator sizes `argv[1]` and
`argv[2]`; substitute into the formulas above. At the width the sweep rule
fixes (`T` = the largest geometry's worker count) and `K` = the published 1000,
the EDT count is `T·5002 + 2` and the DB count `T·4002`.

Counter cross-check: **stale, needs re-measurement.** The previously recorded
absolutes predate the per-chain `check` EDT and its record DB (`+1` EDT, `+1`
DB, `+1` event per chain) and the `finalize` template's paramc change.

## Wiring

- `mainEdt` creates the 8 templates, one `finalize` EDT (`depc = numThreads`,
  paramv `[numThreads, perThreadSize]`), then per chain: one ONCE event
  `evt_finalize_i` (wired RO into `finalize`'s slot `i`) and one `mainLet` EDT
  carrying that event's GUID plus the template GUIDs and sizes through paramv.
- `mainLet` creates the chain's initial `a` (initialized to 2.0, RW-held by the
  creating EDT, no separate release), then one `EDT_PROP_FINISH` `loop` EDT
  (`iter=0`) with `a` wired RW. `mainLet`'s own output event is discarded — the
  finish wrapper's only job is to fence the whole `K`-deep per-chain subtree
  into its own OCR scope.
- `loop(iter)` creates `copy`/`scale`/`add`/`triad`, wires
  `a`(RO)→`copy`,`scale`; `scale_output`(RO)+`copy_output`(RW, destroy-only)→
  `add`; `add_output`(RO)+`scale_output`(RO)+`a`(RW, destroyed)→`triad`; then
  either creates `loop(iter+1)` with `triad_output` RW, or (last sweep) creates
  the chain's `check` EDT with `triad_output` RO and wires `check`'s output
  event into `evt_finalize_i`.
- `check` replays the scalar recurrence `K` times (a constant-size closed form,
  not a re-run of the workload), counts how many of its `pts` elements are
  within `1e-13` relative of it, writes `{matched, a[0]}` into a 16-byte DB,
  destroys the final array and returns the record.
- Every waiter on a ONCE output event is registered before its producer can
  become runnable: the chain-starting `addDependence`s (`a`→`scale`, `a`→
  `copy`) are the last two statements of `loop`, and the `check`/next-`loop`
  wiring precedes them.
- `copy`'s output DB is never read by anything — `add` takes it RW purely to
  authorize `ocrDbDestroy`. The real producer chain is `a →{copy∥scale}→
  add→triad→(new) a`; per-sweep maximum concurrent readers on any one DB is 2.
- No DB is ever written by more than one EDT (fresh GUID every write), and no
  DB is shared across chains — the `T` chains touch disjoint DB sets and join
  only at `finalize`. The bottleneck is the strictly serial `K`-deep hand-off
  within each chain (see Flow).

## Flow

Per chain, sweeps are a serial pipeline of depth `K`: `loop(iter+1)` cannot
even be created until `triad` of sweep `iter` has produced its output DB, so a
chain never has two sweeps' kernels in flight at once. Within one sweep,
`copy` and `scale` are parallel (width 2), then `add` (needs both), then
`triad` (needs `add` and `scale`) — width collapses to 1 for the back half of
every sweep. Across the `T` independent, unsynchronized chains, the
instantaneous frontier ranges from `T` (all chains in add/triad) to `2T` (all
in copy/scale) and never falls below `T` before the join. The `T` chains
converge only at the single `finalize` EDT (`depc = T`), which sums `T`
16-byte records and shuts down. There is no rank-0-only phase inside the DAG —
`mainEdt`'s own `preamble()` (banner + size printout) runs once, natively,
before any EDT is created, and the verification that used to gather every
array into `finalize` is now `T` independent per-chain checks.

## Placement (base)

Every `ocrDbCreate` in this source passes `NULL_HINT`, so every DB homes at its
creator's own executing rank (creator/first-touch) — a copy of the fresh `c`,
`b` or `a` lives wherever the kernel that made it happened to run. Every
`ocrEdtCreate` also passes `NULL_HINT` *except* `mainLet`, which is given an
`OCR_HINT_EDT_DISPERSE`/`NEAR` hint — but the shim's affinity extractor only
inspects the `OCR_HINT_EDT_AFFINITY` bit of a hint's propMask, never
`OCR_HINT_EDT_DISPERSE`, so that hint has no effect: `mainLet`, `loop`,
`copy`, `scale`, `add`, `triad`, `check` and `finalize` are *all* placed by the
same policy — runtime round-robin (a per-creating-rank atomic counter modulo
rank count).

Consequence at multinode: since each phase's consumer EDT is independently
round-robin-placed, essentially every dependence edge — `a` into `copy`/
`scale`, `scale_output` into `add`/`triad`, `add_output` into `triad`, the
churned `a` into the next `loop` — is a coin flip (probability `(N-1)/N` at `N`
ranks) on being a *remote* acquire of a `pts·8`-byte array. What STREAM means
to measure as local memory bandwidth becomes, at `N>1`, dominated by transfers
of full per-chain arrays on nearly every phase transition; the algorithm has no
exploitable locality left to lose (there was never any data reuse to place
near), but the base program adds gratuitous network traffic on top of the
minimum the algorithm requires. This tier anti-scales for that reason, and that
is what it is here to show.

A second consequence, and the reason `printTimes` is written the way it is:
`finalize` is itself round-robin-placed, so it does not reliably land on the
rank that ran `mainEdt`, and the two ends of one sweep (`loop` starts the
sample, `triad` closes it) are independently placed, so a sample can be
half-recorded — started in the reporting process and ended elsewhere, or the
reverse. The per-sweep sample array is allocated only on the `mainEdt` rank and
every write is `if(times)`-guarded; on top of that a cell is sign-encoded, so
the two halves are distinguishable from a complete sample rather than looking
like a plausible duration:

- `loop` stores the *negated* start stamp;
- `triad` closes the cell only if what it finds there is negative, adding the
  current stamp to it — so a cell started here and ended elsewhere stays
  negative, and a cell ended here but started elsewhere is still the `calloc`
  zero and is left alone;
- `printTimes` therefore keeps exactly the cells that are strictly positive,
  i.e. those whose start and end both ran in this process, and averages over
  those; with none it prints `No complete kernel timing: MB/s not reported`
  rather than dividing by a zero minimum, and with no array at all it prints
  `No kernel timings on this process: MB/s not reported` rather than
  dereferencing a null pointer.

The `Solution Validates` scalar the harness reads does not depend on the
samples.

## Placement (hinted)

`stream_hinted` is built (`HINTED_PLACEMENT` in
`benchmarks/apps/CMakeLists.txt`; catalog `hinted: true`), from the same source
under `-DOCR_APP_OPTIMIZED_PLACEMENT`. The guard encloses exactly three
static helpers and their `#else` macro forms; every create site is textually
identical in the two builds and differs only in its `hint` argument. No control
flow, work partition, DB or event count, wiring or arithmetic is inside a
guard.

The map is one pure function of the chain index: chain `tid` and *everything*
belonging to it — its `mainLet`, its first `loop`, every later `loop`, all
`4K` kernels, its `check`, its initial `a` and every array the kernels create —
is placed at PD `tid % pdCount`. Consequences:

- **Coverage and balance are properties of the program**, not of a build
  default: with `T` chains over `N` places each place owns exactly
  `floor(T/N)` or `ceil(T/N)` chains (exactly `T/N` when `N | T`, which the
  sweep's rank counts satisfy at the prescribed width). Creator-pinning would
  have left coverage to the runtime's no-hint policy.
- **Every dependence edge of a chain is local**, including the two that a
  here-pin cannot reach: the initial `a` (created by `mainLet`, homed by its DB
  hint at the chain's place) and the chain's first `loop`.
- The chain's FINISH scope, the per-chain ONCE event and `finalize` are
  unhinted in both tiers.

At one place the helpers return `NULL_HINT`, so base and hinted are the same
program at one node — the anchor property the tier comparison rests on.

## Sizing

`streamArraySize` and `numThreads` jointly set task grain: `pts =
streamArraySize/numThreads` is both the DB payload (bytes moved per dependence
edge) and the per-kernel work size. `numThreads` alone sets DAG width (`T` to
`2T` instantaneous EDTs); `nTimes` sets depth only — more sweeps means more
total EDTs/DBs and a longer serial chain per chain, not more parallelism.

- **Width rule**: `numThreads` is the width knob. Set it to the total worker
  count of the widest geometry (or a multiple of it); the frontier is then
  `1.0x` to `2.0x` that count and never dips below `1.0x` until the join.
- **A per-chain cliff at 512 KiB.** One array crossing 512 KiB per chain
  (65,536 `double` elements) moves the one-node anchor by ~3.5x in a single
  size step, with resident set *falling* across it — an allocation-path
  signature (both a core's L2 and the allocator's large-object limit sit at
  512 KiB, and this measurement cannot tell them apart). Size so that `pts·8`
  is clearly on one side of it, and never straddle it inside one sweep of the
  campaign: below the cliff the program is cache-resident and stops being a
  memory benchmark.
- **Memory**: the live `a` set is `T·pts·8 = streamArraySize·8` bytes for the
  whole run, plus up to 4 transient arrays per *running* chain
  (`workers·4·pts·8`), plus allocator retention behind `T·4K` DB creates. The
  `T` result records are 16 B each. Peak resident was measured at roughly three
  times the live `a` set — but that observation was taken at **100** sweeps,
  and the retention term is driven by the `T·4K` create/destroy count, which
  the campaign's 1000 sweeps multiply tenfold. Size against the formula and one
  re-measurement at the campaign depth; the multiplier is calibration pending.
- The verification tail no longer scales with the array: `finalize` gathers
  `T·16` bytes, and the per-element check is one extra pass over each chain's
  own slice, run in parallel on the chain's own place (about 0.1% of a
  1000-sweep chain's traffic).

Read this benchmark's multinode numbers as a placement/coherence stress test,
not an achievable-bandwidth measurement.
