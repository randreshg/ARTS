# fft

*Recursive Cooley-Tukey FFT — one big datablock, sliced by offset/step at
every recursion level, never copied.*
Source: `third_party/ocr-apps/apps/fft/ocr/{fft.c,verify.c}` (~640 + ~150 lines).

## Overview

Computes the discrete Fourier transform of an `N`-point impulse signal
(`x[1]=1`, else 0) via decimation-in-time Cooley-Tukey: `fftStartEdt(N)`
recursively splits into two `N/2` sub-transforms (even/odd samples, addressed
by doubling `step` and shifting `offset` — never by copying data) until the
block size drops to `serialBlockSize`, at which point the base case runs a
purely serial (function-call, no-EDT) recursive `ditfft2` down to size 1. On
the way back up, one `fftEndEdt` per split level performs the radix-2
butterfly combine, itself farmed out to `fftEndSlaveEdt` slave tasks chunked
by `serialBlockSize` elements. The whole tree operates in place on a single
shared datablock.

The input signal is a pure function of its index, so it is generated in
parallel by the tasks that own it: `fftInitEdt` (a FINISH task) fans out one
`fftInitSliceEdt` per `serialBlockSize` range, each writing its own slice, and
the transform waits on that scope. No rank does an `O(N)` pass alone.

The printed `FFT checksum` (sum of `|Re|+|Im|` over all outputs) is the result
scalar; the top level's combine slaves each sum their own share of the output
into their own slot of the block and `finalPrintEdt` adds the slots, so the
scalar costs no pass of its own. `fftVerifyEdt` recomputes the whole transform
serially and compares it point by point — a reference implementation rather
than part of the work, and at any interesting size the largest single task in
the program, so it runs only when a `v` argument asks for it and never in a
campaign. **The row therefore carries no in-program self-check in either
tier** — unlike its restructured twin `fft_dist`, which validates every bin
against the closed-form spectrum and prints `FFT_DIST INVALID` on a mismatch —
so the driver's cross-configuration consensus vote on that checksum, at the
row's `tolerance: 0.001`, is the entire correctness gate for `fft` and
`fft_hinted`. Unlike a pure task-churn probe, every leaf and slave EDT does real
floating-point work, so the app stresses fine-grain task/DB scheduling *and*
per-task compute, with one very large, heavily-shared datablock at its center.
The catalog also carries a restructured twin, `fft_dist` — a Bailey four-step
transpose rewrite whose tile count is its decomposition and whose transpose is
exchanged pair-wise between places.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `power` | `N = 2^power`, the transform length; **`1 <= power <= 31`**, loud-failing outside | required — one to four arguments, else usage error + shutdown | ✓ parsed in `parseOptions`/`mainEdt`, carried in EDT paramv structs to every rank — multinode-safe |
| `serialBlockSize` | recursion cutoff and grain: below this size `fftStartEdt` computes serially via `ditfft2`; also the combine-slave chunk and the init-slice chunk | `SERIAL_BLOCK_SIZE_DEFAULT` = 1024·16 = 16384 | ✓ any optional argument that is neither `v` nor `t`; must be a power of two in `[1, N]` |
| `verify` | run `fftVerifyEdt`, the serial reference recomputation | off | ✓ an optional argument beginning with `v` |
| `trace` | the upstream per-task trace: six `ocrPrintf` sites inside the recursion (four in `fftStartEdt`, two in `fftEndEdt`), i.e. `≈ 5.2·10^5` lines at `power = 30`, each a `write()` on the one stream every worker shares | **off** | ✓ an optional argument beginning with `t` |

The optional arguments are order-free: `fft 30`, `fft 30 v`, `fft 30 4096`,
`fft 30 4096 v` and `fft 30 t` all parse. `parseOptions` additionally refuses
a combination whose root combine fan-out `(N/2)/serialBlockSize` exceeds
`MAX_FANOUT` (65536), because that fan-out is built on a worker stack.

