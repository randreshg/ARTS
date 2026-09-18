# cholesky_blas

*The published tiled-Cholesky DAG with every kernel body replaced by a
CBLAS/LAPACKE call — the wiring is unchanged, only the grain and what is
linked underneath.*
Source: `third_party/ocr-apps/apps/cholesky/ocr-mkl/ocr_mkl_cholesky.c`
(~955 lines), with the generated instance in
`third_party/ocr-apps/apps/cholesky/ocr-mkl/cholesky_instance.h` (~85 lines,
shared with the restructured row). Restructured counterpart: `cholesky_dist`.

## Overview

Right-looking tiled Cholesky over `t = ds/ts` tiles per dimension. The task
graph is the classic `k,j,i` nest: one POTRF per step, `t-1-k` TRSMs, then a
trailing update of SYRKs and GEMMs, wired through a
`event[row][col][generation]` grid of STICKY events with a single writer per
tile per generation. Every kernel body is a standard dense routine —
`LAPACKE_dpotrf`, `cblas_dtrsm`, `cblas_dsyrk`, `cblas_dgemm` — against the
vendored `third_party/OpenBLAS` submodule (`arts::openblas`, built
**single-threaded**, `USE_THREAD=0`, so a BLAS call never spawns threads
underneath the per-tile EDT and task-level parallelism stays the only
parallelism). The "MKL" in the source path and binary name is historical: the
upstream example targeted Intel MKL, this build links OpenBLAS behind the same
CBLAS/LAPACKE interface.

All four kernels mutate their acquired `depv[0]` in place and satisfy the next
generation's event with the *same* GUID, so a tile's GUID — and therefore its
home — is fixed for the tile's whole life once `mainEdt` creates it.

The correctness scalar is the trace of the factor, `CHOLESKY trace = %.6f`,
summed over the `t` diagonal tiles in the finisher. That is `O(ds)` additions
over data the finisher already holds and is the only result the run emits by
default.

**What the pin covers.** The instance is generated rather than read (see
**Input** below): `A = D + v·vᵀ`, with the vector entry `v_i ∈ [-1,1)` and the
diagonal shift `d_i ∈ [1,2)` pure functions of the global row index. No entry
is zero, so every tile is non-trivial for the whole run and every GEMM, TRSM
and SYRK reaches a diagonal tile through the trailing updates of its own
column: the trace is a function of the whole factorisation. That is what the
identity input the row used to read could not do — under it the factor was the
identity, every off-diagonal tile was exactly zero, and the scalar could not
move for an error in over 96% of the tasks.

The value is no longer analytic, so `expect` is a measured pin, taken at
these arguments. It is still deterministic: the summation order is
fixed by the finisher's slots rather than by scheduling, and a positive
diagonal plus a rank-one term gives `‖v‖² ≈ ds/3` as the largest eigenvalue
against a smallest of order 1, so the condition number is `O(ds)` (≈ 7.5e3 at
`ds = 22400`) and the factorisation is numerically benign.

Disclosed conformance adaptations (all present in both tiers, none of them a
decomposition change):

- **Kernels.** MKL replaced by CBLAS/LAPACKE plus `posix_memalign`; the
  dependency substitution, not a kernel rewrite.
- **Access modes.** The read-only kernel operands are declared `DB_MODE_RO`
  (TRSM slot 1, GEMM slots 1 and 2, SYRK slot 1, and all of the finisher's
  slots). Upstream declared `DB_MODE_RW` on tiles the kernels only read; the
  mode now matches the access. This is a contract fix, but it removes a
  serialisation of the `t-1`-wide level-0 fan-out and so is disclosed rather
  than silent.
