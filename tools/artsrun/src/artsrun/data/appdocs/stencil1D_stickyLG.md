# stencil1D_stickyLG

*1-D three-point stencil, cloning style, halo exchange over labeled
per-timestep STICKY events, with parallelized initialization.*
Source: `third_party/ocr-apps/apps/Stencil1D/refactored/ocr/intel-david/stencil1DstickyLG.c`
(~595 lines; author Bryan Pawlowski, Intel 2015).

## Overview

Same 1-D three-point elliptic relaxation as `stencil1D_sticky`
(`anew(i) = 0.5*a(i) + 0.25*(a(i-1)+a(i+1))`, boundaries pinned to 1,
converges toward all-1s) and the same `N`-chain/`M`-point/`T`-timestep
decomposition, but a from-scratch reimplementation (different author, README
calls it "sticky with labeled GUIDs") with two structural differences from
`stencil1D_sticky`: **initialization is parallel** (`N` independent `init`
EDTs instead of one serial `mainEdt` loop creating all DBs up front), and
the halo handoff uses **labeled GUIDs**. Every rank computes, via
`ocrGuidFromIndex` over four shared GUID ranges, the label of an exchange
from its timestep, direction and boundary — so a sender and its receiver
agree on an event's identity without ever exchanging it. Every timestep
exchanges over labels of its own: the alternating `toggle` (0 or 1) picks
the pair of ranges and each pair of timesteps takes the next block of `N-1`
labels in them, so no label is ever reused. Each label is created once, by
the receiving chain's `stencilEdt` one timestep before the send (by
`realmainEdt` for the first timestep's sends), satisfied once by the sender,
and destroyed by the receiving `stencilEdt` when it runs — mechanically the
same create/destroy churn as `stencil1D_sticky`. Termination differs
from the lineage-A trio too: a `wrapupEdt` with `N` direct DB dependencies
(one per chain's final data block) does the printing, rather than the last
chain shutting down inline — the README notes this `N`-not-`N+1` dependency
count is what distinguishes every parallel-init variant from the ones with
a `realMain`-as-finish-EDT join.

The timestep-0 initialisation sets `a[m-1] = 0.0` on every node but the last,
as the lineage-A variants do; the published file left that point
uninitialised there, and the catalog's scalar (`9 49`, node 9's last point)
depends on it.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `numNodes`/`N` | number of independent chains | 10 | ✓ parsed in `mainEdt`, only when exactly 3 args given |
| `argv[2]` = `chunkSize`/`M` | points per chain | 50 | ✓ same |
| `argv[3]` = `timesteps`/`T` | timesteps | 10000 | ✓ same |

Unlike the lineage-A trio, this file `#include`s `stencil1D.h` and its
defaults **are** that header's `N`/`M`/`T` `#define`s (10/50/10000), used
when `argc != 4`. No other runtime knobs; no `nrank==0` style guard exists
in this file (unvalidated zero/negative inputs are not rejected).

## Structure

Let `N=numNodes`, `M=chunkSize`, `T=timesteps`.

| object | count | size |
|--------|-------|------|
| EDTs total | `N·T + 3N + 3` — `mainEdt`, `realmainEdt`, `N` `init` EDTs, `N` `stencilInitEdt` EDTs, `N·(T+1)` `stencilEdt` chain generations, 1 `wrapupEdt` | — |
| EDT templates | `2N+2` — `initTemplate`/`wrapupTemplate` created once centrally, but `stencilInitTemplate` (in `initEdt`) and `stencilTML` (in `stencilInitEdt`) are each (re-)created once *per chain* (`N` times) for what is the same function every time, rather than created once and shared | — |
| DBs | `4N+1` — 1 shared block + `N` private blocks + `N` data blocks (double-buffered, `2M` doubles) + `2N` halo buffer blocks | shared ≈ 64 B (`ocrGuid_t wrapup` + 3×`u64` + `ranges[4]`); private ≈ 160 B (the toggled event-GUID slots `left/right/leftrcv/rightrcv[2]` = 8×`ocrGuid_t`, a copy of `ranges[4]`, plus scalars); data = `2M×8` B; buffer = 8 B (`sizeof(double)`) |
| Events | `2(N-1)·T` labeled STICKY event creates — one per direction per internal boundary per timestep, each a label of its own: `2(N-1)` by `realmainEdt` for the first timestep, the rest one timestep ahead by the receiving `stencilEdt`; the four ranges hold `(N-1)·⌈T/2⌉` labels each | — |

`stencilInitEdt` (`stencil1DstickyLG.c:286-295`) creates `sendleftDb` and
`sendrightDb` **unconditionally** on every one of its `N` invocations —
unlike the paired `ocrGuidFromIndex` calls just below, which *are*
boundary-guarded (`private->left[...]`/`private->rightrcv[...]` set to
`NULL_GUID` at rank 0/`N-1`). So rank 0's `sendleftDb` and rank `N-1`'s
`sendrightDb` are created and handed to the chain head, which never sends
them on (their matching event GUID stays `NULL_GUID`) — 2 orphaned DBs beyond the `2(N-1)` a
naive "one buffer pair per internal boundary" count would predict; hence
`2N`, not `2(N-1)`, halo buffers, and `4N+1` DBs total, not `4N-1`.