The per-task trace is off in every campaign cell: the campaign passes `power`
alone, so nothing inside the recursion writes to stdout and a whole run prints
exactly seven lines — `Power`, the data block's size, the init-slice count,
`Creating iteration child`, `Final print EDT`, `FFT checksum` and the shutdown
line. `t` exists so that the trace's share of the `[E2E]` window stays
measurable by A/B instead of being unbounded and unavoidable.

The knobs that upstream carried as unreachable constants — `iterations`
(forced 1, its `!=1` branch dead), `verbose` (forced true) and `printResults`
(forced false, a whole-array stdout dump) — are gone; their behaviour at the
values they were pinned to is what the program now does unconditionally.

## Structure

Let `d = log2(N) - log2(serialBlockSize) = power - 14` at the default grain be
the number of split levels (`d = 0` when `N ≤ serialBlockSize`).

| object | count | size |
|--------|-------|------|
| `fftInitSliceEdt` | `2^d` (one per `serialBlockSize` range of the signal) | — |
| `fftStartEdt` | `2^(d+1) - 1` (internal splits `2^d - 1`, leaves `2^d`; `= 1` when `d = 0`) | — |
| `fftEndEdt` | `2^d - 1` (one per internal split node) | — |
| `fftEndSlaveEdt` | `d · 2^(d-1)` (`= 0` when `d = 0`) | — |
| `mainEdt` / `fftInitEdt` / `fftIterationEdt` / `finalPrintEdt` | 1 each; `fftVerifyEdt` only when the reference is asked for | — |
| EDTs total | `3 + 4·2^d + d·2^(d-1)` (`= 7` when `d = 0`), plus 1 with the reference | — |
| Datablocks | **1, constant regardless of `N`** — plus 2 verify blocks when the reference is asked for | data block `12N` bytes plus one `double` per top-level combine slave; each verify block `4N` bytes |
| Events | `5·2^d + 1` (`= 6` when `d = 0`) — finish latches + idempotent output events per FINISH create, plus 1 when the verify EDT runs | — |
| EDT templates | 8 created (7 app + 1 verify), all destroyed | — |

Worked numbers with the reference off, which is how the row runs: `power = 6`
(`N = 64 ≤ serialBlockSize`, `d = 0`) 7 EDTs, 1 DB; `power = 30`
(`N = 1,073,741,824`, `d = 16`) 786,435 EDTs over a 12 GiB data block carrying
32,768 combine slaves' slots, measured at 13 GiB resident. Each `+1` on
`power` roughly doubles the exponential terms; DB *count* never changes.

Counter cross-check: **pending** — the previously verified pair (1 node,
`power=6` vs `power=15`) predates the parallel generation phase, which adds
`2^d + 1` EDTs and one FINISH pair of events. Re-measure against the formulas
above before quoting counter absolutes.

## Wiring

Every `fftInitSliceEdt`/`fftStartEdt`/`fftEndEdt`/`fftEndSlaveEdt` node in the
*entire* tree operates on the *same* one data datablock, sliced by
`offset`/`step`/local-`N` pointer arithmetic rather than fresh per-level blocks
— there are never more than 3 DBs live for the whole run. Every one of those
nodes declares `DB_MODE_RW` on it (create-time dependency arrays default to
RW; explicit `ocrAddDependence` calls also ask for RW), even though sibling
subtrees and init slices only ever touch disjoint offset ranges — the
coherence layer serializes at whole-DB granularity regardless. This single
datablock is therefore the app's sole and severe contention point: at the
widest wavefront up to `2^d` instances contend for RW on it simultaneously
(65,536 at `power = 30`). `fftInitEdt`, `fftIterationEdt` and `finalPrintEdt`
hold it RO/CONST instead. When the reference is asked for, `fftVerifyEdt`
reads the finished block RO and owns two fresh verify blocks exclusively.
End-to-end: `mainEdt` creates the block and both FINISH drivers, then wires
`fftInitEdt`'s output event into slot 1 of `fftIterationEdt` and launches
`fftInitEdt` last, so no consumer of the generation scope is wired after that
scope can complete; `fftIterationEdt` FINISH-wraps the whole tree and its
output event triggers `finalPrintEdt` (or, with the reference on,
`fftVerifyEdt` first), which destroys the data block and shuts down.

