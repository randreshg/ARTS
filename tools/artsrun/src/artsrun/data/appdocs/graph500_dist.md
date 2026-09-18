# graph500_dist

*The same Graph500-shaped instance, search and answer as `graph500`, with the
edge stream partitioned instead of replayed per worker, the adjacency owned
by the vertex it belongs to, a level's exchange reduced to one block per
ordered node pair carrying only the frontier, and the pre-created task
lattice replaced by a per-node persistent chain.*
Source: `third_party/ocr-apps/apps/graph500/dist/graph500_dist.c` (~1.1k
lines, one file).

## Overview

`graph500` builds a graph of `SIZE = 2^SCALE` vertices and
`EDGE_SIZE = SIZE·EDGEFACTOR` uniform-random edges from a fixed seed, spreads
them over an `R×C` logical worker grid, and runs one level-synchronous BFS
from vertex 4, printing `[kernel1 time]`, `[kernel2 time]`, the frontier
sizes, `nodes`/`edges`, `BFS_DIGEST` and `MTEPS`.  This row is its
restructured tier: **same generator, same seed, same root, same printed
lines, same digest**, and a different decomposition.

Three terms of the base row are what the rewrite addresses, and the base
appdoc measures all three:

1. **Construction is `W`-fold redundant.** Every one of the `W = R·C` logical
   workers replays the *whole* `EDGE_SIZE`-long stream twice — once to count
   its share, once to fill it — so kernel 1 does `W · 2 · EDGE_SIZE` stream
   steps for an `EDGE_SIZE`-edge graph.  It is embarrassingly parallel, so it
   scales; it is also the phase that owns the window, which is why the row's
   positive speed-up is a property of a redundancy rather than of the search.
2. **The level exchange is dense in a sparse quantity.** Per level a frontier
   is broadcast along each worker's row (`C` acquires) and the discovered
   slices are pushed along each worker's column (`R` fresh datablocks), one
   `ocrDbCreate` per (worker, level) *whether or not the slice is empty*:
   `W·(R+C)` datablock-carrying edges per level, independent of the frontier.
3. **The setup is a serial `32W`-create burst** from one thread, each create
   carrying an affinity naming a rank, so at `P` nodes a `(P−1)/P` fraction
   of them are remote creation messages issued from one place.

What this program keeps: the vertex-to-worker map, `vSIZE = SIZE/(R·C)`, the
per-worker vertex block, the level-synchronous discipline (a level cannot
start before every node has finished the one below), the argument contract
and every printed line.

## Parameters

Identical to the base row — `SCALE EDGEFACTOR R C`, all four required, all
four validated the same way (positive decimal integers; `SCALE ≤ 62`; `R`,
`C` and `R·C` must divide `2^SCALE`; the derived `SIZE·EDGEFACTOR` must not
wrap 64 bits; the root must lie inside the graph).  `ROOT = 4`,
`SEED = 123456789` and the single search are compile-time as they are there.

Two differences in what the arguments *mean*:

| | base | this row |
|---|---|---|
| `R` | worker-grid rows: fixes the edge-owner map, the row broadcast and the parent tie-break | fixes the parent tie-break only (see Correctness) |
| `C` | worker-grid columns: fixes the column push and the vertex blocks | fixes the vertex blocks |
| `W = R·C` | logical workers; the width of every bulk phase; the multiplier on construction work | logical workers; the width of a level and of construction — but not a multiplier on any work term |

One additional requirement this row states and the base does not: `R·C` must
be at least the policy-domain count, since a node owns a contiguous run of
workers.  A grid smaller than the machine is rejected with a message naming
both.

The depth capacity moves from 10 to 1024.  The base pre-creates a
`3·MAX_LEVEL·W` EDT lattice and aborts if the search is deeper than
`MAX_LEVEL = 10`; here a level is issued when it is reached and the only
bound left is the generation buffer a channel declares, `MAX_LEVELS = 1024`,
which costs one ring entry per level rather than `3W` task slots.  A search
that reaches it stops with a message naming the bound, as the base's does.

## Structure

