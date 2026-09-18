# hpgmg

*Full-multigrid Poisson solve on a cubical box grid — one EDT per box per
operator, one serial barrier per level visit.*
Source: `third_party/ocr-apps/apps/hpgmg/refactored/ocr/sdsc/` (11 files;
SDSC's OCR port of Sam Williams' HPGMG-FV).

## Overview

Solves `-∇·(β∇u) = f` on the unit cube with Dirichlet boundaries by an **F-cycle
(FMG)**: restrict the right-hand side to the coarsest grid, solve there, then
prolong one level at a time, running a full V-cycle after each prolongation.
Each V-cycle level does 4 Chebyshev smooths, a residual, a restriction and (on
the way up) a prolongation; the bottom is closed by a serial BiCGStab
(`solve_edt.c`).  The domain is cut into `boxes_in_i³` cubical boxes, one
datablock each, holding 12 grid vectors (`u`, `f`, `f_Av`, `u_true`, `alpha`,
`beta_{i,j,k}`, `Dinv`, `L1inv`, `valid`, `vec_temp`) plus one ghost layer.

The result scalar `||error||` is the max-norm of `u − u_true` on the finest grid
— the *discretization* error, O(h²), a property of the grid rather than of the
solver's iteration count, so it moves with the arguments (hence `expect` is
pinned to `expect_args`).  It is reduced per box and then maxed, so it is exact
and identical at every node count.  What the program stresses is a
**bulk-synchronous DAG with collapsing parallel width**: every operator is a
fan-out of `num_boxes` short EDTs joined by a finish event and the level chain is
strictly serial, so the solve is a long sequence of barriers whose width falls 8×
per agglomeration step down to a single box.  The arithmetic is real (a 7-point
variable-coefficient stencil), but at the coarse end the cost is entirely barrier
+ coherence.

**Initialization is not part of that exhibit** and is no longer serial.  The
hierarchy's *layout* — the level datablocks, the box datablocks and their guid
table — is still built by one task, because that table is the program's own
structure; everything that fills a box (zeroing, the valid-cell mask, the
analytic evaluation of `u_true`/`f`/`α`/`β`, the coefficient restriction down the
hierarchy, the Gershgorin `Dinv`/`L1inv` sweep and the 26-neighbour ghost
exchange of both diagonals) runs as one task per box, placed with the box it
owns, in the same phase-chained shape as the solve.  The analytic evaluation is a
pure function of a cell's coordinates — no RNG, no shared state — so it is
bit-identical however the boxes are distributed.  The two level-wide scalars it
produces (`dominant_eigenvalue_of_DinvA`, and `alpha_is_zero` from
`dot(α,α) == 0`) are **maxima**, reduced through a merge task: exact, and
independent of the reduction order.  **Disclosure:** `alpha_is_zero` has no
reader — its only consumer is the periodic-boundary mean shift, and the boundary
condition is a Dirichlet literal at both call sites — so its `O(n)` pass per box
is dead work.  It is kept because it is the published program's own operator
build, and both rows pay it identically; dropping it is a two-line change in the
Gershgorin task and its final.  Verification is adapted the same way: the
`||error||` sweep is one O(n) pass per box on the box's own rank, then one max.

## Parameters

`mainEdt` requires exactly two arguments (`argc != 3` prints usage and shuts
down).

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `argv[1]` = `log2_box_dim` | box edge in cells, `box_dim = 1 << log2_box_dim`; must be ≥ 4.  This is the **grain**: `box_dim³` cells per per-box EDT.  Published default **4** | none (required) | ✓ parsed in `mainEdt`, consumed entirely inside `init_all`; every derived value lands in the level/box DBs and the app has no file-scope globals — multinode-safe |
| `argv[2]` = `target_boxes` | **cap** on the *total* box count: the largest `boxes_in_i` with `boxes_in_i³ ≤ target_boxes` is used.  This is the **width** knob | none (required) | ✓ same path |

Both are validated loudly at argument time.  `hpgmg_check_geometry` walks the
level table before any datablock exists and refuses a box grid that (a) would
agglomerate more than `MAX_FINE_BOXES` (125) fine boxes into one coarse box — the
transfer operators carry fixed-size fine-box lists — or (b) does not bottom out
on a single box, which the last restriction's ONCE hand-off and the bottom solver
both require.  It also refuses (c) a level whose level or box datablock size does
not fit the 32-bit datablock size (which a box grid above ~406³, or a very large
`log2_box_dim`, reaches — including the `target_boxes ≥ 10⁹` case where the
derivation loop never leaves its seed) and (d) a geometry whose **per-rank**
footprint exceeds the 190 GB a rank may occupy, naming the computed number.  It
prints the level count, the grid edge, the total datablock footprint in GiB, the
rank count and the per-rank share either way, so one 1-node log answers the
memory question.

