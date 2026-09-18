# RSBench_intel_dist

*The restructured tier of `RSBench_intel`: the same resonance-lookup workload
over the same H-M instance, with the producer, the dataset generation and the
result reduction all decomposed. The lookup range is split into `-r`
partitions, each with its own producer chain and its own `-b`-wide sync batch;
the cross-section dataset is generated in parallel by one task per nuclide and
homed where that task ran; and the lookups' results — discarded by the base
row — are summed into one integer checksum.*
Source: `third_party/ocr-apps/apps/RSBench/refactored/ocr/intel-dist/src/`
(7 C files; `main.c` builds and drives the graph, `init.c` / `material.c` are
thin wrappers over the shared generator header `../../rs_generate.h` — the same
file the base row compiles, so the instance is identical by construction —
`rs_kernel.c` is the per-lookup compute, unchanged from the base row).

## Overview

Read `appdocs/RSBench_intel.md` first for the physics and for the shape this
row departs from. The base row is a Master/Worker program whose master is a
literal: `nprocs` is hardcoded to 1, so one rank's C loop creates every lookup
chain for the whole machine and every batch is a global FINISH barrier. It
anti-scales, and no placement hint can reach either fact — which is why this is
a separate program rather than a hinted flavour. (Data generation is *not* one
of the differences: both rows generate in parallel from the same shared
index-seeded generator, behind the same one-task count pre-pass. What the base
row keeps is the block *creation* in one task, so its dataset stays homed on
one rank unless the hint layer spreads it; here creation, filling and homing
are all the generator's.)

Three changes, and the rest of the program is the base row's:

1. **`-r` lookup partitions.** Partition `p` owns `[⌊pL/P⌋, ⌊(p+1)L/P⌋)` and
   runs on policy domain `p % ranks`. `P` is an argument, not the rank count,
   so the decomposition is node-count invariant and so is every number the run
   prints. Every EDT of a partition's chain is created with a
   "here" affinity hint, so a chain stays on the place it started on.
2. **Batches are per partition.** The FINISH-scoped batch survives — it is
   what bounds in-flight work — but it now scopes one partition's `b` chains
   instead of the whole machine's. Partitions never wait for one another. The
   per-lookup chain `rankLookup → macroxs → microxsAggregator`, and its
   `5 + 3·num_nucs` CONST acquires, are unchanged.
3. **The dataset is sharded and generated in parallel.** One `nuclideGen` task
   per nuclide and one `materialGen` per material create *and fill* their own
   blocks. Nuclide `i` is therefore homed on place `i % ranks`. It is
   deliberately *not* replicated per partition: replication would delete the
   remote-acquire regime the row exists to measure.

   The *values* are the base tier's, not new ones: both programs include the
   same `rs_generate.h` — the same `rs_rand_bits`/`rs_rand_unit` mixer, the
   same stream ids, the same `RS_INDEX3(i, j, k)` draw addressing for poles,
   windows, pseudo-K0RS and concentrations, and the same count multinomial. The multinomial is a histogram of one draw stream over the
   whole nuclide index — a bin depends on every draw, so it cannot be produced
   from the index of the thing that reads it — and it therefore runs once, in a
   pre-pass `countGen` task, whose two count arrays the `n` per-nuclide
   generators consume through a sticky event. Both tiers describe the same
   instance.

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `-l <lookups>` | XS lookups; sizes the whole graph (3 EDTs each) | 10,000,000 | ✓ parsed in `mainEdt`, carried inside the `Inputs` datablock to every consumer — multinode-safe |
| `-r <parts>` | lookup partitions: independent producer chains, partition `p` pinned to place `p % ranks`. **Half of the width knob** | 1024 | ✓ parsed, validated `≥1`, carried in `Inputs` |
| `-b <batch>` | lookups per sync batch *within one partition* — the batch is a FINISH scope and a partition's next batch starts only when it drains, so this is one partition's in-flight width. **The other half of the width knob** | 1024 | ✓ parsed, validated `≥1`, carried in `Inputs` |
| `-s small\|large` | H-M benchmark size; `small` also forces `n_nuclides=68` | large (355 nuclides) | ✓ |
| `-n <n>` | nuclide count, overrides whatever `-s` set | 355 (68 with `-s small`) | ✓ parsed, and **validated**: the H-M material tables name fixed nuclide IDs (≤67 small, ≤354 large) and are picked by count alone, so any value other than exactly 68 or ≥355 is rejected with a message naming the constraint (the base row accepted it and indexed out of bounds) |
| `-p <poles>` | average poles per nuclide — sizes each pole DB | 1000 | ✓ |
| `-w <windows>` | average windows per nuclide — sizes each window DB | 100 | ✓ |
| `-d` | disable Doppler broadening (skip the temperature-dependent Faddeeva kernel) | Doppler ON | ✓ |
| `-t <threads>` | "OpenMP thread count" | 1 | ⚠ **dead**, as in the base row: parsed, range-checked, echoed in the results banner, never read by the graph. Passing it prints a one-line warning that parallelism comes from `-r` and `-b` |
| *(none)* `n_mats` | material zones — the H-M reactor model has exactly 12 | 12 | ✗ compile-time only: the per-material nuclide tables are hardcoded for exactly 12 zones |
| *(none)* `numL` | Legendre moments per nuclide (pseudo-K0RS array width) | 4 | ✗ compile-time only, as in both other RSBench ports |

Height knobs (`-s`, `-p`, `-w`, `-d`, `numL`, `n_mats`) are at the benchmark's
published defaults; `-l` is the run-length axis and is sized to the anchor
window — `calibration pending`, as are `-r` and `-b`.

## Structure

Let `n` = `n_nuclides`, `m` = `n_mats` (=12), `L` = `lookups`, `P` = `-r`,
`b` = `-b`, `S_p = ⌊(p+1)L/P⌋ − ⌊pL/P⌋` (a partition's share, so shares differ
by at most one lookup) and `G_total = Σ_p ⌈S_p / b⌉` (batches over all
partitions, ≈ `L/b`).

| object | count | size |
|--------|-------|------|
| handle DBs (`InputsH_0`, `globalH`, `InputsH`, `dataH`, `templatesH`, `timers`, `psumGuids`) | 7 | tens of bytes each; `psumGuids` is `P` GUIDs |
| `dataH`'s 8 index arrays (n_poles, n_windows, 3× per-nuclide GUID arrays, numNucs, 2× per-material GUID arrays) | 8 | `int`/`ocrGuid_t` arrays of `n` or `m` elements |
| per-nuclide DBs: pole, window, pseudo-K0RS | `3n` | Pole ≈72 B ×~`-p`/nuclide; Window 32 B ×~`-w`/nuclide; K0RS `numL`×8 B |
| per-material DBs: nuclide-ID list, concentration list | `2m` | `num_nucs[i]`×4 B / ×8 B |
| generator result DBs (what a generator publishes through its output event) | `n + m` | 3 GUIDs + 1 u64, or 2 GUIDs + 1 int |
| per-partition: `placeH`, `b` slot accumulators, `psum` | `P·(b+2)` | 8–24 B each |
| **DBs total** | `15 + 4n + 3m + P(b+2)` | — |
| EDTs: fixed setup and teardown (`mainEdt`, `settingsInit`, `init_InputsH`, `globalInit`, `initSpawner`, `initCollect`, `globalCompute`, `globalComputeSpawner`, `lookupReduce`, `timer`, `globalFinalize`) | 11 | — |
| EDTs: dataset generators (`countGen` + `n` nuclide + `m` material) | `1 + n + m` | — |
| EDTs: per partition — `placeInit`, `placeReduce`, and one `rankCompute` + one `rankMultiLookupSpawner` per batch | `2P + 2·G_total` | — |
| EDTs: `rankLookup` + `macroxs` + `microxsAggregator` | `3L` | — |
| **EDTs total** | `12 + n + m + 2P + 2·G_total + 3L` | — |
| explicit STICKY events (5 phase events + the count-pre-pass event + the compute-spawner event + one per batch) | `7 + G_total` | — |
| shim-materialized events (finish events for `EDT_PROP_FINISH` creates, output events for creates naming one) | `13 + n + m + 2·G_total` | — |
| **Events total** | `20 + n + m + 3·G_total` | — |

The counts are derived from the creates in `main.c`, not yet cross-checked
against the runtime's counters — unlike the base row's table, which was.
Nothing here is data-dependent in *count*: the index-seeded draws move DB
payload sizes and the per-`microxsAggregator` dependence count, never how many
objects exist.

Cross-section payload is fixed by `-s`/`-n`/`-p`/`-w`/`numL` and never moves
with `-l`, `-r` or `-b`:
`n·(p̄·72 + w̄·32 + numL·8) + n·8 + Σ_i num_nucs[i]·12` ≈ **27 MB** at the
default (355, 1000, 100, 4) — the app prints it as `Est. Memory Usage (MB)`.

## Wiring

`mainEdt → TS_settingsInit(FINISH) → TS_globalInit(FINISH) →
TS_globalCompute(FINISH) → TS_globalFinalize(FINISH)`, the base row's spine.

**Init.** `globalInit` creates `InputsH`, `dataH` and `templatesH` and one
`initSpawner`. `initSpawner` creates the six EDT templates the compute phase
reuses, the 8 index arrays, one `countGen` EDT (RW on the two count arrays,
its output event routed into a sticky event), one `initCollect` EDT with
`7 + n + m` dependences, and then the `n + m` generators — for each, the
generator's output event is wired into `initCollect` **before** the
generator's own last dependence is added, so a generator can never fire its
output event before its consumer is registered. A nuclide generator's last
dependence is the `countGen` sticky event, and `countGen`'s own dependences
are added after every generator has been created, so no count is read before
it is written. Each generator returns a small result block naming the blocks
it created; `initCollect` writes those GUIDs into the index arrays, sums the
per-nuclide pole checksums and prints `RSBench pole checksum:`.

**Compute.** `globalCompute` creates the `P` `psum` blocks, one
`globalComputeSpawner` (FINISH), the `lookupReduce` EDT (`P+1` dependences)
and the `timer`. `lookupReduce` waits on a STICKY event that the compute
spawner's output event satisfies, so it can be wired in any order: a sticky
event retains its satisfaction for a consumer that registers afterwards. `globalComputeSpawner` creates one `placeInit` per non-empty
partition, placed at `p % ranks`. `placeInit` creates that partition's
`placeH` and its `b` accumulators locally and starts the chain:
`rankCompute → rankMultiLookupSpawner(FINISH) → next rankCompute`, and after
the last batch `→ placeReduce` instead. `placeReduce` sums the `b`
accumulators into the partition's `psum`; when every partition has done so the
compute spawner's FINISH scope drains, `lookupReduce` prints
`RSBench lookup checksum:`, and the `timer` prints the results banner.

Every per-nuclide/per-material DB is written exactly once — by its generator,
before release — and is `DB_MODE_CONST` everywhere after that; no writer ever
returns to one. The only mutable blocks in the compute phase are the
partition-private accumulators, and a slot accumulator is held by at most one
live aggregator by construction (a batch is a FINISH scope and the `b` lookups
of a batch take distinct slots), so there is no shared mutable state and no
contention anywhere in the lookup path.

## Flow

Init is `n + m` concurrent generator tasks behind one `initCollect` join —
about 3.2 M index-seeded draws and 27 MB of writes, spread over every rank.
Ahead of them sits the one serial pre-pass, `countGen`: `(-p + -w)·n ≈ 390 k`
integer mixes into two `n`-element histograms. It is serial because the
multinomial is (a bin depends on every draw), and it is kept because it is what
makes this row's instance the base row's instance; it is one task, once per
run, and it is not replicated per rank. The base row's init has the same shape;
what differs is that there the blocks are created by one task and only filled
by the generators, so their homes come from the creator rather than from the
producer.

The lookup phase is `P` independent producer chains running concurrently, each
a sequence of `⌈S_p/b⌉` FINISH-scoped batches. Within a batch a chain is
strictly serial (`rankLookup` creates `macroxs` and returns; `macroxs` creates
the aggregator and returns), so the instantaneous runnable frontier is
`min(P·b, workers)` — never `3·P·b`. Between a partition's batches there is no
overlap; between partitions there is no synchronisation at all until the
compute spawner's scope drains.

`microxsAggregator` RO-acquires the pole/window/K0RS blocks of every nuclide
in its lookup's material (`num_nucs[mat]`, 5–321 by draw; mean 55.4, so
`5 + 3·55.4 ≈ 171` acquires per lookup, 968 on a fuel draw). Those blocks are
spread over all ranks by construction, so a `1 − 1/ranks` share of them is
remote — the same fine-grained remote-acquire regime as the base row, now
driven by every rank rather than fed from one.