- **Input: generated, in parallel, by the tasks that own it.** With neither
  `--fi` nor `--fib` given — which is what the campaign runs — the matrix is
  not read at all. `mainEdt` still creates each tile's datablock, and one
  generator EDT per tile takes that block `DB_MODE_RW`, fills it from the
  index-seeded instance and satisfies the tile's generation 0, exactly as a
  loader did. The fill is `t(t+1)/2` independent tasks with no I/O, no shared
  state and no ordering between them; the element at global `(i,j)` is a pure
  function of `(i,j)` (a splitmix64-style mix of each index into `v_i`, plus a
  second mix for the diagonal shift), so the same bits come out at any node
  count, in any tier, in any order. The definition lives in one header both
  programs include, so `cholesky_dist` factors the same matrix.
  What this replaces was 25,200 `fread` calls issued one at a time by
  `mainEdt` on rank 0, inside the `[E2E]` window, moving
  `t(t+1)/2 · ts² · 8` = **2.016 GB** off a shared filesystem: `O(ds²)`, no
  smaller at any node count, and varying with filesystem state.
  Both input paths remain accepted and unchanged, so a real matrix can still
  be supplied: `--fib` reads a tile-stream binary (one `fread` per tile
  straight into that tile's datablock; a local adaptation ported from the
  sibling `cholesky/ocr` port, not published code) and the published `--fi`
  reads whitespace text at one `fscanf` per *element* (501.8 M calls at
  `ds = 22400`).
- **Output off by default.** The published default output level writes the
  whole lower triangle to a file one double at a time — at `ds = 22400 /
  ts = 100` exactly 250,891,200 `fwrite` calls and 2.007 GB, serially, inside
  the measured window. `--ol` now takes a documented level 6, "emit no
  solution, only the correctness scalar", and that is the default. This is a
  deliberate deviation from the published default of 2 and the reason is
  measurement: the write is `O(ds²)` filesystem traffic that does not shrink
  with the node count and varies with filesystem state.
- **Loud argument validation.** Every knob is parsed as a decimal with no
  trailing text, an unrecognised option or a trailing non-option token is
  rejected with the usage text, `--fi` and `--fib` may not both be given, a
  zero `ds`/`ts` is rejected, and an input file that cannot be opened is
  reported rather than dereferenced. `--ol` is range-checked (0..6) instead of falling through the
  output switches with no effect. The dead `--fo` knob — parsed but never
  read, the file name being a literal in the finisher — was removed.
- **Release ordering.** `ocrDbRelease` precedes `ocrEventSatisfy` in all four
  kernels, in the generator and in both tile loaders: nothing is exposed while
  its writer still holds it.
- **Create without a hold.** On the generated path the tile blocks are created
  `DB_PROP_NO_ACQUIRE`: the creating task never writes a tile, so it takes no
  hold on one and there is no release to pair with it. That is the standard's
  own form for a block created for a later consumer, and it is not the same as
  acquiring and releasing again — a release by a holder that is not the block's
  home is a write-back, which under the hinted DB plane would have shipped
  2.016 GB of uninitialized bytes from rank 0 on the write-through arms.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `--ds` | matrix size; `t = ds/ts` | required | ✓ `getopt_long`, strict decimal parse, rejected if 0 or not a multiple of `ts` |
| `--ts` | tile edge (the width knob) | required | ✓ same |
| `--fib` | input matrix, tile-stream binary | neither given → generated | ✓ file size is checked against `t(t+1)/2 · ts² · 8` before any tile is read; a short read fails the run |
| `--fi` | input matrix, whitespace text | neither given → generated | ✓ present and correct; not used by the campaign |
| `--ol` | output selection 0..6 | **6 — no solution emitted** | ✓ range-checked; 0 stdout, 1 text file, 2 binary file, 3 text+stdout, 4 binary+stdout, 5 timing CSV + binary file, 6 none |

There is no `--fo`: the option was dead (the finisher hardcodes the output file
name) and was removed rather than wired through.

`ts`, the tile coordinates and (for the finisher) `numTiles` and `ol` reach the
tasks through each EDT's `paramv` (`u64 func_args[]`) — a kernel gets
`(k[,j][,i], tileSize, out-event)` and the finisher `(numTiles, tileSize,
outSelLevel)`; `ds` itself reaches no task. Nothing depends on a global, so the
program is multinode-safe by construction. `mainEdt` runs on rank 0 only.