Everything else is compile-time: ✗ `TIMED`/`WARMUP` (1 / 0 in `hpgmg.h` — timed
and warm-up F-cycle counts, both upstream values; `WARMUP = 0` makes `top_warm` a
pass-through), ✗ `NUM_SMOOTHS`(1) × `CHEBYSHEV_DEGREE`(4) = 4 smooth sweeps per
level visit, ✗ `MG_AGGLOMERATION_START`(8), the box edge below which the hierarchy
stops halving boxes and starts merging 8 into 1; and legitimately fixed:
`NUM_VECTORS`(12) / `NUM_GHOSTS`(1) (they set the box DB layout),
`STENCIL_VARIABLE_COEFFICIENT` + `STENCIL_FUSE_BC` (`operators.h`),
`BOX_SIMD_ALIGNMENT`(1) / `BOX_PLANE_PADDING`(8), `minCoarseDim = 1`,
`jMax = 200`, `desired_reduction_in_norm = 1e-3`.  `BC_DIRICHLET` is passed as a
literal, so `BC_PERIODIC` is unreachable and the periodic mean-shift branch is
not compiled into any behaviour.

## Structure

`hpgmg_level_table` derives the level table (one function, shared with
`hpgmg_dist`): halve `box_dim` while it exceeds 8, then halve `boxes_in_i` (8:1
agglomeration) until one box remains, then halve `box_dim` again down to 1.  For
a power-of-two `boxes_in_i` this always ends in a **four-level single-box tail**
with `box_dim` 8, 4, 2, 1; for a box grid with an odd factor (e.g. 24 = 2³·3) the
table instead reaches a 3-box edge and collapses 27 boxes into one at the bottom,
which is legal but exercises the 27:1 transfer paths.

The worked example below uses `['5','512']` (`box_dim = 32`, `boxes_in_i = 8`,
`L = 9`) — the historical calibration size, kept here because every count in this
section is derived from it.  The campaign size is `['4','13824']` (see
**Sizing**).

| level | box_dim | boxes_in_i | boxes `N` | one box DB | all boxes |
|-------|---------|-----------|-----------|------------|-----------|
| 0 | 32 | 8 | 512 | 3 773 200 B | 1 842 MiB |
| 1 | 16 | 8 | 512 | 559 888 B | 273 MiB |
| 2 | 8 | 8 | 512 | 96 016 B | 46.9 MiB |
| 3 | 8 | 4 | 64 | 96 016 B | 5.9 MiB |
| 4 | 8 | 2 | 8 | 96 016 B | 0.7 MiB |
| 5–8 | 8,4,2,1 | 1 | 1 | 96 016 … 4 048 B | < 0.2 MiB |

A box DB is `16 + 12·(box_dim+2)·kStride·8` bytes (`kStride = (box_dim+2)²`,
floor-padded to 8 in the pencil); each level also carries one *constant box* of
that size (the out-of-domain neighbour sentinel) and a level DB of `240 + 64·N`
bytes.  Long-lived DBs = `2 + 2L + Σ N_l` (the per-level `temp` pointer arrays
are gone — a `box_type**` inside a datablock was never valid across ranks), plus
one scratch per `solve_edt` and one in `print_timing_edt`, plus a stream of
16-byte reduction results that their merges destroy.  Payload **≈ 2.12 GiB** at
this size, live for the whole run.

EDTs come in three tiers.

**The initialization chain**, built by `init_all` inside `mainEdt`, is `4L−1`
level tasks — `L` prep, `L−1` coefficient restriction, `L` Gershgorin, `L` ghost
exchange — plus `L` merges and `L` per-level finals, each level task a FINISH
whose fan-out is that level's boxes.  Per-box init EDTs number
`3·Σ N_l + Σ_{l>0} N_l` (prep, Gershgorin and diagonal exchange on every level,
coefficient restriction on every level but the finest) = **5 936** here, against
`Σ N_l = 1 612` boxes.

