# RSBench_intel

*Every one of the `-l` lookups gets its own three-EDT chain and its own slice
of the cross-section dataset (355 nuclides → 3×355 small datablocks), which is
generated in parallel by one task per nuclide; each lookup's result is reduced
into one integer checksum, which is the pinned scalar and is printed after the
compute phase — see `RSBench_intel_sharedDB` for the opposite design (one big
DB per array, coarse per-thread EDTs).*
Source: `third_party/ocr-apps/apps/RSBench/refactored/ocr/intel/src/` (7 C
files; `main.c` builds and drives the whole graph, `init.c` / `material.c` are
thin wrappers over the shared generator header `../../rs_generate.h`,
`rs_kernel.c` is the per-lookup compute).

## Overview

RSBench is ANL's proxy for the multipole-representation resonance-lookup
kernel of Monte Carlo neutron transport — XSBench's sibling benchmark: the
same "sample a random (material, energy) pair and evaluate a macroscopic
cross-section" workload, evaluated with a different (window/pole-based)
kernel. This port fragments the synthetic cross-section dataset into one
datablock **per nuclide** per array (poles, windows, pseudo-K0RS each get
their own small DB — 3×355 of them at default sizing) and turns every one of
the `-l` lookups into an independent `rankLookup → macroxs → microxsAggregator`
EDT chain that computes a macroscopic cross-section vector.

The published program discarded that vector (`macro_xs` was a stack-local in
`FNC_microxsAggregator`), which left the row with no observable answer. It is
now **reduced**: each aggregator sums the four IEEE-754 bit patterns of its
`macro_xs` into a `u64`, writes it into an 8-byte datablock it creates itself,
and hands that block to its batch's `batchReduce` EDT; the reducers add the
batch totals into one run-global accumulator, and `FNC_timer` prints it as
`RSBench lookup checksum:` in the results banner. Integer addition is
associative and commutative and each lookup's own arithmetic is a fixed-order
loop inside a single EDT, so the scalar is bit-identical across node counts,
tiers, coherence arms and runtime entries. Each result block has exactly one
writer and is read once, so this per-aggregator step adds no write-write
conflict — DB-WRF-valid as well as OCR-legal.  The row is outside DB-WRF
overall on a different block: the run-global accumulator each batch's
`batchReduce` EDT then updates unordered (see the catalog's
`unordered_writes`).

