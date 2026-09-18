# smithwaterman_dist

*The restructured version of `smithwaterman`: the same wavefront and the same
kernel, decomposed so each place builds the tasks for its own band of rows.*
Source: `third_party/ocr-apps/apps/smithwaterman/ocr/smithwaterman_dist.c`.

## Overview

The row this re-implements prescribes its whole DAG from `mainEdt`: a readiness
event trio per tile, then a task per tile with four dependences each. Measured,
its creation loops are essentially the whole run — **9.82 s of a 9.93 s run** in
the run where the phase split was taken, against 0.001 s to read the input. A
tile cannot start before the creator has reached it, so the run cannot end
before the loops do. More workers make it slower (7.91 s at 15 against 10.88 s
at 112) and more ranks make it far worse, because a task placed elsewhere is a
message. The two figures come from different run sets and cannot be read
against each other — a constant 9.82 s creation floor cannot yield a 7.91 s run,
and the most recent campaign measured that cell at 9.411 s. The anchor itself
belongs to the `smithwaterman` row, which owns the program these numbers
describe.

The wavefront itself was never the constraint. A `W x W` tile grid has `W^2`
tasks against an anti-diagonal critical path of `2W-1`, so about `W/2` of
average concurrency — thousands of ready tasks at these sizes, far more than
the machine offers.

## Parameters

`smithwaterman_dist <tileW> <tileH> <seq1> <seq2> <score> [places]`.
The catalog runs `100 100 ... 32`.

The two knobs are independent: total DP work is the product of the sequence
lengths, and the wavefront's peak width is `min(len)/tile`. This row is a
**scaler**, so the window fixes the length -- the anchor goes as `L^2.2`
(7.9 s at 200k, 40.1 s at 400k, 92.7 s at 600k, 178.5 s at 800k), which puts
the window at `L = 800,000`. The tile then sets the width.

`places` is the ownership decomposition and is an **argument, not the rank
count**: the answer does not move over places 1, 2, 4, 8 and 16, nor over one,
two, four and eight nodes. It is not a performance choice either — at one node
the fastest value is 4, not 32 — but the SPMD rule fixes it at the largest
geometry so the decomposition does not move with the sweep.

**Coverage is checked, loudly.** Places are contiguous row bands and reach
ranks through a linear map, so every rank owns rows exactly when `places` lies
between the rank count and the row count `H+1`: at least as many places as
ranks makes the map onto, at most as many places as rows keeps every band
non-empty. Either bound alone is insufficient — with `places > H+1` a band is
empty and the rank holding only that band idles while a one-sided check passes.
`mainEdt` validates both before it reserves anything and refuses the run
otherwise (`every rank must own a band: places P must lie between the rank
count N and the tile row count H+1`), so a roster that sets `places` below the
node count, or above the grid, fails the cell instead of silently idling nodes —
no oracle would have seen it. With the catalog's `places = 32` and `H = 8000`
the check passes at every node count of the sweep; it forbids running this row
above 32 nodes without raising `places` to match, and it forbids `places`
above `H+1` at any small-input smoke.

The rest of the argument surface is checked in the same loud style, and
identically to the base row: a non-positive tile dimension, an unreadable
sequence or score file, and an empty sequence each print a line naming the
violated precondition and end the run before any object is created. None of
them prints a `score:` line, so a refusal always fails the cell rather than
voting.

## Structure

`mainEdt` reads the sequences, reserves one labeled event range, and creates
one `placeInit` per place per phase. That is all of its work: `O(P)`, with no
term in the tile count.

Each `placeInit` owns a **contiguous band of rows** and builds, for its band
only, the border cells and one task per tile. Names are what make this
possible: a readiness event's name follows from the tile coordinate, so a
place can name a tile it does not own -- which is exactly what the top edge of
a band needs -- while only the owner ever creates it. Each name is therefore
created exactly once, and a consumer may register a dependence on a name whose
object does not exist yet.