## Flow

The generation phase runs first: `fftInitEdt` creates `2^d` slice tasks, each
zeroing its own `serialBlockSize` range and writing the impulse if it owns
index 1, and its FINISH scope gates the transform. The tree then unfolds
top-down: each `fftStartEdt(N)` immediately creates its two `N/2` children and
(if not a leaf) an `fftEndEdt` gated on both children's nested FINISH scopes
plus the data block — so descent (splitting) and ascent (combine) are two
passes over the same balanced binary tree of depth `d`, the ascent strictly
following each subtree's completion. Parallel width during descent is `2^L` at
split level `L` (max `2^d` at the leaves); each leaf then runs a fully serial
`ditfft2` of size `serialBlockSize` with no further EDT parallelism inside it.
During ascent, each `fftEndEdt` at level `L` fans out to up to `2^(d-L-1)`
slaves. `mainEdt` itself is a short rank-0-only preamble — allocate the block
and create templates and the two drivers; it touches no element of the signal,
and there is no serial `O(N)` phase anywhere in the timed window.

## Placement (base)

`fft.c` carries an `OCR_APP_OPTIMIZED_PLACEMENT` guard, but in the base build
the guarded token expands to `NULL_HINT` **without evaluating its arguments**,
so base passes `NULL_HINT` on every EDT and DB create and makes none of the
affinity calls. Effective policy: **EDTs** → runtime round-robin (per-creating-
rank atomic counter, modulo rank count), so a node's two children, its
`fftEndEdt` and every init slice scatter across arbitrary ranks; **DBs** →
home = creating rank, and since there is only ever one data block (created by
`mainEdt` on rank 0) that home is rank 0 for the whole run. Consequence:
nearly every RW acquire of that one `12N`-byte block is a remote, whole-DB
migration — the app never expresses the locality that exists in principle
(each subtree, and each init slice, only touches a disjoint slice).

## Placement (hinted)

**A `hinted` target IS built** (`fft_hinted`, `HINTED_PLACEMENT` in
`benchmarks/apps/CMakeLists.txt`; catalog `hinted: true`). The layer was
withheld on 2026-08-19 on the argument that containing a single
per-node-exclusive RW block can only reproduce the one-node number; the
measurement below reversed that: containment is not a horizontal line, because
the base tier's multinode cells degrade far below their own one-node cell and
the layer holds the anchor where base does not. What the layer does is
containment, never distribution — the restructured version carries the
distribution story.

As-born is placement-blind, and for this program that is structurally fatal at
multinode: the entire recursion slices ONE shared data block acquired
`DB_MODE_RW` (see Wiring), write permission is exclusive at rank granularity,
and a round-robin-scattered tree therefore cannot compute in parallel across
ranks — it can only hand the whole `12N`-byte block from rank to rank, paying a
full transfer per hop.

The layer (`fftRangeEdtHint` in `fft.c`, **7 create sites**: the root start,
both children, the end EDT, both slave forms, and the init slice) places every
`fftInitSliceEdt` / `fftStartEdt` / `fftEndEdt` / `fftEndSlaveEdt` by the part
of the transform it owns — its output offset — so the tasks that revisit a
region keep returning to the same place, while the offsets, being a partition
of the transform, keep every place equally loaded. Hints cannot give this
program a multinode decomposition, because the single-RW-block structure IS
the program; what they can do is stop the block from chasing the recursion
around. Measured at power 28 over 15 workers a node, before the slave-key
repair below: base runs 8.96 s at one node and 304.26 s at eight, this layer
8.85 s and 61.90 s (a single unrepeated pair — re-measure interleaved before
quoting it as an A/B result).

