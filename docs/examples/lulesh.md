# lulesh_example

*The LULESH 2.0 Sedov blast on a cube of hexahedral elements, cut into
tiles of six data blocks each; an iteration is four per-tile phases wired by
per-tile events and neighbour latches, then a time-step reduction on rank 0
that launches the next iteration.*
Source: `examples/proxy/lulesh/lulesh.c`, written against the public
`arts.h` alone.

## Overview

The mesh is `N³` elements, cut into `tpd³` tiles of `T³` elements
(`tpd = N / T`); each tile keeps its own `(T + 1)³` nodes. An iteration runs
four phases per tile: `forces_edt` computes each element's corner forces
from pressure plus artificial viscosity and the hourglass filter;
`reduce_kin_edt` sums the corner forces at the tile's nodes, its own and
those of up to 26 neighbouring tiles, and advances velocities and
positions; `elem_props_edt` computes the new volumes, characteristic lengths
and velocity gradients; `visc_eos_time_edt` computes the monotonic
artificial viscosity from its own and its face neighbours' gradients,
evaluates the equation of state and the tile's Courant and hydro time-step
limits. `reduce_dt_edt` on rank 0 then prints `iteration <k>, delta time
<dt>, energy <e>` (the energy of the origin element), applies LULESH's
time-step controller and starts the next iteration, until the simulated
time reaches 10⁻² or the iteration limit. The run ends with `Run
completed:`, the problem size, the iteration count, `Final Origin Energy =
<e>` and the simulated elapsed time.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`lulesh_example_arts_<variant>`), and no reference runtime has a
program for it. A second build, `lulesh_example_prof` (`-DLULESH_PROFILE`),
also prints the time rank 0 spends launching iterations; it is not a test.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `-s N` | elements per edge, default 30 | must be positive |
| `-i n` | iteration limit, default 9 999 999 | the run also stops at simulated time 10⁻² |
| `-t T` | elements per tile edge | absent or non-positive: `T = N / tpd` for the smallest `tpd` that divides `N` and is at least 5 (at least `⌈∛ranks⌉` when that is larger); one tile when `N` is below that |

`N` must be divisible by `T` and the tile count `tpd³` at most 1 000;
otherwise the program prints an error and shuts down. `main_edt` reads the
arguments on rank 0. The smoke test runs `-s 10 -i 10`: 125 tiles of 8
elements for ten iterations.

## Structure

With `nt = tpd³` tiles:

| object | count |
|---|---|
| blocks per tile | 6: constants and connectivity (`const`), node positions and velocities (`pos_vel`), element state (`elem_state`), gradients and viscosity terms (`grad`), corner forces plus the state the next `forces_edt` reads (`force`), time-step limits (`dt_result`) |
| set-up | `nt` `init_tile_edt`, one `init_done` latch of count `nt`, one `post_init_edt` |
| events per iteration | `7·nt + 2`: `fd`, `rd`, `ed`, `vd` per tile (single-fire), `rr` per tile (latch of 1 + its neighbours), `er` and `vr` per tile (latch of 1 + its face neighbours), `setup_done`, and `dr` (latch of `nt`) |
| EDTs per iteration | `4·nt` phase EDTs, one `reduce_dt_edt`, one `next_iter_edt` |

## Wiring

`forces(t)` waits on `setup_done` and takes `const` and `pos_vel` RO and
`force` RW; it satisfies `fd[t]`. `rr[t]` collects the `fd` of `t` and of
every neighbour; `reduce_kin(t)` waits on it and takes its own and its
neighbours' `force` RO, `const` RO and `pos_vel` RW, then satisfies `rd[t]`.
`er[t]` collects the `rd` of `t` and its face neighbours; `elem_props(t)`
takes `const` and `pos_vel` RO and `grad` RW, then satisfies `ed[t]`. `vr[t]`
collects the `ed` of `t` and its face neighbours; `visc_eos(t)` takes its own
`grad` RW (it writes the viscosity terms there), its face neighbours' `grad`
RO, `const` RO, and `elem_state`, `dt_result` and `force` RW, then satisfies
`vd[t]`. `dr` collects every `vd`; `reduce_dt` takes every tile's
`dt_result` RO and tile 0's `const` RO. Each phase EDT releases the blocks
it wrote, and the blocks it read, before it satisfies its event
(`post_init` and a `reduce_dt` that continues do so before they create the
next iteration), so the final `reduce_dt`'s destroys follow every other
hold's release.

## Flow

`main_edt` checks the arguments, prints the decomposition, builds the tile
topology on rank 0, reserves the six block GUIDs of every tile on the tile's
owner, creates `init_done` and `post_init_edt` and an `init_tile_edt` per
tile, which takes its tile's GUIDs, neighbour counts and face directions as
parameters. An `init_tile_edt` creates and fills its tile's blocks under the
reserved GUIDs, releases them and decrements `init_done`.
The mass of each of its nodes is an eighth of the volume of every element
around the node, its neighbouring tiles' elements included: every tile
computes them from the initial mesh, as the reference does on its global
mesh, so a node shared by several tiles has the same mass in each.
`post_init_edt` computes the initial time step from tile 0 and starts
iteration 0. Each `launch_iteration` (on rank 0) creates the iteration's
events, wires them, creates the phase EDTs phase by phase and `reduce_dt`,
then satisfies `setup_done`. `reduce_dt` prints the iteration line, computes
the next time step and destroys the iteration's events, which have all fired
by then; it then either creates `next_iter_edt` for the next iteration, or
prints the summary, destroys every tile's blocks and `init_done`, and shuts
the runtime down.

## Placement

Tile `t` belongs to rank `t` mod the rank count: its blocks are reserved for
and created on that rank, and its four phase EDTs run there. Every event,
`post_init`, `reduce_dt` and `next_iter` is on rank 0, so every phase's
completion and every launch goes through rank 0. Across ranks, `reduce_kin`
reads the `force` blocks of all its neighbouring tiles, `visc_eos` the `grad`
blocks of its face neighbours, and `reduce_dt` every tile's `dt_result`.

## Sizing

Work grows with `N³` and the iteration count; the default `-s 30` takes 932
iterations in the reference LULESH 2.0.3. Each iteration's `4·nt + 1` EDTs
and `7·nt + 2` events are created serially by `launch_iteration` on rank 0,
the part `lulesh_example_prof` measures, so small tiles make an iteration
latency-bound. The run reproduces the reference LULESH 2.0.3 at every tiling
tried and any node count: `-s 30` ends after 932 iterations at `Final Origin
Energy = 2.025075e+05`, and the smoke test's `-s 10 -i 10` at
`2.596764e+05`, which its pass expression pins. The reference prints each
iteration's line before the step and this program after it, so the energy
on the reference's line `k + 1` is the one on this program's line `k`. The
catalog row reads the final origin energy with a relative tolerance of 10⁻⁶
and pins 2.025075e+05 at its own arguments, `-s 30`.
