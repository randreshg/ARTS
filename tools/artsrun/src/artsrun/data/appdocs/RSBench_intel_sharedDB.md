# RSBench_intel_sharedDB

*The opposite design from `RSBench_intel`: one contiguous datablock per array
across ALL nuclides (not one per nuclide), coarse per-thread-per-chunk EDTs
(not one triple per lookup), and real policy-domain-affinity placement (not
`NULL_HINT` round-robin). The voted scalar is a computed digest of the lookup
phase — see Overview.*
Source:
`third_party/ocr-apps/apps/RSBench/refactored/ocr/intel-sharedDB/src/` (7 C
files + `config.tpl`, ~1.1k lines; `main.c` runs the SPMD-fork / per-thread /
reduction machinery, `init.c` / `material.c` generate the shared
cross-section data, `rs_kernel.c` is the per-lookup compute — identical
physics to the plain port).

## Overview

Same RSBench multipole-lookup proxy as `RSBench_intel` (see its doc for the
kernel), restructured to track the MPI+OpenMP reference more literally.
`-p` SPMD "ranks" — a real ARTS-rank fork via policy-domain affinity, see
Placement — each generate their own resonance dataset into ONE contiguous
datablock per array (`DBK_poles`, `DBK_windows`, `DBK_pseudo_K0RS`,
`DBK_mats_all`, `DBK_concs_all`), not one DB per nuclide. `-t` OpenMP-style
"threads" each become a persistent EDT lane that claims a
`CHUNK_SIZE=1000`-lookup slice at a time and computes all of it inside one EDT
body's C loop before creating its own successor lane.

**Data generation is counter-based.** Every generated value is
`mix64(key(instanceSeed, streamId) + position)` — a pure function of the SPMD
instance, of which array is being filled, and of the value's own slot in that
array. Nothing is drawn from a shared generator, so the dataset is identical
at every node count, identical from run to run, and independent of how the
concurrent instances interleave. This replaces `srand(42)` + `rand()`, which
was process-global: the instances co-resident in one process shared one
mutex-protected libc stream, so the generated data was neither reproducible
nor node-count-invariant, and the draws serialised — a term proportional to
instances-per-rank sat inside the measured window and shrank with the node
count for reasons unrelated to the decomposition. `RS_DATA_SEED` is 42 and the
instance index enters the key, so the instances are independent Monte-Carlo
replicas of one problem class rather than 32 bit-identical repetitions.

**The result oracle.** Three `u64` words per lane travel through the app's own
`ARITY=10` reduction trees (`libs/src/reduction/reduction.c`) under
`REDUCTION_U8_ADD` — first over an instance's `t` lanes, then over the `p`
instances:

- `alls` — every Doppler-broadened pole evaluated,
- `abrarov` — how often the slow Abrarov/Faddeeva branch was taken,
- a magnitude digest: each lane sums `|macro_xs[0..3]|` over its own lookups,
  in its own order, and quantizes the running total to 20 mantissa bits
  (`rs_quantize`) once per task.

Integer addition is commutative and associative, so the folded value does not
depend on either tree's shape, on the lane→rank map, on the node count or on
the runtime; the quantization absorbs last-bit float differences while any
real divergence moves the digest. A non-finite lane accumulation cannot be
quantized at all: `rs_quantize` maps it to a value the digest cannot otherwise
produce, and the lane prints an `ERROR:` line, so it fails loudly instead of
converting out of range. The banner prints `Pole Evaluations:`,
`Abrarov Evaluations:` and `XS Checksum:` (a mix of the three, masked to 53
bits so the driver's `float()` comparison is exact), and the catalog votes on
`XS Checksum:`. The counters were previously accumulated INSIDE the lookup
loop (`*g += abrarov` on a running total, once per lookup), which made them
prefix sums rather than counts; the accumulation is now once per task, so
`Slow Faddeeva:` is a real percentage (~0.5%, the value the pole scaling
factor is chosen to produce).

**Every instance's lookups are covered.** Each instance folds its own lanes
into three words and its `summaryEdt` feeds them into a second, cross-instance
`ALLREDUCE` (`U8_ADD`, its own `nprocs`-wide GUID range); `finalSummaryEdt`
waits on that and on the perf-timer allreduce, and rank 0 prints. So all three
printed numbers are functions of every lookup all `p` instances performed — a
silently wrong instance cannot hide behind the printing one. The results
banner's `Runtime:` line (an `F8_MAX` allreduce over instances through the
same library) is not used for measurement: use the runtime's own `[E2E]`
stamp.