Let `P` be the policy-domain count, `W = R·C`, `nw = W/P` (a node's worker
run, exact to a rounding), `K` the number of levels the search executes
(data-dependent, read from the run's own frontier prints), and

    nroute = min(ceil(128/P), nw)      routing tasks per (node, destination)
    nbuild = min(128, nw)              adjacency builders per node

Both are internal task counts, not knobs: they exist so that one task's share
of a one-shot phase stays bounded and so that no consumer holds a dependence
per producer.

| object | count | notes |
|--------|-------|-------|
| EDT templates | 11, each created and destroyed once, plus one per (node, level) for the packing task, whose dependence count is the level's own | |
| generator EDTs | **`W`** | one contiguous slice of the stream each |
| routing EDTs | **`P²·nroute`** | one per (source node, group, destination node) |
| adjacency EDTs | **`P·nbuild`** | each builds the adjacency and visit state of a contiguous worker run |
| setup EDTs | `4P + 4` | per node: init, channel init, construction hub, construction join; plus main, start, finish and the shutdown task |
| search EDTs | **`Σ_L active(L) + (K+1)·P + K·P`** | per level: one task per worker *addressed by the level* (never more, usually far fewer), one hub and one packing task per node.  `active(L) ≤ min(W, candidates)` |
| events | **`P²` channels + `P(P−1)` labeled sticky** | one channel per ordered node pair carries every generation of the run — the `nroute` of construction, then one per level; the sticky events are the publication of the channels' GUIDs |
| DBs | `W` slices + `P²·nroute` routed blocks + `P·nbuild` reports + `2W` (adjacency, visit state) + `3P` per-node setup + `Σ_L (active(L) + P)` + `P` results | |

At the local trend instance (`18 16 32 32` → `SIZE = 262,144`,
`EDGE_SIZE = 4,194,304`, `W = 1,024`, `vSIZE = 256`, `K = 7`), at `P = 8`
(`nroute = 16`, `nbuild = 128`):

| | base | this row |
|---|---|---|
| stream steps in construction | `W·2·EDGE_SIZE` = **8.59 × 10⁹** | `2·EDGE_SIZE` = **8.39 × 10⁶** (1,024×) |
| EDTs | 32,774 | **7,991** |
| events | 1,038 | **120** (`P²` channels + `P(P−1)` labeled) |
| DBs created | 271,368 | **10,431** (peak live 4,106) |
| datablock-carrying transfers, whole search | `K·W·(R+C)` = **458,752** | `K·P²` = **448** (1,024×) |
| construction transfers | 0 (every worker draws everything) | `P²·nroute` = 1,024 blocks, 134 MB in all |
| per-level exchange volume | `W·(R+C)` blocks whatever the frontier | `Σ_{u ∈ frontier} deg(u)` pairs, 16 B each |

(EDT, event and DB counts are exact object counts of this source driven
through a single-threaded emulation of the OCR surface it uses; they are
independent of the schedule.  At one node the same instance is 6,066 EDTs and
8,107 DBs.)

The five thin levels of this instance (frontier 1, 31, 984, 6,257, 0 — 2.8 %
of the work) cost the base row 71 % of its exchange and cost this row 2.8 %
of its exchange, which is the whole point of item 2 above.

## Wiring

**Construction** is a three-step pipeline per node, and nothing in it is
replicated.

- A **generator** takes a contiguous slice of the stream.  The state update of
  `xorshift64star` is linear over GF(2), so the state at the head of any slice
  is a matrix-vector product rather than a walk: each node builds a table of
  the update raised to `2^j` once (64 basis-image arrays, 32 KB, ~0.1 ms) and
  a generator reaches its slice with at most 64 vector applications.  The
  slice is drawn twice — the two passes a counting sort needs, sizing then
  placing — and comes out as one block bucketed by the node that owns each
  entry's source vertex.  Both directions of an undirected edge are emitted
  and a self-loop is skipped, exactly as in the base.
- A **routing** task takes what one group of slices drew for one destination
  node and republishes it as a single block grouped by destination worker.
  This step exists only to make the message count a per-node-pair constant:
  without it a node would receive one message per slice per source.
- An **adjacency** task takes the routed blocks (one contiguous run per worker
  in each) and builds, per worker, a CSR block (`vSIZE+1` offsets and the
  neighbour list) and the worker's visit state.  Both are created by the node
  that will read them for the rest of the run.

**The search** is a chain per node: `hub → workers → pack → hub`.

- The **hub** reads the block each node addressed to it, sums the header
  counts (that sum *is* the previous level's frontier size, so node 0 prints
  the base's `<level>: <count>` line from it), issues one worker task per
  worker the level actually addressed, and hands the level to the packing
  task.  A frontier of one vertex issues one task, not `W`.
- A **worker** folds the candidates addressed to it, keeps those that reach a
  vertex it has not yet visited, writes their level and parent, adds their
  digest terms, and walks the adjacency of exactly those.  Its output is one
  block bucketed by destination node, with the count and digest delta in the
  header.  The fold's scratch is cleared as it is read back, so a level never
  pays for the worker's vertex extent — only for its candidates.
- The **pack** sums the node's deltas into the chain state and turns the whole
  level into `P` blocks, one per destination node, each grouped by destination
  worker.  It satisfies one persistent channel event per destination and
  creates the next hub.

**What the aggregation costs, and why it is one task.**  Turning a node's
whole level into one block per destination is what makes the message count a
function of the node count rather than of the grid, and it is the design's
one single-threaded term: two passes (size, then place) over that node's own
candidate volume, `O(volume/P)` per node per level, `O(volume/P)` for the
whole run.  Splitting it further is possible — one packing task per
destination, or per destination-worker group — but each split multiplies the
*consumer-side* dependence count by the split factor: an aggregator must hold
a dependence on every producer it merges, so `g` aggregators cost
`g · active(L)` registrations per node per level against `active(L)` today.
At the local trend instance that trade is a loss at every `g > 1` (about
`8.4 × 10⁶` candidate-passes for the whole run against `1024 · g`
registrations per level at roughly a microsecond each), so the single
aggregator stands.  The arithmetic of the map it applies per candidate is
shifts and masks, not divisions: `R`, `C` and `vSIZE` are powers of two
because the argument contract makes them divide `2^SCALE`, and the phases
that touch every edge and every candidate take the map in that form.

**Channels, not a lattice.**  Each node creates one `OCR_EVENT_CHANNEL_T` per
destination and publishes its GUID through a labeled sticky event, which is
the only way a consumer can name an event its producer created.  A channel
carries every generation of the run in order, so the whole cross-node
rendezvous is `P²` events created once, against the base's `W` sticky events
and `3·MAX_LEVEL·W` pre-created EDT slots.

**No shared parameter block.**  Every task is handed the instance by value in
its parameter words.  The base's `paramDBK`/`constguidDBK` are acquired
`DB_MODE_CONST` by every EDT of every kind on every invocation from a single
rank-0 home — a request-volume contention point its own appdoc names.  Here
there is no such block at all.

## Flow

`mainEdt` (node 0) validates argv, takes the construction clock, creates the
reporting chain (`start`, `finish`, shutdown) pinned to node 0, and forks one
`rank init` per node.  A node's init builds its chain state, its jump table,
its channels, its construction hub, its `P·nroute` routing tasks and its `nw`
generators, then hands the chain state to the channel-init task, which
records the incoming channels and registers the construction hub on them.
The construction hub reclaims the slices, issues the adjacency builders and
the construction join; the join records the built blocks in the chain state
and reports the node ready.  `start` prints `[kernel1 time]`, restarts the
clock, prints `START Kernel 2 (id=0)` and creates each node's first hub.

The search then runs the chain above.  The first round has no incoming
blocks: its frontier is the root, whose owner marks it at level 0 with itself
as parent and walks its adjacency.  A round whose incoming counts sum to zero
is the end: it reclaims the node's blocks and hands the node's `(count,
digest)` to `finish`, which sums the `P` results, stops the clock and prints
`[kernel2 time]` and the result lines.  Shutdown is a dedicated task on
`finish`'s output event, so the release of `finish`'s own dependences is not
truncated out of the measured run.

Because every node's hub sums the same `P` headers, termination is decided
identically and independently everywhere: the end of the search costs no
extra message.

## Placement

Placement is part of the design, so there is **no `_hinted` family**.

- Workers are cut into contiguous runs, one per node; a vertex belongs to the
  worker the base's map gives it, and therefore to that worker's node.
- Every EDT is created with an explicit affinity naming its node — but only
  ever by a task on that node, except for the `P` per-node inits and the `P`
  first hubs, which node 0 creates.  There is no create burst: the `32W`
  hinted creates of the base setup and the `W` `createEdt` creates become `W`
  generators issued `nw` at a time by each node for itself.
- Every DB is created with `NULL_HINT`, so it homes on the node that created
  it: a worker's adjacency and visit state are built by its own node, and the
  only blocks that cross are the `P` per level and the `P·nroute` of
  construction, which cross once each and are reclaimed.
- A worker's visit state is threaded level to level through that worker's own
  tasks and is never touched by another worker, so it never crosses.

## Correctness

The row's voted scalar is `BFS_DIGEST`, and it is the base's value at the
same arguments, not a re-pinned one.

`BFS_DIGEST` is `Σ mixVisited(vertex, level, parent)` over the reached
vertices, folded with `+` over `u64` — commutative and associative, so the
value does not depend on the order the nodes finish.  Two of the three terms
are decomposition-invariant on their face: the set of reached vertices, and
each one's level, which is its distance from the root.

The parent is the term that has to be argued.  In the grid program a
discovered vertex takes its parent from the first non-empty of `R` inputs,
input `i` carrying the largest frontier vertex `u` with `u mod R = i` that is
adjacent to it.  So:

> **parent(v) = the largest adjacent frontier vertex in the smallest
> non-empty residue class modulo `R`.**

That is a minimum under the total order `(u mod R) ascending, then u
descending`, and a minimum under a total order may be taken in any grouping.
This program therefore evaluates it once, at the owner of `v`, over every
candidate addressed to it — which is what makes the answer identical rather
than merely equivalent.  Note what the rule does *not* depend on: `C`, the
node count, the worker-to-node map, and the order candidates arrive in.

The rule reproduces the base's published reference points exactly, including
the pair that differs only in `R` — and the program's own digest is invariant
across node counts (checked at `P` = 1, 2, 3, 4, 8, 16):

| args | frontier prints | `nodes` | `BFS_DIGEST` |
|---|---|---|---|
| `6 8 1 1` | `0: 1, 1: 8, 2: 51, 3: 4, 4: 0` | 64 | 15352204891860835609 |
| `6 8 2 2` | `0: 1, 1: 8, 2: 51, 3: 4, 4: 0` | 64 | 17423262753130883682 |
| `10 16 8 8` | `0: 1, 1: 33, 2: 638, 3: 352, 4: 0` | 1024 | 903256210448785348 |
| `12 16 16 16` | `0: 1, 1: 26, 2: 783, 3: 3279, 4: 7, 5: 0` | 4096 | 14849956761108488830 |

The frontier print sequence is the base's too: a round reports the level
below it, because what arrives is what the previous round discovered.

`nodes` (the reached-vertex count) and `edges` (`nodes · EDGEFACTOR`) are
printed and extracted as the base prints them, and `MTEPS` is
`edges / [kernel2 time]` on the same definition.

## Sizing

`SCALE` is the size knob, as in the base, and `EDGEFACTOR = 16` is the
density the official benchmark fixes.  What changed is which term the size
buys:

- **Construction is no longer the window.**  It falls from `W·2·EDGE_SIZE`
  stream steps to `2·EDGE_SIZE` plus one exchange of `32·EDGE_SIZE` bytes.
  At the base row's calibrated width (`W = 16,384`) that is a 16,384× cut in
  the phase that owned its window, so **this row's anchor must be re-derived
  from scratch**; carrying the base's `20 16 128 128` over is only a
  placeholder.  Expect the search, not the construction, to set the window,
  and expect `SCALE` to have to rise by roughly two steps to reach the same
  wall — which is the usual shape of a `*_dist` row.
- **`R·C` is still the width** and still a power of two by the divisibility
  requirement, so an exact multiple of a worker envelope stays unreachable
  and the nearest power of two above it is the rule, exactly as for the base.
  Keep the grid square: `R` decides the parent rule and `C` the vertex
  blocks, and a square grid keeps `vSIZE` where the base's calibration put it.
- **Width now follows the frontier.**  A level issues one task per *addressed*
  worker, so a thin level issues few.  A size whose frontier never approaches
  `W` will report a width far below `R·C` — a real property of BFS, not a
  defect, but a reason to keep `SCALE` large enough that the fat levels are
  fat.

**Memory.** Peak resident bytes across all nodes:

```
16·2·EDGE_SIZE          transient: the drawn slices
+ 16·2·EDGE_SIZE        transient: the routed blocks (live at the same time)
+ 8·2·EDGE_SIZE + 8·SIZE   adjacency (CSR: neighbours plus per-vertex offsets)
+ 32·SIZE               visit state (level, parent, fold scratch, touch list)
+ O(16 · level volume)  transient: a level's candidate and exchange blocks
```

The construction transients are reclaimed the moment the adjacency they feed
is built, so steady state through the search is
`8·2·EDGE_SIZE + 40·SIZE` plus the level's own traffic — about half the base
row's edge storage (the base keeps 16 B per directed entry against 8 B here)
against a slightly larger per-vertex state.  As with the base, `SCALE` is
bounded by time and not by memory.

## Family shape

`calibration pending`.  One measured point exists, and it is a one-node
identity check, not a scaling number: at the local trend instance
`18 16 32 32` on one ferrari node (14 workers, `arts_inv_wb`) the two tiers
print the same `BFS_DIGEST` (6239203375441639461), the same `nodes`/`edges`
and the same frontier sequence, with

| | `[kernel1 time]` | `[kernel2 time]` |
|---|---|---|
| `graph500` | 9.673 s | 0.129 s |
| `graph500_dist` | **0.092 s** | 0.120 s |

— construction down **106×**, which is the `W = 1024`-fold redundancy removed,
and the search level with the base at one node, where the per-node
aggregation is the whole of it and no wire exists.  Nothing here is a scaling
claim.  The row exists to move the split the base row prints
per cell: `kernel1_s` should collapse and stop carrying the curve, and
`kernel2_s` — which anti-scales 6.6× from 1 to 8 nodes in the base, its BFS
throughput falling from 25.2 to 2.5 MTEPS the moment a wire exists — should
become the part that scales.  Both are extracted per cell for exactly this
comparison, and the base row's own numbers are the control.
