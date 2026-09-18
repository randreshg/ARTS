# hpgmg_dist

*The restructured version of `hpgmg`: the same F-cycle, kernels, initialization
bodies, verification and answer on a **rank-persistent decomposition** — spatial
box homes, per-rank slice fan-outs in place of the central per-phase spawner, a
face-slab ghost exchange, and per-rank box creation.*
Source: `third_party/ocr-apps/apps/hpgmg/refactored/ocr/sdsc/` — a separate
program (`hpgmg_dist_main.c` + `mg_dist.c` + `exchange_dist.c` + `mg_dist.h`)
that links the base port's kernel, per-box task and initialization sources
unchanged; the base files carry no dist conditionals at all.  Selected as
`hpgmg:restructured`.

## Overview

The base program anti-scales because its solve is a serial rope of ~1,200
phases and every phase is spawned centrally: one level EDT creates all `N`
per-box tasks — at 8 ranks, thousands of remote creations serialized through one
worker, once per phase, while the whole cluster waits.  The restructure attacks
that term and changes no numerics: every `||error||` digit must match the base
program at the same arguments.

(The base program's other historical term — a 94%-of-E2E serial initialization —
is gone from *both* rows: initialization is a conformance adaptation, not a tier
difference, and the per-box bodies that produce the data are now literally the
same functions in both programs.)

**Per-rank phase chains (no spine).**  The solve runs as one chain per rank over
the F-cycle's phase sequence, gated point-to-point through per-rank-homed
completion events: a phase touching only the rank's own boxes waits for that
rank's previous phase alone, the halo hand-offs (pack/unpack) wait for the
rank-grid neighbourhood — which bounds chain skew at every exchange and also
orders prolongation's writes into straddling fine boxes — and level transitions,
the bottom solve and the norm final wait for everyone.  Each phase is one pinned
FINISH *slice* per rank that creates that rank's per-box tasks locally (splitting
into local sub-slices when the box count is large, so creation itself
parallelizes within the rank) and counts their completions locally.  Nothing is
centrally spawned and no per-phase cost grows with the rank count.  Box tasks are
pinned with their spatially-homed boxes (`boxHomePD`), so a box datablock's RW
turns never leave its rank.

**Per-operator timing.**  The base program's spine opens a timer in each
operator's level task and closes it with an explicit closing task on **that same
level** once the operator's fan-out has finished.  This row carries the same
writes at the same frequency, without a barrier: each fan-out phase carries one
`dist_timer_edt` on the rank that homes the level (rank 0), gated exactly as that
phase is gated and gating that rank's slice.  It closes whatever operator was
still open on the phase's level and opens this phase's.  The cost is base's own —
a level-datablock write per phase, which every rank's slice then re-reads on its
next CONST acquire.

**Attribution differs, and the per-level timing lines are therefore not
comparable row-to-row.**  Because the close happens at the *next* phase that
touches the same level rather than immediately after the operator, an interval
that spans a level transition is accumulated here against a wider span than the
base spine measures.  Only the whole-run `Total Time` (`time_operators[4]`, from
the two whole-run stamps, which both rows take identically) is comparable — and
that is the only timing scalar the driver reads.