The dataset itself is also observable: each nuclide generator folds the pole
data it produced into a `u64` and `FNC_initCollect` sums the `n` folds and
prints `RSBench pole checksum:` (the catalog's `extra_scalars.pole_cksum`).
`RSBench_intel_dist` computes both checksums from the same shared generator
over the same instance, so at equal `-s`/`-n`/`-p`/`-w`/`-d`/`-l` the two rows
print the identical `RSBench lookup checksum:`, and at equal `-s`/`-n`/`-p`
(with `numL`) the identical `RSBench pole checksum:`. Either row's value is
therefore checkable against the other rather than only against itself.

The banner is printed from `FNC_timer`, which hangs off the compute phase's
FINISH output event, so the marker and the scalar can only appear after the
last lookup of the last batch has been reduced. (The row previously graded on
`Lookups:` from the *input* banner and on a checksum of the init-phase pole
data — both emitted before any lookup ran — and its bare-`printf` banner left
the runtime's `[E2E]` stamp mid-line, so the driver silently measured `wall_s`.
Both are fixed; every number taken before that fix is a wall time.)

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `-l <lookups>` | XS lookups; sizes the whole graph (3 EDTs and one 8-byte result DB each) | 10,000,000 | ✓ parsed in `mainEdt`, carried inside the `Inputs` datablock to every consumer — multinode-safe |
| `-b <batch>` | lookups per compute-phase sync batch (`NL_SYNC`) — the in-flight width of the whole compute phase, since each batch is a FINISH scope and the next starts only when it drains | 1024 | ✓ parsed, validated `≥1`, carried in the `Inputs` datablock through both settings-init hops — multinode-safe |
| `-s small\|large` | H-M benchmark size; `small` also forces `n_nuclides=68` | large (355 nuclides) | ✓ |
| `-n <n>` | nuclide count, overrides whatever `-s` set | 355 (68 with `-s small`) | ✓ parsed and **validated**: the H-M material tables hold fixed nuclide IDs (up to 62 in the small fuel table, up to 354 in the large one) and which table is used is decided by the count alone, so any value other than exactly 68 or ≥355 would index the per-nuclide arrays out of bounds. `read_CLI` now rejects those loudly with a message naming the constraint. The campaign only ever reaches this through `-s` |
| `-p <poles>` | average poles per nuclide — sizes each pole DB | 1000 | ✓ |
| `-w <windows>` | average windows per nuclide — sizes each window DB | 100 | ✓ |
| `-d` | disable Doppler broadening (skip the temperature-dependent Faddeeva kernel) | Doppler ON | ✓ |
| `-t <threads>` | "OpenMP thread count" | 1 | ⚠ **dead**: parsed and range-checked (`≥1`) but never read again anywhere in the EDT graph — this port creates one EDT chain per lookup regardless of thread count; `Threads:` in the results banner is the only place the value is used. Passing `-t` prints a one-line warning that it has no effect in this port |
| *(none)* `n_mats` | material zones — the H-M reactor model has exactly 12 | 12 | ✗ compile-time only: no `-m` flag, and `load_mats`'s per-material nuclide tables are hardcoded for exactly 12 zones, so this isn't a knob a run could reasonably move |
| *(none)* `numL` | Legendre moments per nuclide (pseudo-K0RS array width) | 4 | ✗ compile-time only — never read from argv in either RSBench port |

The pinned scalar is a function of the dataset knobs (`-s`/`-n`, `-p`, `-w`,
`numL`, `-d`) **and of `-l`**, because `-l` sets how many results are summed. It
is *not* a function of `-b`, of the node count, of the tier or of the runtime
entry. The `pole_cksum` extra scalar is a fold over the pole arrays only, so it
depends on `-s`/`-n`, `-p` and `numL` and on nothing else — not on `-w`, `-l`,
`-b`, the node count or the tier.

## Structure

Let `n` = `n_nuclides`, `m` = `n_mats` (=12), `L` = `lookups`, and
`G = ⌈L / batch⌉` (`NL_SYNC`, the `-b` sync-batch width, default 1024).
`nprocs` is hardcoded to 1 in `FNC_settingsInit` regardless of any parameter —
this port has no rank-partitioning axis at all. (It is also not one waiting to
be switched on: `ilookup` is initialised outside `FNC_globalComputeSpawner`'s
`for(i < nprocs)` loop and never advanced inside it, so raising `nprocs` would
replicate the whole lookup range and the whole dataset per rank, not partition
them.)

| object | count | size |
|--------|-------|------|
| global/handle DBs (`InputsH_0`, `globalH`, `InputsH`, `InputsHs[]`, `rankHs[]`, `timers`, `cksum`) | 7 | tens of bytes each |
| per-rank handle DBs (`InputsH[i]`, `rankH`, `settingsH`, `dataH`, `templatesH`) | 5 | small (GUIDs/ints) |
| `dataH`'s 8 index arrays (n_poles, n_windows, 3×per-nuclide GUID arrays, numNucs, 2×per-material GUID arrays) | 8 | `int`/`ocrGuid_t` arrays, `n` or `m` elements |
| per-nuclide DBs: pole, window, pseudo-K0RS | `3n` | Pole ≈72 B ×~`avg_n_poles`/nuclide; Window 32 B ×~`avg_n_windows`/nuclide; K0RS 32 B (`numL`×8 B) |
| per-material DBs: nuclide-ID list, concentration list | `2m` | `num_nucs[i]`×4 B / ×8 B |
| per-nuclide generator result DBs (the pole-data fold a generator publishes through its output event) | `n` | 8 B each |
| **permanent DBs total** | `20 + 4n + 2m` | — |
| per-lookup result DBs (created by the aggregator, destroyed by its batch's reducer) | `L` created, **≤ `b` live at once** | 8 B each |
| EDTs: setup (`mainEdt`…`init_dataH` chain, plus the `initCollect` join) | 14 | — |
| EDTs: dataset generators (`nuclideGen` per nuclide, `materialGen` per material) | `n + m` | — |
| EDTs: `rankCompute` + `rankMultiLookupSpawner` + `batchReduce` (one triple per sync batch, chained sequentially) | `3G` | — |
| EDTs: `rankLookup` + `macroxs` + `microxsAggregator` (one lookup) | `3L` | — |
| **EDTs total** | `14 + n + m + 3G + 3L` | — |
| explicit STICKY events (global + per-rank + one per sync batch) | `7 + G` | — |
| shim-materialized events (finish events for `EDT_PROP_FINISH` creates, output events for creates with a non-NULL `outputEvent`) | `13 + n + 2G` — the 4 `mainEdt`-level `EDT_PROP_FINISH` phases (`settingsInit`/`globalInit`/`globalCompute`/`globalFinalize`) each carry a finish event *and* an output event (`4·2=8`); `TS_init_InputsH` (created twice total, once from `globalInit` and once per-rank from `rankInit` since `nprocs=1`) and `TS_timer` each pass a non-NULL `outputEvent` with no `FINISH` (`+3`); `TS_globalComputeSpawner` (`FINISH` + output, `+2`); each of the `n` `nuclideGen` creates names an output event, which is how its fold reaches the join (`+n`); and each of the `G` `rankMultiLookupSpawner` creates carries both (`+2G`).  `materialGen` and `initCollect` name none | — |
| **Events total** | `20 + n + 3G` | — |

`batchReduce` adds no event: it is created `EDT_PROP_NONE` with a NULL output
event, and its `1 + nlookups` slots are datablock dependences.  `initCollect`
is the same shape with `n` slots.

Nothing here is data-dependent in *count*: the index-seeded draws (per-nuclide
pole/window counts, per-lookup material pick) change DB **payload sizes** and
per-`microxsAggregator` dependence-slot counts, never how many objects exist.
The shim-materialized events are easy to miss reading the source alone: they
never appear as an `ocrEventCreate` call, only as a non-NULL last argument to
`ocrEdtCreate` or an `EDT_PROP_FINISH` property flag, but the OCR shim
(`benchmarks/ocr_shim/arts_ocr.c`) materializes a real ARTS event for each, and
`arts_event_create` increments `NUM_EVENT_CREATE` regardless of which call
produced it.

Est. cross-section payload ≈26.7 MB (the app's own `Est. Memory Usage` print,
~25.5 MiB), fragmented across the `3n + 2m` per-nuclide/per-material DBs above
— fixed by `-s`/`-n`/`-p`/`-w` and independent of `-l` and `-b`. What actually
sets RSS is allocator high-water from the `3L` EDT creations and the ≈`171·L`
acquires; an earlier sizing run measured ~26.6 GB at one node -- an estimate
carried forward from a pre-fix run at different arguments and on the superseded
oracle, to be re-taken with the first post-fix campaign, but far enough inside
the single-node budget that the margin is not in doubt. Worked object counts at the campaign arguments: `calibration
pending` (they follow directly from `G = ⌈L/b⌉` and the formulas above).

Counter cross-check: the DB/EDT/event formulas were verified against measured
absolutes on a 1-node `-s small -p 50 -w 10` pair of runs before the reduction
and before parallel generation; the terms added since are the `+G` reducer EDTs
and `+1` accumulator DB, and the `+(n + m + 1)` init EDTs, `+n` fold DBs and
`+n` generator output events. The transient per-lookup result blocks are
created and destroyed inside their own batch. `calibration pending` for a
re-take.

## Wiring

`mainEdt → TS_settingsInit(FINISH) → TS_globalInit(FINISH: TS_init_InputsH +
TS_rankInitSpawner → TS_rankInit → TS_init_rankH → TS_init_dataH → the
`n + m` dataset generators and their `initCollect` join) →
TS_globalCompute(FINISH: TS_globalComputeSpawner + TS_timer) →
TS_globalFinalize(FINISH)`.

`init_dataH` is the serial pre-pass: it builds the two count multinomials (a
histogram of one draw stream over the whole nuclide index — a bin depends on
every draw, so it is not a function of the index that reads it) and the
12-entry `num_nucs` table, which is what it needs to *size* the `3n + 2m`
dataset blocks; it then creates those blocks — with the same homes as before —
releases its own hold on each, and creates one `nuclideGen` per nuclide and one
`materialGen` per material to fill them. A generator's output event is wired
into `initCollect` before the generator's own dependences are added, so no fold
can be published before its consumer is registered. The join that the compute
phase waits on is `TS_globalInit`'s FINISH scope, which already covers every
task created transitively beneath it — the generators included — so
`globalCompute` cannot start until the last block is filled. Inside `TS_globalComputeSpawner`, one
`rankCompute` EDT is created (`nprocs=1`); each `rankCompute` spawns one
`rankMultiLookupSpawner` (FINISH) covering up to `batch` lookups and, if more
remain, chains to the next `rankCompute` — the sync batches are **sequential**,
not spawned in parallel. Inside a batch, `rankMultiLookupSpawner` first creates
that batch's `batchReduce` EDT (slot 0 = the run-global accumulator in
`DB_MODE_RW`, one further slot per lookup) and then its own C loop creates up to
`batch` independent `rankLookup → macroxs → microxsAggregator` triples with no
dependence on each other. Each aggregator fills its own slot of the reducer, so
the reducer cannot run before the batch's last result is written, and because it
is created by the FINISH-scoped spawner the batch cannot drain before the
reduction has run. `FNC_timer` reads the accumulator after
`TS_globalComputeSpawner`'s output event — i.e. after every batch — and prints
the banner.

The run-global accumulator's name travels down the chain in the parameter block
(`globalCompute → globalComputeSpawner → rankCompute → rankMultiLookupSpawner`),
as does each batch reducer's name and slot index
(`rankMultiLookupSpawner → rankLookup → macroxs → microxsAggregator`), so no
extra datablock acquire is paid anywhere on the path.

Every per-nuclide/per-material DB is written exactly once (during
`init_dataH`, before release) and only `DB_MODE_CONST`/RO thereafter — no
writer ever returns. `microxsAggregator` RO-acquires the pole/window/K0RS DBs
of every nuclide in its lookup's material (`num_nucs[mat]` of them, 5–321
depending on which material `pick_mat` drew), which averages
`5 + 3·55.4 ≈ 171` acquires and peaks at 968 on a fuel draw. The one write→read
edge per lookup is the 8-byte result block, single-writer and read once.
Because sync batches serialize, **at most `-b` lookups are ever in flight** per
rank; within that window a given nuclide's DBs see concurrent RO readers in
proportion to how many live lookups picked a material containing that nuclide —
the fuel material (`num_nucs[0]=321`, picked with probability ≈0.14) drives the
highest fan-out, on the order of `b×0.14` concurrent readers, further capped by
worker count. No single DB is a contention point — the per-nuclide
fragmentation trades a hot DB for EDT/DB churn instead (contrast
`RSBench_intel_sharedDB`, which makes the opposite trade).