Worked numbers at the calibrated args (`48 50 340000`): EDTs ≈
`48×340000 + 3×48 + 3 = 16,320,147`; DBs = `4×48+1 = 193`, ≈47 KB payload;
Events = `2×47×340000 = 31,960,000` — the same churn as `stencil1D_sticky`'s,
one event per direction per boundary per iteration. A label is created one
timestep ahead and destroyed by the task it feeds, so the live event count
stays `O(N)`.

Counter cross-check: verified (1 node, `4 4 5` vs `6 4 8`): NUM_EDT_CREATE
36 → 70, NUM_DB_CREATE 18 → 26, NUM_EVENT_CREATE 60 → 160 — exactly
`N·T+3N+3` / `4N+1` / `4(N-1)T` (app values 35/69, 17/25, 60/160) plus the
runtime's constant +1 EDT/+1 DB/+0 EVT baseline. The original `4N-1` DB
formula was short by exactly 2 (predicted 16/24 instead of measured
18/26) — the two orphaned boundary buffer DBs above account for it. The
events were measured while both ends created each label every timestep
(`4(N-1)T`); with one create per label `2(N-1)T` predicts 30 → 80, not
re-measured.

## Wiring

- `realmainEdt` reserves 4 label ranges of `(N-1)·⌈T/2⌉` each
  (`ranges[0..3]`), creates the first timestep's `2(N-1)` labels, then the
  shared `wrapupEdt` (depc = `N`) and `N` `init` EDTs, each RW-dependent on
  the shared block.
- `init` creates a private DB, then launches `stencilInitEdt` with 2 slots:
  shared (RW) and private (RW).
- `stencilInitEdt` creates the data block and the two halo buffers, copies
  the four ranges into the private block, fills the 8 toggled event-GUID
  slots (`left[0..1]`, `right[0..1]`, `leftrcv[0..1]`, `rightrcv[0..1]`)
  from the first label block — every `stencilEdt` overwrites the interior
  ones with its own timestep's labels before using them, so only the
  `NULL_GUID`s of the absent neighbours at rank 0 and rank `N-1` are read as
  set — and creates the first chain-head `stencilEdt` wired to the data
  block, the private block and the two buffers.
- Each `stencilEdt` generation after the chain head first destroys the
  receive labels it was wired to. A generation before the last then creates
  its immediate successor (1-ahead, not the 2-ahead grandchild trick of
  `guid`/`once`), wires it to this timestep's receive labels, creates the
  next timestep's receive labels (the ones that successor will name), and
  satisfies this timestep's send labels — its neighbours' receive labels —
  with the outgoing halo buffers (one label of each kind per neighbour).
  Every label exists before the sender that satisfies it is enabled, so no
  grandchild-ahead pipelining is needed despite the 1-ahead cloning.
- At the final timestep, the chain instead wires its data DB (RW) directly
  onto `wrapupEdt`'s slot `mynode` — the only join mechanism in this
  variant, no output event involved.
- Same DB-concurrency shape as lineage A: no DB has more than one
  simultaneous accessor.

## Flow

Parallel initialization removes the `O(N)` *serial* rank-0 preamble that
lineage-A pays (DB creation is now spread across `N` independently
schedulable `init`/`stencilInitEdt` pairs), at the cost of `2N` extra EDT
stages before steady state begins. Steady state is the same `N`-wide,
`T`-deep parallel chain structure as lineage A. Finalization is a true
`N`-way *join* (via `wrapupEdt`'s `N` dependences) rather than a serialized
wave — all `N` chains can reach their last timestep concurrently and
`wrapupEdt` fires once all have. Critical-path depth ≈ `T` (the
initialization and join stages add `O(1)` depth each, not `O(N)`).

## Placement (base)

`NULL_HINT` on every create — no affinity code in this file. Effective
policy differs from lineage A in one structural respect: **DB homes are no
longer concentrated at rank 0**. `init`/`stencilInitEdt` (which create the
private/data/buffer DBs) are themselves round-robin-placed EDTs, so each
chain's DBs home wherever its `init` EDT happened to land — potentially any
rank, not always rank 0. This does not, however, produce locality: every
subsequent `stencilEdt` clone is *independently* round-robin-placed too, so
a chain's DBs stay fixed at whatever rank its `init` drew while the chain's
executing EDT keeps moving — the same worst-case "DB home and executing EDT
are on unrelated ranks" pattern as lineage A, just with the DB homes
scattered across all ranks instead of pinned to rank 0.

## Sizing

`N`, `M`, `T` play the same roles as in lineage A: `N` caps parallel width,
`T` drives total churn/wall-time (the same event churn as
`stencil1D_sticky`/`stencil1D_once` for the same `N,T`), `M` only moves
data-DB payload. Use `N` ≳
target worker count and size `T` for desired wall time at that width. The
calibrated `48 50 340000` differs from this file's own built-in default
(`10 50 10000`) by both `N` and `T` — a deliberate strong-scaling
calibration shared identically across all seven Stencil1D rows, so a
cross-idiom comparison at fixed `(N,M,T)` isolates the effect of the
event/GUID idiom rather than the workload size.
