# stream_dist

*The restructured row of `stream`: the same four STREAM kernels over the same
`numThreads` independent chains and the same argument triple, but each chain's
`a`/`b`/`c` arrays are created once and written in place, and every task and
array is placed by an explicit index map.*
Source: `third_party/ocr-apps/apps/stream/ocr/stream_dist.c` (595 lines).

## Overview

**Provenance, stated plainly.** This is not an ARTS-side redesign. The 2016 OCR
port of STREAM shipped two variants side by side: a single-assignment one, in
which every kernel creates its output array (that is the `stream` row), and one
with three persistent per-chain arrays written in place (`stream_org.c`).
`stream_dist` is the second variant, conformance-adapted the same way the base
row was — argv-driven sizes, GUIDs and sizes carried through paramv instead of
file-scope statics, the `0.42` recurrence multiplier, the added result scalar —
plus one addition of its own: an explicit `tid % affCount` affinity map on
every EDT and every DB. The axis it isolates is therefore **allocator and
datablock churn against persistence**, at identical decomposition and identical
placement outcome, not a different parallel structure.

Both rows take the same argument triple and print the same validation line in
the same format, which is the check that they are one application: every chain
verifies its own final slice against the closed form of the kernel recurrence,
returns the count it verified, and `finalize` sums the counts into
`Solution Validates: all <n> elements match expected value` with
`n = perThreadSize * numThreads`. `STREAM_RESULT a[0] = <value>` rides along as
an extra scalar and is identical to the base row's at the same arguments — the
per-element recurrence depends on the sweep count and nothing else.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `streamArraySize` | total element count, split evenly across chains | source `#define` 9000000; build `EXTRA_DEFINES` 1000 | ✓ parsed in `mainEdt`, forwarded through paramv — multinode-safe |
| `argv[2]` = `numThreads` | number of independent chains (DAG width) | source 32; build 2 | ✓ same path |
| `argv[3]` = `nTimes` | sweeps per chain (DAG depth) | source 1000; build 3 | ✓ same path (`NTIMES` is `#ifndef`-guarded) |

Non-divisible sizes truncate to `perThreadSize = streamArraySize / numThreads`
with a stderr warning. Compile-time only: `SCALAR` (0.42), `STREAM_TYPE`
(`double`). The row must carry the *same* triple as `stream`: differing only in
the sweep count would make the pair a comparison of two problem instances, and
the pinned element count cannot see it (it is sweep-count invariant).

## Structure

Let `T = numThreads`, `K = nTimes`, `pts = perThreadSize`.

| object | count | size |
|--------|-------|------|
| EDTs total | `T·(2 + 5K) + 2` — per chain: `mainLet`, `K` `loop`s, `4K` kernels, one `check`; plus `mainEdt` and `finalize` | — |
| DBs | `T·4` — three persistent arrays per chain plus its 16-byte result record. **Node-count invariant and sweep-count invariant**: the sweep creates and destroys nothing | `pts·8` bytes each; the record is 16 B |
| Events | `T·(4 + 4K)` — per chain: the ONCE completion event, the first `loop`'s output event, its finish event, one output event per kernel per sweep, and `check`'s output event | — |
| EDT templates | 8 (`mainLet`, `loop`, `copy`, `scale`, `add`, `triad`, `check`, `finalize`) | — |

Against the base row at the same arguments: identical EDT and event counts,
`T·4` DBs instead of `T·(2 + 4K)`. That difference — `4K` allocate/first-touch/
free cycles per chain, removed — is the whole of what this row measures.

Worked numbers: calibration pending (the orchestrator sizes `argv[1]` and
`argv[2]`).

## Wiring

- `mainEdt` creates the 8 templates and one `finalize` EDT
  (`depc = numThreads`, paramv `[numThreads, perThreadSize]`), then per chain a
  ONCE event wired RO into `finalize` slot `i` and one `mainLet` EDT carrying
  the event and template GUIDs and the sizes through paramv. Every `mainLet` is
  created with the chain's EDT affinity hint.
- `mainLet` creates `a`, `b`, `c` **once** with the chain's DB affinity hint,
  fills them (`a=2.0`, `b=2.0`, `c=0.0`) in place — this is the whole of the
  initialisation, `T`-way parallel by construction, no serial init pass
  anywhere — and creates the chain's `EDT_PROP_FINISH` `loop` (`iter=0`) with
  all three arrays wired RW.