**Face-slab exchange.**  Each box owns six slabs of `box_dim²` doubles.  An
exchange phase packs the exchanged vector's interior boundary planes into the
box's own slabs (pack fan-out), then fills every ghost layer from the neighbours'
slabs (unpack fan-out, gated on the pack subphase's finish).  Edge and corner
ghosts of the 26-neighbour form read the boundary lines/points a full face slab
already contains.  The box datablocks never cross ranks; the only recurring
cross-rank payload is slabs.  At the campaign grain (`box_dim = 16`) a slab is
2 KiB against the 560 KB whole-box pull it replaces; at the trend grain
(`box_dim = 32`) it is 8 KiB against 3.77 MB.  An out-of-domain neighbour's
dependence slot carries the box's *own* slab — mask-guarded, never read — so the
base port's full-size constant box is gone entirely.

**Per-rank creation.**  Per-rank creator EDTs allocate and describe their own
boxes and slabs *on their own rank* (data is born where it lives; under the base
program every box's payload is first-touched on rank 0 and has to migrate out).
A merge assembles the guid tables into the level datablocks.  Everything that
*fills* a box — zeroing, the valid mask, the analytic evaluation, the coefficient
restriction, the Gershgorin `Dinv`/`L1inv` sweep with its eigenvalue reduction and
the `dot(α,α)` term, the ghost exchange of both diagonals — is the shared per-box
body, run as sliced phases with per-rank partial-max merges.  **Disclosure:** the
`dot(α,α)` term feeds `alpha_is_zero`, which has no reader (its only consumer is
the periodic-boundary mean shift, and the boundary condition is a Dirichlet
literal), so the pass is dead work in both rows; it is restored here because it
is the base program's own operator build and the two rows must do the same
work.  The finalization
error norm is reduced the same way (per box, per rank, then a printing final),
and it writes the same difference vector into `vec_temp` that the base program
writes.

## Parameters

Same dials as `hpgmg` (`log2_box_dim`, `target_boxes`), same loud geometry
validation (`hpgmg_check_geometry`: the 125-fine-box transfer limit, a table that
must bottom out on one box, a representable level/box datablock size and a
per-rank footprint within the 190 GB budget), same startup footprint line — which
now names the total, the rank count and the per-rank share.  At most 64 ranks
(a loud-failed compile-time bound of the slice tables).  Because the two rows now
share initialization, verification and per-operator bookkeeping, **at identical
arguments they must print an identical `||error||`** — running this binary at the
base row's arguments on one node is the cheapest same-instance check there is.
What the dials are set to, and why, is under **Sizing**.

## Structure deltas vs the base program

- **DBs**: +6 slabs per box (`6·Σ N_l`, `box_dim²·8` B each); −1 constant box per
  level; small per-reduction result DBs (destroyed by their merges).  The
  per-level `temp` pointer array and its DB are gone from both rows (nothing
  holds cross-rank native pointers any more).
- **EDTs**: each phase adds R slices plus one rank-0 timer task, and each
  exchange phase becomes pack + unpack subphases; each box's exchange work is one
  pack + one unpack instead of one pull task.  Creation becomes R creator tasks +
  a merge, replacing a single serial layout pass; the fill/operator-build phases
  are the same per-box bodies the base row runs, sliced per rank instead of
  fanned out centrally.
- **Events**: +2 lingering (FINISH + output) per subphase and ~R ONCE events per
  reduction — all bounded by the phase count (`TIMED = 1`).

## Wiring

`hpgmg_dist_main.c` is its own program shell: same arguments and same top-level
flow as the base program, with `init_all`/`do_solves`/`finalize` replaced by
`dist_init`/`dist_solves`/`finalize_dist`.  `dist_init` returns the event the
solve hangs on and takes a start event by reference; `mainEdt` satisfies it only
after `top_warm_dist` is wired, so the init chain cannot complete before its
consumer is registered.

The lattice is built in two rounds: per-rank creators make and home their own
phase events and a rendezvous exchanges the guid table, then per-rank chain
builders create every phase task with its gate dependences — registration may
trail a neighbour's satisfaction, which is legal because events fire and linger.
A phase's slice and all its children complete inside its finish scope before its
completion event fires; the schedule is the exact serial phase order, so the math
is order-identical to the base program.  Slab guids live in the level
datablock's reserved tail (`b_norms + N·8` — space the original allocation always
reserved), so `level_type` itself is unchanged.  Guids ride paramv as 64-bit
images (`memcpy`, matching the base port's own PRM-struct idiom); the helpers are
shared with the base program.

## Flow

A phase is one pinned FINISH slice per rank, preceded on rank 0 by the phase's
timer task.  It fires when its gate events have fired — the rank's own previous
phase for a phase that touches only that rank's boxes, the rank-grid
neighbourhood at a halo hand-off, everyone at a level transition, at the bottom
solve and at the norm — then creates its rank's per-box tasks locally and counts
their completions locally, and its completion event fires when the slice and all
its children have left the finish scope.  An exchange is two subphases: pack
fills a box's own six face slabs from the exchanged vector's interior boundary
planes, and unpack, gated on the pack subphase's finish, fills every ghost layer
from the neighbours' slabs.  Finalization reduces `||error||` and the final
residual norm per box, then per rank, then in a printing final that shuts down.
The schedule is the exact serial phase order, so the arithmetic is
order-identical to the base program.

## Placement (base)

Boxes have spatial homes (`boxHomePD`) and every box task is pinned with its box,
so a box datablock's RW turns never leave its rank.  Per-rank creator EDTs
allocate and describe their own boxes and slabs on their own rank, so a box's
payload is born where it lives; under the base program every box is first-touched
on rank 0 and has to migrate out.  Phase completion events are homed per rank as
well, which is what lets a phase wait point-to-point instead of on a central
spine.  The level datablocks and the per-phase timer tasks stay on rank 0, as in
the base program's hinted tier.

There is no separate `hinted` version.  The decomposition is the placement here —
it is structural, not a layer a hint could add or remove.

## Sizing

**Campaign size: `['4','13824']`.**  The row's historical value
`['6','4096']` — a 1024³ grid of 64³-cell boxes — gave a width of 4096 = 1.19×
the 3456 workers of the largest geometry, over the bare floor but under the 2×
sufficiency a spawn-and-join structure needs, and it cost **~123 GiB** at one
node (the level table keeps 4096 boxes for four levels; an earlier figure of
113 GiB undercounted).  The width knob is `target_boxes` and its reachable
values are the same as the base row's: `N_0 = s³` is an integer multiple of
3456 only when `s` is a multiple of 24.

| args | grid | 1-node footprint | vs 3456 |
|---|---|---|---|
| `['6','4096']` | 1024³ | ~123 GiB | 1.19× |
| `['4','13824']` | 384³ | ~8.6 GiB | 4× |
| `['5','13824']` | 768³ | ~58 GiB | 4× |
| `['5','32768']` | 1024³ | ~136 GiB | 9.5× |

The catalog runs `['4','13824']`, the same 384³ instance the base row runs at
`['4','13824']` and the same grain (`log2_box_dim = 4`) — the same-instance
answer comparison holds exactly, and `expect`/`expect_args` are pinned there
(`||error|| = 0.000000006555138`).  `['5','13824']` would keep a heavier grain
at the same width; `['5','32768']` keeps today's 1024³ grid and every level's
`h`, so the pinned `||error||` should carry over — but the operators are
per-box and the last digit was not verified statically, so one 1-node cell
must confirm it, together with the actual RSS.

Trend sweep (ferrari, 15w+1p, at the trend size `['5','4096']`, E2E / solve),
taken **before** the shared initialization, the restored `dot(α,α)`, the restored
per-operator timers and the restored `vec_temp` store:

| arm | 1 n | 2 n | 4 n | 8 n |
|-----|-----|-----|-----|-----|
| val_wb_nocomb | 22.3 / 6.1 | 21.3 / 12.1 | 17.3 / 11.7 | 17.5 / 13.8 |
| val_wb | 22.2 / 6.1 | 12.9 / 4.3 | 7.5 / 2.9 | **5.1 / 2.5** |
| inv_wb | 21.9 / 6.2 | 12.7 / 4.6 | 7.7 / 3.2 | **5.1 / 2.5** |
| excl_retain | 22.4 / 6.3 | 12.5 / 4.6 | 7.7 / 3.2 | 5.3 / 2.8 |

That was the application's first positive scaling: 4.3× from 1 n to 8 n on the
VAL/INV arms, against a hinted base that *degraded* over the same geometries.
Bare `val_wb_nocomb` is the outlier (12.1 s solve at 2 n): without request
combining the slice fan-outs' per-task RO acquires of the level datablock and
slabs each pay a validation round, so combining is this decomposition's natural
partner; INV needs no ledger at all (a slab read is a covering load) and matches
it.  There is no spine: the phases run as one chain per rank, gated
point-to-point, so no per-phase cost grows with the rank count — the term that
would have dominated the 32-node campaign is structurally absent.

**The table above has to be re-taken.**  Two of the changes move it in opposite
directions: the base row's serial preamble is gone, so the comparison it was
drawn against has shrunk; and the restored per-operator timing puts a
level-datablock writer back into every phase, which under a validation protocol
turns a stable, revalidated hub read into a per-phase re-fetch for every rank.
That cost is the base program's own and is why it was restored, but its size
should be measured before the campaign is sized.
