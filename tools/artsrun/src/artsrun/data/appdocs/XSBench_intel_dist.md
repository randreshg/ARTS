# XSBench_intel_dist

*Restructured tier of `XSBench_intel` — the same Monte Carlo cross-section
lookup, decomposed into independent places: each place owns a share of the
lookup range, one producer over that share, and its own replica of the physics
table, so a lookup chain never leaves the policy domain it was produced on.*
Source: `third_party/ocr-apps/apps/XSBench/refactored/ocr/intel-dist/src/`
(`Main.c`, `io.c`, `CalculateXS.c`, `XSutils.c`, `Materials.c`, `timers.c`;
~1570 lines, mostly `Main.c`).

## Overview

XSBench is a proxy for the macroscopic-cross-section lookup kernel of Monte
Carlo neutron-transport codes (the OpenMC family). A run builds a
Hoogenboom-Martin reactor's per-nuclide microscopic cross-section energy
grids, unions them into one energy grid, indexes every unionized gridpoint
into every nuclide grid, and then issues `-l` randomized lookups: draw an
energy and a material from the lookup index, binary-search the unionized grid,
and interpolate a 5-vector of cross sections for every nuclide of that
material.

This row runs the same benchmark on the **same instance** as the
`base`/`hinted` tiers of its parent row — not merely at the same
`-s`/`-g`/`-l`, but on the same drawn table: both include the generator header
`refactored/ocr/xs_index_rng.h`, so nuclide grids and material concentrations
alike come out value for value identical and the two print the same grid
checksum. Only the decomposition differs (and what each tier votes on — see
*R9 divergence* below). The lookup range is cut into `-p` **places**. A place
owns

* a contiguous share of the lookup indices,
* one producer chain over that share (batches of `-b` lookups, each batch a
  FINISH scope), and
* its own replica of the table — nuclide grids, unionized energy grid, the
  per-gridpoint nuclide index plane (cut into at most `-b` blocks),
  material tables, accumulators,

and every EDT and DB of a place is created at policy domain `place % PDs`.
Consequently no lookup chain, and no data acquire inside one, crosses a
policy domain: what remains distributed is the production of work itself,
which is now `-p`-way parallel instead of one task for the whole machine.