## Flow

Setup (`settingsInit`→`globalInit`→its rank-init chain) reaches `init_dataH`
as a strict FINISH-scoped serial pipeline on one worker, and generation then
fans out. The synthetic data comes from an index-seeded generator — a
splitmix64-style pure function of `(stream, index)` in the shared
`rs_generate.h`, one stream per array — so the instance is deterministic,
order-independent and node-count invariant, with no shared RNG state and no
`rand()`/`srand()` anywhere in the app. Because every value is a function of
its own index, it is produced by the task that owns the data: `n` `nuclideGen`
tasks (poles, windows, pseudo-K0RS) and `m` `materialGen` tasks (nuclide-ID and
concentration lists) run concurrently, ≈3.2 M draws and ~27 MB of writes spread
over the machine. What stays serial is exactly what cannot be indexed: the two
count multinomials (`(-p + -w)·n ≈ 390 k` mixes) and the 12-entry `num_nucs`
table, plus the `3n + 2m` `ocrDbCreate` calls themselves, which stay in one
task so the GUID arrays keep a single writer. `calibration pending` for the
absolute cost; it does not grow with node count.

The same generator header is compiled into `RSBench_intel_dist`, and the count
pre-pass is the same function run once in one task there too, so both rows'
instances — and therefore both rows' checksums — are identical by
construction.

