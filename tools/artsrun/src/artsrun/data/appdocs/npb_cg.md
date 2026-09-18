# npb_cg

*NAS Parallel Benchmarks CG, SDSC OCR port: an inverse power method whose
inner conjugate-gradient solve is a chain of sparse matvecs, each one a
fork-join over row blocks of the matrix.*
Source: `third_party/ocr-apps/apps/npb-cg/sdsc-ocr/` (the DAG is `cg_ocr.c` +
`cg_graph.c` + `cg_edt.c`, the driver loop `main_edt.c`, the matrix generator
`makea_ocr.c` + `cg_gen.h` — shared with the restructured twin).

## Overview

NPB CG estimates the smallest eigenvalue of a random sparse symmetric
positive-definite matrix `A` by the inverse power method: `niter` outer
iterations, each solving `A·z ≈ x` with a **fixed 25-step** conjugate gradient
(no convergence test), then `zeta = shift + 1/(x·z)` and `x = z/‖z‖`. The
result scalar is that `zeta`, printed by `tail_edt` only after it is checked
against the class's reference value to a relative error of `1e-8`, so
`Verification SUCCESSFUL (zeta=…)` is a strong oracle — it observes the matrix
build, every matvec, every dot product and the whole outer loop. The program
stresses all three axes at once: real sparse arithmetic (~2·nnz flops per
matvec), heavy task churn (a fork-join of `na/blk` tasks per matvec, 26
matvecs per outer iteration), and a write-once/read-by-everyone vector that
changes writer every CG step. The restructured twin `npb_cg_dist` is a
separate program and a separate catalog row; see `npb_cg_dist.md`.

Matrix construction is **untimed by NPB's own specification**, and it is
parallel here: one builder task per row block, from an inverse index shared
with the restructured program, producing a matrix bit-identical to the
published serial sweep. What the compute phase does with that matrix is the
published decomposition, unchanged and on display.

## Parameters

Arguments are flag/value pairs walked forward from `argv[1]`. Every flag must
be recognised and must have a value: an unknown flag, a dangling flag or an
unknown class prints the usage line and shuts the runtime down rather than
being skipped.

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `-t <c>` | problem class `T,S,W,A,B,C,D,E`; sets `na`, `nonzer`, `shift`, `niter` and the reference `zeta` from the table in `class_init` (`util_ocr.c`) | `S` (`na=1400`) | ✓ parsed in `mainEdt`, stored in the class DB that every consumer takes `CONST` — multinode-safe |
| `-b <blk>` | rows per row block; the matrix is split into `nb = na/blk` datablocks and each matvec forks `nb` tasks | `1` | ✓ same path (`class->blk`), forwarded to `spmv_edt` in paramv — multinode-safe. **Must divide `na` exactly**, and be `>= 1`; both are checked in `mainEdt`, which prints the offending value and shuts down |
| `-i <n>` | overrides the class's `niter` (outer iterations only; the inner CG count is not affected) | class value | ✓ written into the class DB before its release — multinode-safe. `0` means "no override" |

Only the first character of `-t`'s value is read.

Notable compile-time knobs, none argv-reachable: the **inner CG iteration
count is hardcoded** as `for(it=1; it<24; ++it)` plus an unrolled leading and
trailing iteration (`cg_ocr.c`) — 25 CG steps and a 26th matvec for the
residual; the verification tolerance `1e-8` in `tail_edt` (`1e-3` under
`TG_ARCH`); the timing-detail flag `class->on`, pinned to `1` in `class_init`;
and `rcond = 0.1` in the row assembler, an NPB constant. The construction's
scanner count is derived, not argv: it is `clamp(np/(2·nb), 1, nb)` in
general and pinned to `1` in this program (see Placement).

## Structure

Write `n = na`, `b = blk`, `nb = n/b`, `k = nonzer+1`, `C = niter+1`
conj_grad invocations (one untimed warm-up, `niter` timed), and
`e = 2·⌈(b+1)/2⌉` (the `even` header pad). Each `conj_grad` issues 26 matvecs.

