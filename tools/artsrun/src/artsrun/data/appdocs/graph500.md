# graph500

*A 2D-decomposed, level-synchronized BFS over a randomly generated graph —
Graph500-style Kernel 1 (graph construction) + Kernel 2 (single-root search).*
Source: `third_party/ocr-apps/apps/graph500/graph500.c` (~2090 lines).

## Overview

Builds a graph of `SIZE = 2^SCALE` vertices and `EDGE_SIZE = SIZE·EDGEFACTOR`
random undirected edges, partitions vertices/edges across an `R×C` logical
worker grid, then runs a single level-synchronized BFS from a fixed root,
timing the search phase and printing `[kernel1 time]`, `[kernel2 time]`,
`nodes`/`edges`, `BFS_DIGEST` and `MTEPS`. The harness build (`EXTRA_DEFINES
NO_FILES NO_MAP` in `benchmarks/apps/CMakeLists.txt`'s `graph500` target) compiles the *in-EDT,
PRNG-generated* graph path and the *pre-created EDT lattice* addressing path —
the file-I/O graph generator (the only `srand`/`rand` site in the file) and the
labeled-GUID addressing scheme both exist in the source but are dead code in
this build. Edges are drawn `source = xorshift64star(seed) % SIZE`,
`destination = xorshift64star(seed) % SIZE` from a fixed seed — a
uniform-random (Erdős–Rényi-style) generator, not Graph500's Kronecker/RMAT
model; vertices are 64-bit, which is the one axis on which this port does meet
the specification. The program stresses task-creation *volume* (hundreds of
thousands of EDTs from a single-threaded setup phase) and cross-worker *data
movement* (the row-then-column BFS-frontier scatter each level); the per-EDT
compute is light (array scans, a handful of comparisons).

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `SCALE` | `SIZE = 2^SCALE` vertices | required (usage message if `argc<5`) | ✓ parsed via `ocrGetArgv` in `mainEdt`, packed into `paramDBK` (`DB_MODE_CONST`), read by every EDT kind — multinode-safe |
| `argv[2]` = `EDGEFACTOR` | `EDGE_SIZE = SIZE·EDGEFACTOR`; avg vertex degree ≈ `2·EDGEFACTOR` | required | ✓ same path as `SCALE` |
| `argv[3]` = `R` | worker-grid rows; `SIZE` must be divisible by `R` | required | ✓ same path; also fixes `W = R·C` |
| `argv[4]` = `C` | worker-grid columns; `SIZE` must be divisible by `C` and by `R·C` | required | ✓ same path |
| `ROOT` (BFS root vertex) | fixed | `4` | ✗ compile-time constant (`graph500.c:1907`), never read from argv |
| `NUMBER_OF_SEARCH` | # of BFS searches (Kernel‑2 repeats) | `1` | ✗ hardcoded (`graph500.c:1830`); the `NO_MAP` build additionally `assert(0)`s if a second search is ever chained — the multi-search wiring (`startMap`, per-search finish slots) is unreachable dead code in this build |
| `SEED` (graph PRNG seed) | fixed | `123456789` | ✗ hardcoded (`graph500.c:1978`); a pure function of (seed, edge index), so the graph is identical at every node count and in every runtime |
| `MAX_LEVEL` (BFS depth capacity) | fixed | `10` | ✗ compile-time (`graph500.c:118`); a **capacity, not a tuning knob** — it sizes the pre-created lattice (`3·MAX_LEVEL·W` EDTs, and the `constguidDBK` that indexes them), and a graph whose BFS reaches it aborts with a message naming the cap (`checkLevel`, `graph500.c:510-516`) |
| `NO_MAP`, `NO_FILES` | addressing scheme / graph-source switches | both defined | build-fixed by `benchmarks/apps/CMakeLists.txt`'s `graph500` target, not app CLI |
| `NO_AFFINITIES` | disables the app's own EDT affinity hints | not defined (hints ON) | compile-time only |
| `VALIDATION_MODE` (0–3) | depth of result checking | `0` (lightweight per-worker visited sum + digest, no full-graph reconstruction) | compile-time only — see Correctness |

`PARAMDBK_MODE`/`ARRAYDBK_MODE` (both `DB_MODE_CONST`, i.e. RO) and
`EDGE_MODE_LIST` (edge-list vs. matrix storage — only the list mode is
implemented) are further compile-time knobs with no CLI surface.

**Argument validation is loud and survives a Release build.** The four
arguments are converted with `strtol` and rejected unless the whole string is a
positive decimal integer that the conversion could represent
(`parseCountArg`, `graph500.c:1786-1802`); `mainEdt` then rejects `SCALE > 62`
(`graph500.c:1815-1828`), a grid that does not divide the vertex count
(`SIZE%R`, `SIZE%C`, `SIZE%(R·C)`, `graph500.c:1836-1844`), arguments whose
derived extents `SIZE·EDGEFACTOR` or `(SIZE/C)·(SIZE/R)` would wrap 64 bits
(`graph500.c:1845-1854`), and a root outside the graph
(`graph500.c:1907-1912`) — each with a message naming the offending value and
`ocrAbort`. The divisibility and root checks were plain `assert`s, which
`-DNDEBUG` removes from the measurement build: an illegal `(R, C)` then made
the vertex-to-worker map many-to-one and silently lost vertices instead of
stopping.

## Structure

Let `W = R·C` (logical worker count) and `K` = the number of BFS levels for
which `distribute`/`search`/`apply` actually run — equivalently, the number
of `<level>: <SUM_COUNT>` lines `applyEdt` prints on worker 0
(`graph500.c:1411`), counted from level 0 through the first level whose
`SUM_COUNT` prints `0` (the level-synchronized grid still runs a full
distribute→search→apply pass for that terminal, all-empty level before the
grid stops; `1 ≤ K ≤ MAX_LEVEL=10`, data-dependent — see below).
`sizeof(vertexType) = sizeof(ocrGuid_t) = 8 B`, `sizeof(edge) = 16 B`,
`sizeof(vInfo) = 16 B`, `sizeof(evalData) = 64 B`.

| object | count | notes |
|--------|-------|-------|
| EDT templates | 10, each created+destroyed exactly once | distribute/search/apply/load/finish/stop/shutdown/start/create/finalShutdown |
| EDTs total | **`32W + 6`** | `30W`: `mainEdt` pre-creates distribute+search+apply for *all* `MAX_LEVEL=10` levels × `W` workers up front, regardless of the graph's true BFS depth; `+W` `createEdt`, `+W` `loadEdt`; `+6` singletons — `mainEdt` itself plus start/finish/stop/shutdown/finalShutdown. `mainEdt`'s own creation is a genuine `NUM_EDT_CREATE` (the OCR shim's runtime-created `main_edt` builds the argv DB, then `arts_edt_create`s the `mainEdtTrampoline` that runs this app's `mainEdt`), distinct from the fixed runtime baseline of `main_edt` + the argv DB (`+1 EDT`/`+1 DB` on every app, not counted here) |
| Events | **`W + 14`** | `W` per-worker STICKY `dataEVT` + 10 pre-created per-level LATCH `nextEVT` + 1 ONCE `startEVT` + 1 LATCH `loadEndEVT`, plus 2 shim-materialized output events (`stopEdt`'s and `shutDownEdt`'s non-NULL `outputEvent`); no `EDT_PROP_FINISH` EDTs exist in this app |
| DBs | **`8 + 6W + K·W·(R+5)`** | 5 one-time setup — `paramDBK`/`constguidDBK`/`timeDBK` plus `mainEdt`'s own `LOCAL_VAR_ARRAY` `affinities`/`hints` — + 2 one-time `startEdt` `LOCAL_VAR_ARRAY` `affinities`/`hints` + 1 one-time `eTimeDBK` (`stopEdt`) + `5W` per-worker setup (`createEdt`'s 4 + `loadEdt`'s `arrayDBK`) + `W` `cVisitedDBK` (terminal level only, 2 `u64`: the worker's visited count and its digest term) + `K` executed levels × `W` workers × [2 in `applyEdt` (`toRunDBK` + `LOCAL_VAR_ARRAY` `toRunBool`) + (`R`+3) in `searchEdt` (`R` `toRunxDBK` + `LOCAL_VAR_ARRAY` `destVertices`/`counts`/`positions`)] |