## Parameters

| flag | meaning | default | CLI reachability |
|------|---------|---------|-------------------|
| `-t <threads>` | persistent EDT lanes; each claims `CHUNK_SIZE=1000` lookups per generation | 1 | ✓ drives both intra-rank task fan-out and (via `SINGLE_RUN_ACROSS_PD`) cross-node placement — see Placement. Validated `≥ 1` |
| `-p <procs>` | SPMD "rank" replication count — each rank independently runs the FULL `-l` lookups against its own dataset, not a partition | 1 | ✓ reachable, but **replicates** the run rather than partitioning it — see Sizing. Validated `≥ 1` (0 previously hung the SPMD fork, a negative value wrapped to a huge rank count) |
| `-l <lookups>` | XS lookups *per rank* | 10,000,000 | ✓ validated `≥ 1` |
| `-s small\|large` | H-M size, `small` forces `n_nuclides=68` | large (355) | ✓ rejected loudly if it is neither word |
| `-n <n>` | nuclide count | 355 | ✓ and now **constrained**: `load_num_nucs`/`load_mats` pick the H-M material tables by this count alone (exactly 68 → small tables, highest nuclide ID 67; anything else → large tables, highest ID 354) and every per-nuclide array is sized `n_nuclides`, so only `68` or `≥ 355` avoids indexing outside the arrays. Anything between is now rejected at parse time instead of reading out of bounds in the kernel |
| `-a <poles>` | average poles per nuclide | 1000 | ✓ own flag, distinct from `-p`. Validated `≥ 1` and `≥ -w`: with fewer poles than windows the per-window pole span floors to zero and the kernel evaluates no poles at all |
| `-w <windows>` | average windows per nuclide | 100 | ✓ validated `≥ 1` |
| `-d` | disable Doppler broadening | ON | ✓ |
| *(none)* `n_mats` | 12 material zones, same fixed H-M table as the plain port | 12 | ✗ compile-time only |
| *(none)* `numL` | Legendre moments per nuclide | 4 | ✗ compile-time only |
| *(none)* `CHUNK_SIZE` | lookups per lane per generation — the per-EDT work grain | 1000 | ✗ `#ifndef`-guarded compile-time constant. A grain sweep on this row needs a rebuild, not an argv change |
| *(none)* `SCHEDULER_TYPE` | 0 = one giant generation, 1 = chunked chain | 1 | ✗ compile-time only |

Every rejection names the option and the reason and exits non-zero; an
unrecognised option is echoed before the usage text.

## Structure

Let `t` = `nthreads`, `p` = `nprocs`, `L` = `lookups`, and
`G = ⌈L / (1000·t)⌉` (`CHUNK_SIZE=1000`, `SCHEDULER_TYPE=1` dynamic-scheduling
equivalent — both compile-time constants). `n_nuclides` does **not** appear in
any of these counts: that is the structural point of "sharedDB" — the whole
cross-section dataset is a handful of contiguous arrays regardless of `n`.

