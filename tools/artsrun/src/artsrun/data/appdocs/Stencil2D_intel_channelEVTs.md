# Stencil2D_intel_channelEVTs

*The same 2-D star-stencil SPMD kernel as `Stencil2D_intel_chandra`, ported
onto persistent CHANNEL events for halo exchange instead of a per-round
sticky-event handshake — this variant exists specifically to exercise that
event idiom.*
Source: `third_party/ocr-apps/apps/Stencil2D/refactored/ocr/intel-channelEVTs/stencil_2d.c`
(~1540 lines, built with `EXTRA_DEFINES STENCIL_WITH_DBUF_CHRECV
CHANNEL_EVENTS_AT_RECEIVER`) + `timers.c`.

## Overview

Same physical kernel as `Stencil2D_intel_chandra` (radius-2, 9-point
discrete-divergence stencil over an `NP×NP` domain tiled `NR_X×NR_Y`,
`NT+1` rounds, final-round `ADD`/`MAX` reduction trees, rank-0 correctness
check against the analytic `(NT+1)·2`), but restructured around OCR's
`OCR_EVENT_CHANNEL_T`: each of a tile's 4 neighbor directions gets **one
persistent channel event established once at setup**, not a pair of sticky
events recreated every round. `CHANNEL_EVENTS_AT_RECEIVER` picks which side
of a direction creates that channel (here: the receiver creates it and
publishes its GUID to the sender over a one-time labeled-sticky-event
handshake — `#else` would have the sender create+publish instead);
`STENCIL_WITH_DBUF_CHRECV` doubles the channel count per direction (4→8) so
consecutive rounds' sends don't serialize behind the previous round's still
-draining channel — a double-buffering of the *channel event itself*,
independent of the halo-buffer double-buffering (`LsendBufs[2]` etc., which
this port also keeps). The catalog's marker line is `Computed L1 norm =
…`, printed by rank 0's `FNC_summary` alongside `Solution validates` on a
match. Same stress profile as the chandra variant (per-tile task/event
churn + halo DB traffic, independent of per-tile compute grain), but with
the per-round event-object churn moved out of the steady state and into a
one-time setup cost.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `NP` | side of the square domain — a *single* dial: `NP_X = NP_Y = NP`, there is no aspect-ratio argument, so the per-tile aspect ratio comes only from the `NR` factorisation | 1000 | ✓ parsed in `init_settings` (called from `mainEdt`), propagated via `globalParamH_t` (`DB_MODE_RO` downstream) — multinode-safe |
| `argv[2]` = `NR` | tile count = the **width** knob, factored `NR_X×NR_Y` via `splitDimension_Cart2D` | 16 | ✓ same DB propagation |
| `argv[3]` = `NT` | timesteps = the **height** knob (`NT+1` rounds run, round 0 untimed); stays an argument, the campaign sets it | 10 | ✓ same DB propagation |
| all three, or none | any other argument count is rejected: `rejectSettings` prints a usage line and calls `ocrAbort` | — | ✓ all-or-nothing, loudly — a partial list is never silently defaulted |
| argument ranges | `NP > 2·HALO_RADIUS`, `NR ≥ 1`, `NT ≥ 1`, `NR ≤ NP`, and every tile at least `HALO_RADIUS` wide on both axes (`NP/NR_X`, `NP/NR_Y`) | — | ✓ same rejection path; `-1` in a slot still restores that slot's default before the checks |
| `HALO_RADIUS` | stencil radius | 2 | ✗ compile-time `#define` (the weight table and the `IN`/`OUT` macros are written for it) |
| `CHANNEL_EVENTS_AT_RECEIVER` | which side of a halo direction owns the channel event | on (this target) | ✗ build-time target selection, not a runtime knob |
| `STENCIL_WITH_DBUF_CHRECV` | double-buffer the channel events themselves (4→8 per tile) | on (this target) | ✗ build-time target selection |
| `USE_STATIC_SCHEDULER` | two-level PD-then-tile SPMD fork instead of the direct per-tile fork | off | ✗ not defined for this build — `forkSpmdEdts_Cart2D` (direct fork, below) is what actually runs; it needs OCR's STATIC scheduler, which ARTS does not have |
| `USE_EAGER_DB_HINT` | per-DB eager push (`OCR_HINT_DB_EAGER`) | off | ✗ not defined for this build; the shim ignores the hint by design, so it would be a no-op here. Note the upstream *campaign* scripts (`scripts/job.properties`) turned this and `USE_STATIC_SCHEDULER` on while the upstream *Makefile* default leaves both off — this build follows the Makefile |
| `FULL_APP` | 1 = the real arithmetic, 0 = the OCR skeleton alone | 1 | ✗ compile-time `#define` in `stencil.h` |
| `ARITY` | fan-out of the (now 3) reduction trees | 10 | ✗ compile-time in `reduction.h` |

