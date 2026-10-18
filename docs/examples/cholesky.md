# cholesky_example

*Tiled right-looking Cholesky factorization `A = L·Lᵀ` of a symmetric
positive-definite matrix: one EDT per tile kernel per step, and each tile's
successive versions carried by one event per tile and step.*
Source: `examples/dense/cholesky.c`, written against the public `arts.h`
alone; a native port of the OCR `cholesky` example
(`third_party/ocr-apps/apps/examples/cholesky/cholesky.c`).

## Overview

The lower triangle of an `N×N` matrix is cut into `T×T` tiles, `n = N/T`
per side. For each step `k = 0 .. n − 1`:

- `sequential_cholesky_edt` factors the diagonal tile `(k, k)` into
  `L(k, k)`;
- `trisolve_edt` turns each tile `(j, k)`, `j > k`, into `L(j, k)` using
  `L(k, k)`;
- `update_diagonal_edt` updates each tile `(j, j)`, `j > k`, with `L(j, k)`;
- `update_nondiagonal_edt` updates each tile `(j, i)`, `j > i > k`, with
  `L(j, k)` and `L(i, k)`.

The two update kernels modify their tile in place and forward that block as
the tile's next version. The two factor kernels write a new block for `L`;
the tile they consumed, which no later task reads, is destroyed by them.
`wrap_up_edt` on rank 0 receives the final `L` tiles, computes
`trace(L·Lᵀ)`, the sum of the squares of `L`'s entries, compares it with
`trace(A)`, prints the verdict, destroys every event, frees the event table
and destroys the `L` tiles, and shuts the runtime down.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`cholesky_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `--ds N` | matrix size, default 100 | read with `atoi`; a non-positive or non-numeric value prints the usage and shuts down |
| `--ts T` | tile size, default 50 | as `--ds`; a `T` that does not divide `N` prints a message and shuts down |
| `--fi <file>` | the matrix as `N·N` native doubles, row-major | optional: without it the program generates `A[i][i] = 2N`, `A[i][j] = 0.1·((i + j) mod 7)`; a file it cannot open or read in full prints a message and shuts down |

## Structure

With `n = N/T` and `t = n(n + 1)/2` lower-triangular tiles:

| object | count |
|---|---|
| events | `t·(n + 1)`: one per tile and version `0 .. n`, all created by `main_edt` on rank 0; those past a tile's last version are never satisfied |
| input tiles | `t`, created by `main_edt`, each destroyed by the factor kernel that consumes it |
| `L` tiles | `t`, one per tile, each destroyed by `wrap_up_edt` |
| `sequential_cholesky_edt` | `n` |
| `trisolve_edt`, `update_diagonal_edt` | `n(n − 1)/2` each |
| `update_nondiagonal_edt` | `n(n − 1)(n − 2)/6` |
| `wrap_up_edt` | 1, with `t` slots |

## Wiring

Event `EV(i, j, k)` carries version `k` of tile `(i, j)`: version 0 is the
input tile, and the kernel of step `k` that writes tile `(i, j)` satisfies
`EV(i, j, k + 1)` with the block it produced. Each kernel takes its own
tile's version-`k` event read-write on slot 0 and the `L` tiles it reads,
version `k + 1` of their tiles, read-only on the following slots. A tile's
last version is `EV(i, i, i + 1)` on the diagonal and `EV(i, j, j + 1)`
below it, and these are `wrap_up_edt`'s slots. Every kernel releases the
block it hands on before it satisfies the event, so the block is the event's
data and every reader acquires its written value, and it releases the `L`
tiles it read before that satisfy too, since `wrap_up_edt` destroys them. The events are default
latches (one satisfaction fires them, and they keep serving later
dependences); one version may feed several readers, such as `L(k, k)`, read
by every `trisolve_edt` of step `k` and by `wrap_up_edt`. `main_edt` creates
and wires every EDT before it satisfies any version-0 event.

## Flow

`main_edt` parses the arguments, generates or reads the matrix, adds up its
diagonal and prints

```
Cholesky-ARTS: N=<N> tile=<T> numTiles=<n> nodes=<ranks>
```

then creates the events and the EDTs, and creates, fills and releases each
input tile and satisfies its version-0 event. The kernels then run as their
versions arrive. `wrap_up_edt` prints

```
Cholesky-ARTS: N=<N> tile=<T> numTiles=<n> trace(L L^T)=<t> expect=<trace(A)> rel_err=<e> PASS
```

with `FAIL` in place of `PASS` when the relative error is not below 1e-6,
and calls `arts_shutdown`. A non-positive pivot prints
`Cholesky: non-SPD at k=… kB=… val=…` and shuts down instead. artsrun
requires `PASS` in its completion marker and reads the trace from that line.

## Placement

Tile `(i, j)` is owned by rank `((i(i + 1)/2 + j) / 8) mod N_ranks`: the
row-major index of the lower triangle, dealt out round-robin in chunks of
eight. Each kernel runs on the owner of the tile it writes, and a factor
kernel creates its `L` block there. `main_edt`, the events and `wrap_up_edt`
are on rank 0, and the input tiles are created by `main_edt` with no hint,
so their home is chosen by the build's no-hint data-block policy (rank 0 by
default). With `n ≤ 3` (six tiles or fewer) everything runs on rank 0.

## Sizing

`N` and `T` are the knobs: the work is about `N³/3` multiply-adds in
kernels of `T³`, the EDT count grows as `n³/6` and the event count as
`n³/2`. The defaults (100, 50) make 2×2 tiles on rank 0 alone. The smoke
test runs `--ds 160 --ts 20` (8×8 tiles, 36 lower tiles spread over every
rank); the catalog row runs `--ds 1000 --ts 50` (20×20 tiles, 1 540 kernels)
and compares the printed trace across entries without pinning it. The check
sees the diagonal kernels only: `trace(L·Lᵀ)` is the sum of the squares of
`L`'s entries, which equals `trace(A)` whatever the off-diagonal tiles hold, so
a wrong value from `trisolve` or `update_nondiagonal` passes it.
