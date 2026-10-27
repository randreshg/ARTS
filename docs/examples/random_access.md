# random_access_example

*HPCC RandomAccess (GUPS) over a table split into tiles: a driver chain on
rank 0 runs the updates in steps, each step generating its random numbers
into a frontier block and then applying them tile by tile, and the final
table is checked against a sequential recomputation.*
Source: `examples/random_access/random_access.c` and `random_access_defs.h`,
written against the public `arts.h` alone.

## Overview

The table holds `TABLESIZE` 64-bit words, word `i` starting at `i`, split
into tiles of `tile_size` words (each tile block carries one extra trailing
word). `NUPDATE` updates `table[r & (TABLESIZE − 1)] ^= r`, with `r` the
HPCC random sequence, run in steps of at most 16 384 updates. In a step,
every tile gets a `hpcc_starts_tiled` EDT; only the one with index 0 has
updates in range, and it computes each update's random number by HPCC's
jump-ahead from the step's offset into the frontier block, together with a
per-tile count. `update_driver` then collects the tiles and hands each to an
`update_edt`, which scans the step's random numbers and applies those that
fall in its tile; when every tile is done, the next `random_driver` starts
the next step. After the last step `sync_edt` on rank 0 prints the elapsed
time, recomputes the whole table sequentially, compares every word and
prints `Verified!` or `<n> errors of <m>!`, then `GUPS: <updates per ns> MB:
<table size>`.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`random_access_example_arts_<variant>`), and no reference runtime
has a program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `argv[1]` | tile size in words, default 40 960 | optional; it must be non-zero, at most `TABLESIZE` and divide it, or the program prints its usage and shuts down |

The problem itself is fixed at compile time, each value overridable with
`-D`: `TABLESIZE` (2 621 440 words, 20 MiB; not a power of two, so the
mask `r & (TABLESIZE - 1)` reaches 2²⁰ of its words, and the replay masks the
same way) and `NUPDATE` (`16 × TABLESIZE`). The smoke test is the same
program compiled with `TABLESIZE=4096`, run with tile size 512: four steps
over eight tiles.

## Structure

With `T = TABLESIZE / tile_size` tiles and `S = ⌈NUPDATE / 16384⌉` steps:

| object | count |
|---|---|
| tile blocks | `T`, `tile_size + 1` words each |
| frontier block | 1, `T + 16 384` words: the per-tile counts, then the step's random numbers |
| `random_driver` | `S + 1`, the last one only ending the run |
| `hpcc_starts_tiled`, `update_edt` | `S × T` each |
| `update_driver` | `S` |
| `sync_edt` | 1, with `T + 1` slots |

The defaults give 64 tiles and 2 560 steps.

## Wiring

A `random_driver` takes the frontier RO on slot 0 and, from the second step
on, one control slot per tile, filled by the previous step's `update_edt`s.
`hpcc_starts_tiled[i]` takes tile `i` RW and the frontier — RW for index 0,
the step's only writer, RO for the others, which never read it. Each one
releases what it holds (the frontier, and its tile) before it hands its tile
RW to `update_driver`'s slot `i + 1`; slot 0 is the frontier, RO, from the
`random_driver`, which releases its own RO hold of the frontier before any
hand-off. `update_driver` releases the frontier first and each tile before it
wires it RW to a new `update_edt[i]`, together with the frontier RO. An
`update_edt` applies its updates, releases its frontier and tile and fills
one control slot of the next `random_driver`. Every RO holder releases the
frontier before the hand-off that leads to `sync_edt`, which destroys it.
`sync_edt`'s tile slots are wired RW at start-up and acquired only when its last slot, filled by the final
`random_driver`, makes it ready. Since one writer per step holds the
frontier and the driver chain orders the steps, every block's write
acquisitions are ordered.

## Flow

`main_edt` on rank 0 reads the tile size, creates the tiles (filled with
their indices) and the zeroed frontier, releases each once it is filled, and
reserves the GUIDs of the first `random_driver` and of `sync_edt`. It then
takes the start time, refuses a bad tile size, creates the first
`random_driver` and `sync_edt` and wires them. Each `random_driver` creates
the next one, the step's `update_driver` and its `hpcc_starts_tiled` EDTs;
the one that finds no update left satisfies `sync_edt`. `sync_edt` checks
the table, prints its lines, frees its scratch table and the rank-0 GUID
tables, destroys every tile and the frontier, and shuts the runtime down.

## Placement

Under the default no-hint home (the creator) the blocks are homed on rank 0. Every `random_driver`,
`update_driver` and `sync_edt` runs on rank 0; `hpcc_starts_tiled[i]` and
`update_edt[i]` run on rank `i mod N`, so under that home the frontier's writer
runs on the frontier's home. On more than one rank a tile moves each step to
rank 0 for `update_driver` and back to its `update_edt` rank (under a retained
write grant a tile already on its rank does not travel to its
`hpcc_starts_tiled`; under PURGE it goes home at every release); every `update_edt` and every RO `hpcc_starts_tiled`
off rank 0 receives a copy of the frontier.

## Sizing

A step's critical path is index 0's serial jump-ahead over 16 384 updates;
each `update_edt` scans all of the step's random numbers, so the step's
apply work grows with the tile count while its useful updates do not. The
number of steps is `NUPDATE / 16384` whatever the rank count. `sync_edt`
allocates a second table of `TABLESIZE` words on rank 0 and replays all
`NUPDATE` updates sequentially. The catalog row runs the compile-time default
with the default tile size; its verdict is the `Verified!` line, and it pins
no value.
