# sar_dist

*The SAR pipeline redecomposed so that no task ever holds the whole image: the
image is a set of row-stripes, one per rank, and every phase produces and
consumes stripes.*
Source: `third_party/ocr-apps/apps/sar/ocr/dist/` (7 owned files) plus six
shared files from `../src/`.  Binary: `sar_dist`.  Restructured tier of
`sar_pss`; computes the same thing over the same instance and reports the same
scalar.

## Overview

`sar_pss` anti-scales for one structural reason: every parallel family splits
the image but writes it back through **one whole-image datablock acquired RW**,
and OCR `RW` is per-node exclusive, so the block tours the policy domains and
each hop carries the whole image.  No placement can fix that — the object is
not divisible, so no rank owns a slice.

`sar_dist` makes the object divisible.  Four decomposition changes, each aimed
at one shared object, plus the stripe layer that carries them:

1. **Projection.** Each tile writes its own block; a per-band gather assembles
   a full-width slab; a per-stripe gather assembles the stripe **on the
   stripe's own rank**.  The whole-image RW dependence, the whole-image
   intermediate and the scatter back out of it are all gone.
2. **CCD is fused into CFAR.** The whole-image correlation map never exists: a
   detection block recomputes the correlation values its own windows need plus
   the detection halo, straight from the two images' stripes.  It does *more*
   arithmetic than the base — `(blk+Ncfar−1)²/blk²` correlation values, 1.54×
   at `blk = 100`, over a read footprint of `(blk+Ncfar+Ncor−2)²` pixels — and
   that is what buys the removal of the shared object.
3. **Detections are per block.** Each block returns its own count in its own
   datablock instead of appending into one array under an atomic; blocks are
   created and reduced by a two-level tree, one row of blocks per task.
4. **Resample.** The registered image is produced as stripes by the same
   band/stripe gathers, so the whole-image `output` intermediate and the copy
   that read it both disappear.

`FormImage`'s whole-image memset and `curImage → refImage` copy go with them:
each generation has its own stripe set, so there is nothing to carry over and
nothing to zero.  `curImage`/`refImage` survive only as the per-generation
**tokens** the wiring discriminates on (`post_FormImage_edt` tests whether the
two GUIDs are equal to tell the first imaging round from the second); they
carry no pixel and are never dereferenced.

Everything else — the projection arithmetic, the correlation and detection
arithmetic, the least-squares warp, the parameter grammar, the fixtures, the
`SAR detects:` scalar — is the base program's.

## Parameters

Same argv grammar as `sar_pss`, position for position, so a roster can move an
override between the two rows without translating it:

| arg | meaning | reachability |
|-----|---------|--------------|
| `argv[1..3]` | pulse data, platform positions, pulse timestamps | ✓ paths travel in `file_args`; each task reopens on its node |
| `argv[4]` | detects output path | ✓ required; opened only when `argv[9] = 1`.  **Give this row its own path** — sharing one with `sar_pss` is shared mutable state between two rows of one campaign |
| `argv[5]` | parameter file | ✓ `ReadParams` on rank 0 into the `image_params` DB |
| `argv[6]` | **tile block** `B` — the projection's tile edge and the stripe layout's alignment | ✓ default 50 |
| `argv[7]` | **detection block** `C` — the fused CCD/CFAR block edge | ✓ default 100, **maximum 100** |
| `argv[8]` | **resample block** `A` | ✓ default 250, **maximum 250** |
| `argv[9]` | write the detections file, 0/1 | ✓ default 0 |

`argv[7]` and `argv[8]` are **downward-only** from their compiled values, and
that is checked loudly at start-up naming both numbers: the detection task's
window buffer (`2·(C+64)²` complex samples) and the resample block's source
window (`(A+64)²`) are automatic storage sized from the compiled maxima, so a
larger run-time value would walk off a task's stack.  Raising the ceiling needs
an `EXTRA_DEFINES` change in `benchmarks/apps/CMakeLists.txt`.

