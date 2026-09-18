# quicksort_dist

*The restructured version of `quicksort`: the same array and the same answer,
decomposed as a sample-splitter distributed sort on a place-persistent SPMD
structure whose exchange is aggregated per (place, peer), and whose input is
generated where it is classified.*
Source: `third_party/ocr-apps/apps/quicksort/ocr/quicksort_dist.c` (~900 lines).

## Overview

`quicksort` is a recursion in which every task acquires the SAME datablock
`DB_MODE_RW` -- the children are handed the same GUID and differ only in their
index range -- so its `hinted` version can only keep the chain where the block
is. This rewrite replaces the decomposition with the sample-splitter
(PSRS/samplesort) shape: a sorted sample of the input is cut into `nbuckets-1`
ascending splitters, every element is routed to the bucket its value falls in,
and each bucket sorts what it receives with a local in-place quicksort. Because
the buckets are in splitter order, concatenating them in order is the sorted
array.

Both programs generate their input from the same function -- `genValue(index,
range)`, a splitmix64-style mixing of `index+1` reduced modulo `range`, a pure
function of the index with no state carried between calls -- so at the same
`(arraySize, range)` the two rows sort the same multiset and print the same
`sum`. The index is a `u64` on every path — the old `int` seed is gone — but
that is not where the size ceiling is: stratifying the sample, cutting the chunk
bounds and cutting the splitters each multiply an index by a count before
dividing, so what must stay below 2^64 is `arraySize * max(nsamples, nchunks)`
and `nbuckets * nsamples` (with `nsamples = 32 * nbuckets` capped at
`arraySize`). At the campaign sample size that puts the ceiling near
`arraySize ~ 2^44`; `mainEdt` checks the products and refuses loudly
(`QSORT_ARG_ERROR`) rather than wrapping. The two rows are calibrated at
different sizes, though: the answers
are comparable only against themselves, and consensus voting compares each row
against its own runtimes.

The result scalar is a real check, not a sample of the output: each bucket
reports `{count, sum, sorted, first, last}`, each place combines its buckets'
verdicts, and the finisher confirms order across place boundaries, the element
count, and the permutation-preserving element sum against the input sum the
chunks computed as they generated.

## Parameters

`quicksort_dist <arraySize> <range> [nbuckets] [nchunks] [places]`.

`arraySize` is the size knob and `range` is the program's own default (`RANGE`).
`nbuckets` and `nchunks` are the compute decomposition and the width rule sets
them. `places` is the ownership and communication decomposition -- and, since
the exchange stages are per (place, peer, wave), it is the exchange width knob
too.

`places` is an **argument, not the rank count**: the program never asks how many
ranks exist except to compute a placement hint, so its task count, its datablock
count and the order its partial sums combine in are identical in every geometry.
32 is one place per node at the largest geometry, the convention XSBench's
`-p 32` already uses.

An argument that is present but zero, a sixth argument, or a combination that
cannot describe a run (`places > nbuckets`, `places > nchunks`,
`nbuckets > arraySize`, `nchunks > arraySize`, or a size whose index-times-count
products would not fit in 64 bits) is a **loud failure**: the
program prints `QSORT_ARG_ERROR …` and shuts down without printing
`QSORT_VALID`. These used to be silent clamps. The resolved set, including the
derived wave and sample counts, is echoed once at start as `QSORT_ARGS …`.

Not CLI-reachable: `SAMPLES_PER_BUCKET` (32) sets how much the splitter sample
oversamples, and `SORT_CUTOFF` (32) the grain of the local sort. Both are the
program's own constants and neither is a width.

## Structure

`mainEdt` does O(P) work and nothing else: `P²·W + P + 1` events (one exchange
event per (source place, wave, destination place), one verdict per place, and
the sticky splitter event), and `2P + 2` tasks (one `placeInitTask` and one
`sampleTask` per place, the splitter and the finisher). Everything else is
created by `placeInitTask` **on the place that will run it**, because a task
created with a remote affinity is a message and a graph built in one place
cannot scale.

