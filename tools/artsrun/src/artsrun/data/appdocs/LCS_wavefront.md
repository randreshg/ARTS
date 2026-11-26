# LCS_wavefront

*The restructured version of `LCS_all_db_distributed`: the same computation
with the control plane rebuilt — per-tile boundary strips travel on labeled
sticky events, leaves run in true anti-diagonal wavefront order, each leaf
creates its own south neighbour, and memory follows the frontier.*
Source: `third_party/ocr-apps/apps/LCS/refactored/ocr/intel-jesmin-lcs_all_db_distributed/lcs_wavefront.c`
(~520 lines, C).  Selected as `LCS_all_db_distributed:restructured`.

## Overview

Computes the identical edit-distance-style score as the tiled original —
same per-tile string seeds (a self-contained `splitmix64`-style mixer over
`(tile index, position)`, identical in all four LCS programs), same analytic row-0/column-0 boundary, same
per-cell recurrence — so the two programs print the same `LCS length` and
the same `LCS checksum` (a 31-bit FNV-1a digest of the final tile's cells,
which is the voted scalar) and cross-validate each other — but only at a
shared argument pair, and the tiled original additionally requires `N/base`
to be a power of two while this program requires only `N % base == 0`.  Its
campaign point (`L = 6912`) is one the original rejects, so the equality is
a deliberate one-off validation run (e.g. `1024 256` for both binaries),
not something a campaign cell exercises.  What changes is
everything the original's scaling died of: the quadrant recursion whose
FINISH gating capped speedup at `(4/3)^d` is replaced by real tile-to-tile
data dependences; the original's `(N+1)²`-cell score matrix of per-tile
blocks is replaced by transient per-leaf scratch; and the creation plane
itself is distributed — each leaf creates the leaf below it, so creating
`L²` leaves costs `O(1)` per leaf on `L` ranks in parallel instead of one
serial chain of `L²` creates.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `N` | string length; total work is `N²` cells | 1024 | ✓ parsed in `mainEdt`, carried via paramv — multinode-safe |
| `argv[2]` = `base` | tile side; `L = N/base` is the wavefront width dial (`N % base == 0` required, loud-failed otherwise — no power-of-two constraint) | 256 | ✓ same |

Both arguments are mandatory: a wrong argument count prints the usage line
and shuts down.  There is no bands-per-rank knob — the band count is the
rank count by construction (see Placement).

## Structure

Per run: `2L` init EDTs, each creating and filling one string tile and
then satisfying the tile's labeled STICKY event (`2L` events, created by
`mainEdt` and living to the run's end, unlike the strips' below), each
with an app-supplied COUNTED(1) completion event — the gate below;
**one** row-seed EDT, `L²` leaf EDTs, one wrapup and one dedicated
shutdown EDT.
Row 1's `L` leaves are created by the row-seed EDT; every other leaf is
created by its north neighbour once that neighbour has computed its tile,
before it emits, so creation runs at most one row ahead of the wavefront
and the parked-leaf pool is `O(L)`, not `O(L²)`.  Per interior leaf: 3
strip DBs out (right column `base` ints, bottom row `base` ints, corner 1
int) + 3 labeled STICKY events; edge tiles skip the outputs nobody
consumes, and the final tile's corner strip carries two ints (the answer
cell and the digest).  Every strip, and the event that carried it, is
destroyed by its single consumer, so steady-state resident data is the
frontier's strips plus the `2L` string tiles (which, with their events,
live to the run's end) — the score matrix itself never exists.  The leaf's working set is 3 rolling rows of `base` ints in
transient scratch.  One EDT template is created in `mainEdt` and carried in
the leaf parameters; it outlives every leaf and is never destroyed.

## Wiring

Leaf `(bi,bj)` has exactly five dependences: its S tile (RO, through the
tile's event; homed on the leaf's band, where the tile's band-hinted init
task created it), its T tile (RO, through the tile's event; homed where
its init task ran), and the west/north/northwest strips — each an
ordinary DB riding a **labeled STICKY event with exactly one creator**: the task that creates the emitting leaf creates the leaf's
output events (before it adds the leaf's dependences, since the last one
can make the leaf runnable at once), and `mainEdt` creates the final corner
event whose consumer it also registers.  That creator precedes every use of
the event in the dependence order: the producer satisfies it only after
being created; its south consumer is created by the producer itself, its
southeast consumer by the task the producer's rc strip wakes, and its east
consumer by the row-seed task later in its loop (row 1) or by the task the
creator's own rc strip wakes, which is why a leaf creates its south child
before emitting.  So producer and consumer need no order between them, and
no runtime needs `GUID_PROP_CHECK`: each label is created once and lives
one generation.  The consumer, the only party that knows the strip was
delivered, destroys the event and the strip.  Row-1/column-1 leaves take
`NULL_GUID` in the missing slots and synthesize the boundary strips
analytically.  The row-seed EDT is gated on all `2L` init-completion
COUNTED events, so it becomes runnable once every init task ended; the
gate orders no read, since a leaf receives a string tile only through the
tile's event, which the init task satisfies after its release.