`Ix != Iy` is refused loudly: the stripe layer cuts one axis and every phase
indexes both from it.

## Structure

Let `I = Ix = Iy`, `B` the tile block, `C` the detection block, `A` the
resample block, `N = ⌊√Nc⌋ = 60`, `ns` the stripe count (= the rank count).

| object | count |
|--------|-------|
| stage heads | as `sar_pss`, plus one stripe-collector per producing phase |
| input-read tasks | `2·(128 + 1)` — 128 slab readers plus a join, per image, all on one node |
| projection | `⌈I/B⌉²` tiles + `⌈I/B⌉` band gathers + `ns` stripe gathers + 1 collector, **per image** |
| control points | `N²` tasks + 1 reducer |
| resample | `⌈I/A⌉²` blocks + `⌈I/A⌉` band gathers + `ns` stripe gathers + 1 collector |
| detection | `⌈(I−Ncor−Ncfar+2)/C⌉` row spawners, each creating `⌈(I−Ncor−Ncfar+2)/C⌉` blocks and one row reducer; 1 root |
| stripe DBs | `3·ns` (reference, current, registered) — `8·I²` bytes in total per generation |
| whole-image DBs | **none** |

The stripe layer lives in `dist/blocks.c`: `img_stripes_layout` cuts the image,
`img_stripe_gather_edt` builds one stripe from the full-width row bands that
cover it, `img_stripes_collect_edt` writes the produced blocks into the stripe
set, and `img_stripes_read` copies an arbitrary rectangle out of the covering
stripes into a task's own buffer.  A band whose rows fall in two stripes is
wired to both gathers and each takes the rows it owns, so a phase's band height
need not divide the stripe boundaries.

## Wiring

Every consumer depends on **the stripes it covers**, never on an image.  Each
consumer's footprint is bounded and fits in at most two adjacent stripes, which
is what fixes the minimum stripe height:

| consumer | footprint | slots |
|---|---|---|
| detection block | `C + Ncfar + Ncor − 2` rows of *both* images | 2 stripe slots per image |
| control point | `Sc` square of current, `Sc + 2Rc` square of reference | 2 stripe slots per image |
| resample block | a warp-bounded source rectangle, ≤ `A + 64` a side | 2 stripe slots |

so `min_rows = max(C + Ncfar + Ncor − 2, A + 64, Sc + 2Rc, B)` — 164 rows at
`C = A = 100`, 314 at the compiled defaults.  The layout **aborts loudly** if
the shortest stripe it would realize is below `min_rows` (or if the rank count
exceeds 64) rather than returning fewer stripes than ranks and silently running
the placed phases on a subset of the machine.  It then reduces the stripe count
to a whole multiple of the rank count so no rank owns more stripes than its
neighbour, and re-checks the heights it actually produced.  The two consumers
that wire a fixed **two** stripe slots per image — the detection block and the
control point — check their own footprint against the stripes it lands in and
exit loudly if it reaches a third, the same way the resample already did: a
third stripe would be read out of a slot holding the *other* image's piece,
which is a wrong answer rather than a crash.

Fan-in is bounded everywhere by the grid's side length.  Two levels, not one
dependence per tile, because a dependence costs about forty bytes in the EDT's
control message and the transport bounds that message: a flat gather over the
top rung's grid would be megabytes.  The detection reducer is the same shape —
`⌈Mwins/C⌉` row reducers of `⌈Nwins/C⌉` blocks each, and a root of the row
results — so neither the creation of the blocks nor their fan-in ever passes
through a single task.

When `argv[9] = 1` the row result additionally carries its blocks'
`{nbytes, text}` entries; the root prefixes over rows and creates one row
writer per row, which creates the per-block writers with absolute offsets, and
each block `pwrite`s its own slice at its own offset.  The file is written in
parallel and its line order is block order — strictly more deterministic than
the base's atomic-arrival order, and nothing reads it back.

