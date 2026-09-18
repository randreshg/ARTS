# fft_dist

*The restructured version of `fft`: the same transform, decomposed as a Bailey
four-step transpose over a tile grid, on a place-persistent SPMD structure whose
transpose payload is aggregated per place pair while its task count follows the
tile count.*
Source: `third_party/ocr-apps/apps/fft/ocr/fft_dist.c` (~700 lines).

## Overview

`fft` is one datablock every task in a recursion acquires `DB_MODE_RW`; nothing
about it can be distributed, which is why its `hinted` version only contains the
tree rather than spreading it. This rewrite replaces the decomposition. The
length-N transform is viewed as an `N1 x N2` matrix (`N1 = 2^ceil(m/2)`,
`N2 = N/N1`) and computed in Bailey's four steps: an FFT down every column, a
twiddle, a transpose, and an FFT along every row. Both matrix dimensions are cut
into `t` tiles **by index**, so `t` need not divide either dimension: any tile
count up to the smaller dimension is legal, every tile is non-empty, and no two
tasks ever write the same object -- a tile owns a range of columns on the way in
and a range of rows on the way out.

The input is a single tone, whose spectrum is known in closed form
(`X[5] = X[N-5] = N/2`, every other bin zero), so each row tile verifies its own
output block analytically and the partial checksums reduce to exactly `N`. That
is the result scalar. A run that got the transform wrong prints
`FFT_DIST INVALID` and extracts nothing.

## Parameters

`fft_dist <power> [tiles] [places] [waves]`. The catalog runs
`<power> <tiles> 32` and leaves `waves` derived.

`power` is the size knob. It is NOT the `fft` row's: a restructured tier is
its own row with its own window, and this one transforms a longer signal in
double complex arithmetic. `tiles` is the compute decomposition and the width of
the column and row stages; because tiles are index ranges rather than divisors,
the width rule can be met exactly -- an integer multiple of the 3456 persistent
units the largest geometry provides is always reachable.

`places` is the ownership and communication decomposition: how many partitions
the program divides its tiles into and aggregates its exchange payload in. It is
an **argument, not the rank count**. The program never asks how many ranks exist
except to compute a placement hint, so its task count, its datablock count and
the order its partial checksums combine in are identical in every geometry. 32
is one place per node at the largest geometry, which is where the calibration is
taken, and it is the convention XSBench's `-p 32` already uses here.

`waves` is how many groups a place packs its tiles in. One packing task exists
per (place, wave, destination place), so the wave count is what carries the
exchange stage up to the width of the tile stages around it. Left unset it is
derived as `ceil(tiles / places^2)`, bounded below by 1 and above by
`tiles / places` so a wave still holds a tile; that default makes the exchange
stage at least `tiles` tasks wide by construction. It is exposed because it also
sets the transfer block size and the per-row-tile arrival count.

## Structure

`mainEdt` does O(`places^2 * waves`) work and nothing else: it creates the
`P * waves * P` rendezvous events the places hand blocks through, one report
event per place, the event-grid block, and one `rankInitTask` per place, hinted
onto `place * nranks / places`. Everything else is created by `rankInitTask`
**on the place that will run it**, because a task created with a remote affinity
is a message and a graph built in one place cannot scale.