**`LOCAL_VAR_ARRAY` is a hidden `ocrDbCreate`.** The app's own
`LOCAL_VAR_ARRAY(TYPE,NAME,SIZE)` macro (`graph500.c:78`) expands to an
`ocrDbCreate` (plus a `_DBK` guid) whenever `USING_DATABLOCKS` is defined —
which it unconditionally is (`graph500.c:59`) — so every "local array" the
source declares through it is actually a datablock, invisible to a reading
that only greps for literal `ocrDbCreate` call sites. Eight of its ten use
sites are live in this build: `mainEdt`'s and `startEdt`'s
`affinities`/`hints` (`graph500.c:1961,1963` and `964,966` — one-time each,
sized to the rank/PD count, not scaled by `W`/`K`), and, on the per-level
hot path, `searchEdt`'s `destVertices`/`counts`/`positions`
(`graph500.c:1156,1158,1159`, unconditional — one triple per `searchEdt`
call) and `applyEdt`'s `toRunBool` (`graph500.c:1301`, inside the
`TORUN_DESTROY_MODE` branch — one per `applyEdt` call). `TORUN_DESTROY_MODE`
is `#define`d unconditionally in the source (`graph500.c:206`) — not gated
by the harness's `EXTRA_DEFINES NO_FILES NO_MAP` — so that branch, and its
per-invocation `toRunDBK` create, is the one that actually runs;
`createEdt`'s `toRun0DBK`/`toRun1DBK` are correspondingly the tiny (2- and
1-`vertexType`) `TORUN_DESTROY_MODE`-sized placeholders, not the
`vSIZE`-sized buffers the other branch would size them as. The remaining two
sites (`applyEdt`'s non-`TORUN_DESTROY_MODE` `toRunBool` at
`graph500.c:1368`; `finishEdt`'s `visited` at `graph500.c:1562`, gated on
`VALIDATION_MODE>=2`) are dead in this build.