## Placement

There is no `OCR_APP_OPTIMIZED_PLACEMENT` guard in this program and no
`_hinted` twin: placement *is* the decomposition here, so it is unconditional.

| object | count | rank |
|---|---|---|
| nuclide `i`'s three blocks and its generator | `3n` / `n` | `i % ranks` (EDT hint by index; the blocks pinned where the generator runs) |
| material `i`'s two blocks and its generator | `2m` / `m` | `(n + i) % ranks` |
| partition `p`'s `placeInit` and every EDT of its chain | `2 + 2G_p + 3S_p` | `p % ranks` |
| partition `p`'s `placeH` and `b` accumulators | `b + 1` | `p % ranks` — every accumulator acquire is local |
| partition `p`'s `psum` | 1 | homed with its *reader* (`globalCompute`'s rank): written once, remotely and in parallel, at the end of a partition's chain; read together by `lookupReduce` |
| the 8 index arrays, `dataH`, `templatesH`, `InputsH`, `globalH` | 12 | creator-homed on whichever rank `initSpawner`/`mainEdt` landed on; read-only afterwards, so a validating or invalidating protocol caches them per node |

Balance: partitions are dealt round-robin over ranks and shares differ by at
most one lookup, so per-rank EDT balance is `≈ 1.0` at every rank count, and
`-r 3456` divides evenly at 1, 2, 4, 8, 16 and 32 nodes. No task creates work
for a rank other than the one it names by index, so there is no
creator-pinning funnel.