- `loop(iter)` creates the four kernels with the chain's hint and wires
  `a`(RO)+`c`(RW)→`copy`, `a`(RO)+`b`(RW)→`scale`,
  `a`(RO)+`scale_out`(RO)+`copy_out`(RW)→`add`,
  `a`(RW)+`scale_out`(RO)+`add_out`(RO)→`triad`. Each kernel returns the block
  it already holds, so an output event carries an existing GUID rather than a
  new one. Then either the next `loop` (all three arrays forwarded RW) or, on
  the last sweep, the chain's `check` EDT with `triad_output` RO and `check`'s
  output event wired into the chain's ONCE event. The chain-starting
  dependences are the last statements of `loop`, so every ONCE waiter is
  registered before its producer can run.
- `check` replays the scalar recurrence `K` times (closed form, constant work
  per chain), counts its own `pts` elements against it, and returns a 16-byte
  `{matched, a[0]}` record homed at the chain's place. Unlike the base row's
  `check` it destroys nothing: the three arrays are this program's persistent
  state.
- `finalize` sums `T` records and shuts down. It holds `T·16` bytes, not the
  result.

## Flow

Identical in shape to the base row: `T` independent chains, each a serial
`K`-deep pipeline whose instantaneous width is 2 (`copy`∥`scale`) then 1
(`add`, `triad`, `loop`), joining only at `finalize`. Frontier `T` to `2T`,
never below `T` before the join. What differs is inside a sweep, not between
sweeps: no `ocrDbCreate`/`ocrDbDestroy` occurs after `mainLet`, so a sweep is
four kernel dispatches over blocks that already exist and already live on the
chain's place.

## Placement (base)

This row has one tier and its placement map is unconditional — there is no
`OCR_APP_OPTIMIZED_PLACEMENT` guard in the source, and none belongs in a
restructured row.

`getAffinityHints(tid, …)` resolves PD `tid % affCount` (`affCount` from
`ocrAffinityCount(AFFINITY_PD, …)`) and builds an `OCR_HINT_EDT_AFFINITY` hint,
a `OCR_HINT_DB_AFFINITY` hint, or both. It is applied to: every `mainLet`
create, all three array creates, every `loop` and kernel create, the `check`
create and its record DB. `ENABLE_EXTENSION_AFFINITY` is defined for every
benchmark app, so the map is live rather than compiled out; without the
extension the helper degrades to a default-initialised hint.

Balance and coverage: with `T` chains over `N` places each place owns exactly
`T/N` chains when `N | T` (the sweep's rank counts satisfy this at the
prescribed width) and `floor`/`ceil` otherwise — a property of the program, not
of any runtime default. Every dependence edge of a chain is intra-place: the
arrays are homed at the chain's place at creation and never move, and every
task that touches them is placed there. The only unhinted objects are the
per-chain ONCE events and `finalize`.

`printTimes` reports only the samples the reporting process actually took, by
the same sign encoding as the base row: `loop` stores the negated start stamp,
`triad` closes the cell only when it finds a negative value there, and
`printTimes` keeps only strictly positive cells. A sweep started here and ended
elsewhere stays negative; one ended here but started elsewhere stays the
`calloc` zero; both fall out, so a half-recorded sweep can never enter the
average as a raw epoch stamp. With no complete cell the function states `No
complete kernel timing: MB/s not reported` rather than dividing by a zero
minimum, and on a process with no sample array at all (it exists only where
`mainEdt` ran) it states `No kernel timings on this process: MB/s not reported`
rather than dereferencing a null pointer. It is a diagnostic; the harness reads
the validation line.

## Sizing

Same knobs and the same grain arithmetic as the base row — `pts =
streamArraySize / numThreads` is both the DB payload and the per-kernel work
size, `numThreads` is the width knob, `nTimes` is depth only — and the row must
be run at the *same* triple as `stream`.

- **Width**: set `numThreads` to the widest geometry's total worker count (or a
  multiple); frontier `1.0x`–`2.0x`, never below `1.0x` before the join.
- **The 512 KiB per-chain cliff applies here too** (65,536 `double` elements per
  array): keep `pts·8` clearly on one side of it. Below it the working set is
  cache-resident and the program stops being a memory benchmark.
- **Memory**: `3 · streamArraySize · 8` bytes, flat for the whole run and
  node-count invariant, plus `T·16` bytes of records. There is no allocator
  backlog to add, which is exactly why this row's resident set is lower and
  steadier than the base row's at the same arguments. Absolute numbers at the
  campaign size: calibration pending.
- The verification adds one pass over each chain's own slice, in parallel on the
  chain's own place, and a `T·16`-byte join.