| object | count | size |
|--------|-------|------|
| matrix row blocks | `nb` | `12·Σ_{rows in block} nz + 4·e` B; `Σ` over all blocks `≈ 12·n·k² + 4·e·nb` |
| matrix GUID container | 1 | `8·nb` B |
| construction arrays (transient) | 7 + a 16 B RNG block + one cursor block | `4n + 8(n+1) + 4·np + 8·np + 8n + 8·np + 8(nb+1)` B, `np = Σ arow[i] ≤ n·k` |
| construction per-block scratch (transient) | `2·nb` | a workspace and a slot-count/cursor block per builder, each a few kB |
| construction per-block reference (transient) | `nb` | 16 B each (the block's GUID and its nonzero count, returned to the join) |
| full-length vectors | `x` once per run; `r,p,rho,z` 4, `q` 26 and `pp` 24 per `conj_grad` | `8n` B each |
| row-block results | `26·nb` per `conj_grad` | `8b` B each |
| scalars `alpha, nalpha, dist` | 50 per `conj_grad` | 8 B each |
| **EDTs** | `4 + 2·niter + nb + C·(128 + 26·nb)` | — |
| **DBs** | `13 + 4·nb + C·(104 + 26·nb)` | — |
| **events** | `nb + C·(150 + 26·nb)` | — |

The driver constant is `head_edt`, `makea_join_edt`, `tail_edt` and the
dedicated `shutdown_edt`, plus `loop_top`/`loop_bottom` per timed iteration;
the `nb` term is the construction fan-out (one builder EDT, its output event,
and its block, reference and scratch datablocks).

Per inner CG iteration the loop body is `5 + nb` EDTs (1 `spmv_edt`, `nb`
`rowvec_edt`, 1 `assign_edt`, 1 `alphas_edt`, 1 `daxpy_edt`, 1 `update_edt`),
`4 + nb` DBs and `6 + nb` events; the unrolled first step adds a `square_edt`
and a `scale_edt`, the trailing block adds a second matvec plus `alpha_edt`
and `dist_edt`. No EDT is a finish EDT, so every event is either an explicit
`OCR_EVENT_ONCE_T` (74 per `conj_grad`) or an `ocrEdtCreate` output event
(`76 + 26·nb` per `conj_grad`). EDT templates are created and destroyed around
every single create, but the shim encodes a template into its GUID, so that is
pure app-side churn with no runtime object behind it.

Worked, at the calibrated `-t A -b 4` (`n=14000, k=12, niter=15, nb=3500,
C=16, e=6`): **≈ 1.46 M EDTs, ≈ 1.47 M DBs, ≈ 1.46 M events**. The matrix is
`≈ 12·14000·144 + 4·6·3500 ≈ 24.3 MB` over 3500 blocks, ~6.9 kB each; every
vector is 112 kB; the construction arrays are ~3.6 MB and are all destroyed at
the join; peak live set ≈ 32 MB. At `-t T -b 25` (`nb=2, C=4, e=26`): 732
EDTs, 645 DBs, 810 events, a 38 kB matrix.

Counter cross-check: **calibration pending.** The previous cross-check was
taken against the serial-construction formulas (`2 + 2·niter + …`,
`10 + nb + …`, no `nb` event term) and does not apply to the fan-out above.
What that check did establish and what still holds is the runtime's bootstrap
correction: NUM_EDT_CREATE needs **+2** over the app formula, not +1 — the OCR
shim's bootstrap creates `main_edt` (`libs/src/core/system/runtime.c`) and,
inside it, a `mainEdtTrampoline` that carries the app's `mainEdt` in as a DB
dependence; DB creates need +1 and event creates +0. The same correction
reconciles hpgmg's EDT counts exactly.

## Wiring

`mainEdt` parses the arguments, builds the class, timer and `x` blocks, draws
the matrix entries and their inverse index, then creates one
`cg_block_build_edt` per row block and a `makea_join_edt` with one dependence
per block. Each builder assembles its own block into a datablock it creates
itself and returns a reference to it. `makea_join_edt` collects those
references into the GUID container, prints `number of nonzeros`, disposes of
the construction arrays, and only then wires `head_edt` (6 slots) and calls
`conj_grad` to fill its last two. `head_edt` discards that warm-up result,
re-initialises `x` and starts the timed loop: `loop_top_edt(it)` → `conj_grad`
→ `loop_bottom_edt(it)` → either the next `loop_top_edt` or `tail_edt`; a
dedicated shutdown EDT gated on `tail_edt`'s output event calls `ocrShutdown`
only after the tail's dependence releases have completed, so no release work is
truncated out of the measured run. The driver EDTs otherwise pass `NULL` for
their output events; the only inter-EDT edges there are direct DB dependences.

Inside `conj_grad` every edge is either an EDT output event or a `ONCE` event
satisfied by hand:

- `spmv_edt` takes the GUID container `CONST` and the input vector `CONST`,
  then creates one `assign_edt` with `nb` slots and `nb` `rowvec_edt`s, wiring
  each `rowvec`'s output event into one `assign` slot and handing it its own
  row-block DB (`CONST`) plus the **same** input vector (`CONST`).
- `assign_edt` concatenates the `nb` fragments of `8b` bytes into one `8n`
  block, destroys them, releases and satisfies the matvec's `q` event.
- `alphas_edt` returns `alpha` through its output event and satisfies a
  separate `ONCE` event with `-alpha`; `update_edt` returns the new `p` and
  satisfies a `ONCE` event with a fresh copy `pp`.
- `update_edt` takes `p` and `r` **twice each**, in two `RW` slots — the
  aliasing is deliberate (`r += -α·q` and `p = β·p + r` are computed in
  place), so one EDT holds the same GUID on two dependences.

DB concurrency: the matvec input vector is the fan-out point — up to **`nb`
simultaneous `CONST` readers** (3500 at the calibrated args), refreshed by a
single `RW` writer one CG step earlier. During construction the seven index
arrays have the same shape: `nb` builders read all of them `RO` at once. Six
long-lived blocks (`x`, `r`, `p`, `z`, `rho`, and the timer) take `RW` from a
different, round-robin-placed task every CG step, so their write right migrates
~25 times per `conj_grad`; the row-block DBs are `CONST`-only after
construction and never written again. The one genuine overlap is `scale_edt`
reading `x` `CONST` while the first `update_edt` of the same `conj_grad` holds
`x` `RW` — both are enabled by the same `alphas_edt`, and `update` never
actually writes `x` (see notes).

## Flow

The DAG is a necklace. Per `conj_grad`: 26 beads, each a width-`nb` fork
followed by a width-1 join, separated by two to four single-EDT links doing
full-length `O(n)` vector work (`alphas`' dot product, `update`'s two axpys +
dot + copy, `daxpy`'s axpy). Outer iterations never overlap — `loop_bottom`
creates the next `loop_top` — so:

- **max concurrent EDTs = `nb = na/blk`**, and only during a matvec;
- **`26·C` fork-joins per run** (416 at the calibrated args), each ending in a
  single `assign_edt` that gathers `nb` fragments and copies `n` doubles;
- **no reduction tree anywhere**: every dot product is one EDT looping over
  all `n` elements.

That serial spine is the scaling limit: per CG step the parallel matvec is
`≈ 2·n·k²` flops while the serial vector chain plus the gather is `≈ 12·n`
flop-equivalents — a serial fraction of roughly `6/k²` (~4% at class A), an
Amdahl ceiling near 25× however many workers are added.

Ahead of the necklace, and inside the `[E2E]` window, sit the construction and
one untimed warm-up `conj_grad` whose result `head_edt` discards (so `C =
niter+1` solves are inside the window for `niter` timed — published NPB
structure, mirrored by the restructured version). The construction's own shape
is now a width-`nb` fork-join too: what remains sequential in it is the draw
stream (`sprnvc` rejects on both range and duplicate, so a row's draw count is
data-dependent and no jump-ahead into the generator exists), the running scale
factor, and — in this program only — the single-scanner index pass, together
`O(n·k)` against the construction's `O(n·k²)`. Verification is a constant
comparison against the class table's reference `zeta`, not a re-solve.

## Placement (base)

This application carries no `OCR_APP_OPTIMIZED_PLACEMENT` layer and has no
`_hinted` target: every `ocrEdtCreate` and `ocrDbCreate` in the tree passes
`NULL_HINT`. Effective policy is therefore **EDT → runtime round-robin**, **DB
→ home = creating rank**.

Class, timer, `x`, the GUID container and the construction arrays are homed on
the rank `mainEdt` runs on; the `nb` **row-block DBs are homed wherever their
builder landed**, i.e. spread by round-robin, because R9 has the data created
by the task that owns it; everything created inside a solve EDT (`q`, `z`,
`rho`, `pp`, the scalars, every result fragment) is homed wherever that EDT
landed. At multinode:

- A `rowvec_edt` for row block `e` lands on an arbitrary rank and the rank
  rotates between matvecs, so an **immutable block is re-fetched by a different
  reader almost every matvec** — 416 reads of the whole 24 MB matrix, cheap
  only on arms that keep a durable reader copy. Since the blocks are now spread
  over the ranks rather than all homed on rank 0, those fetches are served by
  every rank instead of one.
- The matvec input vector is written on one rank and `CONST`-read by `nb`
  tasks on every rank: a 112 kB broadcast per matvec from a fresh source.
- The `nb` result fragments are created on scattered ranks and gathered by one
  `assign_edt` on a single rank — `nb` small remote acquires per matvec, ~2.9 M
  over the run, all on the critical path.

The single scanner of the construction's index pass is a consequence of this
surface, not an oversight: scanners write disjoint ranges of one index
datablock `RW`, which is a disjoint write only while they share a policy
domain, and this program cannot confine a fan-out to one domain because it
declares no affinity. The restructured twin, whose tasks are pinned, divides
that pass over its own workers. It costs nothing here — 168 k pairs at class A
is a few milliseconds against a 24 s cell.

The algorithm has obvious locality (a row block and its result belong
together; the matrix never changes) and the compute phase of the base program
expresses none.

## Placement (no hinted tier — measured, not assumed)

A hint layer was written for this application, five maps of it were measured
at two problem sizes, and none beats the base program at any geometry -- so
the base's own (no-preference) map is the best hinted map, the base stands in
for the hinted comparison, and the layer was then **removed**.  Solve time at 8 ferrari nodes, class A (base
17.9/18.0/18.0 s over three runs):

| layer | solve |
|-------|-------|
| band-homed blocks only | 20.3 |
| band-pinned readers only | 20.3 |
| both | 19.3 |
| spine pinned to rank 0 only | 25.3 |
| all three | 25.0 |

and at class A's 4× larger sibling (class B, 180 MB matrix, `-i 5`,
end-to-end): 2 nodes 90.2 base vs 93.3 hinted, 8 nodes **108.9 base vs
147.7 hinted**. End-to-end at class A `-b 25`, the hinted binary read
1.30 / 15.39 / 27.59 s at 1/2/8 ferrari nodes against the base binary's
1.30 / 14.71 / 19.86 s — no form of the layer beat the base at any rank
count above one.

The reason is in what the data does, not in how the hints were written.
The only placement-sensitive object is the matrix, and it is **read-only
after construction** — under a validating protocol every rank ends up holding
its own snapshot after the first read, so pinning a block's reader to a
fixed rank buys a locality the base program already has, while paying to
move the block's home and to resolve an affinity per spawn.  Everything
that actually moves — the operand vector broadcast to every reader each
matvec, and the `nb` result fragments gathered into one EDT — is
all-to-all or all-to-one, which no home assignment improves.  Worse,
pinning the serial spine makes one rank the permanent server for that
broadcast and the permanent sink for that gather; the base program's
round-robin rotates the role and spreads it, which is why the spine pin
alone costs 40%.

One map the removed layer never tried is hinting a result fragment toward the
gather's rank, turning `nb` remote acquires into `nb` pushes already in flight.
It is not a hint-only change: `rowvec_edt` does not know the gather's rank, so
the layer would have to widen its paramv and template, which is a wiring
change. It also reshapes only the fragment traffic and removes neither the
whole-vector broadcast nor `assign_edt`'s serial `n`-double copy.

So the answer to this program's placement problem is not a hint but the
decomposition: see `npb_cg_dist`, where the vector never travels whole
and the matrix band is owned, not fetched.

## Sizing

`-b` is the only dial that changes parallelism without changing the answer:
`nb = na/blk` is both the task count per matvec and the fan-out — and now also
the construction's fan-out. `-t` sets the problem (`na` and `nonzer` move work
as `na·(nonzer+1)²`, and the class also fixes `niter`), `-i` moves duration
only.