| object | count (×`p`) | size |
|--------|------|------|
| global DBs (`argv`, `globalParamH`) | 2 | small |
| per-rank setup DBs (`rankH`, `rankDataH`, `rpPerfTimerDBK`, `rpXsReductionDBK`) | 4 | small |
| per-rank shared-array DBs (`n_poles`, `n_windows`, `num_nucs`, `mat_ptrs`+`mats_all`, `conc_ptrs`+`concs_all`, `pole_ptrs`+`DBK_poles`, `window_ptrs`+`DBK_windows`, `K0RS_ptrs`+`DBK_pseudo_K0RS`) | 13 | `DBK_poles` ≈`n·avg_n_poles·72 B`; `DBK_windows` ≈`n·avg_n_windows·32 B`; rest small |
| per-thread persistent DBs (seed, xs, sigTfactors, reductionVars, loop-reduction) | `5t` | tens of bytes each (`reductionVars` is 4 `u64`: three reduced words plus the lane's running magnitude sum) |
| per-(thread,generation) ephemeral "ptrs" DBs, created **and destroyed** inside the same `iterationsPerThreadEdt` | `5tG` | tens of bytes each |
| reduction-tree fringe (loop-completion ×`t` + perf-timer ×1 + cross-instance result ×1 launches, plus their `ARITY=10` fan-in machinery — `reductionLaunch`/`reductionSendChannelEdt` in `libs/src/reduction/reduction.c`) | **+5 DBs** per launch set, measured at the verified `t=2, p=1` point BEFORE the cross-instance fold existed; not closed-form for general `t` — see notes | small |
| EDTs: global + per-rank setup | `2 + 8p` | — |
| EDTs: per (thread, generation) — `lookUpKernelPerThreadEdt` + `iterationsPerThreadEdt` | `2tGp` | — |
| EDTs: reduction-tree fringe | **+8 EDTs**, measured at `t=2, p=1`; not closed-form — see notes | — |
| STICKY/COUNTED events: global + per-rank | `1 + 4p` | — |
| events: per (thread, generation) — `iterationsPerThreadEdt` create (`EDT_PROP_FINISH` + non-NULL `outputEvent`, so 2 shim events) **plus** `createEventHelper(&iterationsPerThreadOEVTS, 1)` (an explicit `OCR_EVENT_COUNTED_T` create) — 3 events per (thread,generation), not 2 | `3tGp` | — |
| events: reduction-tree fringe | **+6 events**, measured at `t=2, p=1`; not closed-form — see notes | — |

At `p=1` the totals are: `DB = (2+17+5t) + 5tG + fringe`,
`EDT = (2+8) + 2tG + fringe`, `EVENT = (1+4) + 3tG + fringe` — the fringe terms
were pinned by measurement at `t=2` (see Counter cross-check below) when there
were two reduction launch sets; there are now three (the cross-instance result
fold is the new one), and their general-`t` scaling was never statically
closed-formed anyway (the `ARITY=10` tree's depth grows with `t`).

Worked numbers at the campaign arguments (`-l 2000000 -t 216`): `-l` and `-t`
are the orchestrator's sizing handles, and `G = ⌈L/(1000·t)⌉` evaluates to
`10`; then, per instance, `19 + 5t + 5tG` DBs, `10 + 2tG` EDTs, `5 + 3tG`
events, times `p = 32` instances, plus the reduction fringe. Shared-array payload is ≈26.7 MB **per instance** at
the default `-s large -a 1000 -w 100` (same total bytes as the plain port's
fragmented 1,065 DBs, now 13 DBs), ≈854 MB across 32 instances.

Counter cross-check: verified (1 node, `-s small -t 2 -l 30` vs `-l 2500`,
both `p=1`, `G=1` vs `G=2`) BEFORE this cycle: measured absolutes EDT 22/26,
DB 44/54, EVT 16/22; subtracting the runtime's constant baseline (+1 EDT,
+1 DB, +0 EVT per run) gave app-side EDT 21/25, DB 43/53, EVT 16/22 — the
formulas above with the pre-cycle constants (`t=2, p=1`:
`DB=(2+16+10)+5·2·G+5=33+10G`, `EDT=9+4G+8=17+4G`, `EVENT=4+6G+6=10+6G`).
This cycle adds, per instance, one datablock (`rpXsReductionDBK`), one EDT
(`finalSummaryEdt`, the printing/teardown half of the old `summaryEdt`) and
one event (`rpXsReductionEVT`), plus one more reduction-tree launch set; and
`reductionVars` grew from 2 words to 4 in the same datablock. **The
cross-check must be re-measured** — `calibration pending`.

## Wiring

`mainEdt → forkSpmdEdts_Cart1D → initEdt (×p) → channelSetupEdt →
FNC_xsbenchMain → FNC_initSimulation (initOcrObjects + initSimulation: fills
the 13 shared-array DBs, RW then released) → lookUpKernelEdt` (creates the
`5t` per-thread persistent DBs, spawns `t` independent lanes) `→
lookUpKernelPerThreadEdt` chain, one link per generation per lane; each link
spawns one `iterationsPerThreadEdt` (FINISH) that RO-acquires the rank's
shared arrays plus its own 5 fresh "ptrs" DBs (created and destroyed inside
the same body) and runs the `CHUNK_SIZE`-lookup C loop. On a lane's last
generation it calls `reductionLaunch` into the loop-completion tree and
decrements `loopCompletionLatchEVT` (a LATCH counting down from `t`) →
`launchReductionEdt` (waits on the latch) launches the perf-timer reduction.
Two joins then run: `summaryEdt` (RO on the instance's loop-reduction output)
launches the cross-instance result `ALLREDUCE`, and `finalSummaryEdt` (RO on
the perf-timer output and on the result output) prints the results banner,
destroys the shared arrays, satisfies `finalOnceEVT` → `wrapUpEdt` shuts down.
`FNC_globalFinalize` in `main.c` is dead code — a second `ocrShutdown` path
nothing references; the live teardown is `wrapUpEdt`.

Shutdown is gated on the whole workload, not on the printing instance: every
instance's `finalSummaryEdt` waits on two `ALLREDUCE`s over all `p` instances
(the perf timer and the result fold), so no instance is cut short and no
instance's result is left out of the printed value. The window also contains
each instance's teardown of its eight shared-array DBs (one of them ≈25.6 MB,
cached on every node under the base map) — genuine coherence cost of the app's
own lifecycle, but a node-count-dependent term that is not lookup work.

DB concurrency: the 8 shared-array DBs are written exactly once (during
`initSimulation`, released before any lookup runs) and RO thereafter — no
writer ever returns. Once released, up to `t` `iterationsPerThreadEdt`s per
instance run concurrently — the lanes have no inter-lane dependency — and can
hold simultaneous RO acquires of the same rank's copy. **`DBK_poles`
(≈24.4 MB at default sizing: `355×1000×72 B`) is the contention point** — a
single DB instead of the plain port's 355 small ones. The 5 per-generation
"ptrs" DBs are private scratch, never shared across EDTs.

## Flow

Setup is a strict FINISH-scoped serial pipeline per rank (`initEdt →
channelSetupEdt → xsbenchMain → initSimulation`, one worker), and the `p`
instances run it concurrently at every node count, so data generation is
`p`-way parallel across the run and no longer serialises on a shared
generator. Per NODE the picture is different and worth stating: an instance's
generation is one EDT on one worker, so at the widest geometry — one instance
per node — one of the node's workers generates while the rest idle. That is a
fixed, node-count-independent cost inside the `[E2E]` window whose SHARE grows
as the lookup phase shrinks with `N`; the `init_s` extra scalar is what makes
it readable per cell. It is not split across lanes because the arrays it fills
are single shared datablocks: concurrent lane EDTs writing disjoint slices of
one DB is a write-write conflict at DB granularity — outside DB-WRF, though
serialised (and therefore legal) under `OCR` — and giving each lane its own DB would
change the DB granularity the row exists to exhibit. The lookup phase is `t` independent generation-chains
running fully in parallel — steady-state concurrency is close to `t` (a lane's
`lookUpKernelPerThreadEdt` and its child `iterationsPerThreadEdt` overlap only
briefly at each generation hand-off) — each lane working through `G`
**sequential** generations of up to 1000 lookups apiece; a lane never runs
ahead of its own generation counter, but the `t` lanes have no cross-lane
ordering until the final latch. `partition_bounds` splits each generation's
`1000·t` lookups evenly across lanes, so the lanes are balanced by
construction and wall time tracks `G`×(per-generation compute). The reduction
join (loop-completion latch → `launchReductionEdt` → `summaryEdt` →
`finalSummaryEdt`) is the one whole-rank barrier, at the very end.

## Placement (base)

`forkSpmdEdts_Cart1D` hints each of the `p` SPMD rank-EDTs (`initEdt`) with a
genuine `OCR_HINT_EDT_AFFINITY` from `ocrAffinityGetAt(AFFINITY_PD,
getPolicyDomainID_Cart1D(i, {p}, {affinityCount}), …)` — a cart-1D block
spread of SPMD ranks across policy domains. At `p=32` the fork's block
partition maps 32 instances onto however many PDs the run has (8n → 4 per
node; 32n → 1 per node; 1n → all 32 on the node). Inside each instance,
`getAffinityHintsForDBandEdt` snapshots `ocrAffinityGetCurrent()` (wherever
`initEdt` landed) into `rankH_t.myEdtAffinityHNT`/`myDbkAffinityHNT`, and the
same-rank EDTs — `channelSetupEdt`, `FNC_xsbenchMain`, `FNC_initSimulation`,
`lookUpKernelEdt`, `launchReductionEdt`, `summaryEdt`, `finalSummaryEdt` —
carry it. On the DB
side only `rankDataH` carries an explicit `OCR_HINT_DB_AFFINITY`; all thirteen
shared-array datablocks and every other per-rank datablock are created with
`NULL_HINT` and are homed by the shim's no-hint policy, which is first-touch —
so they land on the rank where `initEdt` ran, the same place, by a different
mechanism.

The one exception is the per-thread lane: with `SINGLE_RUN_ACROSS_PD` compiled
in (`benchmarks/apps/CMakeLists.txt`'s `OCR_EXT_DEFINES`, matching the app's
own `Makefile.x86-base`), `lookUpKernelEdt` re-hints each
`lookUpKernelPerThreadEdt` (and everything it spawns) with
`ocrAffinityGetAt(AFFINITY_PD, getPolicyDomainID_Cart1D(tid, {t},
{affinityCount}), …)`, where `affinityCount` is the **live**
`ocrAffinityCount(AFFINITY_PD, …)` queried at run time. `mype` never enters
that formula, so EVERY instance uses the same lane→rank map and spreads its
`t` lanes across ALL the run's ranks independent of where its own dataset
lives. Two consequences, both structural and both invisible below 8 nodes:

- **Balance.** Per-node lanes are `p × block`, where `block` is the block
  partition of `t` over `N` ranks. At `t=216`: exact at N=1,2,4,8; `{14,13}`
  at N=16 (max/mean 1.037) and `{7,6}` at N=32 — 224 lanes on 24 nodes
  against 192 on 8, max/mean **1.037**, an efficiency ceiling of 0.964
  at the widest cell before any protocol effect.
- **Locality.** Every node runs lanes of all 32 instances and therefore
  RO-caches all 32 datasets: ≈854 MB per node at EVERY node count. The
  per-node read-only working set never shrinks with N. This is also what makes
  the arms separate on this row: the lane spread forces machine-wide remote RO
  acquires of the arrays.

The 5 ephemeral per-generation "ptrs" DBs use `NULL_HINT`, so they are homed
wherever the compute EDT itself runs — always local. The four mutable
per-lane DBs (`DBK_seed`, `DBK_xs`, `DBK_sigTfactors`, `DBK_reductionVars`)
are also `NULL_HINT` in base, hence homed at the instance's rank while the
lane that writes them every generation runs elsewhere.

## Placement (hinted)

`-DOCR_APP_OPTIMIZED_PLACEMENT` (binary `RSBench_intel_sharedDB_hinted`)
changes two things and nothing else — no control flow, no partition, no
object count, no wiring:

1. The lane map is evaluated at the lane's index among **all** `p·t` lanes,
   `mype·t + tid` over `{p·t}` slots, instead of at `tid` over `{t}`.
2. The per-lane `OCR_HINT_DB_AFFINITY` the program already computes (and, in
   base, ships to the lane but never passes to a create) is passed to the five
   per-thread datablock creates.

`partition_bounds` still slices each generation by the within-instance `tid`,
so every lane does exactly the same work as in base.

Derived properties (from `getPolicyDomainID_Cart1D` = `getPartitionID`'s
`s = r·N/R`, `e = (r+1)·N/R − 1` block map):

| ranks | base lanes/node | base max/mean | hinted lanes/node | hinted max/mean |
|---|---|---|---|---|
| 1 | 6912 | 1.000 | 6912 | 1.000 |
| 2 | 3456 | 1.000 | 3456 | 1.000 |
| 4 | 1728 | 1.000 | 1728 | 1.000 |
| 8 | 864 | 1.000 | 864 | 1.000 |
| 16 | 448 / 416 | 1.037 | 432 | 1.000 |
| 32 | 224 / 192 | **1.037** | 216 | **1.000** |

(at `p=32`, `t=216`; balance is exactly 1.000 whenever `N | p·t`, which holds
for every Dane node count over 6912.) Coverage is full at every geometry and
the map is index-based, never `ocrAffinityGetCurrent` — no creator-pinning.
A node's block is `p·t/N` CONSECUTIVE global lanes, i.e. the lanes of exactly
`32/N` whole instances, and `initEdt` for instance `i` lands on PD `⌊i·N/p⌋`,
the same node — so the per-node read-only working set becomes `32/N × 26.7 MB`
(26.7 MB at 32 nodes) instead of a flat 854 MB, and the four mutable per-lane
DBs stop crossing the node boundary at every release.

**This tier is gated, not assumed.** Base already scales, so the drop rule
applies unless the layer earns its place: keep it only if hinted ≥ base at
**16 and 32 nodes** of one campaign. Eight nodes cannot settle it — the
balance term there is exactly 1.000, below the materiality bar, and every measurement
this row has was taken at 15-worker nodes at ≤ 8 nodes, where width 3456 ≫ 120
workers hides the remainder and the working set is small under either map.
Expect the hinted cells to be arm-INDIFFERENT: with a node holding whole
instances there is almost no cross-node acquire left, so a flat arm spread
there is the map working, not evidence about the protocols. Base is the row
that exercises the read path.

## Sizing

The campaign fixes `-s large -l <lookups> -t <lanes> -p 32` at every node
count — the strict sweep invariant (every logical count node-invariant, same
convention as `XSBench_intel_sharedDB`):

- **`-p 32`** is the SPMD instance count, 1× the largest campaign geometry
  (32 nodes); the fork's block partition maps the 32 instances onto however
  many PDs the run has, so smaller runs pack more instances per node instead
  of changing any count. Each instance generates its own dataset and runs its
  own full `-l`: `-p` is a **replication** knob, so the aggregate is
  `32 × -l` lookups of one problem class, not a 32× larger problem.
- **`-t`** lanes per instance. **Width = `p · t`** concurrently runnable lanes
  (spawn-and-join: each lane is a `G`-link chain and one link is runnable at a
  time). At `t=108`, 1× a campaign node's persistent workers, width would be
  `32 × 108 = 3456` = exactly 1.0× the 32-node worker count — an integer
  multiple with no slack, but with the base map's 1.185 remainder on top.
  The catalog uses `t=216`, giving `32 × 216 = 6912` = 2.0× the 32-node
  worker count and cutting base's 32-node remainder to 1.037 at no change in
  total work (`G = ⌈L/(1000·t)⌉` absorbs it).
- **`-l`** per instance is the size handle and the only calibrated knob; the
  published default is 10,000,000 and the catalog runs `-l 2000000`, below it
  (see the catalog row's disclosure). `G = ⌈L/(1000·t)⌉`
  generations per lane; the last generation is clamped to `lookups-1`, so
  there are no dummy lanes.
- Every other height/grain knob is at its published default: `-s large`,
  `-a 1000`, `-w 100`, Doppler ON, `CHUNK_SIZE 1000`, `SCHEDULER_TYPE 1`.
- **Memory** is independent of `-l`. Per instance:
  `n·a·72 + n·w·32 + n·numL·8` ≈ `355·1000·72 + 355·100·32 + 355·4·8` ≈
  **26.7 MB**, plus `5t` tiny persistent per-lane DBs and at most 5 live
  ephemeral "ptrs" DBs per running lane (≈8.7 KB per lane). One node holding
  all 32 instances therefore carries `32 × 26.7 MB` ≈ 854 MB of payload plus
  `p·t × 8.7 KB` ≈ 30 MB of scratch — ≈0.9 GB app-side, ≈5.5 GB with runtime
  structures, three orders under the 190 GB budget. `-n`/`-s`/`-a`/`-w`/`-p`
  are the only levers that move it. `DBK_poles`/`DBK_windows` are allocated
  from the REALISED per-nuclide counts (`Σ n_poles[i]` / `Σ n_windows[i]`),
  not from `n·a` / `n·w`: the counts are a multinomial draw with a "bump any
  zero bin up to 1" floor, and the floor can push the sum above the requested
  average, so sizing from the average wrote past the end of the datablock at
  small `-a`/`-w`. Sizing from the realised sum bounds it for every argument;
  the allocated size is `n·a` (`n·w`) plus the number of bins the floor
  raised, i.e. equal to the old size except in exactly the case that used to
  overflow.

## Family shape (measured, 15w+1p × 1/2/4/8 nodes, `-l 1000000 -t 108 -p 32`)

**Taken before this cycle's changes** — the generated dataset and the printed
scalar have both changed since, so these are shape evidence only and no value
in them transfers. base (there was no hinted flavour then), e2e seconds:

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 121.6 | 76.1 | 46.2 | 22.2 |
| val_wb | 124.8 | 62.7 | 34.2 | 16.9 |
| inv_wb | 116.4 | 63.0 | 33.6 | 17.1 |
| excl_retain | 116.2 | 65.8 | 33.5 | 16.3 |

A Dane-geometry single node (108w+4p) ran it in 74.6 s. Unlike its
`XSBench_intel_sharedDB` sibling (arm-indifferent to <1%), the arms separate
mildly here at 2–4n (val_wb_nocomb trails the pack by up to ~1.2×): the
`SINGLE_RUN_ACROSS_PD` lane spread makes every instance's lanes remote-acquire
its arrays machine-wide, so the read path is exercised cross-node even though
the datasets are block-homed. All four arms scaled 5.5–7.1× from 1n to 8n.
Part of that ratio was the shared-`rand()` serialisation unwinding as
instances landed in separate processes, which is gone; the row's `init_s`
extra scalar is what separates generation from lookups now. The banner's
`Runtime:` line is not used for any of this (see Overview); the numbers above
are the runtime's `[E2E]` stamp.