The lookup phase is `G` sequential sync batches — a hard serialization point
independent of `-l`'s absolute size, since batch width is capped at `-b`
regardless of workload or worker count. Within one batch the instantaneous
runnable frontier is `min(batch, workers)`: a lookup chain is **strictly
serial**, because `FNC_rankLookup` creates `macroxs` and returns and
`FNC_macroxs` creates the aggregator and returns, so a chain never has two
runnable EDTs at once. Between batches there is no overlap — batch `g+1`'s
`rankCompute` depends on batch `g`'s `rankMultiLookupSpawner` output event, and
that scope includes `g`'s reducer. `mainEdt`'s four top-level FINISH phases
(`settingsInit → globalInit → globalCompute → globalFinalize`) never overlap
either.

Nothing but the workload runs inside the measured window: there is no
verification recompute (the lookup checksum is an O(1) fold per lookup plus an
O(b) reduction per batch, and the pole checksum is folded as the data is
generated), no file is written, and there is no per-iteration stdout — the
per-1000-lookup progress print that used to run on the single serial producer
has been removed. What remains on stdout is the input banner, four one-line
init messages, the pole checksum line, and the results banner.

## Placement (base)

Every `ocrEdtCreate` and `ocrDbCreate` in this port passes `NULL_HINT` — there
is no affinity/labeling code at all outside the guard. Effective policy: **EDT →
runtime round-robin** (per-creating-rank atomic counter via
`ARTS_HINT_ANY_RANK`); **DB → home = creating rank**. Because `nprocs` is
hardcoded to 1, the *entire* per-nuclide dataset is created — and therefore
homed — by whichever rank the single `rankInit`/`init_dataH` chain happens to
land on (call it rank R, itself round-robin-placed from `mainEdt`). The
generator tasks that fill those blocks are round-robin-placed like every other
unhinted EDT, so each block's single write happens wherever its generator
landed: under a write-back policy the payload then stays with that writer until
a reader asks for it, while under write-through it returns to rank R at
release. That is the one placement consequence of parallel generation in this
tier, and it is a property of the write policy rather than of the program.
Every lookup's `rankLookup` / `macroxs` / `microxsAggregator` triple is
independently round-robin-placed too, so on an `N`-rank run only ~1/`N` of
lookups execute where the data lives. The
rest remote-RO-acquire the touched per-nuclide DBs over the network — up to
`num_nucs[mat]` nuclides × 3 arrays per lookup (as many as 963 DBs for a
fuel-material draw). The per-lookup result block is created wherever its
aggregator ran and is read once by its batch's reducer, which is also
round-robin-placed. The algorithm's real locality — one lookup only ever needs
one material's handful-to-few-hundred nuclides — is never expressed in
placement, the same "worst-case coherence stress by construction" shape as
`fibonacci`, but at per-nuclide-DB (KB-scale) rather than 4-byte granularity.