**EDT and Event totals are exact, closed-form and data-independent** — the
`NO_MAP` build eagerly instantiates the full `MAX_LEVEL`-deep EDT lattice, so
the counts don't depend on how far the BFS actually gets (levels beyond the
true depth simply never receive their dependences and never fire). **DB
totals are not** — they scale with `K`, which is a property of the generated
graph, not a formula of the args. `K` *is* exactly reproducible run-to-run
(the generator is a fixed-seed PRNG, no wall-clock/PID entropy), but it has no
closed form; `K` must be read from the run's own frontier prints (the
`<level>: <SUM_COUNT>` stdout lines above), then treated as "measure once, same
every time" for that argument set.

Worked numbers at the campaign geometry (`20 16 128 128` → `W = 16,384`,
`R = C = 128`, `vSIZE = SIZE/W`): EDTs = `32·16384+6 = 524,294`; Events =
`16384+14 = 16,398`; DBs = `8 + 6·16384 + K·16384·133 = 98,312 + 2,179,072·K`,
where `K` is read directly from that run's own frontier prints (at average
degree `2·EDGEFACTOR = 32` a random graph of these sizes has `K` on the order
of 5–7, but that is only a pre-run estimate — the exact value comes from the
run's own output, not a formula). Dominant persistent memory is the per-worker
edge lists (`arrayDBK`, total ≈ `32·EDGE_SIZE + 8W` B) and the visited arrays
(`visitedDBK`, total ≈ `16·SIZE` B — independent of `W`, since `W·vSIZE = SIZE`
always); `constguidDBK` is `(17 + 3·MAX_LEVEL·W + W + MAX_LEVEL)·8` B ≈ 4.06 MB
at `W = 16,384`, and every EDT of every kind acquires it `DB_MODE_CONST` on
every invocation. Per-level `toRun`/`toRunx`/`toRunBool`/`destVertices`/
`counts`/`positions` traffic is transient (created and destroyed each level),
bounded by O(`SIZE`) live data per level.

