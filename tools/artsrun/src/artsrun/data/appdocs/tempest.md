# tempest

*A cubed-sphere halo exchange with the physics removed: 6 panels of `k×k`
patches trade an 8-byte "who am I" block with up to 8 neighbours, `duration`
timesteps (published default 100), over persistent CHANNEL events.*
Source: `third_party/ocr-apps/apps/tempest/refactored/ocr/intel-bryan/tempestCommunication.c`
(~1050 lines).

## Overview

Bryan Pawlowski's (Intel, 2015) OCR-ification of the Tempest atmosphere model
(P. Ulrich, UC Davis) — but only the *communication* skeleton survived the
port. The program builds the cubed-sphere patch topology (6 panels, `k×k`
patches each, `patchNum = panel·k² + k·x + y`), works out each patch's 8
neighbours across the panel seams, and then exchanges one `nbData_t` (a single
`s64` patch number) per direction per timestep. There is no state vector and
no arithmetic: a patch's whole timestep is "stamp my number into each block I
received and hand it on".

Patch `TEST_PATCH` (0) prints, in its terminal generation only, its computed
neighbour grid, then `*CROSS-CHECKING NEIGHBOR DATA EXCHANGE*` and the 3×3
grid of patch numbers it actually *received*; the two match iff every block
travelled the edge it was wired to. The catalog's scalar is the last cell of
the received grid — the SE neighbour of patch 0, which is `5k² + 1` for every
`k` and every `duration`: `21` at the default `k=2`, `11521` at `k=48`,
`46081` at `k=96`. It is a narrow oracle: one direction of one patch, the
other eight cells printed but not extracted.

What it stresses is task churn and fine-grain exclusive data movement: every
timestep is `6k²` tasks, each acquiring **nine RW datablocks** and issuing
eight event satisfies, with nothing to compute in between. The persistent-
channel idiom keeps event objects out of the steady state (created once at
setup, reused for every generation), so what remains is EDT creation plus
per-node-exclusive block migration — a coherence probe, not a FLOPS benchmark.

Adaptations of the published source, identical in both tiers and to be
disclosed with the measurement:

1. **Persistent CHANNEL events replace the published per-timestep labeled
   sticky create/destroy.** The published idiom re-creates a shared labeled
   GUID every timestep, and nothing orders the next generation's satisfy
   behind the previous generation's destroy through the label's home, which
   label reuse requires (`docs/programming_model/guids.rst`, Known
   weaknesses). The substitution also removes ~16
   event operations per patch per timestep from the steady state of a program
   that is nothing but event traffic.
2. **`duration` became `argv[2]`** (the compile-time `#define DURATION 100`
   remains its default), so the run length is a campaign knob rather than a
   build.
3. **The panel dependence is declared `DB_MODE_CONST`, not `DB_MODE_RW`.** No
   task other than `realmainEdt` ever writes a panel block, so the RW
   declaration was untrue; the effect is that a panel's `k²` `patchInit`s no
   longer take exclusive turns on it. This is a declaration fix with a
   measurable consequence (the init phase widens), which is why it is
   disclosed rather than silent.
4. **The terminal-generation neighbour test reads the pointer, not the GUID.**
   The generation-0 seed leaves a block in an absent neighbour's slot, so the
   GUID test dereferenced NULL at `duration=1`.
5. **Loud argument validation** (`strtol` + positivity + arity) replaces the
   silent `atoi`.
6. **The per-generation `timestep: <g>` line is gone.** Patch 0 printed one
   stdout line per generation on its own dependence chain, inside the measured
   span; only the published cross-check output remains.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|------------------|