## Structure

The DAG shape depends only on `t`. Object counts, on the campaign's generated
path:

| object | count | size |
|--------|-------|------|
| EDTs (kernels) | `2 + t² + C(t,3)` — POTRF `t`, TRSM `t(t-1)/2`, SYRK `t(t-1)/2`, GEMM `C(t,3)`, plus `mainEdt` and the finisher | — |
| EDTs (generators) | `t(t+1)/2`, one per tile, only when no input file is named | — |
| DBs (tile payload) | `t(t+1)/2`, created once each in `mainEdt`; no kernel ever creates a replacement | `ts²·8` bytes each |
| DBs (event-guid metadata) | `1 + t + t(t+1)/2` | negligible |
| DBs (`temp2D` pointer-array staging) | `t(t+1)/2` on the **`--fi` text path only**, created and destroyed inside the loader; neither the generated nor the `--fib` path creates any | `ts·8` bytes each |
| Events | `t(t+1)/2 · (t+1)` STICKY, none destroyed | — |

Worked numbers at `--ds 22400 --ts 100` → `t = 224`: EDTs = 1,923,602 (POTRF
224, TRSM 24,976, SYRK 24,976, GEMM 1,848,224, generators 25,200, plus
`mainEdt` and the finisher); DBs = 50,625 (25,425 metadata + 25,200 tile
payload); Events = 5,670,000. Tile payload is 25,200 × 80 kB = **2.016 GB**,
and because no kernel creates a replacement DB that is also the peak resident
payload — nothing dead accumulates on the tile plane.

Most of those events are never satisfied: tile `(i,j)` only ever carries
generations `0..j+1`, but the grid is allocated `(t+1)` deep for every tile.
That is the published wiring and it is part of what the base tier exhibits.

## Wiring

STICKY events indexed `[row][col][generation]`; `DB_MODE_RW` on a kernel's own
tile in slot 0, `DB_MODE_RO` on its `L`-factor inputs:

| kernel | writes | slot 0 (RW) | RO slots |
|---|---|---|---|
| generator `(i,j)` | `(i,j)` gen 0 | the tile's datablock itself | — |
| POTRF `(k,k)` | `(k,k)` | `ev[k][k][k]` | — |
| TRSM `(j,k)` | `(j,k)` | `ev[j][k][k]` | `ev[k][k][k+1]` |
| GEMM `(j,i)` | `(j,i)` | `ev[j][i][k]` | `ev[j][k][k+1]`, `ev[i][k][k+1]` |
| SYRK `(j,j)` | `(j,j)` | `ev[j][j][k]` | `ev[j][k][k+1]` |
| finisher | — | — | all `t(t+1)/2` final tile versions |

Each kernel satisfies the next generation with its own `depv[0]` GUID, so the
per-tile chain is a single-writer sequence over one datablock. The RO fan-out
at level `k` is `t-1-k` readers of each level-`k` factor, maximal at `k = 0`.

The generator is the head of that chain rather than a new kind of edge: its one
slot names the tile's *datablock*, which a dependence satisfies at once, and it
satisfies `ev[i][j][0]` with the same GUID — the event a loader would have
satisfied. Everything downstream is identical whether the tile was generated
or read.

The finisher names every tile version, not just the diagonal ones, because the
published program writes the whole factor out. With output off it still
acquires all of them and reads only the `t` diagonal tiles for the trace. That
terminal gather — 2.016 GB RO onto one rank — is published structure and is
kept; it is the base tier's business to exhibit it. (`cholesky_dist`, the
restructured row, gathers only the `t` diagonal versions, so the two rows
differ here by design, not by adaptation.)

## Flow

`mainEdt` on rank 0 does the whole preamble serially: allocate and create the
`t(t+1)/2 · (t+1)` events, then walk the `k,j,i` nest issuing
`t + t(t-1) + C(t,3)` EDT creates and roughly 1.9 M `ocrAddDependence` calls.
That serial construction from one task IS the published decomposition and is
exactly what the base tier is here to show; it is not adapted away.