A place's tile count is its **index span**, `tpg = floor((g+1)*t/places) -
floor(g*t/places)`, which equals `t/places` only when `places` divides `t`;
spans of consecutive places abut and differ by at most one. Per place:

- **`colTileTask`** (`tpg`, no dependences) generates its own columns of the
  tone analytically, runs a length-`N1` FFT down each, applies the twiddle, and
  writes a first-touch block in row-major order over the matrix rows.
- **`packTask`** (one per (wave, destination place)) reads a run of this place's
  column tiles and writes **one** block -- what this place owes that destination
  for that wave -- then signals. Aggregating the payload per place pair is what
  keeps the message count off the tile count; one task per block is what keeps
  the stage's width on it. Each block is created on its consumer's home.
- **`reapTask`** (one per wave) frees that wave's column tiles once every
  destination's pack has signalled. A shared object outlives its readers, so its
  reaper is a task ordered after all of them.
- **`forwardTask`** (one per (wave, source place)) is the sole waiter on one
  rendezvous event and republishes its arrival on a place-local event. It moves
  no data. A rendezvous event is homed where it was created -- `mainEdt`'s rank,
  neither endpoint -- so a registration on it and every delivery from it is a
  message to and from that one rank; the readers of an arrival number `tpg`, so
  binding them to it directly would put `t * places * waves` registrations and
  as many deliveries through a single rank. With the forwarder, the rendezvous
  events carry `places^2 * waves` registrations and `places^2 * waves`
  deliveries **whatever the tile count**, and the fan-out to the readers is
  rank-local.
- **`rowTileTask`** (`tpg`) reads this place's arrivals through those local
  events -- one dependence per (wave, source place) -- assembles each of its
  rows as a walk of contiguous runs, runs a length-`N2` FFT along it, checks
  every output bin against the closed-form spectrum, and reports
  `{partial checksum, max err}`. There is no repacking stage between the
  exchange and the row FFT: a separate one would copy the whole dataset a second
  time inside the measured window and would be `places * waves` tasks wide. What
  it did buy -- being the single local fan-out point for the arrivals -- the
  forwarder buys without the copy.
- **`rankJoinTask`** (one) sums this place's partial checksums, takes the worst
  of their errors, destroys the partials, and -- being the task ordered after
  every row tile of the place -- reclaims the arrivals and the sticky rendezvous
  events they arrived on.
- **`finishTask`** combines the `P` reports, reclaims the event grid, prints the
  scalar and shuts down.

**What the builder costs.** `rankInitTask` is serial within its place (and
parallel across places, on the place's own rank). Its dependence-addition count
is dominated by the row stage's fan-in, `tpg * places * waves` -- 48,384 per
place at `t = 6912, places = 32` (waves 7), 193,536 at `t = 13824` (waves 14).
With the derived wave count that is `t * waves ~ t^2 / places^2`: **quadratic in
the tile count** at a fixed place count, and linear in `t` if `waves` is pinned
by argument instead. Every one of those calls is rank-local. The column stage is
built **first**, before any of it, so the place starts computing after
`O(tpg + places * waves)` calls rather than after the whole row-side wiring; the
row side waits on the exchange either way.

## Wiring

A column tile's block is `[matrix row][column of the tile]`; row-major over the
matrix rows is what makes a destination place's whole share of it one contiguous
run, so the pack copies it per column tile rather than per row. A transferred
block is `[source's column tile][row of the destination][column]`, which is
exactly the order a row tile of that destination walks -- for one of its rows,
each (source place, wave, column tile) contributes one contiguous run, so the
row tile gathers rather than searches.

Every object is reclaimed by the task that can know it is finished with: the
per-wave reaper frees the column tiles, the place's join frees the arrivals and
the sticky rendezvous events that carried them, the join frees the partials, the
finisher frees the reports and the event grid. Nothing outlives its readers.

The rendezvous events are STICKY by necessity, not preference: their one waiter
is registered inside the destination place's own init task while the producer
satisfies from the source place's pack, and nothing orders the registration
before the satisfy. Every intra-place event is single-fire, and every one of
their consumers registers before the producing task is created -- which is why
the forwarders are the last thing a place's builder creates.

## Flow

Column tiles have no dependences and run as soon as their place is created. A
wave's packs fire when that wave's tiles are done -- not when the place's are --
the wave's reaper when all of that wave's packs have signalled, a forwarder when
its one transfer lands, a row tile when every arrival for its place has been
forwarded, the join on its place's partials, and the finisher on the `P`
reports.

## Placement (base)

A place is mapped to rank `place * nranks / places` and everything the place
creates carries that hint. That is the only thing the program asks the machine,
and it asks it for a hint: the decomposition above it is fixed by argument, so
where a place lands changes nothing about what the program is.

Blocks bound for a peer are created on the peer's home, since a block consumed
exactly once cannot amortize an ownership migration.

## Sizing

**Width.** The column stage and the row stage are `t` tasks each, all
dependence-free within their stage. The exchange stage is `places * waves *
places` packing tasks, which the derived wave count holds at or above `t`.
Because a tile is an index range, `t` can be set to any integer multiple of the
3456 persistent units the largest geometry provides. The control-only stages are
narrower and carry no data: `places * waves` reapers and `places^2 * waves`
forwarders.

**Memory (1 node).** Live payload is bounded structurally, not by scheduling
order: the column form of the matrix is `16 * 2^power` bytes and the arrival
form is another `16 * 2^power`, and no third form of the data exists, so

    peak payload  =  32 * 2^power bytes

whatever order the scheduler interleaves the stages in -- 137.4 GB at `power`
32, 274.9 GB at 33, which is what excludes 33. Two much smaller terms ride on
top, both charged to a node only for the places resident on it, because both the
waiting task and the event it waits on are created on the place's own rank:

    scratch       =  32 * max(N1, N2) bytes per running task
                     (~0.24 GB at power 32 over 112 threads)
    pending edges ~  72 bytes * t * places * waves
                     (0.11 GB at power 32, t = 6912, waves 7; 0.45 GB at
                      t = 13824, waves 14; places = 32, all places co-resident)

The 72 bytes per pending edge is `sizeof(arts_edt_dep_t)` (40 -- the slot in the
waiting task's dependence array, live from the task's creation until it runs)
plus `sizeof(arts_event_dep_s)` (32 -- the waiter record on the event, live from
registration until the event fires): the two structures the runtime allocates
per registered dependence.

One term is charged elsewhere. The waiter records on the **rendezvous** events
live on the rank that created them (`mainEdt`'s rank) at every geometry, not on
the places that use them. There is exactly one per transfer -- `places^2 * waves`
records, 0.23 MB at `places = 32, waves = 7` -- so that concentration is bounded
by the place count and is negligible. Binding the readers to those events
directly, with no forwarder, would put `t * places * waves` records there
instead.

`t` and `places` are payload-neutral: the column form is `N1*N2*16` and the
arrival form `N1*N2*16` at every tile and place count. They move the *block
size* -- a column tile is `16 * 2^power / t` bytes and a transfer block is
`16 * 2^power / (places^2 * waves)` -- and the arrival count a row tile takes,
`places * waves`, which is what the bookkeeping term above follows.

**Messages.** The transfer count is `places^2 * waves` blocks, and the exchange
control traffic is `places^2 * waves` registrations plus `places^2 * waves`
deliveries through the rank that homes the rendezvous events (7168 of each at
`places = 32, waves = 7`). All three are independent of the tile count; nothing
else in the program crosses a rank boundary except the `places` builder tasks,
the grid block they read and the `places` reports.

**Calibration.** `calibration pending` -- the decomposition changed (the
repacking stage is gone, the exchange is task-per-block, and the arrivals reach
their readers through a per-place forwarder), so no earlier timing on this row
carries over and the trend must be re-taken before the campaign size is fixed.
