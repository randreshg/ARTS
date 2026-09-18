# quicksort

*In-place recursive quicksort over EDTs — every partition step is a task,
and every task shares the same one datablock.*
Source: `third_party/ocr-apps/apps/quicksort/ocr/quicksort.c` (~390
lines). A restructured twin, `quicksort_dist.c` (sample-splitter/PSRS-style
distributed sort whose exchange is aggregated per place and whose buckets sort
locally), is registered beside it as this row's `restructured` version.

## Overview

Sorts a generated array of `u64`s **in place**, entirely within a single shared
datablock: each `qsortTask` partitions its `[low,high]` range around a
pseudo-randomly indexed pivot (Hoare partition), then creates a child
`qsortTask` for each non-empty side — both wired to the *same* array DB via a
fresh ONCE event. Below a fixed size the range is finished with a simple
selection sort. Recursion is bracketed in an `EDT_PROP_FINISH` scope so a
single output event fires once the entire tree (including the nested finish
scopes each child opens) has drained; `finishTask` then prints the first
`min(arraySize, 30)` sorted elements, the whole-array check, and shuts down.

The array is produced ahead of the recursion by `fillTasks` `fillTask` EDTs,
each owning a contiguous index range. The value at index `i` is
`genValue(i, range)` — a splitmix64-style mixing of `i+1` reduced modulo
`range` — so it is a pure function of the index, carries no state between
calls, and does not depend on how the array was divided among its producers.
The same function, byte for byte, generates the restructured row's input, so
the two programs sort the same multiset at the same `(arraySize, range)` and
print the same `sum`. The whole run — including the exact shape of the
recursion tree — is deterministic for a given `(arraySize, range)`.

There is no closed-form task count (see Structure), and — because every
recursive step contends for the one shared DB (see Wiring) — the program is a
data-movement/coherence stress rather than a compute or scheduler-width
benchmark.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `arraySize` | element count to sort | `ARRAY_SIZE` = 1000 (`#define`, used when the argument is absent) | ✓ parsed via `getArgc`/`getArgv` in `mainEdt`. Under the hinted build it is also carried in every EDT's parameter block, because the hint formula normalizes a range's midpoint by the whole extent |
| `argv[2]` = `range` | values are `genValue(i) % range` | `RANGE` = 1000000 (`#define`, same rule) | ✓ same path |
| `argv[3]` = `fillTasks` | how many tasks the generation is cut into | `FILL_TASKS` = 128 (`#define`, same rule); clamped down to `arraySize` | ✓ same path |

An argument that is present but zero, or a fourth argument, is a **loud
failure**: the program prints `QSORT_ARG_ERROR …` and shuts down without ever
printing `QSORT_VALID`, so the cell fails rather than silently sorting 1000
elements. The resolved set is echoed once at start as
`QSORT_ARGS arraySize=… range=… fillTasks=…`.

**Not CLI-reachable**: `CACHE_LINE_SIZE` (= 64, `#define`) sets the serial
cutoff at exactly 8 elements (`size*8 <= 64`) below which `qsortTask` falls
back to selection sort instead of recursing. It is a compile-time constant with
no argv path: a user cannot tune task grain without recompiling, and a grain
sweep would change the published decomposition rather than its size.

## Structure

Every recursive call is one `qsortTask` EDT. A partitioning node creates **one
child per non-empty side — 0, 1 or 2** (a side the pivot leaves empty gets no
task; the bounds are unsigned, so a child for an empty low side would not
describe an empty range, it would wrap and read off the block). There is no
closed form for the node count `Q(N)`: the pivot's *index* is a size-keyed
deterministic function, but the resulting partition *sizes* depend on the
generated values, so tree shape depends on `range` as well as `arraySize`.
Loose bounds: `Q(N) ∈ [2⌈N/8⌉−1, ~N−7]` (balanced versus
one-element-peeled-off-per-level). This pivot rule (a size-keyed pseudo-random
index, no median-of-three) skews noticeably in practice, so `Q(N)` sits well
above the balanced estimate.