Names come first, in a phase of their own: every band creates all of its own
readiness events (`PHASE_NAMES`) before any band binds tasks to them
(`PHASE_TASKS`). That is what lets creation happen everywhere at once, and it
is also what sets the memory floor — the whole event grid is live from the end
of the naming phase, so this row holds 106 GB at the catalog cell against the
base row's 3. The blocks the events carry are not: a tile's three inputs and
the three events that carried them are destroyed by their single consumer, so
the payload live set follows the wavefront rather than the tile count.

`placeInit`'s only per-rank table is the first row of each rank, sized from the
rank count and scanned once over the places.

## Wiring

A tile's task takes the west tile's right column, the north tile's bottom row
and the north-west tile's bottom-right corner, plus the shared parameter block.
It writes three blocks of its own and satisfies the three events it owns, then
destroys the three blocks it read **and the three events that carried them** —
each readiness event is named by exactly one dependence, so its single consumer
is also its last user.

## Flow

Band `r` starts as soon as band `r-1` has produced its first column, so the
bands run as a pipeline rather than in turn.

## Placement (base)

Rows are banded, not scattered, because a wavefront is a pipeline. Only a
band's top edge crosses a rank -- `O(P*W)` against the `O(W^2)` a cyclic or
round-robin map pays, every neighbour of which is remote. The price is
**pipeline fill, and it is not small**: the last band's first row lies
`(1 - 1/P)` of the way down the grid, so its place has nothing to run before
anti-diagonal step `(1 - 1/P)*H` of the `W+H-1` — at `P = 32` and `W = H =
8000` that is step 7751 of 15999, 48% of the steps and 47% of the tiles — and
at any step only the bands the frontier's row window covers are busy. Splitting
a place's rows into `k` separated bands would divide that fill by `k` and
multiply the crossings by `k`; that variant is unmeasured and the shipped map
is one band per place. What is measured is the comparison against scattering: a
cyclic map was worse than the base row's own hinted banding (155 s at two nodes
against 105 s), and banding is 192x the base row at eight.

Places are bands and ranks are bands, so the linear place-to-rank map is the
right one here: it keeps a rank's rows adjacent, which is the whole point. (A
two-dimensional ownership would lose an axis through that map. A
one-dimensional one does not — the same line of code is a defect in one
structure and correct in the other.)

The event names are laid out so a tile's events are homed on the rank that owns
the tile: the tiles are numbered densely within each rank's band and then
interleaved by rank, and the three events of a tile are strided by a multiple
of the rank count so they share that home.

Over four nodes the counters put this row and the hinted tier at the same
locality -- 0.21% of acquires remote against 0.27%, both from an imbalance of
1.00x -- and separate them by what crosses: **2.4 MB against 274 MB**.
*These four figures are at the trend size, not at the catalog arguments*: the
0.27% companion is a four-node counterset taken at `L = 70,000` (tile 100, a
700 x 700 grid, 1,959,999 acquires), and the byte pair carries no recorded size
at all. They are **pending a re-take at the catalog arguments**; read them as
the shape of the difference, not as calibration-size locality. The
acquire ratio cannot see the difference because it counts what a task reads,
and both band their tiles; the bytes can, because the row this replaces builds
every task on one rank and each creation and dependence registration is itself
a message.

## Correctness

The score is the bottom-right cell of the DP, which every tile feeds; the
bottom-right tile prints it (`score: <n>`) and compares it with the expected
value read from the score file, then shuts the runtime down. Nothing else is
computed for verification — no sequential reference and no second pass.

At the trend size the two ports agree on `86360`, and this row reproduces it at
one, two, four and eight nodes. At the catalog size a sequential reference is
not available -- 800,000 x 800,000 is 6.4e11 cells -- so the pin is agreement
between two independently built graphs: this row and the port it re-implements
both give `493680`.

## Sizing