Counter cross-check: verified (1 node, args `6 8 1 1` (`W=1,R=1`) vs
`6 8 2 2` (`W=4,R=2`); same seed ⇒ same generated graph ⇒ `K=5` executed
levels in both runs, per the run's own frontier prints `0:1, 1:8, 2:51, 3:4,
4:0`). Raw counters: NUM_EDT_CREATE = 39/135, NUM_EVENT_CREATE = 15/18,
NUM_DB_CREATE = 45/173. Subtracting the runtime's constant baseline (+1
EDT, +1 DB, +0 EVT per run — `main_edt` plus the argv DB, common to every
app) gives the app-only totals `32W+6` = 38/134, `W+14` = 15/18, and
`8+6W+K·W·(R+5)` at `K=5` = 44/172 — all three formulas exact at both
points, constants and `K`/`R` coefficients included, not just the deltas
(ΔEDT=96, ΔEVT=3, ΔDB=128).

## Wiring

Setup (`mainEdt`, rank 0) creates 3 globally-shared DBs — `paramDBK` (56 B,
the 4 CLI args + derived sizes + `SEED`), `constguidDBK` (template/GUID
lookup table), `timeDBK` (kernel-1 timer) — all `DB_MODE_CONST`
(RO), read by essentially every EDT the run creates; this is a real
**contention point**, not by write conflict (RO has no exclusive lock) but
by request volume against a single rank-0 home. Each worker `w`'s `loadEdt`
builds its own edge-list `arrayDBK` (RO, `DB_MODE_CONST`) and feeds it
through a per-worker STICKY `dataEVT`; `createEdt` builds a per-worker
`visitedDBK` (`vSIZE` `vInfo`s) that stays `DB_MODE_EW` (ARTS RW) for the
worker's *entire* run, threaded serially level-to-level through that
worker's own `applyEdt` chain — never touched by any other worker, so this
RW traffic never crosses ranks. The actual cross-worker channel is the BFS
frontier: each level, `distributeEdt(w)` row-scatters its frontier DB (RO)
to the `C` `searchEdt`s in its row; each `searchEdt(w)` then column-scatters
`R` freshly-built `toRunxDBK`s (RO) to the `R` `applyEdt`s in its column.
This two-hop row-then-column pattern is O(`W·(R+C)`) fan-out per level, not
the O(`W²`) of a full all-to-all. **The row broadcast is this row's coherence
exhibit**: one frontier DB per (worker, level) is acquired RO by `C` searches
that, under the placement below, span every rank — several of them on the same
remote rank concurrently, which is exactly the requester-side combining window
the `ocr_val_*_nocomb` ablation twins remove. `applyEdt` folds its column's `R`
inputs into a new frontier (`toRunDBK`) and either spawns level `L+1`'s
distribute/search/apply triple (already pre-created — just wires slots) or,
once its frontier is empty, wires its per-worker result summary and
`toRunGuidDBK` into the single `finishEdt` and a `NULL`-mode signal into the
single `stopEdt`.

## Flow

`mainEdt` (rank 0, single-threaded): parses and validates argv, computes
`SIZE`/`EDGE_SIZE`, creates the 10 templates, then issues the entire
`32W`-EDT creation burst (`createEdt`×`W`, `loadEdt`×`W`, the `3·MAX_LEVEL·W`
pre-created distribute/search/apply triple) plus `W+13` event creates — an
O(`W`) serial preamble before any parallel work exists, independent of graph
size, and *every one of those creates carries an affinity hint naming a
specific rank*, so at `P` ranks a `(P-1)/P` fraction of them are remote
creation messages issued from one thread. `W` `loadEdt`s then run in parallel,
each *redundantly* replaying the full `EDGE_SIZE`-long PRNG edge stream twice
(count + fill) to extract its own share — O(`W·EDGE_SIZE`) total work for an
O(`EDGE_SIZE`) graph, and the phase that owns most of the measured window at
the campaign width. A `loadEndEVT` (W-way LATCH) barriers Kernel 1 against
Kernel 2's `startEdt`, which prints `[kernel1 time]`. The BFS proper is
**level-synchronized**: each level is a 3-stage pipeline (distribute → search →
apply) of width `O(W)` per stage; a level cannot start until the previous
level's `applyEdt`s have all fired (no cross-level pipelining). The run ends in
two rank-0 global joins: `finishEdt` (`depc = 2W+3` — every worker's summary
and toRun-guid DB, and `3W` `ocrDbDestroy`s of remotely homed DBs) and
`stopEdt` (`depc = W+1` — every worker's termination signal plus the kernel-2
timer, printing `[kernel2 time]`), both single EDTs that cannot fire until all
`W` workers have reached their terminal level (the level-sync design keeps `K`
uniform across the grid). `shutDownEdt` then prints the result scalars,
destroys the 10 templates, and a dedicated `finalShutdownEdt` (chained off its
output event) calls `ocrShutdown()` — deliberately split out so shutdown never
truncates `shutDownEdt`'s own dependence-release work out of the measured run.

## Placement (base)

The affinity usage below is the app's own, unconditional mechanism (gated only
by its own `NO_AFFINITIES`, which is not defined in this build) — it is what
the program ships with, not a harness addition.

- **Data-plane EDTs** (`load`/`create`/`distribute`/`search`/`apply`, all
  `W`-scaled): each worker `w` gets an explicit `OCR_HINT_EDT_AFFINITY` hint
  computed once by `mainEdt`/`startEdt` as `w % rank_count`, held constant
  across all 10 pre-created levels for that worker (so one worker's whole
  per-level chain stays on one rank for the run). Balance is exact: `W/P`
  workers per rank at every rank count of the sweep.
- **What that map actually partitions.** `w = row·C + col`, and the program
  requires `R·C | 2^SCALE`, so `R` and `C` are powers of two and `P | C` at
  every power-of-two rank count. Then `w % P == col % P`: the shipped map is a
  partition **by whole columns**, so a worker's `C-1` column partners are
  *already* co-located and only the `R` axis is split. (An earlier reading of
  this map as "scatters every row and column partner" is wrong on the column
  axis, and the axes are not symmetric: the column axis carries `R` separate
  `toRunx` transfers per search per level, the row axis one broadcast frontier
  DB — so the shipped map already keeps the heavier axis local.)
- **Control-plane EDTs** (`start`/`finish`/`stop`/`shutdown`/`finalShutdown`):
  each passes `local_hint = ocrAffinityGetCurrent()` of its *creator* — since
  `mainEdt` runs on rank 0 and `startEdt` (created by `mainEdt` with that same
  hint) re-captures its own (rank-0) affinity for what it creates, this whole
  chain is pinned to rank 0. It is five singleton EDTs, not a work-carrying
  subtree.
- **DBs**: every `ocrDbCreate` in this app passes `NULL_HINT` → home =
  creating rank. Combined with the EDT pinning above, each worker's own DBs
  (`visitedDBK`, `arrayDBK`, `toRunGuidDBK`, `toRun0/1DBK`) co-locate with
  that worker's pinned rank by construction; the 3 shared control DBs
  (`paramDBK`, `constguidDBK`, `timeDBK`) home on rank 0 and are read
  remotely by every other rank's workers.

## Placement (hinted)

The layer is present in the source and still compiled, but the tier is not in
the roster. The source carries one `OCR_APP_OPTIMIZED_PLACEMENT` arm
(`graph500.c:552-591`, with the shipped map in the `#else` at
`graph500.c:592-596`): a `gridTiles()` that factors the rank count into
`pr × pc` maximising `R/pr + C/pc`, and a `getAffinityIndex()` that returns the
tile index `(w/C)/(R/pr)·pc + (w%C)/(C/pc)`. It is hint-only — same signature,
called only as an index into the hint array, no control-flow, partition, DB or
wiring difference — and it covers all ranks with exact balance.