## Placement (hinted)

As-born round-robins every link of every lookup independently: rankLookup
lands somewhere, spawns macroxs there, which lands somewhere else, which spawns
the aggregator on a third rank — each chain's intermediate results cross the
wire twice for nothing (see above).

The layer (in `rsbench.h`, because the creates span several files) is the
same two-part recipe as `XSBench_intel`'s, and every use of the guard is an
argument to a create — no control flow, partition, count, size or wiring
differs between the tiers. **Chain pinning** (`mcChainEdtHint`): the two
SPAWNED links of each chain pin to the rank the chain's first link landed on;
the first link stays round-robin — that IS the load balance across independent
lookups — so the distribution across ranks is untouched and only the chain's
interior becomes local. **Home spreading** (`mcSpreadDbHint`): every dataset
object is created inside the one `init_dataH` task and would otherwise be homed
on that single rank, which then serves the whole machine's reads.
**Generator placement** (`mcSpreadEdtHint`): a dataset block's generator task
is placed on that block's own home, so the one write a block ever receives
happens where the run will read it, whatever the write policy. It is a hint
argument on the generator's `ocrEdtCreate` and nothing else — base creates the
same `n + m` tasks with `NULL_HINT`.

The spread index space is disjoint by band, so no two classes of object are
forced to share a home:

| band | objects | home |
|---|---|---|
| `0..7` | `dataH`'s eight index arrays | `index % P` |
| `8`, `9` | `InputsHs[i]`, `templatesH` | `index % P` |
| nuclides | nuclide `i`'s pole, window and K0RS blocks (all three co-located) **and its `nuclideGen` task** | greedy load balance over expected read rate |
| `10+n .. 10+n+m-1` | material `i`'s nuclide-ID and concentration lists **and its `materialGen` task** | `index % P` |

Two corrections were made in this pass. The material band: the tables used to
be spread at the bare material index `0..11`, so at *every* rank count the fuel
material's two tables — the highest-fanout draw — shared a home with the
`nPoles`/`nWindows` index arrays that every lookup acquires, and materials 10
and 11 shared homes with nuclides 0 and 1.