What `mainEdt` no longer does is fill the tiles. Creating each tile's block
stays where it was, but the `t(t+1)/2` fills are their own tasks and run
wherever those tasks are placed, concurrently with each other and with the
`k,j,i` nest that is still being built. Nothing in the window reads a file, so
the term that used to sit here — 25,200 serial `fread` calls on rank 0 pulling
2.016 GB off the filesystem, node-count invariant and filesystem-state
dependent — is gone rather than divided. The generators are the same tasks in
both tiers; only the hint they carry differs.

The critical path is the `~3t`-deep POTRF→TRSM→SYRK backbone (`LAPACKE_dpotrf`
is one dense factorisation call per tile, not itself parallel). Peak
instantaneous width is the `k = 0` trailing update, `t(t-1)/2`, decaying as
`(t-k)(t-k-1)/2`.

Acquires grow as `Θ(t³)`: every one of the `C(t,3)` GEMMs fetches its own two
panel tiles, and the per-task dependence model has no way to express a
per-rank panel broadcast. No node count reduces that count, which is why the
row anti-scales and why the restructured row exists.

## Placement (base)

Hint-free. `choleskyTileHint` and `choleskyTileDbHint` both return `NULL_HINT`
outside `OCR_APP_OPTIMIZED_PLACEMENT`, so every kernel EDT, every generator and
the finisher take the shim's no-hint path and round-robin across ranks, and
every `ocrDbCreate` defers to the runtime's no-hint DB policy — creator-homed,
and the creator is `mainEdt` on rank 0. So all 25,200 tiles and all 25,425
metadata blocks home on rank 0 for the whole run, while the kernels that touch
them are scattered: nearly every dependence edge is a remote acquire, with a
deterministic remote target rather than a scattered one.

The generators are in that same population, so the fill of a tile is normally
not on the tile's home: a generator round-robins onto some rank, acquires the
rank-0-homed block `DB_MODE_RW` and releases it there. On the write-through
arms that release publishes the tile back to rank 0. The bytes are the same
2.016 GB the file read used to move, but they now move over the fabric from
`t(t+1)/2` tasks at once instead of through one serial reader, so unlike the
read they divide with the node count. That is a consequence of the base tier
being hint-free, not of generating: the hinted tier puts the generator on the
tile's home and the fill is local (see below).

## Placement (hinted)

`-DOCR_APP_OPTIMIZED_PLACEMENT` adds hints and nothing else — no control flow,
partition, DB count, event wiring or arithmetic differs between the two
binaries, and every call site is byte-identical.

The map is the canonical ScaLAPACK 2-D block-cyclic owner map. Factor the
policy-domain count into a near-square `P × Q` with `P` the smallest divisor
of `nranks` with `P² ≥ nranks`, then own tile `(row,col)` on rank
`(row % P) * Q + (col % Q)`: 2→(2,1), 4→(2,2), 8→(4,2), 16→(4,4), 32→(8,4).
The grid is derived once per run.

Two hint planes ride the *same* key:

- **EDT.** Each kernel is keyed on the coordinate of the single tile it
  writes, so it co-locates with its RW output and the per-tile ownership
  acquire settles locally and stays there across generations. The generator of
  a tile carries the same key, so the tile is filled on the rank that owns it
  and its first version never crosses the fabric.
- **DB.** Each tile payload is created with `OCR_HINT_DB_AFFINITY` on the same
  coordinate, so the block's home is the rank that writes it in every
  generation. Without this the tiles stay creator-homed on rank 0 in the
  hinted binary as well, and under a write-through policy every non-home
  release publishes its payload to that one rank.

The finisher is keyed on the map's origin. It names every tile, so no
coordinate localises it; keying it makes its rank a property of the map rather
than of creation order. The event-guid metadata blocks stay unhinted: they are
dereferenced only by `mainEdt`, so their home belongs where `mainEdt` runs.

