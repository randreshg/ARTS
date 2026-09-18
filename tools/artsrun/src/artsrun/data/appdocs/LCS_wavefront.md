# LCS_wavefront

*The restructured version of `LCS_all_db_distributed`: the same computation
with the control plane rebuilt — per-tile boundary strips travel on labeled
counted events, leaves run in true anti-diagonal wavefront order, each leaf
creates its own south neighbour, and memory follows the frontier.*
Source: `third_party/ocr-apps/apps/LCS/refactored/ocr/intel-jesmin-lcs_all_db_distributed/lcs_wavefront.c`
(~450 lines, C).  Selected as `LCS_all_db_distributed:restructured`.

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
data dependences; the upfront creation of the whole `(N+1)²`-cell score
matrix is replaced by transient per-leaf scratch; and the creation plane
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

Per run: `2L` string-tile creates + `2L` init EDTs (each with an
app-supplied COUNTED(1) completion event — the gate below), **one** row-seed
EDT, `L²` leaf EDTs, one wrapup and one dedicated shutdown EDT.  Row 1's
`L` leaves are created by the row-seed EDT; every other leaf is created by
its north neighbour when that neighbour completes, so creation runs at most
one row ahead of the wavefront and the parked-leaf pool is `O(L)`, not
`O(L²)`.  Per interior leaf: 3 strip DBs out (right column `base` ints,
bottom row `base` ints, corner 1 int) + 3 labeled COUNTED(1) events; edge
tiles skip the outputs nobody consumes, and the final tile's corner strip
carries two ints (the answer cell and the digest).  Every strip is
destroyed by its single consumer, so steady-state resident data is the
frontier's strips plus the `2L` string tiles — the score matrix itself
never exists.  The leaf's working set is 3 rolling rows of `base` ints in
transient scratch.  One EDT template is created in `mainEdt` and carried in
the leaf parameters; it outlives every leaf and is never destroyed.

## Wiring

Leaf `(bi,bj)` has exactly five dependences: its S tile (RO, band-steered
home), its T tile (RO, round-robin home), and the west/north/northwest
strips — each an ordinary DB riding a **labeled COUNTED(1) event** the
producer creates at satisfy time (a consumer that registered first parks on
the absent label and is fired by the install; a consumer that registers
after the install takes the value immediately, and the declared count of
one reclaims the event as soon as its consumer has registered — nothing
lingers).  Both orders occur by design now that a leaf's creator is its
north neighbour: the north strip is always registered after its satisfy,
the west strip usually before.  Row-1/column-1 leaves take `NULL_GUID` in
the missing slots and synthesize the boundary strips analytically.  The
wavefront must not start before the strings exist (an RO read of a tile
still being written is unordered), so the row-seed EDT is gated on all `2L`
init-completion COUNTED events.

## Flow

The row-seed EDT creates row 1; each leaf, having emitted its strips,
creates the leaf directly below it, so the creation plane is as wide as the
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
and the tiled sibling shares the replacement formula).  Leaves are hinted
to their band, and strip/S-tile homes follow through labeled-index
re-encoding (`band + nranks·idx`).  West strips are always band-local;
north/corner strips cross ranks only at band boundaries; T tiles stay
round-robin (a column's readers span every band).

## Family shape (measured, 15w+1p x 1/2/4/8 nodes, `131072 512`)

**Measured before the 2026-09-03 restructure of the creation plane** (the
serial spawner chain became leaf-creates-south) **and before the balanced
band formula**.  At `L = 256` and ≤ 8 ranks both maps cover every rank but
assign different rows to them, and the spine was small, so the shape below
is indicative only — not a like-for-like locality reference for the new map
— and every number predates the change.

| arm | 1n | 2n | 4n | 8n |
|---|---|---|---|---|
| val_wb_nocomb | 3.24 | 2.31 | 1.51 | 1.11 |
| val_wb | 3.27 | 2.32 | 1.51 | 1.12 |
| inv_wb | 3.26 | 2.01 | 1.42 | 0.74 |
| excl_retain | 3.24 | 1.92 | 1.36 | 1.10 |

At this small instance (`L = 256`, average width 128 ≈ the 8-node worker
count) ramp-up/down is a visible fraction, so 2.9-4.4x at 8 nodes is a
width-limited floor, not the asymptote.  On one 108-worker node the rewrite
runs `131072 1024` in 0.82 s where the tiled original needs 15.5 s (19x) —
the wiring, not the machine, was the original's limit.

## Sizing

`N` sets total work (`N²` cells) and `base` dices it: `L = N/base` is the
wavefront width, so `base` trades per-leaf grain against parallel width at
fixed work.  Fixed-work granularity on the Dane-mirror geometry (1 node,
108w+4p, Release, val_wb_nocomb): `2097152/512` = 122.6 s vs `2097152/256` =
202.1 s — finer tiles cost runtime object churn, so pick the coarsest
`base` whose `L` still spans the largest geometry.  Measured lattice:
1048576/256 = 45.3 s, 1572864/256 = 104.6 s, 2097152/256 = 202.1 s,
2097152/512 = 122.6 s, 3145728/512 = 275.8 s.  The calibrated point is
**`1769472 256`**: `L = 6912`, whose average anti-diagonal width (3456)
equals the 32-node x 108-worker campaign's total worker count, and the
nearest feasible point to the ~150 s anchor.  `calibration pending` on the
anchor itself — the 141.0 s figure was measured with the serial creation
chain, whose `L² = 47.8 M` sequential creates were a floor of the same
order as the compute; with the spine gone the point should be re-measured
before it is quoted, and the 16- and 32-node cells re-run.

Memory is frontier-bound by construction: strips are `≈ L · 3 · base` ints
plus the `2L` string tiles (tens of MB at the calibrated point), the score
matrix never exists, and the parked-leaf pool is now bounded by one row of
leaves rather than by the creation-vs-completion race — against the tiled
original's eager `4·(N+1)²` ≈ 69 GB table at its own calibrated size.

The scalar to pin is `LCS checksum:` (the final tile's digest), which the
tiled original prints identically at a shared power-of-two `N`/`base`
validation run (see Overview); `LCS length:`
is still printed and remains a useful cross-row cross-check.  Both pins
must be re-derived: `calibration pending`.