- **Width**: `W = na/blk`, node-invariant, spawn-and-join, one fork-join live
  at a time. The catalog's `-t A -b 4` gives **3500 = 1.01×** the 3456 workers
  of 32 Dane nodes × 108 — barely above the bare-floor multiple, not the 2×
  sufficiency bar a spawn-and-join structure needs (`-b 2` would give
  **7000 = 2.03×** and clear it). An exact integer multiple is unreachable at
  every class: `na/blk = m·3456` requires `3456 = 2^7·3^3` to divide `na`, and
  none of `{50, 1400, 7000, 14000, 75000, 150000, 1500000, 9000000}` is
  divisible by 27 (`14000 = 2^4·5^3·7`, `1500000 = 2^5·3·5^6`,
  `9000000 = 2^6·3^2·5^6`). The missing multiple is structurally forced, not
  an oversight; `-b 1` reaches 4.05× with a better tail, at 14000 blocks.
- **Grain**: `b·k²` multiply-adds with an indirect gather — `4·144 = 576` at the
  calibrated args, well under a microsecond, so per-task runtime overhead is a
  real fraction of the cost. Raising `b` buys grain and costs width, both
  linearly; the serial spine means width past ~25× the serial cost buys nothing.
- **Constraints**: `blk` must divide `na` exactly, and `nb` must stay within
  the runtime's dependence-count limit (65534 under the shim) — both
  `assign_edt`'s `depc` and the construction join's are `nb`, so classes B and
  above cannot run at the default `-b 1`. All are rejected with a message
  rather than run: the blocking in `mainEdt`, the fan-out at the failing create
  in `spmv_edt`, the construction join at its own create.