`nranks <= 1` returns `NULL_HINT` on both planes, so a single-node run of the
hinted binary is the base program by construction — which is what makes the
1-node cell a clean size anchor for both tiers.

**Coverage and balance.** `home` ranges over all of `[0, P·Q)`, and every
residue class is populated at any `t` past `P·Q` — 167 cleared it and so does
the catalog's `t = 224`. Weighting each tile `(j,i)` by the number of tasks
that write it (`i+1`), the per-rank load ratio max/avg : min/avg was
1.009:0.991 at 2 ranks, 1.018:0.982 at 4, 1.036:0.965 at 8, 1.054:0.947 at 16
and 1.091:0.913 at 32 **at the previous `t = 167`**, with **no idle rank at
any geometry**; the exact ratios move with `t` and need re-deriving at the
catalog's `t = 224`.

**What the map cannot fix.** A GEMM `(j,i)` at step `k` reads `(j,k)` from its
process row and `(i,k)` from its process column; no static tile→rank
assignment makes both local. The layer trades remote acquires for local ones
and removes none — which is why it improves the constant and not the slope,
and why the restructured row is the answer to the slope. When `P` and `Q`
share factors the diagonal-tile tasks land on a strict subset of the ranks
(8 of 32 at `P=8, Q=4`); that is what ScaLAPACK itself does and the GEMMs
dominate the balance.

**What the DB plane costs up front.** Homing every tile away from its creator
turns all `t(t+1)/2` = 25,200 tile creates into *remote* creates issued
serially from `mainEdt` on rank 0. That is the whole preamble term, on every
arm:

| term | write-through arms | write-back arms |
|---|---|---|
| remote `DB_CREATE_COHERENT` messages | 25,200 | 25,200 |
| creator-side publish payload | none | none |