## Correctness

This row grades on the workload:

* each `microxsAggregator` folds its `macro_xs[4]` into the sum of their
  IEEE-754 bit patterns and adds it into its partition's slot accumulator;
* `placeReduce` sums a partition's `b` accumulators into its `psum`;
* `lookupReduce` sums the `P` `psum` blocks and prints
  `RSBench lookup checksum:` — the catalog's voted scalar, and the marker.

The reduction is integer addition with wraparound, so it is exact and
order-independent: the total does not depend on `-r`, on `-b`, on the node
count, on the scheduler or on the runtime. Per-lookup floating point is never
reassociated — same operations, same order, the same data everywhere.

Because the dataset generator, the per-lookup seed (`(i+1)·19 + 17`) and the
bit-pattern fold are all the base tier's, the value is **the same number the
base row prints** at equal `-s`/`-n`/`-p`/`-w`/`-d`/`-l`. That is the row's
instance-identity statement and the cheapest available check on the pin.

`RSBench pole checksum:` is an `extra_scalar` — the dataset-integrity check,
printed by the init join. It is a fold over the pole arrays only, so it depends
on `-s`/`-n`, `-p` and `numL`, and not on `-w`, `-l`, `-r` or `-b`. The base
row folds and prints the same quantity from the same generator, so this scalar
too must match between the rows.

