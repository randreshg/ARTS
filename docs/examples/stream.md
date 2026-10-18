# stream_example

*McCalpin's STREAM kernels over arrays split one partition per rank: each
phase runs one kernel EDT per rank on that rank's blocks, joined by a latch
event and timed on rank 0.*
Source: `examples/stream/stream.c`, written against the public `arts.h` alone.

## Overview

The four STREAM kernels — copy `c = a`, scale `b = s·c`, add `c = a + b` and
triad `a = b + s·c` — run `NTIMES` times each, in that order, over three
arrays of `STREAM_ARRAY_SIZE` doubles divided evenly across the ranks. Every
rank holds its share of `a`, `b` and `c` as three data blocks. One phase is
one kernel over every rank's share: rank 0 creates a kernel EDT on each rank
and a latch the kernels decrement, and the EDT the latch wakes records the
phase's elapsed time and starts the next phase. After the last phase rank 0
prints the bandwidth table (best, average, minimum and maximum time per
kernel, excluding the first repetition), the values the kernel recurrence
predicts for one element, and the line `Solution Validates (mathematical
check)`. That line states the program's own closed-form check: it does not
read the arrays, so the run's result is its timing and its completion.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`stream_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

The program takes no run-time argument; the problem is fixed at compile time
and each value can be overridden with `-D`:

| macro | default | meaning |
|---|---|---|
| `STREAM_ARRAY_SIZE` | 10 000 000 | elements of each array, summed over all ranks |
| `NTIMES` | 10 | repetitions of each kernel; at least 2, since the statistics skip the first and divide by `NTIMES − 1` |
| `SCALAR` | 0.42 | the `s` of scale and triad |

The run refuses more than 256 ranks (the metadata block's GUID tables) and an
array too small to give every rank an element, printing an error and shutting
down. The smoke test is the same program compiled with
`STREAM_ARRAY_SIZE=200000 NTIMES=3`.

## Structure

For `N` ranks:

| object | count |
|---|---|
| array blocks `a`, `b`, `c` | `3N`, each `STREAM_ARRAY_SIZE / N` doubles |
| metadata block | 1: every array's GUID and the `4 × NTIMES` phase times |
| `init_node` | `N` |
| `start_benchmark` (the collector) | 1, with `3N` slots |
| `run_phase`, `phase_done`, latch events | `4 × NTIMES` each |
| `stream_kernel` | `4 × NTIMES × N` |
| `finalize_benchmark` | 1 |

## Wiring

The collector takes every rank's three arrays, RO, on slots `3n … 3n + 2`;
it reads only their GUIDs. A `run_phase` takes the metadata block RO. Each
`stream_kernel` takes its rank's `a`, `b` and `c` RW — which array a kernel
writes depends on the kernel, so all three slots are RW — and decrements the
phase's latch, created with a count of `N`. `phase_done` takes the latch as a
control slot and the metadata block RW, since it stores the phase time;
`finalize_benchmark` takes the metadata block RO. Every block is released
before the satisfy that lets another EDT acquire it: `init_node` releases its
new arrays before it hands them to the collector, the collector releases the
metadata block before it wires the first `run_phase`, a kernel releases its
arrays before it decrements the latch, and `phase_done` releases the
metadata block before it wires the next phase. The read-only holders release
too before the satisfy that leads to `finalize_benchmark`, which destroys the
blocks: the collector releases the arrays once it has recorded their GUIDs,
and `run_phase` releases the metadata block once it has wired the kernels.

## Flow

`main_edt` checks the rank count, creates the collector on rank 0 and an
`init_node` on every rank. Each `init_node` creates its rank's arrays, fills
them (`a = 2.0` after STREAM's timing-precheck doubling, `b = 2.0`,
`c = 0.0`) and satisfies its three collector slots. The collector creates the
metadata block, records the GUIDs, prints the preamble and starts phase
`(0, copy)`. Each `run_phase` takes the start time, creates the latch, one
kernel per rank and the `phase_done` that waits on the latch. `phase_done`
stores the elapsed time, destroys its phase's latch, and wires the next
phase — copy, scale, add, triad, then the next repetition — or, after the
last, `finalize_benchmark`. That EDT prints the table, the predicted values
and the validation line, destroys every array and the metadata block, and
shuts the runtime down. artsrun reads the completion line as the row's
marker and times the run by the runtime's `[E2E]` stamp.

## Placement

The collector, every `run_phase`, every `phase_done` and the finalizer run
on rank 0, as does the metadata block they pass along. Rank `n` runs its
`init_node` and every kernel on its share, and its arrays are created there
with no hint, so under the default no-hint home (the creating rank) each
kernel's blocks are home-local and a phase moves no payload. The one bulk
transfer is the collector's: its RO slots acquire every rank's arrays, so on
more than one rank, rank 0 fetches a copy of each other rank's arrays once,
before the first phase.

## Sizing

The total array size is fixed, so adding ranks shrinks each rank's share:
a node-count sweep is strong scaling. A run allocates `24 ×
STREAM_ARRAY_SIZE` bytes of arrays across the ranks, plus rank 0's copies
from the collector. A phase's time, measured on rank 0, includes creating the
`N` kernels and the latch round trip, so for small shares it measures task
and message latency rather than memory bandwidth. The catalog row runs the
compile-time default and pins no value: the program prints none that is
computed from the arrays.