**Width (the tile grid).** With sequence lengths `len1`, `len2` and tile
`tw x th`, the grid is `W = ceil(len1/tw)` columns by `H = ceil(len2/th)` rows.
A tile is ready no earlier than anti-diagonal step `i+j`, so the instantaneous
frontier at step `k` is `|{i : max(1,k-W) <= i <= min(H,k-1)}|`: it ramps
`1 -> min(W,H) -> 1` over `W+H-1` steps, peaking at **`min(W,H)`** with an
average of `W*H/(W+H-1)`. The width knobs are argv — the tile pair (`argv[1]`,
`argv[2]`) and the sequence length (the input files); `places` is not one.
At the catalog arguments `L = 800,000` and tile 100 give `W = H = 8000`: peak
**8000 = 2.31x** of the 3456 workers at the largest geometry, average 4000 =
1.16x, and the frontier is at or above 3456 for 57% of the steps, which carry
81% of the tiles.

**Memory (one node).** The naming phase materialises every readiness event, so
the event grid and not the frontier sets the floor. Counting objects (`P` =
places, and the `O(P)` terms are the per-place announcement event and block):

```
events   = 3*W*H + 2*W + 2*H + 1 + P    (three per tile, the row-0/column-0 border
                                         names and the (0,0) corner, one announcement
                                         per place)
EDTs     = W*H + 2*P + 1
DBs      = 5*W*H  (2 temporaries and 3 outputs per tile, all destroyed by their last user)
           + 2*W + 2*H + 1 border blocks + P announcement blocks
           + 1 parameter block of len1+len2+64 bytes
           + 3 input file buffers of len1, len2 and the score text, never destroyed
live     ~= c_ev * 3*W*H  +  2*(len1+len2)  +  frontier * 4*(tw+th)  +  workers * 4*(tw+1)*(th+1)
```

`c_ev` is the resident cost of one live event name, and **the three measured
points do not agree on it**, so it is a range, not a constant:

| point | `3*W*H` | resident | implied `c_ev` |
|---|---|---|---|
| `L = 700,000`, tile 100 | 1.47e8 | 48 GB | 326 B |
| `L = 700,000`, tile 50 | 5.88e8 | 199 GB | 338 B |
| `L = 800,000`, tile 100 | 1.92e8 | 106 GB | 552 B |

The two `L = 700,000` points agree with each other and disagree with the
catalog point by 1.65x: 1.31x the names cannot cost 2.2x the memory, and the
other three terms of `live` are megabytes at every one of them. Something not
in this model separates the series — a different coherence arm, a different
worker count, or a peak sampled at a different moment — and nothing on hand
settles which, so **`c_ev` is 330-550 B and the campaign must size off the
pessimistic end** until the point is re-taken at the catalog arguments.

**At the anchor** (one node, 108 workers + 4 progress, catalog arguments) the
tightest of the three coherence arms runs **178.5 s holding 106 GB**, 41% of
what a node has; the other two run 163.6 s and 155.6 s at 88 and 106 GB. Against
the 190 GB one-node budget the cap on `3*W*H` is therefore **3.4e8 at 550 B and
5.7e8 at 330 B** — a square grid of 10,600 to 13,800 tiles a side, `L` between
1.06e6 and 1.38e6 at tile 100. Size to 3.4e8; the wider figure is only available
once the discrepancy above is resolved. Beyond one node the grid is split across
ranks and the per-node figure falls with the node count, so the one-node cell is
the binding one.

**The trend** is taken at `L = 140,000`, the same tile, at 15 workers a node,
two runs a cell:

| nodes | 1 | 2 | 4 | 8 |
|---|---|---|---|---|
| time | 6.07 s | 3.69 | 1.88 | **0.98** |

**6.19x from one node to eight** — 77% of linear. The price is at one node,
where the parallel creation phase costs 1.42x against a serialised one; from
two nodes on it wins, and at eight it is 4.6x faster in absolute terms.

**DELIBERATE DEVIATION on width**, structural and smaller than the base row's.
A wavefront needs `W^2` tasks to offer width `W`, and distributing the creation
does not reduce the count: at `L = 700,000` a tile of 50 buys 4.05x the
workers but costs 493 s and 199 GB against tile 100's 108 s and 48 GB (the two
`L = 700,000` rows of the memory table above). The tile
stays 100 and the row runs at **2.31x** the workers -- against the base row's
0.41x, which is the width this tier actually bought.