| `argv[1]` = `k` | patches per panel side; total patches `6k²` | 2 | ✓ parsed in `mainEdt` (`strtol`, positive-integer checked), passed as `realmainEdt`'s `paramv[0]`, stored in every panel DB and copied into every patch DB — multinode-safe |
| `argv[2]` = `duration` | timesteps, i.e. `patchEdt` generations per patch | 100 (the source's `#define DURATION`, used when the argument is absent) | ✓ an argument — width comes from `k`, run length from here, and the two are therefore independent |
| (arg count) | `argc == 1` (no args) keeps the documented default `k=2`; more than two arguments, a non-numeric one, or a non-positive one is a loud usage error (prints `USAGE:` and shuts down) | — | ✓ |
| `TEST_PATCH` | which patch prints the cross-check | 0 | ✗ compile-time (`#ifndef`-guarded, so `-DTEST_PATCH=` would work, but the CMake target does not set it) |
| channel `maxGen`/`nbSat`/`nbDeps` | requested `2`/`1`/`1` per halo channel | — | ✗ in-source; the shim *requires* `nbSat=nbDeps=1` and ignores `maxGen` (ARTS channels are unbounded MPSC), so there is no backpressure knob |

`k` moves parallel width, object count and memory together — nothing scales the
work *per* task — so `k` is the width dial and `duration` is the length dial.
The campaign holds `duration` at the published 100 and sizes with `k=216`
(measured: see the anchor below).

## Structure

Every patch has 8 neighbours except the 4 corner patches of each panel, which
lack one diagonal, so the directed halo edges number `E(k) = 48k² − 24`. (The
topology was checked exhaustively up to `k=96`: every edge has a matching
reverse edge, no patch names the same neighbour twice, and every handshake slot
has exactly one publisher and one learner.)

| object | count | size / note |
|--------|-------|-------------|
| EDTs | `6k²·duration + 12k² + 9` | `6k²·duration` `patchEdt` (`6k²` chains × `duration` generations) + `6k²` `patchInit` + `6k²` `channelSetup` + 6 `panelInit` + `realmain` + `wrapup` + 1 `mainEdt` itself (the OCR shim creates it as an EDT — `arts_edt_create(mainEdtTrampoline, ...)` — before its body runs; not one of `mainEdt`'s own explicit `ocrEdtCreate` calls) |
| DBs | `102k² − 18` | 6 panel (80 B) + `6k²` patch (232 B: `sizeof(patch_t)`) + `E(k)` channel-handoff (8 B) + `48k²` halo seeds (8 B) — all duration-independent |
| Events created | `144k² − 70` | see accounting below |
| Live event objects | `96k² − 46` | `E(k)` CHANNEL + `E(k)` labeled sticky + 2; nothing is ever destroyed |
| EDT templates | `12k² + 9` | pure GUID encodings under ARTS, not runtime objects |

Only the EDT count carries `duration`; every other count is a function of `k`
alone, because nothing is destroyed and the halo blocks are reused rather than
re-minted.

Event accounting: one `OCR_EVENT_CHANNEL_T` per directed edge, plus **two**
`ocrEventCreate` calls per labeled sticky slot — publisher and learner both
create the same `GUID_PROP_IS_LABELED | GUID_PROP_CHECK` GUID; the second
create parks at the label's home behind the first for the rest of the run
(nothing is destroyed), so `2·E(k)` creates yield `E(k)` objects and `E(k)`
parked creates. Exactly one
`ocrEdtCreate` passes a non-NULL `outputEvent` (`realmain`, feeding `wrapup`)
and that same EDT is the only `EDT_PROP_FINISH`, whose finish event the shim
pre-creates: `+2`. Every other `ocrEdtCreate` passes NULL and creates nothing.

Worked numbers at the local-trend geometry `96 100` — 55,296 patches, 442,344
halo edges: **5,640,201** EDTs (5,529,600 of them `patchEdt`), **940,014** DBs,
**1,327,034** event creates for **884,690** live events, ≈19.0 MiB of payload
(`2160k² + 288` bytes), ≈49.8 M RW acquires over the run
(`9 · 6k² · duration`). Default `k=2`,
`duration=100` → 24 patches, 2,457 EDTs, 390 DBs, 506 event creates. The
campaign runs `k=216`; substitute it in the formulas above.

Counter cross-check: verified (1 node, `k=4` vs `k=8` at `duration=100`,
measured totals 9,802/39,178 EDTs, 1,615/6,511 DBs, 2,234/9,146 events).
NUM_EVENT_CREATE matches `144k² − 70` exactly with no offset; NUM_DB_CREATE
matches `102k² − 18` plus the runtime's constant +1 DB per run; NUM_EDT_CREATE
matches `6k²·duration + 12k² + 9` plus that same constant +1 EDT per run.

## Wiring

`mainEdt` (rank 0) creates 6 panel DBs and hands them RW to `realmainEdt`, the
run's single FINISH EDT, whose output event fires `wrapupEdt` (`DONE.` +
`ocrShutdown()`). `realmain` reserves eight labeled sticky GUID ranges of
`6k²` each — one per direction, used only for the one-time channel handshake —
stamps them into every panel DB and forks 6 `panelInit`s.

`panelInit` loops `k²` times: one 232 B patch DB and one `patchInit` per patch,
wired `panel DB → slot 0 (CONST)`, `patch DB → slot 1 (RW)`. No `patchInit`
writes the panel block — it only reads `patchRange`, `duration` and the GUID
ranges — and the dependence says so, so the `k²` readers of a panel share it
instead of taking exclusive turns.

`patchInit` computes the 8 neighbours, then per existing direction `i` creates
a CHANNEL event (its *receive* queue for that direction) and publishes it: an
8 B DB holding the channel GUID, satisfied into the labeled sticky at
`(range[rel], neighbour)`, where `rel` is the neighbour's direction back at me;
symmetrically it wires its `channelSetup`'s slot `i` (RO) to `(range[i], me)`,
where that neighbour publishes. `channelSetup` records the learned GUIDs as
`sendChannels[]`, mints 8 fresh 8 B `nbData` seed blocks (unconditionally,
corner slot included) and launches generation 0. `patchEdt(g)` then creates
generation `g+1`, wires `recvChannels[i] → slot i (RW)` (one dependence = one
pop from the FIFO), writes its own patch number into each received block,
releases it and satisfies the neighbour's channel with it, and finally releases
its patch block into slot 8 (RW). Blocks are never re-minted: `P`'s seed for
direction `i` ping-pongs across the `P↔Q` edge for the whole run.

Steady-state DB concurrency is uniformly exclusive — one accessor at a time,
no block takes RW from more than two patches (halo) or one (patch state). The
remaining contention points are (a) the **6 panel DBs**, read by `6k²+6` tasks
during init — shared reads, but a fan-out from wherever the block lives — and
(b) the **single global finish-scope latch**, which every EDT create INCRs and
every completion DECRs: `2·(6k²·duration + 12k² + 8)` satisfies on one event,
11.3 M at `96 100`.

## Flow

`mainEdt` is rank-0-only but O(1); `realmain` is one EDT. Width is then **6**
for the creation phase: each `panelInit` runs a `k²`-iteration serial
create+wire loop. The `patchInit`s it spawns are `6k²` ready tasks gated only
on a shared (CONST) panel block, so they no longer serialise behind it;
`channelSetup` is `6k²` wide as well, each gated on its 8 published slots.
Steady state is `duration` generations of `6k²` independent tasks with no
global barrier and no rank-0-only phase; because generation `g+1` of a patch
needs generation `g` of each neighbour, skew between two patches is bounded by
their graph distance rather than by a barrier. The tail is symmetric:
`patchEdt` at `timestep == duration−1` prints (patch 0 only) and returns
without a successor, the finish scope drains, `wrapup` shuts down. Nothing is
destroyed anywhere in the program — no `ocrDbDestroy`, no `ocrEventDestroy` —
so every object created stays live to the end.

## Placement (base)

Every create passes `NULL_HINT`: the `makePatchEdtHint`/`makePatchDbHint`/
`makeLocalEdtHint` helpers return `NULL_HINT` unless
`OCR_APP_OPTIMIZED_PLACEMENT` is defined (the `_hinted` build), and there is no
affinity use outside that guard — the base program never calls `ocrAffinity*`
at all. Effective policy: **EDTs round-robin, DB home = creating rank.**
Hence:

- The 6 panel blocks are homed on rank 0 (`mainEdt`'s rank). `realmain`'s RW
  take is the only write; after it, the six `panelInit`s and all `6k²`
  `patchInit`s read the block as CONST, so a single rank-0-homed object serves
  a `6k²`-wide read fan-out.
- A panel's `k²` patch blocks are all homed on that one `panelInit`'s rank (six
  ranks host every patch block), while the tasks that touch them are scattered.
  Each patch block is then RW-acquired by `duration + 2` successive EDTs
  (`patchInit`, `channelSetup`, `duration` generations), each placed
  independently at random — 232 bytes of per-patch state migrating once per
  timestep, remote with probability `(N−1)/N`.
- Halo seeds are homed wherever `channelSetup` ran and move once per timestep
  each: `(duration−1)·E(k)` migrations of 8-byte blocks between two moving
  holders.
- Channel events live on their patch's `patchInit` rank, so every satisfy and
  every dependence registration is a message to a third, unrelated rank; the
  labeled sticky slots are spread by index, making each handshake a three-party
  rendezvous.

The algorithm has textbook nearest-neighbour locality on the sphere and the
base program expresses none of it: no two objects of a patch are placed
together, and re-placing the chain every generation means locality can never
even accumulate.

## Placement (hinted)

As-born is placement-blind: each timestep's patch EDT lands round-robin, so a
patch's halo exchange partners are arbitrary ranks and its persistent halo
blocks (created once at setup, reused every generation) are acquired remotely
almost every turn.

The layer (`patchHomeRank`) maps the cube-sphere's `6 × k × k` patches onto a
`P × Q` rank grid chosen from the divisors of the rank count to minimise the
cut (the number of patch edges crossing rank boundaries), unrolling the six
faces along one `k × 6k` strip; each patch EDT is pinned to its patch's home
rank every generation (`OCR_HINT_EDT_AFFINITY`) and each patch's state block is
homed there (`OCR_HINT_DB_AFFINITY`). With EDTs stationary, the reused halo
blocks' ownership settles on the consumer's rank after the first turn. The six
panel blocks are hinted to the home of their own panel's first patch, which is
also where that panel's creation loop runs — in the base tier they all stay on
rank 0 and every `patchInit` reads them from there.

The `P × Q` rule now covers **every** rank count. It used to be preceded by a
special case — below 6 ranks the map was a contiguous patch-number band — and
that case was strictly worse wherever the two differed. A patch number runs
along the strip's *row* (`idx = row·k + col`, so consecutive numbers advance
`gcol` at fixed `row`), which makes a band a split on the row axis, not the
column split the `P × Q` rule picks there. Enumerated on the strip's own
adjacency at `k = 48` (the metric the rule minimises: a cut between two
columns severs `k` edges, one between two rows `6k`):

| ranks | band cut | `P × Q` cut | same partition? |
|---|---|---|---|
| 2 | `1k` | `1k` (P=1,Q=2) | yes |
| 3 | `2k` | `2k` (P=1,Q=3) | yes |
| 4 | `5k` (`3k` column + `2k` row) | `3k` (P=1,Q=4) | no |
| 5 | `8k` | `4k` (P=1,Q=5) | no |

Both maps are exactly balanced at 2, 3 and 4 ranks (the band to within one
patch always; the grid exactly whenever `Q | 6k`, true for even `k` at 4
ranks). 4 ranks is a live geometry, so the band was removed rather than
documented. Only at 5 ranks — not a geometry of either machine — does the
band buy anything: balance to within one patch (1.0001) against the grid's
1.007, for exactly twice the cut.
Neither map is face-aligned: at 4 ranks the grid's boundaries fall at
`gcol = 1.5k, 3k, 4.5k`, two of them inside a face. Face alignment is a
property of the face count, not of the cut, and at a rank count that does not
divide 6 it can be bought only with imbalance.

What no closed-form cut on this strip can see is that the strip's adjacency is
not the sphere's: faces 0-3 are an equatorial ring, so the `3|4` and `4|5`
strip seams are free cuts, while each pole face borders all four equatorial
faces at arbitrary strip distance. The residual is bounded by that intrinsic
polar adjacency; a genuinely better map would have to be a precomputed
partition of the true `6k²` adjacency, and would only be worth taking if its
balance stayed at 1.0.

Everything under the guard is a hint: the guard occurs only inside
`patchHomeRank` and the three `ocrHint_t *` helpers, and every call site is
unconditional and identical in both tiers. Load balance is the patch
partition's, i.e. exactly 1.000 at 2, 4, 8, 16 and 32 ranks whenever `P | k`
and `Q | 6k` (which holds for `k` a multiple of 24 at every one of those
counts). The one phase the layer cannot balance is the program's own width-6
creation phase: the six `panelInit`s land on the homes of patches
`0, k², …, 5k²`, so at 8 ranks two ranks run no creation loop. R1 forbids the
layer from changing that.

Two objects stay out of the layer's reach in both tiers: the eight labeled
sticky GUID ranges are reserved round-robin by the shim, so the one-time
handshake's `2·E(k)` event creates and `E(k)` satisfies are three-party
rendezvous at unrelated ranks, and OCR offers no hint surface on a labeled
range. That term is `O(k²)` against a steady state of `O(k²·duration)`.

## Sizing

`k` scales the *number* of tasks and blocks (`6k²` patches, `∝ k²` time) and
never their size — a `patchEdt` does the same eight stores at any `k` — so `k`
is the width dial and `duration`, the second argument, is the length dial.

The window is reached with `k`, at the published `duration = 100`; the width
follows as a consequence, not the other way round. `6k²` independent patch
chains are runnable at once (this is a dataflow frontier, not a fork-join:
`patchEdt` creates only its own successor, there is no barrier, and the shim
ignores the channels' `maxGen`), so the largest geometry's 3456 workers are a
floor with no ceiling: any `k` that lands the window clears the width rule with
room, and a `k` that is a multiple of 24 also keeps the `P × Q` balance exact
at 2/4/8/16/32 ranks.  The campaign runs `k=216`.  The one-node anchor at
`duration=100` is measured: 20.1 s base / 19.3 s hinted, 10 GB resident,
against a 20 s target (hinted kept, flat).  Every anchor published before
this predates the current tree and was taken at `duration = 1900`, which the
size knob has since replaced and which does not carry over; the
base-vs-hinted multi-node pair is what a fresh campaign takes.

Memory never decides. Object counts are duration-independent, so live objects
are `102k² − 18` DBs and `96k² − 46` events — `198k² − 64` in all, 456,128 at
`k = 48`. Both terms are `∝ k²` and the payload is a small part of the total,
so the measured 1 GB at `k = 48` scales as ≈`(k/48)² GB` — about 20 GB at
`k = 216`, against a 190 GB per-node budget.

This row anti-scales harder than anything else in the roster — a program whose
task does eight stores and then exchanges with eight neighbours pays the whole
cost in communication once half those exchanges cross a rank — and the
placement layer is worth more here than anywhere else in its cycle. Both
statements are qualitative until the re-take: the numbers previously quoted
here (a 235× 1→2 node degradation at `k=16, duration=4400`, and 454.2 s → 34.7 s
at four nodes with `48 1900`) come from lattice points that are neither the
calibrated size nor the published height, and are not this row's headline.