**It cannot differ from base at this row's geometry.** `gridTiles` requires
`pr | R` and `P/pr | C`; with `R` and `C` powers of two it therefore succeeds
only for power-of-two `P`, and on a **square** grid the `pr = 1` and `pr = P`
factorisations score identically, so the strict `>` at `graph500.c:582` keeps
the first — `pr = 1, pc = P`, i.e. `place = col/(C/P)`: the same whole-column
partition as base, with the columns merely grouped differently. Balance and
per-axis local-partner counts are identical at every rank count (128×128:
(3 row, 127 col) local at 32 ranks under both maps). For non-power-of-two `P`
the function returns 0 and the code falls back to `w % P` literally. The two
maps *can* differ for `R < C`, which this row never uses.

The tier is therefore not registered (`hinted: false`), which keeps it out of
every roster on its own. `HINTED_PLACEMENT` stays on the `add_benchmark_app`
call so the guarded arm keeps compiling and is still checked by every build —
nothing uncompiled is left behind — and the `_hinted` binaries it produces are
simply never selected by a cell.

No better static map was found: the only alternative lever is
`pr = P, pc = 1`, which makes the row broadcast local but turns the `R`
disjoint `toRunx` slices remote — ≈ 2× more bytes at the campaign geometry —
and a square tiling is worse on both axes.