- **`sampleTask`** (`P`) draws its share of the `SAMPLES_PER_BUCKET * nbuckets`
  sample **and sorts it**; **`splitterTask`** merges the `P` sorted runs through
  a binary min-heap over the run heads and cuts the splitters, publishing them
  on a sticky event. This is the one width-1 stage on the critical path, so its
  serial term matters: it is `O(nsamples * log P)` — one heap step per sample
  element — with `nsamples = SAMPLES_PER_BUCKET * nbuckets`. The fan-in is the
  place count and does not grow with the chunk count, and the merge is cheaper
  than the whole-sample sort it replaced (`nsamples * log nsamples`), but the
  term **is** linear in `nbuckets`, because that is what sets the sample size.
  At `nbuckets = 6912, P = 32` it is ~221k elements × 5 heap steps.
- **`splitLocalTask`** (one per place) copies the splitters into a block of the
  place's own and republishes them locally, so the central splitter event's
  reader set is the place count rather than the whole decomposition.
- Per place, with `cpg = nchunks/places`, `bpg = nbuckets/places`,
  `W = waveCount(nchunks, places)` (8 when `cpg >= 8`, else 1) and
  `narr = places*W` arrivals:
  - **`chunkTask`** (`cpg`) generates its own slice, groups it by destination
    place, and carries its own partial input sum in its own block -- no two
    chunks share a writable object.
  - **`packTask`** (`W * places`) is one task per (wave, peer): it copies what
    this place owes ONE peer in one wave into one block and sends it once,
    indexing straight to that peer's run in each chunk through the count header
    the chunk carries.
  - **`reapTask`** (`W`) releases a wave's chunk blocks and their events once
    every peer's pack has read them. It touches no element.
  - **`countTask`** (`narr`) classifies ONE arrival and reports its per-bucket
    counts.
  - **`offsetTask`** (one) turns the `narr × bpg` count matrix into bucket
    starts and per-(arrival, bucket) cursors, and creates the place's element
    array. Its work is the exchange grid, never the elements.
  - **`scatterTask`** (`narr`) moves ONE arrival into the place array at those
    cursors and frees the arrival. The arrivals write disjoint runs of one
    block and all run on the place's own rank.
  - **`readyTask`** (one, O(1)) is the single edge from "every scatter done" to
    the buckets, so a bucket takes one dependence rather than `narr` of them.
  - **`bucketTask`** (`bpg`) sorts its bucket **in place** inside the place
    array -- its elements are a contiguous run of it -- and reports five words.
  - **`placeJoinTask`** (one) combines this place's verdicts into one and
    releases the place array, the offsets, the splitter copy and the place's
    sticky events.
- **`finishTask`** combines the `P` verdicts, prints the scalar and shuts down.

Every stage that touches an element is `nchunks`, `nbuckets` or `places²·W`
wide. The stages that are one per place do arithmetic over the counts.

## Wiring

A block is `[count][elements]` at every stage but two: the chunk's, which
prefixes its own sum and per-destination counts so a pack takes a peer's share
as a contiguous run, and the arrival's, which prefixes the summed chunk sums and
the count.

Every transfer block is destroyed by a task that knows it is finished with: the
wave's reaper destroys the chunks (a chunk has one reader per peer, so no reader
can free it), the offset task destroys the count blocks, each scatter destroys
its own arrival and its sticky exchange event, the join destroys the verdicts,
the place array, the offsets and the local splitter copy, and the finisher
destroys the place reports. Only the central splitters outlive their readers,
and they are 55 KB.

The exchange events are STICKY, and that is a correctness requirement rather
than a preference: the consumer registers inside the DESTINATION place's own
init task while the producer satisfies from the SOURCE place's pack -- two tasks
on two ranks whose only common ancestor is `mainEdt` -- so nothing orders the
registration before the satisfy. A sticky event delivers to a late registrant.