- **Memory** is `≈ 12·na·(nonzer+1)²` bytes for the matrix and small beside it
  for everything else: 24 MB at class A, ~176 MB at B, ~461 MB at C (class D
  would be 8.7 GB by the same formula). The construction adds a transient
  `≈ 20·na·(nonzer+1) + 12·na` bytes — 3.6 MB at class A — released at the
  join. Peak app footprint ≈ 32 MB at the calibrated args, far inside the
  190 GB one-node budget.
- **This is an anti-scaler.** Measured on the **base** binary, val_wb, class A
  `-b 25` on 15w+1p ferrari ranks: 1.30 s at one node, 14.71 s at two,
  19.86 s at eight. At `-b 2` (the row's previous calibration, not the
  catalog's current `-b 4`) on the same geometry: 194.8 s at two nodes and
  253.0 s at eight, against the 108-worker anchor node's own time. Every added
  node costs time, because the operand broadcast, the
  single-EDT gather and the serial vector spine all grow with the rank count
  while the per-task grain shrinks. One class up the wall is absolute: class B
  `-b 10` finishes in 116 s at one node and **times out past 590 s at two**. So
  the class stays at A, and the 32-node cell, not the 1-node one, is what the
  budget has to hold.

  **All of the multi-node numbers in this section predate the parallel
  construction and the spreading of the matrix homes that came with it.** The
  one-node anchor at these arguments (`-t A -b 4`) is measured at 11.5 s,
  taken after both; the node-count curve above does not carry over past one
  node, and is what a fresh campaign sweep takes. The anti-scaling mechanism
  is structural and unchanged, but its magnitude beyond one node may move.

  Every number above was taken on a tree built with every counter OFF. That
  is not pedantry: the tree these were first measured on still carried an
  earlier campaign's counter set, and it moved this app's anchor by 6%.
- `expect_args` equals `args` and the pin is the class's own reference zeta
  (17.1721077015265), which the anchor reproduces exactly. The construction is
  bit-identical to the published serial sweep, so the pin is unaffected by it.