And the nuclide map. Round-robin at `10+i` equalises the *number* of nuclides
per rank, but one datablock is served by one rank, so the quantity that has to
balance is expected **reads** per rank. A nuclide's read rate is a closed form
of the fixed composition: the sum of the volume fractions of the materials that
contain it (`pick_mat`'s distribution), which spreads about fivefold — the four
nuclides every water-bearing material holds (4, 5, 24, 41) are read on ≈0.655
of lookups, the fuel-only extension nuclides (68..354) on ≈0.139. The map
(`mcNuclideSpreadIndex`, `material.c`) therefore places the heaviest remaining
nuclide on the rank serving least so far, starting from the load the
fixed-index objects above already impose (the handle/index singletons are
acquired one to three times *per lookup* each, so they are a rank's worth of
serving on their own). It is a pure function of the composition and the rank
count — no run-time measurement, no data dependence — and it stays a hint
argument: nothing outside the guard sees it, and base is byte-identical. The
table is memoised, with one precondition: it is only ever called from the
single task that creates the dataset blocks and names both their homes and
their generators' placement, so it needs no synchronisation. The generator
tasks themselves never call it — they are handed a placement, they do not
compute one.

`pdCount <= 1` returns `NULL_HINT` for both helpers, so base and hinted coincide
at one node by construction. Per-rank EDT balance under hinted equals base's
exactly (`L/P` at every rank count), because the pinned links inherit the
distribution of an unhinted, round-robin-placed chain root.

Measured A/B of the two parts predates both the oracle fix and the band
correction and was taken at superseded arguments on wall time, so it is not
quoted here: `calibration pending`. What is structural and does not need a run:
the aggregator's ~171 nuclide acquires are remote in *both* tiers by
construction (the blocks are deliberately spread over all ranks), so chain
pinning saves two control crossings per lookup and home spreading redistributes
serving load — and every spread object is written once and read-only
thereafter, which is the read-only case a validating protocol already caches per
rank.

## Sizing

`-l` sets total work and `-b` the in-flight width — `-t` is dead (see
Parameters). Doubling `-l` linearly doubles EDT count (`3` per lookup, `+3` per
additional batch) and wall time; permanent DB count and payload memory are
fixed by `-s`/`-n`/`-p`/`-w` alone and never move with `-l` or `-b`.

- **Width is `W = -b`**, a spawn-and-join frontier: `b` independent chains per
  batch, each chain strictly serial, batches FINISH-gated so they never
  overlap. `-b` is a plain argv int carried multinode-safely in the `Inputs`
  datablock, so it can be set to any multiple of the campaign's worker count.
- **but the ceiling is nominal.** There is exactly one producer (`nprocs=1`), so
  by Little's law the realized in-flight population is `throughput × chain
  latency` — about 540 chains at the single-node anchor, roughly a sixth of a
  3456-slot window. Raising `-b` moves the ceiling, not the occupancy; a genuine
  ≥2× *realized* width for this class has to come from a restructured row's
  parallel producers.
- `-b` is also the concurrent-reader fan-out knob (`b × p(mat) × num_nucs` per
  nuclide block), which is why a reduced `-b` in a local trend distorts exactly
  the quantity the coherence sweep is reading.
- Height knobs stay at the benchmark's canonical defaults (`-s large`,
  `-p 1000`, `-w 100`, Doppler on, `numL 4`, `n_mats 12`). Upstream publishes a
  second, smaller size class (`-s small -p 1000 -w 100 -l 100000`); `-s small`
  is rejected on the record because it drops the fuel material from 321 to 34
  nuclides and collapses the ~171-acquire per-lookup fan-out that is this row's
  exhibit. The window is therefore reached on `-l`, with the size alternative
  named rather than assumed away.
- Because `nprocs` is hardcoded to 1, there is no work-partitioning axis for
  the *lookups* across nodes at all: multinode runs measure remote-acquire /
  wiring cost, not throughput scaling — unlike `_sharedDB`'s fork axis. (Data
  generation does spread — one task per nuclide and per material — but it is
  init, not the exhibit, and it is `O(n)` against the compute phase's `O(L)`.)
- Campaign `-l`/`-b`: `calibration pending`. Every previously recorded time for
  this row came from the driver's silent `wall_s` fallback (the app's bare
  `printf` banner left the `[E2E]` stamp mid-line and the line-anchored regex
  never matched), so the calibration has to be re-taken on `[E2E]`, not
  rescaled from the old figures.

## Family shape

`calibration pending`. The previous table in this section was labelled "e2e
seconds" but no cell of this row has ever produced an `[E2E]` measurement — the
numbers were process wall time (or the app's own `Runtime:` print), taken at
`-l 150000`, on the superseded oracle, and before the material-band correction.
Re-take it after the first post-fix campaign.

What is expected to survive the re-take, because it is structural rather than
measured: every arm anti-scales from 1 to 8 nodes in **both** tiers — one rank's
serial spawner feeds the whole machine, so extra nodes add wire distance and no
production capacity — and the 1n→2n cliff is the fine-grained remote-acquire
regime switching on, with per-nuclide KB-scale blocks and up to 963 acquires per
fuel-material lookup. The arm separations on this write-once, read-fine-grained
dataset dwarf the base-vs-hinted delta.