**The level chain** of the solve is materialised up front — `do_solves` is a
plain C function running inside `top_loop`, so all of it exists before most of it
runs.  With `S = 4`, `T = L(L−1)/2` (= 36 here): `exchange_level_edt`
`(L−1)+(2S+1)T+1` = 333, `smooth_level_edt` `2S·T` = 288, `time_edt`
`3+(L−1)+5T` = 191, `restrict_level_edt` and `interpolate_level_edt` `(L−1)+T` =
44 each, `residual_level_edt` `T+1` = 37, `zero_vector_level_edt` `T` = 36,
`solve_edt` `L` = 9, plus 2 `init_ur_level_edt`, 3 norm/mulv and 6 driver EDTs —
**993**.  Each spawns one EDT per box of its level (restriction and zeroing per
*coarse* box); fan-out phases per level number `23` at level 0, `21(l+1)` for
`1 ≤ l ≤ L−2` and `3L+1` at the coarsest, so solve-phase per-box EDTs
`= 23N_0 + Σ 21(l+1)N_l + (3L+1)N_{L−1}` = **72 221**.

**The finalization** is one FINISH level task, `N_0` per-box error tasks, one
merge and one printing final — the fine-grid gather that used to hold every
level-0 box RW in a single EDT is gone.

Events: `ocrEventCreate` (`2L+4`: the init start, one per Gershgorin level
(`L`), the two solve-start ONCE events of `top_warm`/`top_loop`, the coarsest
box hand-off of `restrict_all` and one per V-cycle's last restriction (`L−1`),
and the error reduction), the output event of every
`ocrEdtCreate` with a non-NULL `outputEvent` — which now includes each per-box
Gershgorin and error task — and one finish event per `EDT_PROP_FINISH`.
Dependence slots resolved are dominated by the solve's 26-neighbour exchanges;
initialization adds one 28-slot task per box per level for the diagonal exchange.

Counter cross-check: **stale, needs a fresh run.**  The previous absolutes were
taken against a since-reverted norm-collector variant and predate the
distributed initialization and the reduced finalizer.  One structural fact that
does survive: every total is `app formula + 2` EDTs and `+1` DB, because
`main_edt` (`libs/src/core/system/runtime.c`) is itself one EDT and the OCR
shim's own `main_edt` creates the argv DB *and* a second EDT,
`mainEdtTrampoline`, to carry the app's `mainEdt` in as a DB dependence — a
shim-bootstrap fact, not specific to this app.

## Wiring

`mainEdt` lays out the hierarchy, wires the initialization chain, wires
`top_warm` behind that chain's terminal event, and only *then* satisfies the
chain's start event — a chain whose consumer is not yet registered could
otherwise complete first.  `init_all` returns that terminal event and takes the
start event by reference for exactly this reason.

The initialization chain is a dependence rope of one phase per level per kind:
`prep(0..L−1)`, then for each level `coeff(l)` (`l > 0`), `gersh(l)` →
`eigen_final(l)`, `diag(l)`.  Each phase task takes its level DB CONST and the
previous phase's finish event on its last slot.  Its children take the level DB
CONST and their own box **RW**; the coefficient task adds the finer level DB CONST
and its `count` fine boxes CONST; the diagonal-exchange task adds the 26
neighbour boxes CONST (out-of-domain neighbours resolve to the level's
`constant_box_guid` and are skipped by id).  Each Gershgorin task *returns* a
16-byte datablock holding `{eigenvalue bound, dot(α,α) term}`; a merge with one
slot per box takes the elementwise max, destroys the inputs and satisfies a ONCE
event with the pair; `eigen_final` takes the level DB **RW**, that pair, and the
phase's finish event, and writes `dominant_eigenvalue_of_DinvA` and
`alpha_is_zero`.  No task ever writes a shared datablock slot concurrently with
another, which is what makes the two scalars node-count invariant.

The solve's level chain is the published rope: every level EDT takes its
predecessor's finish event on its last slot, so phase `k+1` cannot start until
every child of phase `k` has completed and released.  Its other events are one
ONCE event per restriction sequence, which `restrict_edt` satisfies **with the
coarsest box's GUID** (`ocrDbRelease` then `ocrEventSatisfy`) — that is how the
bottom solve receives its datablock, on an **RW** slot, since BiCGStab updates
that box in place.

