# LCS_all_db_distributed

*Same recursive quad-tree wavefront DP, but now the score matrix is a
genuine `L×L` grid of per-tile DBs too, each created by the task that
produces it and handed on through a labeled sticky event — a real
tile-to-tile wavefront DAG, not a single shared DB coordinated by events.*
Source: `third_party/ocr-apps/apps/LCS/refactored/ocr/intel-jesmin-lcs_all_db_distributed/lcs_distributed.cpp`
(~1,000 lines, C++; author Jesmin Jahan Tithi, Intel 2016).

## Overview

The third point in this family's decomposition spectrum: where
`LCS_shared` keeps `S`, `T`, and the score array each in one DB, and
`LCS_distributed_ST` distributes only `S`/`T`, this variant distributes
**everything**, including the score matrix itself — hence "all DB
distributed". The score is no longer the `O(N)` antidiagonal-collapsed
array the other two variants use; it is the *full* `(N+1)×(N+1)` DP
table, tiled `base×base` into `(L+1)²` separate DBs (`L = N/base`), each
created by its one writer and read by up to three read-only consumers
(its east/south/southeast neighbors) through a labeled sticky event of
its index — a real tile-grid wavefront DAG at the DB level, not just at
the EDT level. The
recursion shape is unchanged (`recLCSEdt` quad-tree: `x11` unblocked,
`x12`/`x21` gated on `x11`, `x22` on both; `seqLCSEdt` fills the base
case), but reaching a leaf now wires real dependencies onto four
neighboring score tiles, not one shared DB. Initialization is parallel
here (a separate `InitEdt` finish scope with one fill task per string or
boundary tile, each creating the tile it fills).

Note that this row's kernel is not the other two rungs' arithmetic: it is
a MIN recurrence over the full table (an edit-distance-shaped answer,
≈0.5·N), while `LCS_shared`/`LCS_distributed_ST` collapse to a MAX
recurrence whose answer is analytically `N`. The rows share an argument
surface, a DAG shape and — since 2026-09-03 — the same generated strings,
but not one computed quantity.