## Flow

1. `mainEdt` — parameter file, path validation, the three stripe sets laid out.
2. `refReadData` → 128 slab readers + join → `refFormImage` → `BackProj` →
   band spawners → tiles → band gathers → stripe gathers → collector.  The
   reference image now exists as `st_ref`.
3. The same for the current image into `st_cur`.
4. `Affine` → `N²` control-point tasks, each reading its window out of the two
   stripe sets it covers → reducer → least-squares warp → `⌈I/A⌉²` resample
   blocks → band gathers → stripe gathers → collector.  The registered image
   exists as `st_reg`.
5. `CFAR` → `⌈Mwins/C⌉` row spawners → blocks (correlation + detection fused) →
   row reducers → root, which prints `SAR detects: <Nd>`.
6. `post_main` shuts down.

Stage boundaries are still hard barriers, so the instantaneous frontier is one
phase and the structure is spawn-and-join.

## Placement (base)

This tier's map is an **ownership-range map over the stripe partition**, EDT
and DB alike:

```
rank(s) = floor(s * nr / ns)          ns = nr = rank count, so rank(s) = s
```

applied to: the projection's band spawners, their row gathers and their tiles
(by the stripe the band fills); the stripe gathers and the stripe blocks
themselves (by the stripe); the resample blocks (by the stripe of their source
rectangle) and their band gathers (by the stripe they fill); the control points
(by the stripe of the reference window); and the detection rows, their blocks
and their reducers (by the stripe of the registered image they read).  The
collectors and the reducers' root are unplaced — they touch no pixel.

There is no `OCR_APP_OPTIMIZED_PLACEMENT` guard: this is a restructured row, so
its map is part of its decomposition, not a hint layer over someone else's.

Derived load imbalance (max/mean over ranks), at `I = 18400`, `B = 50`:

| phase | unit | 2 | 8 | 16 | 32 |
|---|---|---|---|---|---|
| projection bands (368) | band | 1.00 | 1.00 | 1.00 | 1.04 |
| detection rows, `C = 100` (184) | row | 1.00 | 1.00 | 1.04 | 1.04 |
| resample bands, `A = 100` (184) | band | 1.00 | 1.00 | 1.04 | 1.04 |
| resample bands, `A = 250` (74) | band | 1.00 | 1.03 | 1.13 | **1.50** |
| control points (`N = 60` grid rows) | grid row | 1.00 | 1.14 | 1.20 | **2.00** |

`A = 100` is therefore required for balance as well as for width.  The control
points' 2:1 at 32 ranks follows from `N = ⌊√Nc⌋ = 60` and can only be fixed by
raising `Nc` past the 3629 every shipped parameter file carries — a deviation
that changes the fitted warp and therefore the pinned scalar.

The one place that is deliberately *not* stripe-placed is the input read: the
three input blocks are one object each and their consumers read them whole, so
the 128 slab readers of each image are pinned to the node running the task that
hands the blocks on.  That is a correctness constraint (concurrent writers to
disjoint regions of one block are defined within a node and undefined across
one), and it is identical in both tiers.

## Correctness

Same input files, same parameter grammar, same `SAR detects:` marker and
integer scalar as `sar_pss`.  The arithmetic is the base's, re-partitioned:

- each output pixel of the projection accumulates over the same pulses in the
  same order regardless of tile size;
- each correlation window is computed from scratch from the same pixels, and
  each resampled pixel is an independent bilinear interpolation, so the
  detection and resample blocks are **bit-invariant** to `argv[7]`/`argv[8]`;
- the least-squares reducer sums `A'A` and `A'F` over the retained control
  points, which is order-independent.

What *does* move the scalar is `Ix` and the tile block `argv[6]`: the
projection uses a strength-reduced recurrence whose accumulation length is the
tile edge, so a different tile changes low-order bits and can flip a borderline
detection.  Keep `argv[6]` at the published 50 unless the pin is re-measured.
`argv[9]` never moves it.