| object | count | size |
|--------|-------|------|
| `fillTask` | `fillTasks` | — |
| `qsortTask` | `Q(N)` (every node, leaf or internal) | — |
| `mainEdt` / `finishEdt` | 1 each | — |
| DBs | **exactly 1** for the whole run — the original array DB; no sub-DB is ever created | `8·arraySize` bytes |
| Events | 2 per created child (its ONCE data event and the runtime finish event every child opens), plus `fillTasks` generator output events, plus the root's finish and output events and the STICKY `coordEvt` | — |
| Templates | `qsortTemplate` (reused across the whole tree), a second one-off template for the root (which carries the generation's completion on its extra slots), `fillTemplate`, `finishTemplate` | — |

Counter cross-check: **pending** — the previous NUM_EDT_CREATE / NUM_EVENT_CREATE
figures were taken against the old table-walking generator and the old
unconditional two-child rule, so both the values and the `4·I(N)+3` event
formula they confirmed are stale. Re-measure on the current binary before
quoting counts.

## Wiring

The single defining fact of this app's wiring: **every** dependence on the
array DB — every `fillTask`, the root `qsortEdt`, every recursive child
(create-time depv, `DB_DEFAULT_MODE` = RW), and `finishEdt` — is `DB_MODE_RW`,
and there is only one DB. ARTS's RW is **per-node exclusive with OCR RW
semantics**: the workers of one rank share the node's grant and can partition
disjoint ranges concurrently, but only one rank at a time holds it, and each
hand-off between ranks moves the whole `8·arraySize` payload. So a one-node
cell uses all of its workers, and adding nodes adds whole-array migrations
rather than concurrency. That is the anti-scaling exhibit, and it is
structural, not a defect: there is no RO access anywhere in the recursion.

A node releases mid-body, right after partitioning and before creating its
children; the children's RW requests then race for the free grant. The two
per-node wiring events (`qsortLowDataEvt`/`qsortHighDataEvt`) are how a child
is handed the same GUID its parent held.

The root is the only task in the recursion with more than one dependence slot:
slot 0 is the array, and the remaining `fillTasks` slots are the generators'
output events (`DB_MODE_NULL`, control only). Every generator is created with
no dependence, has its output event registered on the root, and only then is
given the array — so no completion can be published before its consumer is
registered, on any runtime.

## Flow

`mainEdt`: parse and validate the arguments, echo them, allocate the array DB
and release it, create the `fillTasks` generators, create the root and the
finisher, wire the coordination chain (`coordEvt`), then release the generators
by giving each of them the array. The generators fill their own ranges; the
root partitions once all of them are done; execution then walks the recursion
tree, and — because of the single-DB RW exclusivity above — wall time tracks
`Q(N)` ownership hand-offs, each publishing the **entire** array (not just the
touched sub-range), rather than the number of available workers. The root's
`EDT_PROP_FINISH` scope drains only once every descendant — including the
nested finish scopes each recursive call also opens — has completed; that drain
fires `outEvt` → `coordEvt` → `finishEdt`, which RW-acquires the sorted array
once more to print its first `min(arraySize, 30)` elements and the check.

The check inside the timed window is one O(n) sweep: non-decreasing order plus
the element sum a permutation of the input must preserve, printed as
`QSORT_VALID sum=… sorted=… n=…`. There is no re-computation of the answer.

## Placement (base)

Every `ocrEdtCreate` in the recursion and the `ocrDbCreate` pass `NULL_HINT`.
Effective policy:

- **EDTs**: NULL hint → round-robin (`ARTS_HINT_ANY_RANK`) — `qsortTask` at
  every depth, and `finishEdt`, scatter across ranks.
- **DBs**: NULL hint → home = creating rank. The one array DB is created in
  `mainEdt`, which only ever runs on rank 0, so its home is **fixed at rank 0
  for the entire run**.

The one exception, and it is the same in both tiers: the `fillTask` generators
carry an `OCR_HINT_EDT_AFFINITY` for `ocrAffinityGetCurrent()` — the rank
`mainEdt` runs on, which is where the no-hint array block homes. This is part
of the generation adaptation, not a performance tier: the published
decomposition is one datablock, so a generator anywhere else would migrate the
whole array in order to write a slice of it. Its consequence is that
generation parallelism is bounded by one node's workers.

Combined with Wiring's single-writer serialization: since the recursion
scatters round-robin while the DB's home stays pinned to rank 0, most RW
hand-offs cross ranks, and every hand-off moves the whole array — by
construction close to the worst achievable coherence traffic pattern for an
in-place sort.

## Placement (hinted)

**A `hinted` target IS built** (`quicksort_hinted`, `HINTED_PLACEMENT` in
`benchmarks/apps/CMakeLists.txt`; catalog `hinted: true`).

As-born combines the two worst placements this program can have: execution
scatters round-robin while the single array block homes on rank 0, so most RW
hand-offs cross ranks and each one moves the whole array. The layer is
CONTAINMENT of a single-RW-block structure, never distribution — hints cannot
decompose the array; that is the restructured version's job.

The layer is `qsRangeEdtHint`, applied to both recursion children, the root and
the finisher. Its map, for a task sorting `[low, high]` of an array of `N`:

```
mid   = low + (high - low)/2
place = floor(mid * P / N),  clamped to P-1,  with P = ocrAffinityCount(AFFINITY_PD)
P <= 1 -> NULL_HINT
```

so a subarray's tasks go to the place that owns the middle of the range they
sort, and the tasks that revisit a region keep returning to the same place.

**The middle and not the start.** Keyed on `low`, every range that begins at
the front of the array maps to place 0 however much of the array it covers, so
the root and the entire left spine (`low = 0` is inherited by every low child)
stack on rank 0 while the leaves spread evenly: the task COUNT balances and the
WORK does not, and the imbalance worsens with rank count — rank-0 work over the
ideal share is roughly `1 + 2P/31`, i.e. ~1.1 / 1.5 / 2.0 / 3.1 at 2 / 8 / 16 /
32 ranks. Keying on the midpoint places a task by the POSITION its range
occupies, so a range's key moves with its extent and a shrinking spine walks
down the places instead of resting on one. The spine is the worst case for that
map (it is the only chain whose extents shrink geometrically while sharing an
endpoint), and on it the excess on any place is bounded by `O(N/P)` — the same
order as the ideal share — giving `≈1.13` at 2, 8, 16 and 32 ranks. Read that
`1.13` as **an upper bound derived on the left spine**, not as a measured
imbalance of the whole map: away from the spine both endpoints move, so ranges
of a given extent are spread over the places rather than banded, which is the
easier case. A leaf's middle is its start, so leaf placement is exactly what it
was and task-count balance is ≈1.0 under BOTH maps — which is also why no
campaign scalar can tell the two apart; only per-rank busy time can.

The cost of the change, stated plainly: spreading the top of the tree moves
large partitions off the block's home, and with one whole-array block each such
move is a whole-array migration. The map is chosen for the balance property,
not on measured wall time.

Two disclosures about the tier boundary:

- The guard also adds one `u64` (`arraySize`) to `qsortPRM_t`, so the hinted
  binary passes **one more parameter word per EDT** than base. It is the hint
  formula's own input and it changes no control flow, partition, DB count,
  event wiring or computation — but it is a real data-layout difference.
- `pdCount <= 1` returns `NULL_HINT`, so at one node the layer places nothing
  and the two tiers are *equivalent*, not *bit-identical* (that extra parameter
  word is still there).

## Sizing

`arraySize` is the only lever that changes anything about parallelism or task
count; `range` only changes which values appear (and, secondarily, tree shape
via the pivot's data-dependent split point); `fillTasks` divides the generation
and nothing else. The serial-sort cutoff (8 elements) is fixed at compile time,
so grain cannot be tuned via CLI.

Width: the recursion is a dataflow generator, not a spawn-and-join phase. Both
children are created and satisfied immediately, so the ready frontier grows
with the tree — of order `0.2·N` leaves at any size the campaign uses, far
above any worker count, with no integer-multiple constraint. The
*coherence-admissible* concurrency is one node at a time, and that is the
exhibit rather than a width violation.

Memory: `8·arraySize` bytes of payload plus the runtime objects of `Q(N)`
finish EDTs and their events, created and retired progressively — order 1–4 GB
at a few million elements. The generation adds `fillTasks` EDTs and nothing
else.

Because coherence never allows more than one node to hold the block, **adding
nodes does not add measured concurrency for this app** — extra ranks wait on
the grant. Sizing is therefore about producing enough sequential RW hand-offs
and enough cross-node whole-array transfer to be a meaningful stress, not about
feeding N×C workers. Calibration values are the orchestrator's: `calibration
pending`.