The per-lookup task chain is deliberately unchanged — `lookup → macroxs →
aggregator`. At `-s large` a chain registers `7 + 5 + (5 + num_nucs)` = 72.4
dependences on average (`E[num_nucs] = 55.4` under `pick_mat`'s distribution,
of which the 55.4 nuclide grids are the aggregator's fan-in); every one of
those dependences is a datablock, so the acquires are the same 72.4, over 66.4
distinct blocks (the place handle and the nuclide-guid array are taken by more
than one link). The row still pays the per-lookup runtime cost that the
Master/Worker sibling exists to exhibit. What it removes is the two things
that made that sibling anti-scale: the single central producer and the
machine-wide fan-in on one rank's copy of the table.

The result scalar is `XSBench lookup checksum: <u64>`, a function of the
**lookup results**: each aggregator quantizes its macroscopic cross-section
vector to integers (scale 1024), weights the sum by the lookup index, and adds
it into its place's accumulator; place totals are summed once at the end. The
value is therefore invariant under the partitioning (`-p`), the batch width
(`-b`), the node count and the completion order, and changes only with
`-s`/`-g`/`-l`. It verifies the lookup arithmetic, which the Master/Worker
sibling's grid checksum does not.

The row additionally prints `XSBench grid checksum: <u64>` — the parent row's
scalar line, computed the same way over the same domain (the sum of the
IEEE-754 bit patterns of the unionized energy grid), once, at place 0. It is
an `extra_scalars` entry here, not the voted scalar, and it exists so that the
two tiers have one value that must agree: at equal `-s`/`-g` the parent row
and this row print the **same** number, whatever `-l`, `-p`, `-b`, the node
count, the arm or the runtime. Every replica holds the same table, so one
place's copy is the whole answer.

### R9 divergence from the tiers it is compared against

R9 asks that generation, parsing, verification and output be
conformance-adapted *identically in every tier of a row*. Grid generation now
is: both tiers include one header (`refactored/ocr/xs_index_rng.h`) and both
run one generator task per nuclide grid, so the tables agree value for value
and the cross-tier grid checksum above is what proves it. What is left:

| | `XSBench_intel` (base, hinted) | `XSBench_intel_dist` (this row) |
|---|---|---|
| grid generation | `N_i` tasks, `rn_indexed(XS_STREAM_GRID, xs_grid_index(...))` — **identical to this row** | `N_i` tasks per place, same generator, same index |
| material concentrations | `rn_indexed(XS_STREAM_CONC, xs_conc_index(j,i))` — **identical to this row** | same call, in the place's own task |
| voted scalar | checksum of the generated **grid** | checksum of the **lookup results** (the grid checksum is carried as an extra scalar) |

Two consequences that must be read with every number this row produces:

1. **The instance is identical end to end** — grids and concentrations alike
   are drawn from the same shared header, with the same streams and the same
   index maps, so the tiers run the same physics table and not merely the same
   parameters. What still differs is only *what each tier votes on*: the parent
   row discards its lookup results and has nothing but the grid to check, so it
   votes on the grid checksum while this row votes on the lookup checksum. The
   two scalars are still not each other's values; the value that is comparable
   across the tiers is the grid checksum, which both print.
2. **Init timing comparisons across the tiers are now like for like** for
   generation, which was the substantive risk: both tiers generate in
   parallel from the index, so a gap measured against the parent row is no
   longer partly a parallel-init advantage. What still differs in init is
   structural and belongs to the decomposition being measured: the parent row
   builds one table and one `U`-wide alignment fan-out for the whole machine,
   this row builds `P` replicas each with a `K`-wide place-local one.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `-s <size>` | H-M benchmark size: `small`/`large`/`XL`/`XXL`; only `small` sets `n_isotopes=68`, others leave the 355 default (`XL`/`XXL` also set `n_gridpoints` unless `-g` was given) | `small` | ✓ parsed in `mainEdt` |
| `-g <gridpoints>` | gridpoints per nuclide (`N_g`); sets `U = N_i·N_g` | 11303 | ✓ |
| `-l <lookups>` | number of XS lookups (`L`) | 15000 | ✓ |
| `-p <places>` | partitions of the lookup range; one producer and one table replica per place, placed at `place % PDs`. The parallelism knob of this row: it sets both how many producers exist and how many replicas the machine holds | 1 | ✓; validated `1 ≤ p ≤ l` |
| `-b <batch>` | lookup chains in flight per place (a batch is a FINISH scope; the next batch starts only when it drains) **and** the target number of blocks the unionized index plane is cut into, so the alignment phase is at most as wide as the compute phase it feeds | 1024 | ✓; validated `≥ 1` |

`n_mats` is fixed at 12 (H-M's material count at either size). There is no
`-t`: this program has no thread knob, and the argument parser rejects unknown
flags loudly rather than accepting an inert one.

Offered concurrency is `W = p · b` lookup chains; see Sizing.

## Structure

Let `N_i` = n_isotopes, `N_g` = n_gridpoints, `U = N_i·N_g`, `L` = lookups,
`P` = places, `b` = batch, and `NB_p = ⌈(L/P)/b⌉` batches per place. The index
plane is cut as the code computes it (`Main.c`, `FNC_placeInit`):
`blockLen = ⌈U / min(b,U)⌉`, then `K = ⌈U/blockLen⌉` blocks — so `K ≤ min(b,U)`,
with equality exactly when `blockLen` divides `U` (at `U = 34,080`, `b = 3456`
the rounding gives `blockLen = 10`, `K = 3408`, not 3456). Every count below is
stated in `K`.

| object | count | size |
|---|---|---|
| place handle (`placeH_t`: run parameters, every handle of the replica, the place's two prebuilt hints) | `P` | ~410 B |
| nuclide grid | `P·N_i` | `N_g · 48` B |
| unionized energy grid | `P` | `U · 8` B |
| index-plane block | `P·K` | `blockLen · N_i · 4` B |
| material index / concentration lists | `P·24` | ≤ `321·4` B / `321·8` B |
| per-slot accumulator | `P·b` | 8 B |
| place digest | `P` | 8 B |
| guid arrays (nuclide, block, shard, material ×2) | `5P` | `≤ (N_i + K + b) · 8` B |
| global `Inputs`, timers | 2 | small |

DBs total `P·(N_i + K + b + 33) + 2`. EDTs total
`P·(N_i + K + 2·NB_p + 7) + 3L + 3`: per place one `placeInit`, three spawn
scopes plus `matInit`, `N_i` grid generators, `K` block aligners, `NB_p`
`placeCompute`/`placeBatch` pairs and one `placeFinalize`; and three EDTs per
lookup. Events are `4P` phase events plus `NB_p` batch events per place, plus
each EDT's output event where one is taken.

## Wiring

Init, per place (three spawn scopes so that a reader's dependence on a block
is always registered by a task that ran after that block's writer released
it — the pattern the rest of this app tree uses):

```
placeSpawner ──(P×)──> placeInit
   placeInit creates every DB of the replica and the six templates, then:
     gridSpawner  (FINISH) ──> N_i × gridGen        [index-seeded generation + sort]
        └─ OET ─> EVT_grid
     uegSpawner   (FINISH, gated EVT_grid) ──> uegBuild   [union + sort]
        └─ OET ─> EVT_ueg
     alignSpawner (FINISH, gated EVT_ueg)  ──> K × align  [binary searches, per block]
        └─ OET ─> EVT_align
     matInit ─ OET ─> EVT_mat                        [material tables]
     placeCompute (gated EVT_align, EVT_mat)
```

Compute, per place:

```
placeCompute ──> placeBatch (FINISH, b chains)  ──OET──> EVT_batch
   EVT_batch ──> next placeCompute      (while lookups remain)
   EVT_batch ──> placeFinalize          (last batch)
placeBatch ──(b×)──> lookup ──> macroxs ──> aggregator
placeFinalize ──> dependence slot 2+place of the global reduce EDT
reduce (Inputs, timers, P digests) ──> checksum, results, ocrShutdown
```

The chain's deps are the sibling's: `lookup` takes the place handle plus the
five handle arrays and the unionized grid (7); `macroxs` takes the handle, the
nuclide guid array, the index block, the material's index and concentration
lists (5); `aggregator` takes those four plus its accumulator (RW) plus one
nuclide grid per nuclide of the material (`5 + num_nucs`, up to 326).

## Flow

1. `mainEdt` runs on rank 0: it parses the CLI, prints the input summary,
   publishes `Inputs` and the timers (both created with no hint, so both are
   homed at rank 0, their creator), starts the timer, and creates the global
   reduce EDT (`2 + P` slots) and the place spawner. Those two EDTs are
   created with `NULL_HINT`, so the runtime round-robins them and neither is
   bound to rank 0. Rank 0 is then touched `O(P)` times in total — one `CONST`
   acquire of `Inputs` per `placeInit` — and never once per lookup.
2. Each place builds its replica: `N_i` generators fill and sort their own
   nuclide grid from a generator that is a pure function of (stream, index);
   `uegBuild` copies the energies into the unionized grid and sorts it, and at
   place 0 prints the grid checksum; `K` aligners each index one block of
   unionized gridpoints into every nuclide grid; `matInit` builds the 12
   material index lists and concentrations.
3. The place's producer walks its share of the lookup range in batches of `b`.
   Each lookup draws its energy and material from its own index, binary-
   searches the unionized grid, and hands the chain on; the aggregator
   interpolates the material's nuclides and folds its quantized result into
   the accumulator of its slot.
4. `placeFinalize` sums the place's `b` accumulators into the place digest and
   attaches it to the reduce EDT. When all `P` digests are in, `reduce` stops
   the timer, prints the checksum and the results line, and shuts down.

## Placement

Internal and unconditional — this is a restructured row, so there is no
`_hinted` family and no `OCR_APP_OPTIMIZED_PLACEMENT` guard. Every object of
place `p` is hinted to policy domain `p % PDs`. The affinity is a constant of
the place, so `placeInit` derives it once
(`ocrAffinityGetAt(AFFINITY_PD, p % pdCount)`) and stores the two finished
hints — EDT and DB — in the place handle; every later create copies the value
out of the handle. The per-lookup path therefore makes no affinity or hint
call at all. A single policy domain leaves the handle unhinted, so a one-node
run passes `NULL_HINT` by construction.

Balance: `P` places over `N` domains gives `⌈P/N⌉`/`⌊P/N⌋` places per domain,
exact when `N | P`. Every campaign node count divides 32, so a `P` that is a
multiple of 32 is exactly even at 1/2/4/8/16/32 nodes, and both the EDT count
and the DB count per node are then `P/N` times a constant. Within a domain,
placement across workers is the runtime's.

The map is an ownership map, not a creator pin: `place` travels in the
paramv of every task of a place, so a chain's links are placed by index, not
by where their creator happened to run.

## Correctness

`XSBench lookup checksum: <u64>`, printed once by the reduce EDT.

Each aggregator computes `q = Σ_k round(macro_xs[k] · 1024)` over the
5-vector, adds `q · (ilookup + 1)` to its accumulator, and the place digests
are summed. Three properties follow: the accumulation is integer, hence
independent of completion order and of how lookups were partitioned; the
index weight makes it sensitive to *which* lookup produced which result; and
the quantization keeps the value stable against last-bit differences in the
floating-point kernel. The value depends only on `-s`, `-g` and `-l`.

The value is comparable across node counts, arms and runtimes. It is **not**
comparable against the parent row's grid checksum, which checks a different
thing; the value that is comparable across the tiers is the second line this
row prints.

`XSBench grid checksum: <u64>`, printed once by `uegBuild` at place 0, over
that place's unionized grid: the sum of the IEEE-754 bit patterns of `U`
doubles, exact and order-independent, a function of `-s` and `-g` only. It is
the parent row's voted scalar and this row's `extra_scalars` entry, and the
two rows must print the **same** value at equal `-s`/`-g` — the cross-tier
oracle. Both tiers generate it from the same shared header, so a disagreement
is a real defect (a table built differently), not a tolerance question.

The lookup checksum is pinned (`5139514524579690947` at the campaign's
argument set); the grid checksum carries no separate pin here — it is
cross-checked live against the parent row's value at the same `-s`/`-g`. Both
must agree at every node count, in every coherence arm, and under xsocr and
ocr-vx. Cheap checks that need no reference
value: `-s small -g 32 -l 4096` with `-p 1 -b 64`, `-p 8 -b 64` and `-p 8 -b 7`
must print identical lookup checksums (the partition is a partition, and a
partial final batch loses nothing), and the grid checksum must equal the
parent row's at the same `-s`/`-g`.

## Sizing

**Width.** `W = P · b` lookup chains offered concurrently (each place offers
`b`, and places never synchronize with each other). For `class: mw` the width
declaration is checked, not merely compared: `Selection._check_width` rejects a
`width_max` below the campaign's 3456 workers **and** any `width_max` that is
not a whole multiple of it. So `P · b` must be an exact integer multiple of
3456 (≥ 2× preferred) — a value merely above the floor does not qualify
(`P=32, b=200 → 6400` is above 3456 and is refused). Checked pairs:
`32 × 108 = 3456` (1×), `64 × 108 = 6912` (2×), `128 × 108 = 13824` (4×). The
catalog runs `-p 384 -b 36` = `13,824` (4×) as well.

The realized frontier per place is bounded by its producer, which emits a
chain every few microseconds, so `P` — not `b` — is what actually fills the
machine: a place keeps roughly (chain latency / emission cost) chains
running. `P` must therefore be several times the node count, not equal to it.
The catalog runs `-p 384`, 12× the 32-node campaign.

**Memory (R8).** Per place the replica is

```
M_place = N_i · N_g · (4·N_i + 56) bytes
        = index plane (U·N_i·4) + nuclide grids (U·48) + unionized grid (U·8)
```

and the whole run holds `P · M_place`, all of it on one node at the 1-node
cell. At `-s large -g 96`: `M_place = 50.3 MB`, so `P = 128` is 6.4 GB and the
190 GB budget is reached only near `P ≈ 3700`. Accumulators, guid arrays and
per-DB metadata add well under 1 MB per place.

**Height.** `-l` is the run length; the physics table is `-s`/`-g`. The
catalog runs the sibling's table (`-s large -g 96`) with `-l 480000000`,
`-p 384`, `-b 36`.

## Difference from the two sibling rows

* **`XSBench_intel` (base / hinted, Master/Worker).** Same per-lookup chain,
  one logical rank: `nprocs` is hardcoded to 1, one producer emits every
  lookup of the machine, and the whole table is homed wherever the single init
  EDT landed. This row keeps the chain and removes the funnel. Generation is
  the same in both — one task per nuclide grid, one shared index-seeded
  generator for the grids and the concentrations alike — so the two build the
  same table, print the same grid checksum, and neither tier's init is serial
  where the other's is parallel. What still differs is the voted scalar (grid
  checksum there, lookup checksum here); see *R9 divergence*.
* **`XSBench_intel_sharedDB` (SPMD).** Also replicates per instance, but its
  compute phase is an **inline loop**: one persistent EDT per thread runs its
  chunk of lookups inside a `for` loop, so there is no per-lookup task, no
  per-lookup dependence registration and no per-lookup acquire. It measures a
  coarse-grain SPMD program on the same physics; this row measures the
  fine-grain task chain freed of the distribution funnel. It also runs its own
  problem instance (its `-l` is per instance, not the machine's total), so it
  is a peer row and not this row's tier.