There is no verification recompute anywhere in either tier; the scalar is a
count the workload produced.

## Sizing

| knob | effect |
|---|---|
| `Ix` (parameter file) | the size dial; projection `⌈I/B⌉²`, detection `⌈(I−28)/C⌉²`, resample `⌈I/A⌉²`, memory `∝ I²` |
| `argv[6]` `B` | projection width and grain; also the stripe alignment.  Moves the scalar |
| `argv[7]` `C` | detection width, the recomputed correlation halo `(C+Ncfar−1)²/C²`, and the read footprint `C+Ncfar+Ncor−2` that sets `min_rows` |
| `argv[8]` `A` | resample width and its 32-rank balance |
| `Nc` | control-point width, `⌊√Nc⌋²` = 3600 at every shipped rung |

At `Parameter45` (`I = 18400`) with `B = 50`, `C = 100`, `A = 100`, against
3456 workers: projection **135 424** (39×), detection **33 856** (9.8×),
resample **33 856** (9.8×), control points **3600** (1.04×).  At the compiled
`A = 250` the resample is 5476 = 1.58× and fails the spawn-and-join rule, which
is why `argv[8] = 100` is part of the row's arguments.

Coverage envelope.  Stripe boundaries fall on multiples of `B = argv[6]`, and
the units are spread as evenly as whole units allow, so the **shortest** stripe
is `⌊⌈Iy/B⌉/ns⌋ · B` — the condition is on that, not on the average height:

```
⌊⌈Iy/B⌉ / ranks⌋ · B  ≥  min_rows        and   ranks ≤ 64
```

At `B = 50` and `min_rows = 164` this admits **64 ranks at `Parameter45`**
(18400 — 92 by the formula, capped by the descriptor's array bound), **30** at
`Parameter14` (6000), **22** at `Parameter10` (4400) and **20** at `Parameter9`
(4000).  So the row's own rung covers the whole 1–32 sweep with margin, while a
short-image rung of *this* row is a ≤ 16-node trend; 32 ranks at `B = 50`,
`A = C = 100` needs `Iy ≥ 6400`.  A count above the admissible one is refused
loudly by the layout, and so is a realized stripe below `min_rows`.

Memory is **cumulative, not a frontier**.  No intermediate datablock in this
tier is ever destroyed: `dram_free`/`bsm_free`/`spad_free` expand to nothing and
there is no `ocrDbDestroy` anywhere under `dist/`, so every phase's tiles, band
slabs and per-block scratch live until shutdown.  The app-side live set is

```
  8·Ix² ·( 2 projections' tiles + 2 band slabs + 3 stripe sets
           + resample blocks + resample band slabs )        =  72·Ix²
+ 12·Ix²                                (per-block detect lists)
+  4·Ix²·(1 + (Ncfar−1)/C)²             (per-block correlation windows)
+ 134 MB                                (X, Pt, Tp)
≈ 90·Ix²                                ≈ 31 GB at Parameter45
```

The tiling knobs `B`, `C`, `A` move the **datablock count**, not the bytes: a
level's payload is one image's worth however it is cut.  The only knob-sensitive
term is the correlation window, which grows as `C` falls; `Ix` is the only knob
that moves the whole figure, quadratically.  Measured RSS *before* the
whole-image blocks were removed was 49.9 GB at one node.  The retaining arms
replicate stripes, so read the per-node RSS the driver records at the 8- and
16-node cells before committing to a 32-node run.

The row is sized to the ~150 s scaler window rather than the base's 10–30 s,
which is why it runs its own rung of the ladder (`Ix = 400·(n+1)`, so 18400 is
`Parameter45`).  It carries no row `timeout`, so cells fall back to the
profile's; the calibration anchors on record are from the fastest arm, and the
worst arm in this catalog has been recorded at up to 19× the crown — declare an
expected worst-arm time, or pin a row timeout, before the campaign.
