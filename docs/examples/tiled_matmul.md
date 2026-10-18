# tiled_matmul_example

*A blocked matrix product as a dataflow of tile blocks: tiles cut from two
matrices, one multiply task per block product, one sum task per result tile,
and a final task that assembles C.*
Source: `examples/dense/tiled_matmul.c`, written against the public `arts.h`
alone.

## Overview

The program computes `C = A·B` for square float matrices of side `MATSIZE`
cut into square tiles of side `TILE`, `b = MATSIZE / TILE` tiles per side.
`main_edt` creates `A` (entry `(i, j)` is `i·MATSIZE + j`), `B` (the
identity) and `C` (zero) under GUIDs it reserves on rank 0, releases them and
builds the graph:

- per tile index `(i, j)`, an `init_block_mm` that cuts that tile of `A` and
  of `B` into two blocks under GUIDs from two ranges reserved on rank 0,
  releases both and decrements a latch event of count `b²`;
- per result tile `(i, j)`, a `sum_mm` with one slot per `k`;
- per `(i, j, k)`, a `multiply_mm` that takes tile `(i, k)` of `A` and tile
  `(k, j)` of `B`, and waits on the latch through a control slot, so that no
  tile is read before every tile is filled.

A `multiply_mm` creates a zeroed partial-product tile, releases it, and
hands its two input tiles (read-only) and the partial tile (read-write) to an
`mm_kernel_cpu`, which accumulates the product into the partial tile,
releases it and delivers it to the `sum_mm`. A `sum_mm` adds its partial
tiles into a new tile, destroying each partial once it has added it, releases
the sum and delivers it to `finish_block_mm`, which copies every sum tile
into `C`, prints the elapsed time, destroys the sum tiles, the tiles of `A`
and `B`, the three matrices and the latch, and shuts the runtime down. The
program neither prints nor checks `C`: its completion line is its result.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`tiled_matmul_example_arts_<variant>`), and no reference runtime has
a program for it.

## Parameters

None at run time. `MATSIZE` (3) and `TILE` (1) are compile-time constants,
and `TILE` must divide `MATSIZE`.

## Structure

With `b = MATSIZE / TILE` (3 as built):

| object | count |
|---|---|
| matrices `A`, `B`, `C` | 3 blocks of `MATSIZE²` floats |
| tiles of `A` and `B` | `2·b²` blocks of `TILE²` floats |
| `init_block_mm` | `b²` |
| `multiply_mm`, `mm_kernel_cpu` | `b³` each |
| partial-product tiles | `b³`, each destroyed by its `sum_mm` |
| `sum_mm` | `b²`, `b` slots each |
| sum tiles | `b²` |
| `finish_block_mm` | 1, with `1 + b²` slots |
| latch event | 1, count `b²` |

## Wiring

- `init_block_mm`: `A` and `B` read-only on slots 0 and 1; its parameters are
  the tile index and the latch.
- `multiply_mm`: its `A` and `B` tiles read-only on slots 0 and 1, wired by
  `main_edt` to the tile GUIDs before the tiles exist (a slot is acquired
  when its EDT becomes ready, not when it is satisfied), and the latch on
  control slot 2.
- `mm_kernel_cpu`: the same two tiles read-only and the partial tile
  read-write; its parameters name the `sum_mm` and the slot `k`.
- `sum_mm`: the `b` partial tiles read-only, slot `k` for the product over
  `k`.
- `finish_block_mm`: `C` read-write on slot 0 and sum tile `(i, j)` read-only
  on slot `1 + i·b + j`; its parameter is the latch.

Every block's creator or writer releases it before the satisfy that hands it
on, and the latch fires once every `init_block_mm` has released its tiles.
Every read-only holder also releases its blocks before the satisfy that leads
to `finish_block_mm`, which destroys them: `init_block_mm` releases `A` and
`B` once it has cut its tiles, `multiply_mm` its two input tiles before it
hands them on, and `mm_kernel_cpu` its two input tiles once the product is
accumulated.

## Flow

`main_edt` prints `Starting`, takes the start time and builds the graph,
printing one `SUMGUID: …` line per `sum_mm` and one `… Signaling: …` line
per `multiply_mm`. The `init_block_mm` tasks fill the tiles; once the latch
fires every `multiply_mm` runs, printing its indices and tile GUIDs, then the
kernels, the sums and the final copy follow. `finish_block_mm` prints its name
and then

```
DONE <elapsed ns>
```

and calls `arts_shutdown`. artsrun takes `DONE \d+` as the completion marker
and reads no value.

## Placement

Every EDT is placed on rank 0, and every block is created under a GUID
reserved on rank 0, so the whole program runs on rank 0; on more than one
rank the other ranks run nothing. The matrix and tile GUIDs and the tile
count live in process globals that only rank 0 sets, which holds because
every EDT that reads them runs there.

## Sizing

`MATSIZE` and `TILE` are the knobs, at compile time: the EDT count grows as
`b³` and each kernel does `TILE³` multiply-adds. As built, 27 products of
1×1 tiles make a dataflow-shape demonstration, not a measurement; the
catalog row runs it as built and pins nothing.