Per-box solve EDTs uniformly take the level DB on slot 0 and their own box on
slot 1.  `smooth_edt`, `residual_edt`, `init_ur_edt`, `mulv_edt` and
`zero_vector_edt` take level RO + box **RW**; `norm_edt` takes the level **RW**
and its box RO, depositing one double into the level's `b_norms` slot — so the
whole norm fan-out serializes on the level datablock's per-node-exclusive turns,
the port's starkest one-datablock fan-in.  **It is kept exactly as it ships**
(it is part of what the base tier exhibits) and its printed line
`f-cycle,    norm=` is therefore **not an oracle at ≥ 2 ranks**: concurrent RW
turns can drop other nodes' slices.  The reliable twin is `final residual norm =`,
computed by the finalizer's per-box reduction over the same `vec_temp` before it
is overwritten, and it is what the catalog reads as `residual`.  `exchange_edt`
adds 6 (face) or 26 (face+edge+vertex) neighbour boxes **RO**; out-of-domain
neighbours resolve to the level's single `constant_box_guid`, so one DB appears
`6B²` times per 6-neighbour round and `27B³ − (3B−2)³` times per 26-neighbour
round.  `restrict_edt` is one per coarse box: coarse box **RW**, both level DBs
RO, its `N_fine/N_coarse` fine boxes RO; `interpolate_edt` mirrors it with the
fine boxes **RW** and the coarse box RO for the piecewise-constant (V-cycle)
prolongation, **RW** for the linear (FMG) one, which applies the boundary
condition to that box's ghost cells before reading it.

