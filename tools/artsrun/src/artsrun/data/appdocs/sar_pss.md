# sar_pss

*The SAR pipeline with its inputs on disk instead of in the binary — one
executable and a parameter ladder, the roster's largest single datablock.*
Source: `third_party/ocr-apps/apps/sar/ocr/src/` (11 files, ~4.8k lines),
built from `ocr/problem_size_scaling/`.  Binary: `sar_problem_size_scaling`.

## Overview

The pipeline: two SAR images are formed from a pulse stream and compared.  Per
image, `ReadData` fills the pulse-return block `X` (plus `Pt`, `Tp`),
`FormImage` copies the previous image into `refImage` and zeroes `curImage`,
and `BackProj` fans out `⌈Ix/B⌉²` tiled tasks that each accumulate all `P1`
pulses into their `B×B` pixels.  After the second image, `Affine` registers
current against reference (2-D correlation at `⌊√Nc⌋²` control points →
6-parameter least-squares warp → tiled resampling), `CCD` builds a normalized
correlation map, `CFAR` declares a detection wherever the cell under test is
less correlated than its local clutter ring, and `post_CFAR` prints
`SAR detects: <Nd>` — the catalog marker and scalar.  Provenance: the Georgia
Tech Research Institute Streaming Sensor Challenge Problem reference, ported to
OCR through the `RAG_*` macro layer.

Three things distinguish this target from the retired compiled-in SAR sizes:

1. **Inputs at runtime, not at link time.** It is the one SAR target built
   *without* `RAG_IMPLICIT_INPUTS`, so `mainEdt`'s argv block is live: the
   pulse data, platform positions and pulse timestamps are read from files
   named on the command line, and the radar/image parameters come from a text
   file rather than a compiled-in `Parameters.h`.  The problem size is data, so
   one binary covers the whole ladder.
2. **A parameter ladder ships with it.** `Parameter0.txt … Parameter9.txt` come
   from upstream with `Ix = Iy = 400, 800, … 4000` and everything else fixed
   (`P1 = 4200`, `S1 = 4000`, `Nc = 3629`).  This project added six more by
   the ladder's own rule `Ix = 400*(n+1)`: `Parameter10` (4400), `Parameter14`
   (6000), `Parameter15` (6400) and `Parameter19` (8000) along the way,
   `Parameter17` (7200) for this row's calibrated width, and `Parameter45`
   (18400) for the restructured row.
3. **A coarser tile: the blocking factor defaults to 50, not 32** — this
   variant's own upstream `Makefile.x86` value, preserved by CMake as the
   default of the run-time knob.  Its CFAR percentile is also looser
   (`Tcfar = 75` vs 90), so it reports more detects per scene.

The catalog **pins an `expect`** together with `expect_args`: the detect count
is a deterministic function of the fixture, the parameter file and the tile
block, so the pin is only valid for the argument list beside it.

## Parameters

argv positions 1–4 are **all-or-nothing**: the four path overrides are taken
only when `ocrGetArgc() >= 5` (program name + 4).  One to three arguments is a
usage error that names the four paths and exits.  Positions 5–9 are each
optional and default to the compiled value.