There is no payload term because the creating task never holds a tile: the
blocks are created without an acquire and the generator writes each one where
it is homed. (When the block is created acquired and released by a creator that
is not its home — which is what a loader does, and what the `--fi`/`--fib`
paths still do — a write-through release publishes the payload synchronously,
`will_publish = (!is_home && buf != NULL)`, so those paths do carry
`t(t+1)/2 · ts² · 8` = 2.016 GB of serial rank-0-sourced traffic in the hinted
binary. The campaign's generated path does not.)

So on the write-back arms the 25,200 remote creates are a pure addition with
nothing to earn them back, and on write-through the layer's return has to come
from the steady state rather than from the preamble.

**Gate.** `calibration pending` — one geometry- and argument-labelled table at
the catalog arguments across 2/8/32 nodes. Both earlier number sets (an
appdoc pair at unstated arguments and a design-note table at `--ds 10000`
through the text-input path) are superseded and were removed rather than
reconciled. The DB-affinity plane needs a per-arm A/B that measures **both
ends**: the steady-state saving (a non-home release no longer publishes ~80 kB
to rank 0 on every one of the ~1.9 M kernel releases, and the 25,200 fills stop
crossing the fabric at all) *and* the preamble cost in the table above. Expect
it to pay on the write-through arms, where the home holds the newest bytes, and
to be close to inert under write-back, where the home is a directory and
ownership migrates with the writer. Gate on per-rank remote-acquire and
payload-byte counters at 8 nodes, `ocr_val_wt` vs `ocr_val_wb`, with and
without the hint.

## Sizing

`ts` is the tile edge and sets grain (`~ts³` flops a kernel, `ts²·8` bytes a
block); `t = ds/ts` sets DAG depth (`t` serial POTRF steps) and peak width
(`t(t-1)/2`).

The structure is a dataflow frontier, not a spawn-and-join grid, so the width
rule binds on the instantaneous frontier: four times the largest geometry's
3456 workers, i.e. 13,824, would put `t` at 167 (`ds = 16700` at `ts = 100`,
peak 13,861 = 4.01×); the catalog instead runs `t = 224` (`ds = 22400` at
`ts = 100`), peak **24,976 = 7.23×**. The frontier decays quadratically and
drops below 3456 once fewer than 84 tiles remain in the trailing update — a
threshold set by the 3456 floor alone, independent of `t` — so **94.9%** of
the update tasks execute while it is at or above 3456, against 87.7% at the
smaller `t = 167`. The generated instance is SPD by construction with no zero
entry, so the trace depends on the whole factorisation and the pin is measured
rather than analytic (see Overview). The `t(t+1)/2` generators are a second,
wider frontier that runs before and alongside the factorisation's own —
25,200 independent tasks at `t = 224`, each `ts²` writes of one multiply and
one add.

`ds` is the size knob and `ts` the width knob. Shrinking `ts` to reach a
window is the one move the width rule forbids — arithmetic per byte moved goes
with `ts`, so a smaller tile is a different program, and only `ds` may move
for a trend.

**Memory (R8).** Three planes, all bounded analytically in `t`:

| plane | count | bytes each (upper bound) | at `t = 224` |
|---|---|---|---|
| tile payload | `t(t+1)/2` | `ts²·8` | 2.016 GB (`≈ 4·ds²`, independent of `ts`) |
| event objects | `t(t+1)/2·(t+1)` | 224 = 128 (`arts_event_s`) + 32 (shared cb) + 64 (route slot) | 1.27 GB |
| task objects | `t + t(t-1) + C(t,3) + t(t+1)/2 + 2` (the generators are the `t(t+1)/2` term) | 384 = 128 (`arts_edt_s`) + `8·paramc` (≤ 40) + `40·depc` (≤ 120) + 32 + 64 | 739 MB |
| pending dep nodes | ≤ one per `ocrAddDependence` | 32 (`arts_event_dep_s`) | ≤ 180 MB |

(the finisher is the one task outside the 384-byte bound: its `depc` is
`t(t+1)/2`, so its trailing `depv` alone is `40·25,200` = 1.01 MB. Struct sizes
are the runtime's, taken from `runtime_types.h` / `gas/route_table.h` at
`ARTS_CACHE_LINE_SIZE` alignment; allocator rounding and per-DB coherence state
are not in them, so treat the totals as an analytic floor with the doubling
below as the ceiling.)

Nothing on the event or task plane is ever destroyed, so those two are
whole-run resident; the payload plane is also its own peak, since no kernel
creates a replacement DB. Summed at the catalog arguments the object planes are
**~2.2 GB** against **2.016 GB** of payload — about **4.2 GB**, and under
10 GB even at a 2x allowance for allocator slack and coherence bookkeeping.
That is **~2-4% of the 190 GB per-node budget**, and the bound is `O(t³)` in
objects against `O(t²·ts²)` in payload, so `ds` would have to grow far past
any plausible campaign size before either plane threatened it.

The generated path and the `--fib` path hold no whole-matrix buffer; the `--fi`
path additionally holds `ds²·8` = 2.231 GB until it is freed after tiling, so
the input paths do not have the same residency — the 7 GB previously on record
was measured through the text path and includes that 2.231 GB. This row's
measured figure through the generated-fill path is 12 GB (see the anchor
below), well inside the analytic bound above; the family's fuller memory
picture is `cholesky_dist`'s to give.

**Anchor and trend.** The one-node anchor is measured at these arguments:
16.8 s base / 17.0 s hinted, 12 GB resident, against a 20 s target (hinted
kept, flat). Every figure previously recorded here was taken through the
`--fi` text path, which the catalog no longer uses, and measured neither the
current output default nor the current input. Three removals from that path
account for the drop: the text parse (the same DAG from the tile stream was
12.3 s against a 35.2 s text-path run at 15 workers), the 1.116 GB output
write, and the 1.122 GB serial input read — replaced by `t(t+1)/2` parallel
fills. Re-anchor only through `ds`, never through `ts`.

The campaign cell needs no staged fixture at all now, so `ds` is free of the
cost of baking and storing a tile stream: any size the window asks for is one
argument away.