DB concurrency, worst first.  The **level DB is the contention point**: its level
EDT holds it RW while all `N_l` children hold it RO (children are created and
satisfied inside the parent's body, so the overlap is real, not nominal).  Next
is the constant box, read-only but with the fan-in above.  A regular box has at
most 27 simultaneous accessors during a 26-neighbour exchange — its own writer
plus 26 readers; the writer touches only ghost cells and the readers only
interiors, so the regions are disjoint even though the datablock is not.

## Flow

One F-cycle: `time_all` → `init_ur` ×2 → `restrict_all` (`L−1` restrictions down
the hierarchy) → bottom `solve_edt` → for `l = L−1 … 1`: prolong to `l−1` and run
`vcycle(l−1)` → `scaled_residual_norm` → `time_all`.  `vcycle(ln)` descends
`ln … L−2` (smooth ×4, residual, restrict, zero), solves at the bottom, and
ascends with interpolate + smooth ×4.  Ahead of all of it, the initialization
chain runs `4L−1` phases of the same shape.

Parallel width in a phase at level `l` is exactly `N_l = boxes_in_i(l)³` and is
the *only* parallelism — nothing at level `l` overlaps anything at level `l'`,
and the same holds for the initialization phases.  For `['5','512']` the widths
are 512, 512, 512, 64, 8, 1, 1, 1, 1, while the *number* of phases runs the other
way (FMG visits a coarse level once per V-cycle that reaches it): 23 at level 0,
168 at level 7.  So **469 of the 786 solve fan-out phases (60%) have width 1** and
together carry 0.015% of the arithmetic; a level-8 phase is a full barrier around
one EDT that updates one cell.  That inversion — barrier count rising as work
falls — is the defining shape of the program.

Serial bottlenecks, largest first: (1) the `L` bottom solves, each a BiCGStab of
up to 200 iterations inside one EDT on one box; (2) the single-box tail; (3)
`top_loop`'s body, which issues all 993 level creates and ~2 000
`ocrAddDependence` calls itself; (4) `mainEdt`'s layout pass, which issues
`Σ N_l` box-datablock creates and writes the guid table.  The former
94%-of-E2E serial preamble — the analytic evaluation and the operator build on
one worker of rank 0 — is gone.

## Placement (base)

Unusually for this catalog the base program places things explicitly —
`ENABLE_EXTENSION_AFFINITY` is on for every benchmark build and most of the
affinity code sits *outside* the `OCR_APP_OPTIMIZED_PLACEMENT` guard.  (The
guarded layer exists: it swaps the box home for a spatial 3-D partition and adds
hints to the operators and to the control spine.)  As-born:

- **Box DBs** carry an explicit `OCR_HINT_DB_AFFINITY = box_num % rank_count`
  (the `#else` branch of the guard in `init_create_level`) at every level —
  round-robin over a linear 3-D index.
- **`smooth_edt`, `residual_edt`, `restrict_edt`, `interpolate_edt`** get EDT
  affinity from `ocrAffinityQuery(box)`, i.e. the box's home — these four *are*
  co-located with the box they write, in both tiers.
- **Everything else is hint-less** → runtime round-robin (the shim's no-hint EDT
  policy is ROUNDROBIN, not self-rank): `exchange_edt`, `init_ur_edt`,
  `zero_vector_edt`, `mulv_edt`, `norm_edt`, every per-box *initialization* task,
  and all 993 solve level EDTs plus the `4L−1` initialization phase tasks.
- Level, `mg` and per-level constant-box DBs are created in `mainEdt` with
  `NULL_HINT` (the shim's no-hint DB policy is CREATOR) → home = rank 0; each
  level's constant box inherits box 0's affinity → also rank 0.  Each
  `solve_edt` scratch is `NULL_HINT` created *inside the running task*, so in
  base it lands on whatever rank runs that bottom solve, and only under the
  guard is it rank 0.

The resulting traffic: the ghost exchange — the most data-heavy phase, roughly
32 k of the 72 k per-box solve EDTs, each acquiring 1 box RW and 6–26 RO — is
placed with no relation to any of its boxes, so essentially every one of its
neighbour RO slots is remote.  Round-robin over a linear box index also scatters
spatial neighbours onto consecutive ranks, so even a co-located operator's
*neighbours* are remote, and a coarse box and the 8 fine boxes it covers land on
unrelated ranks, making every restriction and prolongation edge cross.  The same
applies to the initialization phases: the coefficient restriction and the
diagonal exchange read the same neighbourhoods.  Rank 0 further homes all `L`
level DBs and all `L` constant boxes.  The locality the algorithm has — spatial
adjacency within a level, parent/child nesting across levels — is real and
completely unexpressed.

## Placement (hinted)

The layer replaces the map, not the mechanism.  It has exactly two halves, and
both matter to an audit.

**The box map.**  `boxHomePD` partitions the unit cube once into a near-cubic
PX × PY × PZ rank grid (`boxFactor3`) and sends every level's box through THAT
one partition by its normalized `(i,j,k)/S` position, so neighbouring boxes of a
level co-locate AND a coarse box lands on the rank of its fine children —
inter-level transfers stay rank-local.  Every per-box EDT, in the solve *and* in
the initialization, follows its box via `ocrAffinityQuery(box)`
(`mgBoxEdtHint`), keeping compute with data.  Coverage is all ranks and load
imbalance is exactly **1.00** at 2, 8, 16 and 32 ranks wherever a level has ≥ 1
box per rank (`PX`, `PY`, `PZ` all divide `S` at the campaign geometries); the
coarse levels reach fewer ranks only because they hold fewer boxes, which is the
program's collapsing width and is identical in base.

**The spine pin.**  The guard also pins the whole control spine to rank 0
(`mgHomeEdtHint`): all ~993 solve level EDTs, the `4L−1` initialization phase
tasks, the reduction merges and finals, and `top_warm`/`top_loop`/`finalize`/
`print_timing_edt`/`shutdown`.  This looks like the creator-pinning R2 forbids
and is not, for a reason worth writing down: **the spine carries no parallelism
to confine.**  Every level EDT is a FINISH task chained on its predecessor's
output event, so at most one runs at a time in *base* too; base merely lets each
one land on a different rank while dragging the rank-0-homed level DB's RW turn
around the cluster once per phase.  Coverage of the *work* is unchanged — all
per-box fan-outs follow `boxHomePD` across every rank at imbalance 1.00 — and the
spine is ~1.4 % of the EDT population.  What the pin removes is a per-phase
ownership migration of the level datablock: locality, which is what R2 asks for.

The residual granularity amplification (~460× per remote face read) is structural
and out of a hint's reach; the second-order map that would spread the `L` level
DBs (home level `l` at rank `l mod P` and pin that level's spine task there) is
legal under R1 and was judged not worth the churn.

Measured (ferrari trend sweep, 15w+1p, `['5','4096']`, val_wb_nocomb / best-of-arm
range, **before** the distributed initialization): base 111 s → 348 / 330 / 350 s
at 2/4/8 n — the whole-box exchange saturates immediately and flattens; hinted
112 s → 160 / 175 / 184 s (range across the four arms ≤ 8 %, `val_wb`
consistently best).  Those numbers were dominated by the serial preamble that no
longer exists, so **the hinted-vs-base materiality gate (R3) has to be re-run**;
the only base-vs-hinted cell in `logs/exp` is 1 node, where hinted is 12 % slower
because at one rank every hint resolves to rank 0 and only the query cost
remains.  What the layer is expected to change is unaltered in kind: the
exchange's neighbour reads and the inter-level transfer edges become rank-local,
and the level datablock stops migrating per phase.

## Sizing

`log2_box_dim` sets the **grain** (`box_dim³` cells per per-box EDT: 4 096 at 4,
32 768 at 5, 262 144 at 6) and dominates memory.  `target_boxes` sets the
**width**: `N_0 = boxes_in_i³` with `boxes_in_i = ⌊target_boxes^{1/3}⌋`, a cap
rather than a count — 500 and 343 both give 343 boxes.  Neither changes the
number of F-cycles (compile-time `TIMED = 1`), and `L` grows only
logarithmically, so the level chain grows as `O(L²)` while work grows as
`N_0·box_dim³`.

**Reachable widths.**  A phase's instantaneous frontier is exactly `N_l`, and
phases never overlap, so `N_0 = s³` is the width.  For `s³` to be an integer
multiple of `3456 = 2⁷·3³`, `s` must be a multiple of **24**:

| `s` | `target_boxes` | `N_0` | vs 3456 | grid at grain 4 / 5 |
|---|---|---|---|---|
| 24 | 13824 | 13824 | **4×** | 384³ / 768³ |
| 48 | 110592 | 110592 | 32× | 768³ / — |

Powers of two are legal but never a multiple of 3456.  Any box grid is now
checked at argument time: an agglomeration step asking for more than 125 fine
boxes per coarse box, or a table that does not bottom out on one box, is refused
with a message naming the level pair and the ratio.  `s = 24` and `s = 48` reach
the "agglomerate everything" branch of the table and the 27:1 transfer paths,
which **no catalog size has ever run** — a new pin there needs an O(h²) sanity
check, not just a re-measurement.

**Memory** = `Σ_l N_l · boxdb(box_dim[l])` to better than 1 %, with
`boxdb(d) = 16 + 12·(d+2)·kStride·8` = 26.32 MiB at d=64, 3.598 at 32, 0.534 at
16, 0.0916 at 8.  The program prints this total at startup.

| args | grid | 1-node footprint |
|---|---|---|
| `['5','512']` | 256³ | 2.1 GiB |
| `['4','13824']` | 384³ | 8.6 GiB |
| `['5','13824']` | 768³ | 57 GiB |
| `['4','110592']` | 768³ | 70 GiB |

**Campaign size: `['4','13824']`.**  The row's historical value
`['5','512']` gave a width of 512 = 0.15× the 3456 workers of the largest
geometry — it met the bare worker count only through 4 nodes and failed the 2×
sufficiency from 4 nodes up, so part of the anti-scaling it showed at 8/16/32
nodes was starvation rather than structure.  The catalog's `['4','13824']`
fixes the width at 4× *and* restores the published grain (`log2_box_dim = 4`);
`['5','13824']` would have kept a heavier grain, needing a declared grain
deviation, at the same 768³ instance `hpgmg_dist` runs at the identical
arguments — the same-instance answer comparison, whichever size is chosen.
`expect`/`expect_args` are pinned at `['4','13824']`
(`||error|| = 0.000000006555138`).

Duration is the counter-pressure: this program's worst cells are its LARGE
geometries, so a wider size raises the 16/32-node cells toward the ceiling.  That
trade — the width rule against the anti-scaler anchor rule — is a user decision,
and whichever way it goes it belongs in the catalog comment as an explicit,
reasoned deviation.

## Family

The submodule holds one other HPGMG port, `hpgmg4`
(`apps/hpgmg4/refactored/ocr/intel/`): a *fourth-order* HPGMG with red-black
smoothing whose own README opens "Beginnings of an OCR implementation" — a
different discretization and an unfinished one, so it is neither registered nor
comparable.  This SDSC port is the family's only member; there is no scaling
sibling, which is why the structural anti-scaling above has no in-family control
and the catalog carries a restructured decomposition instead: `hpgmg_dist` (see
its own appdoc) rebuilds the port on a rank-persistent data plane — spatial box
homes with per-rank slice fan-outs, a face-slab exchange, per-rank box creation —
with the kernels, F-cycle, initialization bodies, verification and answer
unchanged, and is the pairing that supplies the fork-join-vs-persistent
comparison for this family.