The chunk's own sum riding in its own block is what removed an earlier version's
one shared writable object: 6,912 chunks acquiring one block `DB_MODE_RW`,
exclusive at rank granularity and migrating across ranks, measured at **57% of
that version's eight-node time**.

## Flow

The samplers run first; the splitter fires on all of them, each place copies the
splitters locally, and that copy releases the place's chunks. A wave's packs fire on
that wave's chunks -- not on the place's -- and its reaper on those chunks and
those packs. On the destination side the counts fire per arrival, the layout
task on all of them, the scatters on the layout, the readiness edge on all the
scatters, the buckets on it, the join on its buckets, and the finisher on the
`P` verdicts.

## Placement (base)

A place is mapped to rank `place * nranks / places` and everything the place
creates carries that hint. That is the only thing the program asks the machine,
and it asks it for a hint. Blocks bound for a peer are created on the peer's
home, since a block consumed exactly once cannot amortize an ownership
migration. With `places` a multiple of the rank count, every rank owns exactly
`places/nranks` places at every node count in a doubling sweep.

## Sizing

Peak resident bytes on a 1-node cell:

```
peak ≤ 24 * arraySize                                  the generated (chunk) form,
                                                       the packed arrivals and the
                                                       place arrays, all resident
     + 16 * (arraySize / nchunks) * (running chunks)    per-chunk malloc scratch
     + 8 * places * (2 + places*W) * (nbuckets/places)  offset tables
```

The bound is `24 *` and not `16 *` because nothing in the graph limits chunk
residency: a `chunkTask`'s only dependence is the place-local splitter event, so
all `nchunks` chunk blocks (`8 * arraySize`) may be live at once; a wave's
`reapTask` is merely ENABLED once its packs have run, not necessarily executed,
so the packed arrivals (`8 * arraySize`) can coexist with them; and `offsetTask`
creates the place array (`8 * arraySize`) as soon as the counts are in, while the
arrivals live until their scatters run. A scheduler that runs the reapers and
scatters promptly settles near `16 * arraySize`, but that is an observation about
scheduling, not a bound.

At `arraySize = 4e9` the bound is ~96 GB of payload plus ~1 GB of scratch — inside
a 190 GB budget with a ~1.9x margin, so **measure the 1-node RSS before committing
to that size**. Bounding it at `16 *` would mean chaining wave `w+1`'s chunks on
wave `w`'s reaper, which would cut the chunk stage's instantaneous width from
`nchunks` to `nchunks/W` and put it below the 3456-worker frontier; the residency
is left unbounded on purpose and the budget carries it. The peak is still one
whole-array copy better than the version this replaces, which copied every bucket
out of its arrival before sorting it; buckets now sort in place.

Width, per phase, with `P = places`, `W = waveCount(nchunks, places)`:

| phase | count | touches elements |
|---|---|---|
| `sampleTask` | `P` | no |
| `splitterTask` | 1 | no — but its serial term is `O(32·nbuckets·log P)`, linear in `nbuckets` |
| `splitLocalTask` | `P` | no |
| `chunkTask` | `nchunks` | yes |
| `packTask` | `P²·W` | yes |
| `countTask` | `P²·W` | yes |
| `offsetTask` | `P` | no |
| `scatterTask` | `P²·W` | yes |
| `readyTask` | `P` | no |
| `bucketTask` | `nbuckets` | yes |
| `placeJoinTask` / `finishTask` | `P` / 1 | no |

so the widest concurrent per-element phase is
`max(nchunks, nbuckets, places²·W)`. The structure is spawn-and-join, so the
width rule wants an integer multiple of the persistent units the largest
geometry provides. Campaign values: `calibration pending`.

One property of the program is worth stating because it is not this tier's to
fix: the base recursion's pivot is `getRandNum(size/2) % (high-low)`, so every
subrange of a given length picks the same relative pivot and the base tree is
far from balanced. This tier does not inherit that, but its splitters come from
a random sample, so bucket occupancy is balanced only to the accuracy
`SAMPLES_PER_BUCKET` (32) buys.