**Balance.** A combine slave writes `X[k]` and `X[k+N/2]` alike, so its share
of a subtree is two ranges, not one. Keying it on `offset + kstart` named only
the low half of the subtree, which left the top `log2(P)` combine levels using
exactly half the machine; the key is `offset + 2*kstart`, which walks the
subtree's *whole* range monotonically at stride `2·serialBlockSize`, so it is
both exactly balanced and still clustered. Combine-slave load (max/mean over
524,288 slaves at `power = 30`):

| ranks | low-half key | `offset + 2*kstart` |
|-------|--------------|---------------------|
| 2 | 1.0625 | 1.000 |
| 8 | 1.1875 | 1.000 |
| 32 | 1.3125 | 1.000 |

Init slices and leaf starts are exactly balanced at every `P` by construction
(their keys are a uniform stride over `[0, N)`). The one residue is the
recursion's leftmost spine: an internal `fftStartEdt` and its `fftEndEdt` are
keyed on their subtree's *low corner*, so the `log2(P)+1` largest subtrees all
land on rank 0. Those tasks only create children — whole-program EDT-count
load is 1.0004 max/mean at `P = 32` — and no key fixes it without separating an
end EDT from the range it owns.

Placing by the creating task instead would keep the whole recursion on the one
place the root started on — perfectly local, and using a single node of
however many the machine has, which wins by not using the machine. A real
distributed FFT is the `restructured` version's job (`fft_dist`).
`pdCount <= 1` returns `NULL_HINT`, so a single-node run is placement-identical
to base (not cost-identical: the hinted binary still makes one
`ocrAffinityCount` call per create, ~7.9·10^5 at `power = 30`, which the
recorded one-node pair puts inside noise).

## Sizing

`power` is the size dial and `serialBlockSize` the grain dial, and together
they set the width: the init-slice frontier and the leaf frontier are both
`N / serialBlockSize`, and the root combine fans out to `(N/2) /
serialBlockSize`. At the default grain that is `2^(power-14)` and
`2^(power-15)`. To clear a 3,456-worker frontier: `2^power / serialBlockSize ≥
3456`, i.e. `power ≥ 26` at the default grain (`power = 30` gives 65,536 =
18.96 × 3,456). Raising `power` multiplies the split/combine tree width by 2
per level; `power` below ~15 collapses to `d = 0` (a single leaf, no
parallelism).

**Two ceilings sit below the memory one, and both are now checked rather than
silent.** The binding one is the worker stack: an `fftEndEdt` builds its whole
fan-out as two slave-count-sized VLAs — `ocrGuid_t slaveGuids[(N/2)/sbs]` (8 B
per slave, the array actually used) and `u64 slaveParamv[5·(N/2)/sbs]` (40 B
per slave, upstream dead code shadowed by the per-iteration
`endSlavePRM_t slaveParamv` and never read, but still occupying the frame).
That is 48 B per slave whether or not the dead array is ever removed: 1.57 MiB
at `power = 30`, 3.15 MiB at 31 and 6.29 MiB at 32, against the profiles'
8 MiB worker stack — hence `MAX_FANOUT = 65536` on `(N/2)/serialBlockSize`.

`MAX_POWER = 31` is **not** an arithmetic limit of any index in the program:
every size-scaled induction variable is `u64` (`fftEndSlaveEdt`'s `k` was the
last `u32` one and was widened), and `finalPrintEdt`'s `u32 i` sweeps only the
`N ≤ serialBlockSize` branch. It is deliberate headroom one doubling below the
fan-out ceiling, loud-failing in `parseOptions` rather than being discovered as
a stack overrun. Memory alone would allow `power = 33`; the knob's usable
headroom above the calibrated value is one power.

**Memory (R8).** `dataBytes = 12·2^power + 8·(2^(power-1) / serialBlockSize)`
bytes, one block, on rank 0's home. At `power = 30` and the default grain that
is **12.885 GB** (13 GB measured resident), node-count invariant — the block is
never replicated, only migrated. Well inside a 190 GB single-node budget; the
campaign power is `calibration pending` against the Dane cell timeout rather
than against memory.