Both pinned values are `calibration pending`: this row has not been run. Two
checks settle them cheaply — both checksums must equal `RSBench_intel`'s at the
same dataset arguments (and the same `-l`, for the lookup checksum), and the
same `-l` run at two different `(-r, -b)` pairs must print the identical pair
of checksums (partitioning, not replication).

## Sizing

`-l` sets total work; `-r × -b` sets width. Doubling `-l` doubles the EDT count
(`3` per lookup, `+2` per additional batch) and the run time; DB count and
payload are fixed by `-s`/`-n`/`-p`/`-w` and never move with `-l`.

- **Width `W = P · b`.** A partition's chain contributes at most one runnable
  EDT, a partition has `b` chains live, and partitions are independent.
  Structure is spawn-and-join per partition, so the width rule asks for at
  least twice the widest geometry's worker total. `-r 3456 -b 8` gives
  `27648 = 8.0 ×` the 32-node Dane worker total, holds `3456/ranks` partitions
  on every rank exactly, and costs `2/b = 0.25` batch EDTs per lookup; any
  `b ≥ 2` at `-r 3456` clears the floor. The values are `calibration pending`.
- **Memory.** Payload ≈ 27 MB (above) and fixed. Two terms move relative to
  the base row, and only one of them is small:
  - *EDT count.* `3 + 2/b` EDTs per lookup, so 3.25 at `-b 8` against the base
    row's `3 + 2/6912 = 3.0003` at `-b 6912` — **+8.3 %**, not the 17 % an
    earlier draft claimed.
  - *Simultaneous in-flight population.* The base row has one global batch of
    `-b 6912` lookups live; this row has `P·b = 27648` — **4×** — each holding
    `6 + 3·num_nucs ≈ 172` outstanding CONST acquires, i.e. ≈ 4.8 M concurrent
    acquire records against the base row's ≈ 1.19 M. At an assumed ~64 B per
    record (the dependence slot plus the runtime's per-acquire bookkeeping —
    an assumption, not a measurement) that term alone is ≈ 0.3 GB, but it is
    the term that grew and it is not covered by extrapolating the EDT count.

  The base row measured 26.6 GB at one node at a larger `-l`; that figure
  cannot be scaled by the EDT ratio alone, so the one-node RSS of this row is
  `calibration pending` and must be read off the first cell. `-b` is the knob
  that trades it against batch overhead: halving `-b` halves the in-flight
  population and costs `2/b` more EDTs per lookup.
- **What the row is for.** The base row's exhibit is that a central producer
  and a global batch barrier make every added node cost time; this row keeps
  the same per-lookup coherence traffic — ~171 fine-grained CONST acquires,
  a `1 − 1/ranks` share of them remote — and removes the producer and the
  global barrier, so what it measures is the coherence families' behaviour
  under that traffic rather than the funnel in front of it.