## Flow

The row-seed EDT creates row 1; each leaf, once its tile is computed and
before it emits its strips, creates the leaf directly below it (and the
events that leaf will emit on), so the creation plane is as wide as the
wavefront.  Leaves fire in anti-diagonal order as their strips arrive,
width ramps 1→L→1.  The last tile's corner event carries the answer and the
digest to the wrapup (which prints both), and shutdown runs in its own EDT
behind the wrapup's completion, so the wrapup's release work is inside the
measured window.

## Placement (base)

Row-band, baked in (a restructured program owns its placement, so its base
form IS the placed form — there is no `_hinted` flavour): block-row `bi`
maps to `band(bi-1)` with `band(row) = (row · nranks) / L`, a balanced
contiguous partition of the `L` rows in play — bands differ in height by at
most one row and no rank is left without work at any node count (the
earlier `ceil((L+1)/(nranks·k))` height was safe only because `L ≫ nranks`
at the calibrated point; it stranded ranks whenever that stopped holding,
and the tiled sibling shares the replacement formula).  Leaves and the S
tiles' init tasks are hinted to their band, so an S tile is created on its
readers' band; on ARTS and ocr-vx, which home a labeled range's members
round-robin by index (xsocr homes a whole range on the rank that reserved
it), the strip and S-tile events are homed there too through labeled-index
re-encoding (`band + nranks·idx`).  West strips are always band-local;
north/corner strips cross ranks only at band boundaries; T tiles are left to
the no-hint policy (their init tasks carry no hint, a column's readers span
every band) and their events stay round-robin by index.

## Validity shape (measured, 15w+1p x 1/2/4/8 nodes, `131072 512`)

**Measured before the 2026-09-03 restructure of the creation plane** (the
serial spawner chain became leaf-creates-south), **before the balanced
band formula and before the string tiles were created by their init
tasks**.  At `L = 256` and ≤ 8 ranks both maps cover every rank but
assign different rows to them, and the spine was small, so the shape below
is indicative only — not a like-for-like locality reference for the new map
— and every number predates the change.

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_retain_owner | 3.27 | 2.32 | 1.51 | 1.12 |
| inv_retain_owner | 3.26 | 2.01 | 1.42 | 0.74 |
| excl_retain_owner | 3.24 | 1.92 | 1.36 | 1.10 |

At this small instance (`L = 256`, average width 128 ≈ the 8-node worker
count) ramp-up/down was a visible fraction, so 2.9-4.4x at 8 nodes was a
width-limited floor, not the asymptote.  On one 108-worker node, measured
on the same earlier programs, the rewrite ran `131072 1024` in 0.82 s where
the tiled original needed 15.5 s (19x) — the wiring, not the machine, was
the original's limit.

## Sizing

`N` sets total work (`N²` cells) and `base` dices it: `L = N/base` is the
wavefront width, so `base` trades per-leaf grain against parallel width at
fixed work.  Fixed-work granularity on the Dane-mirror geometry (1 node,
108w+4p, Release, `val_retain_owner`), measured before the string tiles
were created by their init tasks: `2097152/512` = 122.6 s vs
`2097152/256` = 202.1 s — finer tiles cost runtime object churn there, so
the coarsest `base` whose `L` still spans the largest geometry was picked.
Measured lattice, on the same program: 1048576/256 = 45.3 s,
1572864/256 = 104.6 s, 2097152/256 = 202.1 s, 2097152/512 = 122.6 s,
3145728/512 = 275.8 s.  The calibrated point is
**`1769472 256`**: `L = 6912`, whose average anti-diagonal width (3456)
equals the 32-node x 108-worker campaign's total worker count, and the
nearest feasible point to the window.  The anchor at that point, one node
of 108 workers under INV·RETAIN·OWNER, measured before the string tiles
were created by their init tasks, was 100.7 s and 4.3 GB resident, inside
the restructured tier's 120 s band; the earlier 141.0 s was measured with the
serial creation chain, whose `L² = 47.8 M` sequential creates were a floor
of the same order as the compute.

Memory is frontier-bound by construction: strips are `≈ L · 3 · base` ints
plus the `2L` string tiles (tens of MB at the calibrated point), the score
matrix never exists, and the parked-leaf pool is now bounded by one row of
leaves rather than by the creation-vs-completion race.  The tiled
original creates its `4·(N+1)²` ≈ 69 GB table (at its own calibrated
size) tile by tile in the tasks that produce it, on any runtime, and
keeps each computed tile until its south-east successor destroys it (the
last row and column to the end).

The scalar to pin is `LCS checksum:` (the final tile's digest), which the
tiled original prints identically at a shared power-of-two `N`/`base`
validation run (see Overview); `LCS length:`
is still printed and remains a useful cross-row cross-check.  The pinned
checksum matched at the anchor.