| arg | meaning | CLI reachability |
|-----|---------|------------------|
| `argv[1]` | pulse-return data (`2·P1·S1` complex pairs) | ✓ carried to every node in the `file_args` block, reopened per task — multinode-safe |
| `argv[2]` | platform positions (`2·P1·3` floats) | ✓ same |
| `argv[3]` | pulse timestamps (`2·P1` floats) | ✓ same |
| `argv[4]` | detects output path | ✓ passed by value in `post_CFAR`'s paramv; required, but opened only when `argv[9] = 1` |
| `argv[5]` | radar/image parameter file | ✓ read by `ReadParams` on rank 0 only, into the `image_params` DB — multinode-safe |
| `argv[6]` | **tile block** `B` for every parallel family | ✓ default 50; bounded to `[1, Ix]`, refused loudly outside |
| `argv[7]` | detection block | ✓ parsed and range-checked, **unused in this tier** (it is the restructured row's fused-detection knob; the grammar is identical in both) |
| `argv[8]` | resample block | ✓ same |
| `argv[9]` | write the detections file, 0/1 | ✓ **default 0** |
| `Ix`, `Iy` (param file) | image pixels | ✓ via `argv[5]`; **`Ix != Iy` is unsupported** (see below) |
| `P1`, `S1` (param file) | pulses per image; samples per pulse | ✓ but must match the `.bin` fixtures byte for byte (4200, 4000) |
| `Sx`, `Sy` (param file) | spotlight subimage | ✓; `TF = Ix/Sx > 1` aborts ("digital spotlighting not yet supported") |
| `Nc`, `Sc`, `Rc`, `Tc` | control-point budget / window / radius / threshold | ✓; 3629 → `N = ⌊√Nc⌋ = 60`; 15, 16, 0.7 |
| `Ncor`; `Ncfar`, `Nguard`, `Tcfar` | CCD window; CFAR window / guard / percentile | ✓; 5; 25, 17, 75 |
| `NumberImages` | images to process | ✓ but effectively fixed: any value but 2 is a loud run-time error |
| `DEBUG_SSCP` | dump image/correlation planes | ✗ deliberately left out of every variant |

`argv[9] = 0` is the campaign setting.  The detections file is the program's
bulk output — 4 M lines and ~180 MB at `Ix = 4000`, written by one task inside
the measured window — not its result; the result is the count, which is printed
either way.  `argv[9] = 1` restores the published behaviour byte for byte.

`Ix != Iy` is accepted by the parser and then mis-indexed: `backproject_async`
walks `m` over the `Ix` extent against a row table built with `Iy` rows, and
`affine_async_2` clamps its `Y` loop against `Xend`.  Every shipped parameter
file is square — a hand-written one must be too.

## Structure

With `g = ⌈Ix/B⌉` (all three tiled window grids collapse to `g²` because every
shipped `Ix` is a multiple of 400 and `B` divides it) and `N = ⌊√Nc⌋ = 60`:

| object | count | size |
|--------|-------|------|
| stage heads | 18 (`mainEdt`, `post_main`, `main_body`, `ReadData`×2, `FormImage`×2, `post_FormImage`×2, `BackProj`×2, `Affine`, `post_Affine`, `post_affine_async_1/2`, `CCD`, `CFAR`, `post_CFAR`) | — |
| input-read tasks | `2·(128 + 1)` — 128 slab readers plus a join, per image | — |
| tile EDTs | `2g²` backprojection + `N²` correlation + `g²` resample + `g²` CCD + `g²` CFAR | — |
| global DBs | 14, all in `mainEdt` | `X` 128.2 MiB, `curImage`/`refImage`/`output` `8·Ix²` each, `corr_map` `12·(Ix−4)²`, `Y` `12·(Ix−28)²`, `Pt`/`Tp`/axis vectors, `file_args` 4 KiB, four parameter blocks |
| `Affine` DBs | `5 + N²` | `Fx`/`Fy` (`Nc·4`), `A` (`Nc·32`), `output`, 56 B per control point |
| per-task scratch | `2N²` correlation windows + `6g²` backprojection + `g²` clutter windows + 5 fixed | — |
| EDT templates | 25 per rank | into a fixed `templateList[256]`, bounds-checked on every claim |

Closed forms: `EDT = 3876 + 5g²`, `DB = 10 824 + 7g²`, and **20 events
regardless of size** — the app never calls `ocrEventCreate`, and the 10
`ocrEdtCreate` calls that pass a non-NULL `outputEvent` are exactly the 10 that
carry `EDT_PROP_FINISH`, giving 10 output + 10 finish events.

The live set is also the *total* allocated set: `main.c`, `back_proj.c` and
`registration.c` each `#define` `bsm_free`/`dram_free`/`spad_free` to nothing;
only `cfar.c` destroys anything.  Unlike the retired compiled-in sizes the
binary carries no dataset: the 268.8 MB of input is read from disk, once per
image, by the node that runs that image's `ReadData`.

## Wiring

There is not one explicit event in the program: stages chain through **finish
events**, each head a finish EDT whose fan-out lives in its scope and whose
event lands on the next head's last dependence slot.  `post_FormImage` closes
an imaging round by wiring the refilled `X`/`Pt`/`Tp` into the next `ReadData`
(first round) or `curImage` into `Affine` (second).

| datablock | RW writers | RO readers | max concurrent readers |
|-----------|-----------|------------|------------------------|
| `X`, `Pt`, `Tp` | 128 slab readers, on one node | `backproject_async` | `g²` |
| `curImage` | `backproject_async` (`g²`), `post_Affine` (1) | `affine_async_1/2`, `ccd_async` | `N²` |
| `refImage` | `FormImage` (1) | `affine_async_1`, `ccd_async` | `N²` |
| `output` | `affine_async_2` (`g²`) | `post_Affine` | 1 |
| `affine_params`, `Fx`, `Fy`, `A` | `affine_async_1` (`N²`) | `post_affine_async_1/2` | 1 |
| `corr_map` | `ccd_async` (`g²`) | `cfar_async` | `g²` |
| `Y`, `Nd` | `cfar_async` (`g²`) | `post_CFAR` | 1 |
| `image_params` | `ReadData` (twice) | *every* task in the program | all of them |

Two facts dominate.  **Every parallel family writes one whole-image block**,
and OCR `RW` is per-node exclusive — the tiles of a family all take the same
block RW, so tiles are round-robin placed, each of those blocks is written from
every node's tasks, and a family can never write on two nodes at once.
`curImage` is the contention point: `2g²` exclusive acquisitions of an
`8·Ix²`-byte block in backprojection alone, over disjoint pixel ranges — pure
protocol cost, not an algorithmic dependence.  The fan-out is genuinely
broadcast-shaped: `X` (128.2 MiB) read concurrently by every tile of a round,
the images by up to `N²` tasks, and the 88-byte `image_params` by essentially
every task.  Tiles sharing a writable block coordinate with plain atomics on
the shared copy (`__sync_fetch_and_add` on `affine_params->Nc`, on `Nd`),
varying the *order* of rows and detects but not the counts or the fitted warp.

Distribution-legality adaptations, applied identically to every tier and every
backend: a `FILE*` is process-local, so the input paths (not handles) travel in
the `file_args` block and each file-touching task reopens on its own node;
row-pointer tables and the `xr`/`yr` axis vectors are rebuilt locally from
`(Ix, Iy, dr)` after a block is relocated; and ~108 dependence sites whose EDT
body provably never writes the block are declared `DB_MODE_RO` where upstream
declared `RW`.  The whole-image `RW` *writers* — the exhibit — are untouched.

## Flow

Strictly serial stages, each a one-EDT head fanning out to a tile family and
joining on its own finish event:

1. `mainEdt` (rank 0, serial) — reads the parameter file, validates the three
   input paths by opening them, 14 `ocrDbCreate`s, axis vectors.
2. `refReadData` → **128 slab readers** (all on `refReadData`'s node) + a join
   → `refFormImage` — 134 MB of pulse data off disk, then zero `curImage`.
3. `BackProj` → **`g²` `backproject_async`** — the heaviest stage; each tile
   sweeps all 4200 pulses over `B×B` pixels.
4. `post_FormImage` → `ReadData` (the second 134 MB read, slabbed the same way)
   → `FormImage` (which now really does the `curImage → refImage` copy) →
   `BackProj` → **`g²` tiles**.
5. `Affine` → **`N²` `affine_async_1`**, one per control point,
   `(2Rc+1)²·Sc²` ≈ 245 k operations each.
6. `post_affine_async_1` — serial `A'A`/`A'F` accumulation and two 6×6
   Gaussian eliminations → **`g²` `affine_async_2`** resampling tiles.
7. `post_affine_async_2` → `post_Affine` — serial full-image copy
   `output → curImage`.
8. `CCD` → **`g²` `ccd_async`**; `CFAR` → **`g²` `cfar_async`**; `post_CFAR`
   prints the detect count (and writes the file only under `argv[9] = 1`);
   `post_main` shuts down.

Stage boundaries are hard barriers, so no two tile families overlap: the
**instantaneous frontier is one phase**, and the structure is spawn-and-join.
What bounds how much machine a rung keeps busy is therefore the *narrowest*
wide stage — and the correlation stage is `⌊√3629⌋² = 3600` at **every** rung,
because `Nc` is 3629 in every shipped parameter file.  Remaining serial terms,
in cost order: `FormImage`'s copy-and-zero pair, `post_Affine`'s full-image
copy, and `post_affine_async_1`'s least-squares accumulation.  The two file
reads and the detects write, which used to head that list, are respectively
slabbed and off by default.

## Placement (base)

Every `ocrEdtCreate` in the program passes a literal `NULL_HINT`, and every DB
is created through `bsm/dram/spad_malloc`, which pass `NULL_HINT` too — with
one exception: the 128 slab readers and the join of each `ReadData` carry
`OCR_HINT_EDT_AFFINITY = ocrAffinityGetCurrent()`.  That is a correctness
constraint, not a performance hint: `X`/`Pt`/`Tp` are one object each and their
consumers read them whole, so the readers cannot own separate pieces, and
concurrent writers to disjoint regions of one block are defined within a node
and undefined across one.  The precondition is loud on both axes: the file
refuses to compile without `ENABLE_EXTENSION_AFFINITY`, and a failing
`ocrAffinityGetCurrent` exits instead of falling through to `NULL_HINT` — a
silently round-robin read would be a cross-node write race whose only symptom is
a wrong detect count, which is this row's scalar.

There is **no hinted tier and no guard**: no `OCR_APP_OPTIMIZED_PLACEMENT`
appears anywhere in the SAR sources, in any commit, and CMake defines no
`_hinted` target for this row.  That is the right answer, not an omission: hinted
was tried.  A placement layer here, measured, either collapses the pipeline
onto one rank (97% of the useful work on one of four, five tasks between the
other two) or, written honestly as a coordinate map, runs 23–36% slower than
no hints at all -- so the base's own map is the best hinted map and the base
stands in for the hinted comparison.
The structural reason is decisive: a stage's entire output is one datablock, so
no placement can let two ranks write it concurrently, and the read-only inputs
must reach every rank in any case because a balanced map must put tiles
everywhere.

So: **EDTs** → the shim passes `ARTS_HINT_ANY_RANK` → runtime round-robin, and
every tile task plus every stage head lands on an arbitrary rank; **DBs** →
home = creating rank, so the 14 global blocks are homed on rank 0 while
`output`, `A`, `Fx`, `Fy` and the per-tile scratch home wherever their
round-robin-placed creator ran.

## Sizing

`Ix`/`Iy` is the real dial — all three tile grids grow as `(Ix/B)²` and five of
the blocks grow as `Ix²`.  `P1`/`S1` change no object count, only the bytes of
`X` and the inner-loop length of a backprojection tile, and they must match the
fixture, so in practice they are fixed at 4200×4000.  `Nc` moves only the
correlation stage, as `⌊√Nc⌋²`, and every shipped file leaves it at 3629 —
which makes that stage a *constant* 3600-task floor that neither shrinks with a
smaller image nor grows with a larger machine.  The tile block `B` (`argv[6]`)
trades width against grain at fixed work.

| parameter file | `Ix = Iy` | tiles per grid at `B = 50` | EDTs | DBs | image block | app-side live set |
|---|---|---|---|---|---|---|
| `Parameter2.txt` | 1200 | 576 | 6 756 | 14 856 | 11.0 MiB | ~229 MiB |
| `Parameter5.txt` | 2400 | 2 304 | 15 396 | 26 952 | 44.0 MiB | ~495 MiB |
| `Parameter9.txt` | 4000 | 6 400 | 35 876 | 55 624 | 122.1 MiB | ~1.1 GiB |
| `Parameter10.txt` | 4400 | 7 744 | 42 596 | 65 032 | 147.7 MiB | ~1.3 GiB |
| `Parameter14.txt` | 6000 | 14 400 | 75 876 | 111 624 | 274.7 MiB | ~2.3 GiB |
| `Parameter17.txt` | 7200 | 20 736 | 107 556 | 155 976 | 395.5 MiB | ~3.3 GiB |

Memory: `live ≈ 48·Ix² + 134 MB` app-side; measured RSS was 3.8 GB at
`Ix = 4000` on one Dane-class node, scaling as `Ix²` from that anchor: ~4.6 GB
at 4400, ~8.6 GB at 6000, and ~12.3 GB at the catalog's `Ix = 7200`
(`Parameter17`) — far inside a 190 GB budget.  Width, against 3456 workers and
the spawn-and-join rule (integer multiple, ≥ 2× acceptable): the tile phases
need `Ix ≥ 4151` for 2× and `Ix ≥ 5951` for the project's 4× fork-join slack,
so `Parameter10` gives 7744 = 2.24× and `Parameter14` gives 14 400 = 4.17×.
`Parameter9` (6400 = 1.85×) does not clear either.  **`Parameter17`
(`Ix = 7200`) is the catalog's calibrated choice: `g² = 20 736 = 6×` the
3456-worker floor** — an exact integer multiple, clearing the fork-join slack
with more headroom than `Parameter14`.  The correlation stage stays 1.04× at
every rung regardless of `Ix`; raising `Nc` is the only fix and is a
deliberate deviation from every shipped file, which changes the fitted warp
and therefore the pin.

Width is not throughput here, though: every tile of a stage acquires the same
whole-image block RW, exclusive per node, so write concurrency across nodes is
1 however wide the stage — extra nodes buy image-sized transfers, not
parallelism.  The anti-scaling is a memory blow-up as much as a time one:
measured at 2000 a side, 13.2 s / 1.4 GB at one node against 78.7 s / 110.5 GB
at two.  Declare the expected per-geometry RSS before running; a 4- or 8-rank
death on the uncombined VAL arm is a result about that arm, not a sizing error.

Both tiers need `datasets/sar-huge/` staged (268.8 MB, checksum-gated)
regardless of rung, and every rank must see the three input paths.