## Correctness

`VALIDATION_MODE` is `0` (`graph500.c:201`) in every tier and must stay there:
the `>= 2` branch replays the entire `EDGE_SIZE` edge stream twice, serially,
inside `finishEdt` — i.e. inside the measured span. What runs instead is a
cheap O(`vSIZE`) fold in each worker's terminal `applyEdt`, over the visited
array it already owns, producing two `u64`s (`cVisitedDBK`): the worker's
visited count, and a digest term. `finishEdt` sums both over the `W` workers —
an O(`W`) sweep — and `shutDownEdt` prints

```
nodes <visited count> edges <count·EDGEFACTOR> edge factor <f> root 4
BFS_DIGEST <u64>
MTEPS <f>
```

`BFS_DIGEST` is the row's voted scalar. It is `Σ mixVisited(vertex, level,
parent)` over the visited vertices, `mixVisited` being a splitmix64-style
avalanche of the triple (`graph500.c:599-615`); the fold is `+` over `u64`,
which is commutative and associative, so the value does not depend on the order
workers finish or on how the sum is distributed. It is therefore invariant
across node counts, coherence arms and reference runtimes, and unlike the
`nodes` count — which at average degree `2·EDGEFACTOR = 32` is just `2^SCALE`,
an echo of `argv[1]` — it fails if any vertex is reached at the wrong level or
given the wrong BFS parent. `nodes` is kept as an extra scalar.

The digest is a function of all four arguments, the parent included: parent
selection is deterministic (the searches merge their sorted frontiers in
ascending vertex order, so the largest reaching source wins within a level, and
`applyEdt` takes its `R` inputs in fixed slot order) but it depends on the
`R × C` decomposition. Any future row that recomputes the same BFS with a
different decomposition must pin its own value, or digest `(vertex, level)`
only — that reduced digest *is* grid-invariant.

Model-derived reference values, to be confirmed by one run before they are
pinned (the frontier sequence in each is the app's own `<level>: <SUM_COUNT>`
print, and the `6 8 2 2` line matches the counter cross-check run above):

| args | frontier prints | `nodes` | `BFS_DIGEST` |
|---|---|---|---|
| `6 8 2 2` | `0: 1, 1: 8, 2: 51, 3: 4, 4: 0` | 64 | 17423262753130883682 |
| `6 8 1 1` | `0: 1, 1: 8, 2: 51, 3: 4, 4: 0` | 64 | 15352204891860835609 |
| `10 16 8 8` | `0: 1, 1: 33, 2: 638, 3: 352, 4: 0` | 1024 | 903256210448785348 |
| `12 16 16 16` | `0: 1, 1: 26, 2: 783, 3: 3279, 4: 7, 5: 0` | 4096 | 14849956761108488830 |

## Sizing

`SCALE` is the size knob: it drives both memory and load-phase compute
(`SIZE`, `EDGE_SIZE` grow with it) and is the only handle the campaign moves;
the catalog runs `SCALE = 20`.
`EDGEFACTOR` is the density knob (`EDGE_SIZE = SIZE·EDGEFACTOR`), fixed at the
16 the official benchmark uses and the app's own README names — note the
shipped example argv in `Makefile.tg:31` says 8. `R×C` is a purely *logical*
decomposition width, independent of physical ARTS worker-thread count: raising
it increases `W` (hence EDT/Event/DB totals, the rank-0 preamble, the two
global joins and the size of `constguidDBK`) without changing the graph. Keep
`EDGEFACTOR` generous enough (≥ ~4–8) that `K` stays comfortably under the
hardcoded `MAX_LEVEL=10` cap for the chosen `SCALE`; `K` can't be predicted
statically, only observed from a run's own frontier prints, but a graph that
does reach the cap aborts there instead of wiring the wrong EDTs.

**Width.** The structure is spawn-and-join (level-synchronous bulk phases): `W`
`loadEdt`s joined by the `W`-way `loadEndEVT` latch, then per level `W`
distributes → `W` searches → `W` applies behind a per-level latch. So the
instantaneous width is `W = R·C`, and it is set by `R` and `C` alone. An
*integer multiple* of a target worker count is unreachable by construction —
`R·C | 2^SCALE` forces `W` to be a power of two — so the width is the nearest
power of two above the target. `R = C = 128` (`W = 16,384`) is 4.74× a
3456-worker envelope; the quantisation cost is one short wave (at most 28 of
108 threads idle in the last of 4.74 waves, ~5% of a phase). Keep the grid
square: that is what makes the two placement maps identical in balance and in
per-axis locality, and it balances the two communication axes.

**Memory.** Peak resident bytes across all ranks:

```
32·SIZE·EDGEFACTOR      edge lists (2 directed entries of 16 B per generated edge)
+ 16·SIZE               visited arrays
+ 8·(17+3·MAX_LEVEL·W+W+MAX_LEVEL)   constguidDBK, per rank holding a copy
+ ~24·271·MAX_LEVEL·W   pre-created dependence slots (5 + (C+4) + (R+6) per (w,level))
+ ≤ 32·SIZE·EDGEFACTOR  transient per-level toRun/toRunx, bounded by the edge total
```

i.e. ≈ `64·SIZE·EDGEFACTOR + 16·SIZE + 65,000·W` bytes to an order of
magnitude. At the campaign's `SCALE = 20`, `EDGEFACTOR = 16`, `W = 16,384`
that is ≈ 2–3 GB on one node — two orders of magnitude under a 190 GB
per-node budget, so `SCALE` is bounded by time, not by memory (each `+1` of
`SCALE` doubles both).

## Restructured tier

`graph500_dist` (catalog `restructured_as`) is the family's restructured pole: partitioned generation with a matrix jump into each slice of the xorshift stream, edges routed to their vertex owner, a sparse owner-computes frontier exchange of one block per node pair per level, and persistent per-node chains — the same instance, the same `BFS_DIGEST`, generation ~1000× cheaper and the BFS exchange proportional to the frontier.  See `graph500_dist.md`.