## Structure

Setup is only 2 fixed EDTs (`mainEdt`, a `shutdownEdt` gated on a
cluster-wide join reduction — there is no separate `globalInit`/
`globalCompute` chain; the SPMD fork happens directly from `mainEdt` via
`forkSpmdEdts_Cart2D`), then 4 one-time EDTs per tile
(`initEdt`→`channelSetupEdt`→`FNC_stencil`→`FNC_initialize`), then `NT+1`
rounds of 11 EDTs per tile (`timestepLoopEdt`, `timestepEdt` [FINISH],
`Lsend`/`Rsend`/`Lrecv`/`Rrecv`/`Bsend`/`Tsend`/`Brecv`/`Trecv`, `update`),
plus one `FNC_summary` per tile at its final round.

This port runs *three* reduction trees per tile through the shared
`reduction.c` library, not two, and the norm/timer pair use `ALLREDUCE`
(every tile needs the answer, unlike chandra's root-only `REDUCE`) while
the `spmdJoin` shutdown barrier stays plain `REDUCE`. With
`C(NR) = ⌊(NR-2)/10⌋ + 1` the count of tiles with at least one `ARITY=10`-ary
child (`NR≤11` ⇒ `C=1`), a single `ALLREDUCE` tree costs `4·NR + 2·C(NR) - 2`
EDTs and `4·NR - 3` DBs (the extra EDT per non-root tile over `REDUCE` is
`reductionRecvDown`'s clone, waiting on the broadcast; the extra DBs are
`reductionSendDown`'s one-per-child payload, sent by every tile that has
children, root included); a `REDUCE` tree costs `3·NR + 2·C(NR) - 1` EDTs
and `3·NR - 2` DBs (same as the chandra ports' two trees). Both tree types
cost the same `5·NR + C(NR) - 5` events. Two `ALLREDUCE` + one `REDUCE`
together: `11·NR + 6·C(NR) - 5` EDTs, `11·NR - 8` DBs, `15·NR + 3·C(NR) - 15`
events.

| object | count | notes |
|--------|-------|-------|
| EDTs | `-3 + 16·NR + 11·NR·(NT+1) + 6·C(NR)` | folds the three reduction trees into the fixed 2-EDT chain (incl. `mainEdt`) + `5·NR` one-time per-tile setup (4 setup EDTs + 1 `FNC_summary`) |
| DBs | `32·NR - 7` | `1+21·NR` (global + per-tile control-block/payload/transient DBs, NT-independent) plus the three reduction trees' `11·NR-8` |
| DB payload | same as `Stencil2D_intel_chandra`: `xIn` `8·(np_x+4)·(np_y+4)` B, `xOut` `8·np_x·np_y` B | `rankH_t` itself is a few hundred bytes (control-block only, no bulk payload) |
| Events | `10 + 31·NR + 11·NR·(NT+1) + 3·C(NR)` | `11` fresh events per tile per round (not 9 — see below) plus `16·NR` one-time per-tile handshake events at setup, plus the three reduction trees' `15·NR+3·C(NR)-15` |

Each round's 11 events are the `OCR_EVENT_COUNTED_T` helper that
`createEventHelper` attaches to each of the 8 halo EDTs (`Lsend`/`Rsend`/
`Bsend`/`Tsend`/`Lrecv`/`Rrecv`/`Brecv`/`Trecv`, `EDT_PROP_OEVT_VALID` so no
*extra* event is materialized beyond the one `createEventHelper` already
made) plus `timestepLoopEdt`'s own loop-continuation `COUNTED` join, plus
`timestepEdt`'s own creation: `EDT_PROP_FINISH` with a *freshly materialized*
(non-`OEVT_VALID`) output event, so it costs 2 more (output + finish) on top
of the explicit `COUNTED` already counted — 8+1+2 = 11, not the 9 the
original count found by only tallying `createEventHelper` calls and missing
`timestepEdt`'s own FINISH/output pair. The `16·NR` one-time setup figure
(8 labeled sticky "own publish" + 8 more labeled sticky "reverse
pre-declare", one pair per tile per of 4 directions — the same
double-creation pattern as the reduction tree's labeled events, see
Uncertain in the notes file — plus 8 `OCR_EVENT_CHANNEL_T` from
`STENCIL_WITH_DBUF_CHRECV`) matches the original doc's "8 labeled + 8
channel = 16" claim; only the *count of labeled creates* needed
correcting, not their sum.

At `NR=13824` (108×128) the NT-independent terms are fixed: DBs =
`32·13824-7` = **442,361**, `C(13824)=1383`, and the NT-independent EDT term
`16·NR + 6·C(NR) − 3` = **229,479**, which decomposes as the 2-EDT fixed chain
(`mainEdt` + `shutdownEdt`), the `5·NR` = 69,120 one-time per-tile EDTs the
application itself creates (`initEdt`, `channelSetupEdt`, `FNC_stencil`,
`FNC_initialize`, and the final-round `FNC_summary`), and the three reduction
trees' `11·NR + 6·C(NR) − 5` = 160,357 EDTs from `reduction.c`. The
NT-independent event term is `10 + 31·NR + 3·C(NR)` = 432,703. The NT-dependent
terms are `11·NR·(NT+1)` EDTs and the same count of events, so the totals follow
the height the campaign picks — **calibration pending**. At the historical
`NT=400` the round term is 60,977,664, giving **61.21 M** EDTs and **61.41 M**
events; at `NT=100` it falls 4× to 15,358,464, giving **15.59 M** EDTs and
**15.79 M** events.

Counter cross-check: verified (1 node, `NP=64 NR=4 NT=2` vs `NP=64 NR=4
NT=4`): predicted absolutes 200/122/269 and 288/122/357 (`NUM_EDT_CREATE` /
`NUM_DB_CREATE` / `NUM_EVENT_CREATE`) match the measured counters exactly,
against a runtime baseline of `+1 EDT, +1 DB, +0 EVT`. As in both chandra
ports, the fixed control-chain/payload-DB formulas
(`2+5·NR+11·NR·(NT+1)` / `1+21·NR`) were already exactly right — the gap
was the un-derived three reduction trees (`+45` EDT / `+36` DB at `NR=4`)
plus an event slope of 11 instead of the previously-claimed 9 per
tile-round. A later `NR=16` re-check (`NP=2048 NT=100/400`) confirms the
EDT/DB formulas and the per-tile-round slopes exactly (11 events created,
10 destroyed — the surviving one is the `timestepEdt` FINISH/output pair's
runtime-minted half), but finds the event TOTAL under-predicted by a flat,
NT-independent 72: the setup/reduction terms vary with the grid's actual
corner/edge/interior neighbor mix, which the `NR=4` (2×2, all-corner)
verification point could not expose. The formula is kept with that caveat
rather than re-fit.

**Conformance adaptations** (there is one tier, so these are the whole
adaptation list; none of them touches the compute-phase decomposition):

- the `DB_MODE_RW` on the handshake's reverse sticky leg corrected to
  `DB_MODE_CONST`, honouring the authors' own `//TODO should be RO`;
- the `Computed L1 norm` result line (the row's marker);
- an `#ifndef` guard around `ENABLE_EXTENSION_LABELING`;
- **loud argument validation** (`rejectSettings`): a wrong argument count, a
  non-positive size, `NR > NP`, or a tile thinner than the halo radius prints
  a usage line and aborts instead of silently running the defaults;
- **the shutdown EDT is hinted** at `mainEdt`'s own affinity. It was the one
  create in the program with `NULL_HINT`, so it round-robined onto an
  arbitrary rank; the join reduction it consumes returns to tile 0, which the
  tile map puts on that same rank;
- **teardown covers every channel endpoint**: `destroyOcrObjects` looped over
  4 of the `NB_SEND_CHANNELS` = 8 double-buffered endpoints, so the second
  phase's channel events (created by the neighbour, destroyed by this side)
  outlived the run — `4·NR` leaked events. The loop is now bounded by the
  channel count the build actually creates, so creates and destroys balance.

## Wiring

`rankH_t` is one consolidated per-tile control-block DB carrying the
command-line-derived params, the 11 EDT template GUIDs, both
affinity-hint structs, and the 8/16 halo-channel-event GUIDs, alongside
GUID handles to the payload DBs — where chandra spreads that same
information across 9 separate small handle DBs, this port keeps it as one
DB passed `DB_MODE_CONST` into almost every EDT (resolves the same as RO
under the shim). Halo data flow per direction: `Lsend`/`Rsend`/`Bsend`/
`Tsend` copy `xIn` (RO) into their phase's send buffer (RW), then
`ocrAddDependence` that buffer's guid directly into the neighbor-owned
persistent CHANNEL event (`haloSendEVTs[GET_CHANNEL_IDX(face,phase)]`) —
no separate "recv-ready" sticky leg, the channel itself provides the
producer/consumer rendezvous and FIFO ordering across generations.
`Lrecv`/`Rrecv`/`Brecv`/`Trecv` depend RO on their own `haloRecvEVTs` slot
and write into `xIn` RW at the matching halo region. Every send/recv EDT is
created with `EDT_PROP_OEVT_VALID` against a pre-made `OCR_EVENT_COUNTED_T`
helper (`createEventHelper`), so the same event GUID both signals `update`
that this leg finished (the join) and — on the send side — is itself the
producer end of the neighbor's channel. As with the chandra port, a halo
region has exactly one producer and one consumer per round, and no DB ever
takes concurrent RW from two tiles; the three reduction trees (norm ADD,
timer MAX, and the `spmdJoin` REDUCE-typed barrier every tile calls before
shutdown) are the only cross-tile fan-in, again mediated by `reduction.c`'s
own channel/sticky machinery outside the app's own graph.

## Flow

`mainEdt`'s only work is command-line parsing and validation, allocating the
four `ocrGuidRangeCreate` label spaces plus the join event and the hinted
shutdown EDT, and one call to
`forkSpmdEdts_Cart2D`, which itself issues `NR` hinted `ocrEdtCreate` calls
for `initEdt` in a single loop (no separate spawner EDT, unlike chandra) —
parallel width reaches `NR` immediately once that loop completes. Each
tile's `initEdt`→`channelSetupEdt`→`FNC_stencil` sequence is a 3-deep serial
per-tile setup (the one-time channel-event handshake happens inside this
window), then `FNC_initialize` runs in parallel with the *first*
`timestepLoopEdt`'s early setup (both depend only on `channelSetupEdt`'s
output, not on each other) before the timestep recursion actually touches
data. From there, compute is `NR` independent per-tile chains of `NT+1`
serial rounds (`timestepLoopEdt` re-creates itself on its own `FINISH`
child's join event), each round's 8-EDT halo exchange internally parallel.
Parallel width is `NR` throughout compute and does not grow with `NT`. Every
tile's final-round `update` triggers its own `FNC_summary`, which destroys
all of that tile's now-dead per-tile objects (`destroyOcrObjects` — payload
DBs, templates, both reduction return events and all 8 channel endpoints)
and joins the `spmdJoin` reduction; only after all `NR` tiles have joined
does the `shutdownEdt` (created once, from `mainEdt`, hinted at `mainEdt`'s
own rank, gated on `EVT_OUT_spmdJoin_reduction`) call `ocrShutdown()`.

Everything inside the measured span is workload, setup for it, or the
checksum that proves it ran. Data generation is per tile and index-seeded
(`FNC_initialize` fills only its own tile from the pure function
`IN(i,j) = i + j`, so the values are node-count invariant and no RNG or
shared state is involved); verification is the per-tile `O(np_x·np_y)`
absolute-value sweep over that tile's own `xOut`, summed by one `ADD`
`ALLREDUCE` and compared once at rank 0 — there is no serial recomputation
of the stencil anywhere. There is no input parsing beyond the three
arguments and no output file. The only stdout on the critical path is the
published one-time banner from `init_settings` and the four result lines
from rank 0's `FNC_summary`; the per-round `DEBUG_PRINTF`s compile out
unless `DEBUG_APP` is defined. The serial part that remains — rank 0's
`NR`-iteration fork loop, and the one-time per-tile handshake — is the
program's own task-graph construction, i.e. the SPMD decomposition this row
exists to exhibit, and is deliberately left alone.

## Placement (base)

Not a NULL-hint program, and not gated by any hint-layer guard (this
source carries no `OCR_APP_OPTIMIZED_PLACEMENT` code at all — there is no
`_hinted` build). `forkSpmdEdts_Cart2D` (`ocrAppUtils.c`, shared with the
chandra port's hand-rolled equivalent) queries
`ocrAffinityCount(AFFINITY_PD, …)` — the ARTS run's actual node count,
independent of the app's own `NR` — factors it into a `PD_X×PD_Y` grid via
`splitDimension_Cart2D`, and maps each tile's `(id_x,id_y)` onto a PD-block
coordinate via `getPartitionID` per axis (the identical block-partition
algorithm chandra uses), hinting each `initEdt` onto that PD. `initEdt`
captures `ocrAffinityGetCurrent` into the tile's control block, and every
later EDT and every payload DB of that tile carries it, so the map is
exactly index-derived — imbalance 1.000 at 1/2/4/8/16/32 nodes, since every
cut of the 108×128 tile grid divides. Net effect is the same 2-D
contiguous tile-block-per-rank locality as `Stencil2D_intel_chandra`:
neighbor tiles in both directions are usually co-resident, and cross-rank
halo channel traffic is limited to the tiles straddling a block edge
(directed interior halo edges `4·NR − 2·(NR_X+NR_Y)` = 54,824 at the
campaign grid, of which 904 cross a node at 8 nodes and 2,280 at 32).

Four qualifications the "hinted through the tile" summary does not cover:

- Three DB creates *in the application source* pass `NULL_HINT` — the per-tile
  control block, the per-direction handshake block, and `mainEdt`'s
  `globalParamH` — and land correctly only because a no-hint DB is homed at
  its creating rank. The linked `reduction.c` adds more no-hint DBs of its own,
  one per reduction phase per tile (`reduction.c:323`, `:376`, `:466`, `:631`),
  inside the measured window. A campaign that runs with
  `ARTS_NOHINT_DB_HOME=ROUNDROBIN` would scatter both sets — the per-tile
  control block, which is passed `DB_MODE_CONST` into nearly every EDT of every
  round, and every reduction payload — and must re-measure this row.
- Every EDT now carries a hint; the shutdown EDT was the last one that did
  not (it round-robined onto an arbitrary rank) and is now pinned to
  `mainEdt`'s rank, which is where the join reduction returns.
- The one-time handshake performs a **cross-rank racing labeled event create**:
  a tile creates the labeled sticky at range index `nbrUb·id + nbr`
  (`stencil_2d.c:1384`) and *also* at `nbrUb·nbrRank + nbrImage`
  (`stencil_2d.c:1416`), which is exactly the label its neighbour creates as
  that neighbour's own send event — so at a block edge two ranks race one label,
  and one of them races its create against the other's `ocrEventSatisfy`
  (`stencil_2d.c:1409`). That rendezvous is correct because no create of a live
  label touches the installed event: the first create to reach the label's home
  installs, a satisfy that arrives before it waits for it, and the other create
  parks behind it. The receiver's one destroy (`stencil_2d.c:1164`) then admits
  the parked create as an unused generation — one leftover sticky event per such
  label for the rest of the run (`docs/programming_model/guids.rst`).
- The one-time handshake's labeled sticky events live in a reserved GUID
  range, and a range's homes are spread by GUID index (`idx % nrank`), not by
  the tile map — so most of the `4·NR` handshake creates/satisfies are
  cross-rank even between co-resident neighbours. That is a property of the
  range policy, not of the placement, and no map can move it. It happens once
  per run rather than once per round, so it shows as a setup-phase traffic
  spike at high node counts and not in the steady state. At the domain edge
  the handshake is also periodic, so each edge tile establishes channel pairs
  that are never satisfied (`2·(NR_X+NR_Y)` = 472 pairs at the campaign grid)
  — published structure, kept.

## Placement (hinted)

Audited 2026-09-08 from source.  **The tile → node map is the minimum-cut
factorisation at every node count**, proved by replaying
`splitDimension_Cart2D` and `getPartitionID` over all `NR` tiles (`cut.py` in
the audit unit): directed halo edges crossing a node boundary per round at the
campaign grid (108 × 128 tiles), library's pick in bold — 2n `1×2` **216** vs
256 · 4n `2×2` **472** vs 648/768 · 8n `2×4` **904** vs 984/1512/1792 · 16n
`4×4` **1416** vs 1768/2008 · 32n `4×8` **2280** vs 2440/3496 — minimum on
edges and on bytes, imbalance 1.000, in both the campaign and the local trend
geometry (the cut objective is convex with its optimum at `0.919·√N`, and the
library returns the largest divisor ≤ √N with the larger factor on the
tile-richer axis).  Balanced non-uniform strip maps beat it only at 8n
(880 vs 904) and 32n (2232 vs 2280) — ~0.02 % of a round, and they would
replace the shared library fork — recorded, not implemented.  Every one of the
23 EDT creates carries a hint, events take no hint, the three `NULL_HINT` DBs
are creator-homed (optimal under `ARTS_NOHINT_DB_HOME=CREATOR`), the labeled
ranges are homed `idx % nrank` and unmovable, and the handshake is strictly
pre-loop.

**The one real freedom** is the halo send buffers (`DBK_{L,R,T,B}sendBufs`):
homed on the producer, but each has exactly one reader for the whole run —
the neighbour's recv EDT, RO, every round, on a node the index map fixes.
This is the same freedom as `hpcg_intel`'s `HPCG_HALO_HOME_HINT`.  The layer
(`stencil_2d.c`, per-face home hints in `channelSetupEdt` under
`OCR_APP_OPTIMIZED_PLACEMENT`; the base passes the identical
`&myDbkAffinityHNT` to all eight creates) homes each face's buffers on its
reader.  Re-homed buffers = 2 × crossing edges (432 / 944 / 1808 / 2832 / 4560
at 2/4/8/16/32n, 0.4 % → 4.3 % of the round's halo bytes).  Prediction before
measuring: write-through saves one message and the consumer's request round
trip per cross-node edge per round; write-back and exclusion are neutral by
construction; ceiling ≈ 0.1 % of a round at 2n rising to 1–2 % at 32n.

**Measured** (15w+1p × 1/2/4/8, two repeats, `8400 480 100`, scalars
identical in every cell; min/max of the two runs):

| arm | tier | 1n | 2n | 4n | 8n |
|---|---|---|---|---|---|
| INV × WB | base | 6.74/6.75 | 3.48/3.51 | 1.80/1.82 | 0.89/0.91 |
| | hinted | 6.75/6.75 | 3.51/3.54 | 1.80/1.81 | **0.99/1.02** |
| INV × WT | base | 6.74/6.76 | 3.53/3.53 | 1.81/1.84 | 0.89/0.93 |
| | hinted | 6.75/6.76 | 3.47/3.47 | 1.78/1.79 | **0.97/0.99** |
| VAL × WT | base | 6.73/6.74 | 3.52/3.52 | 1.79/1.82 | 0.88/0.89 |
| | hinted | 6.72/6.73 | 3.47/3.48 | 1.78/1.81 | **0.95/1.03** |

Write-back is neutral at 1–4 nodes and 11 % slower at 8; write-through gains
~2 % at 2 and 4 nodes — inside the box's drift — and loses 8–12 % at 8, in both
families.  The sign follows the crossing count, exactly as it did for
`hpcg_intel`: the reader-home saves the request round trip but the buffer now
travels as a push on the producer's critical path, and at 8 nodes (904 of
54,824 halo edges cross) that costs more than the trip it saves; at 16 and 32
nodes the crossing share only grows (1416, 2280).  **Verdict: hinted was
tried and no map beats the base** — the tile map is already the minimum cut
and the one movable object is better left where the base puts it — so the
base tier stands in for the hinted comparison (`hinted: false`), and the layer
stays in the source behind its guard as the record of the attempt.


## Family shape (measured on bentley, 15w+1p x 1/2/4/8 nodes, `6144 768 200`)

base (there is no `_hinted` — the port's own affinity layer IS the
placement), e2e seconds, counters all off. **Taken on the retired host
bentley** (`logs/exp/20260821-055930`), so the absolute seconds carry over to
nothing; the shape does:

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 7.33 | 3.94 | 2.05 | 1.14 |
| val_wb | 7.29 | 3.90 | 1.98 | 1.13 |
| inv_wb | 7.33 | 3.97 | 2.06 | 1.27 |
| excl_retain | 7.31 | 3.97 | 2.03 | 1.24 |

The ferrari re-measure of the same shape at a reduced size
(`5184 13824 400`, 15w+1p, `logs/adhoc/2026-08-31-base-calib/track.txt:143`)
is 31.11 / 21.39 / 11.93 / 8.86 s over 1/2/4/8 ranks = 3.51x; tcp loopback
prices the cross-rank halo edges far above a real fabric, so that number is a
floor on the shape, not a prediction.

Every arm strong-scales and the arms are indistinguishable: the block
placement confines halo traffic to tile-block edges, each halo buffer has one
producer and one consumer per round, and **in the steady state** no hot
globally-shared RO DB exists — so VAL has nothing to re-validate en
masse (combining changes nothing) and INV/EXCL pay only boundary-edge
rounds. This is the coherence-friendly pole of the application set, the
structural opposite of the one-hot-DB fan-in programs. Setup is the one
exception, and it is one-time: `forkSpmdEdts_Cart2D` hands *both* of its two
rank-0-homed blocks — the runtime's argv block and `globalParamH` — `RO` to
every one of the `NR` `initEdt`s, so a per-arm difference at setup is a real
`NR`-way RO fan-in on two blocks and should not be read as noise.

The scaling verdict is measured to 8 ranks only; the 16- and 32-node cells
are what test it. The term to watch there is the node-count-invariant setup
floor: rank 0 issues `NR` `ocrEdtCreate` plus `2·NR` `ocrAddDependence` in
one loop, and the four `ocrGuidRangeCreate` and the `4·NR` handshake pairs do
not shrink with node count either.

## Sizing

The CLI is `NP NR NT`, all three or none (any other count is now rejected
loudly). `NR` fixes the SPMD width and must factor near-square through
`splitDimension_Cart2D` — a prime `NR` degenerates to a 1×NR strip. `NP` is
the size dial; `NT` is the real timestep count — rounds are usage, not
repetition padding — so the window is reached with `NP` and `NT` stays at the
published height (100 in every upstream run script; the source default is 10,
and 400 is a member of the upstream sweep).

**Width.** The structure is spawn-and-join SPMD: all `NR` `initEdt`s are
issued in one loop and each tile is then a serial chain of `NT+1` rounds, so
the instantaneous frontier is `W = NR` for the whole run (a round briefly
fans to ~9 EDTs inside each tile, so the peak is ≈`9·NR`; `NR` is the number
that matters). The rule for a spawn-and-join structure is an *integer*
multiple of the widest geometry's worker count, so `NR = 13824 = 4×3456`
(32 nodes × 108 workers) — not "slack". Uniform tiles additionally want `NP`
to be a multiple of `lcm(NR_X, NR_Y) = lcm(108,128) = 3456`; a non-multiple is
legal (a remainder just makes some blocks one point wider, and sender and
receiver always agree on a strip's length) but leaves the tiles unequal, so
the rungs are 3,456 apart.

**Memory (1 node, all `NR` tiles resident).**
`payload = NR · (8·(np_x+2R)·(np_y+2R) + 8·np_x·np_y + 32·R·(np_x+np_y))`
with `np_x = NP/NR_X`, `np_y = NP/NR_Y`, `R = 2`. The halo term is `32·R`, not
`16·R`, per axis pair: the program creates **four** strip DBs per pair —
`LsendBufs[0..1]` and `RsendBufs[0..1]` at `8·R·np_y` each, `TsendBufs[0..1]`
and `BsendBufs[0..1]` at `8·R·np_x` each (`stencil_2d.c:1205-1222`). Those
strips are still small next to the two grids, so the whole thing is ≈`16·NP²`
bytes, independent of `NR` and of `NT`.

All figures below are **GiB** (the harness's `rss=` column is `RSS_KB`, i.e.
KiB, so its "48.5 GB" is 48.5 GiB):

| `NP` | per-tile bytes | payload |
|---|---|---|
| 31104 (`288×243`) | 1,170,848 | **15.07 GiB** (16.19 GB) |
| 62208 (`576×486`) | 4,581,056 | **58.98 GiB** (63.33 GB) |
| 65664 (`608×513`) | 5,098,208 | **65.64 GiB** (70.48 GB) |

Measured RSS at `31104 13824 400` was `RSS_KB=50,870,080` = **48.51 GiB**
(`logs/adhoc/2026-08-31-base-calib/cells/stencil2d_31104.log`, summarised at
`track.txt:159`), i.e. **3.22× the payload** — that is the one multiplier this
row has on record, and it is the figure both this document and the unit notes
use. The 33.4 GiB residual is runtime-side — 442,361 live DBs plus one
surviving FINISH/output event per tile-round (`NR·(NT+1)`, measured slope
exactly 1.0) — and it is **object-count driven, not payload driven**: it does
not grow with `NP`, and its event half falls 4× when the height drops from 400
to 100. So a multiplicative extrapolation of 3.22× to a larger edge
over-estimates, and an additive one (`payload + residual(NT)`) under-estimates
if the DB/route-table half also moves. Neither is a substitute for one
measurement at the committed rung.

**Measured lattices, by host.** The `NP²·(NT+1)` law holds exactly on one
host — ferrari `238.2 · (31104/41472)² = 134.0` against a measured 134.03 —
but the two hosts are 1.65× apart, so the lattices must not be mixed.

| NP | NT | e2e (bentley, retired) | e2e (ferrari) |
|---|---|---|---|
| 13824 | 100 | 5.2 s* | — |
| 27648 | 100 | 16.8 s* | — |
| 13824 | 400 | 18.6 s* | — |
| 27648 | 400 | 69.1 s | — |
| 41472 | 400 | 144.4 s | 238.2 s |
| 55296 | 200 | 131.6 s | — |
| 34560 | 400 | — | 162.8 / 171.0 / 155.7 s |
| 31104 | 400 | — | 134.0 / 127.6 / 154.0 s |

All of these are **one node at 108 workers + 4 progress threads** (the
single-node run reclaims the progress threads as workers), Release, counters
off; the ferrari triples are the three family representatives
`val_wb / inv_wb / excl_retain`. The 15w+1p geometry belongs only to the
reduced trend series in the section above, never to a sizing point.
(*) measured with an object-counter set still compiled in; a clean-tree
re-measure of `27648 400` moved 67.2→69.1 s, so that instrumentation is
within run-to-run noise and the starred points stand for shape.

**Campaign size: calibration pending.** The height goes back to the published
`NT = 100` and the edge is re-derived from the one-node 108w+4p ferrari anchor
(`134.03 s` at `31104 13824 400`). Rewriting that anchor at the published
height gives the law the orchestrator sizes from:

    T(NP, NT=100) = 134.03 · (NP/31104)² · (101/401) = 33.76 · (NP/31104)²   seconds

so the ~150 s point is `NP = 31104 · sqrt(401/101) · sqrt(150/134.03) ≈ 65,560`.
Two rungs bracket it (rungs are `3456` apart, and both of these divide **both**
axes exactly):

| rung | per tile | predicted e2e at `NT=100` | payload |
|---|---|---|---|
| `62208 = 18·3456` | `576×486` | **135.0 s** | 58.98 GiB (63.3 GB) |
| `65664 = 19·3456` | `608×513` | **150.5 s** | 65.64 GiB (70.5 GB) |

`65664` is the ~150 s candidate; `62208` reproduces very nearly the same
duration the anchor itself was measured at, so it is the ~135 s fallback, not
the ~150 s point (`62208 = 2 × 31104`, and `2² · 101/401 = 1.008`).

**Both rungs need a one-node RSS check before either is committed.** Against
the corrected payload the recorded 3.22× multiplier puts `65664` at ≈211 GiB
(227 GB) and even `62208` at ≈190 GiB (204 GB) — over the 190 GB planning cap
in both cases. That multiplier is the pessimistic reading: the residual it
encodes is object-count driven and `NP`-independent, so the additive reading
(`65.64 GiB` + a residual no larger than the 33.4 GiB measured at 4× the
height) lands at **≈99 GiB** for `65664` and ≈92 GiB for `62208`. The two
readings differ by more than a factor of two and straddle the cap, which is
precisely why this must be measured and not extrapolated.

`expect` moves with the height and with nothing else: the printed norm is
`(NT+1)·2` exactly, so `NT=100` gives `202.000000000000` where `NT=400` gave
`802.000000000000`.