`wrapupEdt` prints `LCS length:` (the table's bottom-right cell) and
`LCS checksum:`, a 31-bit FNV-1a digest of the whole final score tile —
the voted scalar, and the same digest `LCS_wavefront` prints for the same
arguments. The catalog marks this row `hinted: true` (a companion
`OCR_APP_OPTIMIZED_PLACEMENT`-guarded build exists; see Placement). DAG
shape depends only on `N`/`base`, never on string content.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `N` | string length | 1024 | ✓ `atol` in `mainEdt`, reaches the recursion via `LCS_task_params.N` (`paramv`) — multinode-safe |
| `argv[2]` = `base` | recursion base case / tile side length (both the recursion's and the score grid's) | 256 | ✓ same, via `LCS_task_params.base` |

Both arguments are mandatory and validated loudly: a wrong argument count
prints the usage line and shuts down, and `N/base` must be a positive
power of two — the leaf addresses its tile as `(xi-1)/base` while the
recursion halves `n`, so any other ratio silently computes a different,
wrong table. `GAP_PENALTY` is compile-time only. The hint helpers
(`bandOfRow`, `rankHint`, `scoreIdx`, `sIdx`, `blockRowHint`,
`tileRankHint`) and the two event-range sizings belong to the
`OCR_APP_OPTIMIZED_PLACEMENT` build only — see Placement. There is no
bands-per-rank knob: the band count *is* the rank count (see Placement
(hinted)), so nothing needs sweeping.

## Structure

`L = N/base = 2^d`, where `d` is the recursion depth (shifts of `N` until
`≤ base`); `grid_block_size = L+1`.

| object | count | size |
|--------|-------|------|
| `recLCSEdt` (incl. root) | `(4^(d+1)-1)/3` | — |
| `seqLCSEdt` (compute tile, `depc=5`) | `L²` (`=4^d`) — one per interior score tile, which it creates | — |
| `InitEdt` | 1 (finish scope around the string and boundary set-up: it creates their tiles' events and fill tasks, and no DB) | — |
| `randInitEdt` | `2L` — one per `S`/`T` tile, which it creates and fills | — |
| `scoreInitEdt` | `2L+1` — creates and fills the boundary row-0/col-0 tiles and the single corner tile; the `L²` interior score tiles are created by the `seqLCSEdt` that computes them | — |
| `mainEdt` / `wrapupEdt` / `shutDownEdt` | 1 each | — |
| DBs | `(L+2)²` — 3 pointer-array DBs (`S`,`T`,`score`, created by `mainEdt`) + `2L` `S`/`T` tiles + `(L+1)²` score tiles (1 corner + `2L` boundary + `L²` interior), every tile created by its producer | pointer arrays: `8L`/`8L`/`8·(L+1)²` B; `S`/`T` tiles: `4·(base+1)` B (tile 0), `4·base` B (others); score tiles: `4` B (corner), `4·base` B (boundary), `4·base²` B (interior — the dominant term) |
| Events | `7L² + 4L + 4` — `6L² + 3` (`= 6·4^d + 3`) from the task wiring: every `recLCSEdt` create *and* the single `InitEdt` create are `EDT_PROP_FINISH` with a non-NULL output event, costing 3 events apiece (1 app STICKY + 1 shim group latch + 1 shim output event); every `seqLCSEdt` create (count `L²`) costs 2 (app STICKY + the shim's completion event, no group); `wrapupEdt`'s output event costs 1; `randInitEdt`/`scoreInitEdt`/`shutDownEdt` cost 0 — plus `(L+1)² + 2L` labeled STICKY tile events, one per tile, which live to the run's end | — |

Worked numbers at the calibrated `args = ['131072', '1024']` (`L = 128`,
`d = 7`): `recLCSEdt` = 21,845; `seqLCSEdt` = 16,384; `randInitEdt` = 256;
`scoreInitEdt` = 257; total app EDTs = 38,746; Events = `6·128² + 3` +
`129² + 256` = 98,307 + 16,897 = 115,204; DBs = `(128+2)²` = 16,900. Score
payload is the headline number: `4·(N+1)²` ≈ **68.7 GB** created over the
run at this `N` (interior tiles alone total exactly `N²` cells) — three to
four orders of magnitude more than `LCS_shared`/`LCS_distributed_ST`'s
`O(N)` score payload, because this is the only variant that computes the
full DP table. It is never resident at once: each interior tile is created
by its leaf, and each tile that has a south-east neighbour is destroyed by
that neighbour's leaf once its other readers completed (see Sizing).

Upstream inefficiencies preserved in base: one STICKY event per leaf is
created and immediately orphaned, and an EDT template is created per
recursion call and never destroyed.

## Wiring

- `InitEdt` (`depc=3`): `S`/`T`/`score` pointer-array DBs, all RW, which
  it neither writes nor reads (disclosed below). For each `S`/`T` tile and
  each boundary score tile its body creates the tile's labeled STICKY
  event and then the `randInitEdt`/`scoreInitEdt` child (`depc=0`, the
  event in its parameters) that creates the tile where it runs, fills it,
  releases it and satisfies the event. `InitEdt` creates no DB.
- Root `recLCSEdt` (`depc=1`) is wired to `InitEdt`'s finish-scope
  completion event — the whole computation is gated on `InitEdt` and every
  fill task finishing first, so every string and boundary tile's event
  exists, and has been satisfied, before any leaf registers on it.
- Internal `recLCSEdt` calls: identical `x11`/`x12`/`x21`/`x22`
  event-chain wiring to the other two variants, still carrying no DB
  dependence.
- Each leaf's `seqLCSEdt` (`depc=5`) is wired to **five** tile events:
  `S` tile (RO, slot 0), `T` tile (RO, slot 1), and its
  **left**/**above**/**diagonal** neighbor score tiles (RO, slots 2-4).
  Its own **current** score tile it creates: the leaf-level `recLCSEdt`
  creates that tile's labeled STICKY event before it creates the leaf and
  passes it in the leaf's parameters (the last tile's event is created by
  `mainEdt`, which registers the wrap-up on it), and the leaf creates the
  tile, computes it, releases it and satisfies the event. Every reader of
  the tile is registered by a leaf-level task under a quadrant that waits
  on the producer's quadrant's FINISH. This is a genuine tile-to-tile DAG:
  the dependency structure *is* the data structure. The leaf destroys its
  diagonal tile, whose other readers (at most two) have all completed by
  construction — a base adaptation that frees every tile that has a
  south-east neighbour once nothing else reads it and adds `L²` destroys
  to the measured window of both tiers.
- `wrapupEdt` takes the root finish EDT's output event and the final score tile's event RO,
  prints the answer and the digest; `shutDownEdt` is a separate task gated
  on `wrapupEdt`'s output event, so the digest and the final release are
  inside the measured window and no dependence is held across
  `ocrShutdown` — the same convention `LCS_wavefront` uses.
- **DB concurrency.** Every score tile has exactly one writer and at most
  three RO readers (east, south, southeast); no DB in this program is
  touched concurrently by more than a handful of tasks, so concurrency
  tracks the DAG's real data dependences with no DB-level serialization.
- **Width.** The tile grid's anti-diagonal would allow `L` concurrent
  leaves, but the recursion's FINISH gating is the only ordering the
  program actually has (a leaf's dependences are on events its
  neighbours' producers satisfied before the leaf was created, so they
  satisfy on arrival), so the schedule's *peak* width is `2^d` and its
  *time-average* is work/span = `(4/3)^d` = 7.5 at the calibrated point.
  The recursion is a departure from the anti-diagonal formula, not a
  scheduling strategy for it.

## Flow

Phase 1 (`InitEdt`, finish scope): a single task body creates `4L+1`
tile events and spawns `4L+1` `randInitEdt`/`scoreInitEdt` children, each
creating and filling one `S`, `T` or boundary/corner score tile in
parallel across ranks and then satisfying the tile's event; it creates no
DB, and the `L²` interior score tiles are created by their leaves in
phase 2. The root recursion is gated on this whole finish scope. The
string fill is index-seeded — a tile's
characters are a pure function of `(tile index, position)` through a
self-contained `splitmix64`-style mixer, so no library generator and no
host C library enters the instance — and uses the same seed numbering, tile
widths and leading sentinel as the other three LCS rows,
so all four compute the same instance at the same `N`/`base`, independent
of node count, worker count and task order.

Phase 2: the quad-tree recursion unfolds to depth `d`, each of the `L²`
`seqLCSEdt` leaves creating and computing one tile from its three
neighbours. Phase 3:
`wrapupEdt` prints the answer cell and the checksum of the final tile, and
a separate `shutDownEdt` stops the runtime.

Nothing but the workload is inside the `[E2E]` window: the published
program's serial `O(N²)` reference recomputation was already disabled here
and has now been deleted along with the dead `CHECK_RESULTS`/`PRINT`
scaffolding in `mainEdt` (which referenced pointer-array entries no task
writes, and no longer compiled).

## Placement (base)

The `OCR_APP_OPTIMIZED_PLACEMENT` guard's `#else` branches make every
helper a no-op: `scoreIdx`/`sIdx` return the identity index and
`blockRowHint`/`tileRankHint` return `NULL_HINT`. So in base every EDT
and DB create in this file — `InitEdt`, `recLCSEdt`, `seqLCSEdt`, the fill
children, every tile — passes `NULL_HINT`, same as the other two variants.
**Labeled GUIDs place by index regardless of hint** on ARTS and ocr-vx: a
range from `ocrGuidRangeCreate` gets each index's home fixed round-robin
(`home = index % nranks`) at range-creation time, baked into the GUID
itself — here the tiles' events (xsocr homes a whole range on the rank
that reserved it). Effective policy:

- **EDTs**: NULL hint → round-robin. `InitEdt` is *not* pinned to rank 0.
- **`S`/`T`/`score` pointer-array DBs**: ordinary create, NULL hint →
  home = rank 0 (`mainEdt`).
- **`S`/`T`/score tiles**: created with NULL hint by their producers (a
  string or boundary tile by its fill task, an interior tile by its
  leaf), so each is homed where its producer ran; a tile's home is no
  function of its index and can differ between runs.
- **Tile events**: home = `index % nranks` (ARTS, ocr-vx), fixed at range
  creation.

Two base-tier adaptations must be disclosed with this row, because both
change base's multinode behaviour and neither is in the published source:
every tile is created by the task that produces it and named through a
labeled sticky event of its index — the published `InitEdt` creates every
tile, and each leaf writes its own through an RW `current` slot, which is
gone — so a tile's home follows its producer rather than its index; and
each leaf destroys its diagonal tile. Both are outside the guard, so the
hinted tier buys nothing from them: the hinted-vs-base delta is hints
alone. (The three pointer arrays the published `InitEdt` wrote are still
created and passed to it as before, and are neither written nor read.)

Consequence for base: every tile is written on the rank that creates it,
but EDT placement is an independent round-robin, so whether a leaf's three
RO neighbours and its strings are on its own rank is coincidence rather
than design.

## Placement (hinted)

The layer places by contiguous ROW BANDS. `bandOfRow(row) = (row *
nranks) / L` splits the `L` block rows in play into one contiguous band
per rank: bands differ in height by at most one row, every rank owns one,
and load imbalance is 1.0 at 2, 8, 16 and 32 ranks alike. (The previous
`ceil((L+1)/nranks)` height was an off-by-one over the grid side rather
than the used rows and stranded 1 of 16 and 6 of 32 ranks with no leaves
and no score-tile homes at `L = 128`; that is the defect this formula
replaces. The band count now follows the rank count by construction, so
the old bands-per-rank knob is gone.)

A block's West neighbour shares its row and therefore its rank; North/NW
share its band except on the `nranks-1` boundary rows, so the dominant
payload stays rank-local while the anti-diagonal frontier still reaches
every band once it is a band tall. The same band function is applied on
both sides: EDTs pin to their block's band rank, so each tile's producer
(its fill task or its leaf) creates the tile on that rank, and the labeled
index is re-encoded as `band(row) + nranks*t` so the label-derived
round-robin home of each tile's event lands on the same rank where a range
homes by index (above) — pure index arithmetic, agreed by every producer
and consumer without communication.
`S` string tiles are band-steered the same way (their fill task's hint and
`sIdx`: row `i`'s tile is read only by block-row `i+1`'s band); `T` tiles
deliberately stay round-robin by index (their fill task's hint and their
event's home), since a column's readers span every band.

The guard covers, in full: the two helpers `bandOfRow`/`rankHint`; the
index re-encodings `scoreIdx` and `sIdx`; the EDT hints `blockRowHint`
(band of a block row) and `tileRankHint` (a `T` tile's own round-robin
home); the four recursive `recLCSEdt` creates, hinted to the band of their
quadrant's first block row so a quadrant creates its leaves and registers
their dependences locally; every fill child, hinted to the rank of the
tile it creates; and the two `ocrGuidRangeCreate` sizes, multiplied by
`nranks` because the `band + nranks*t` encoding needs that much index
space (an allocation-size argument, not a hint: it changes no DB, EDT or
event count, and a range reservation is O(1)). No control flow, work
partition, DB/EDT/event count or wiring differs between the tiers.

At one node the layer is the exact identity (`nranks = 1` makes both
re-encodings the identity and every hint rank 0), so the one-node cell is
a valid anchor for both tiers by construction.

## Validity shape (measured, 15w+1p x 1/2/4/8 nodes, `32768 1024`)

**Measured before the 2026-09-03 band fix and conformance change**, and
on the labeled program whose `InitEdt` created every tile — the band
arithmetic, the shutdown split, the checksum and the producer-created
tiles all postdate these cells. At 1/2/4/8 ranks both maps cover every
rank, but they assign different rows to them (old boundaries every `ceil(129/8) = 17`
rows, new every 16), so the cells below are indicative of the *shape* only
— they are not a like-for-like locality reference for the new map. The 16-
and 32-rank cells the fix targets were never measured at all with the old
map's stranded ranks; the node-count trend is what a fresh campaign sweep
re-measures.

| arm | base 1n/2n/4n/8n | hinted 1n/2n/4n/8n |
|---|---|---|
| val_retain_owner | 1.09 / 2.80 / 3.30 / 3.36 | 1.08 / 1.66 / 2.06 / 2.34 |
| inv_retain_owner | 1.02 / 2.99 / 3.60 / 3.81 | 1.02 / 1.69 / 2.04 / 2.40 |
| excl_retain_owner | 1.02 / 7.76 / 6.70 / 5.86 | 1.01 / 1.79 / 2.29 / 2.64 |

At the calibrated workload (`131072 1024`, `L = 128`) the same sweep, on
the same labeled program, read base 41.6 / 57.6 / 52.7 / 40.7 s and hinted
41.7 / 45.2 / 31.4 / 19.0 s across 1/2/4/8 nodes: base never beat its own
single node, while the band placement scaled inside the wiring cap — 2.2x
at 8 nodes against the `(4/3)^7 = 7.5x` span bound. Lowering the cap
showed the plateau directly: the same `N` at `base 8192` (`L = 16`, cap
3.16x) ran 36.0 / 40.8 / 36.5 / 34.9 s — eight nodes bought nothing. No
placement can move that cap: the
recursion's quadrant gating is the only ordering (the leaf's datablock
dependences are satisfied before the leaf exists), so the span caps
speedup at `(4/3)^d`.
On one node at fixed work (`131072 1024`), measured on the labeled
program, 1..8 workers gave 47.4 / 29.1 / 24.2 / 16.5 / 16.0 / 14.9 / 14.2 /
13.6 s — saturation at ~3.5x — and 112 workers ran no faster than 12. (Two
single-node numbers for that workload are on record, both measured on the
labeled program, 41.6 s at 15 workers and 15.5 s at 112; they are
different geometries, and one interleaved re-measurement should settle
which the sizing argument uses.) Memory was the second wall of the labeled
program on xsocr, where a labeled create allocates its block at the
reserving rank, and on ARTS before a non-acquiring create's payload was
placed with its first user (c7f182ad): its creation phase materialized the
whole `(N+1)²`-cell table before the first leaf ran, so the destroy-at-diag
only trimmed the tail, never the peak. Now that each tile is created by its
producer, no runtime materializes the table at creation; what is resident
is the strings, the boundary, the last row and column and the computed
tiles not yet destroyed. The
span wall is the 2016 source's own structure; the answer to it is the
restructured version (`LCS_wavefront`).

## Sizing

`N/base` must be a power of two (now enforced at startup). Given that,
`base` trades tile *count* (`L²`) against tile *size* (`base²` cells) at
**fixed total score memory** — the `(N+1)²`-cell table's size depends only
on `N` (`≈4·N²` bytes, created over the run). The labeled program
materialized all of it before the first leaf on xsocr, and on ARTS before
c7f182ad, which made `N` the binding constraint this variant had and the
other two do not; each tile is now created by its producer, and each tile
that has a south-east neighbour is destroyed by that neighbour's leaf, so
the resident payload is the strings, the boundary, the last row and column
and the computed tiles not yet destroyed:

- At the calibrated `131072 1024` the table is `4·(N+1)²` ≈ **68.7 GB**,
  against the 190 GB one-node budget the labeled program was sized to (on
  ARTS, multinode, its labeled homes split it `1/nranks`, so the one-node
  cell was the memory-binding one).
- Width is short, structurally: peak `2^d` = 128 and time-average
  `(4/3)^d` = 7.5 against 3456 workers at the widest geometry. `(4/3)^d ≥
  3456` needs `d ≥ 29`, and even `L ≥ 3456` at this `N` needs `base = 32`
  (16.8 M tiny DBs) with the span cap unmoved. **This row is the
  wiring-capped anti-scaling exhibit; every scaling claim for this
  application comes from `LCS_wavefront`.**
- Shrinking `base` (holding `N` fixed) grows `L` — more tiles, smaller
  per-tile compute and payload — without changing total score memory.

The calibrated arguments stay `131072 1024`, set by the labeled
program's memory and wall-time argument above, and the pinned scalar is
re-derived at them — the row now
votes on `LCS checksum:` (a digest of the final tile, equal to
`LCS_wavefront`'s at the same arguments) instead of the answer cell, with
`expect_args` equal to the campaign `args` (the old three-element list never
matched, and its third element would now be rejected by the argument
validation).
