# pi_hpx

*One integration block per locality, one reduce — the smallest row in the
HPX-origin section and its liveness smoke.*
Origin: HPX's `distributed_pi` collectives example
(`third_party/hpx/libs/full/collectives/examples/distributed_pi.cpp`, pin
`v1.11.0`), copied to `benchmarks/hpx/pi_hpx/distributed_pi.cpp` and built
as `pi_hpx_hpx`.

## Overview

Approximates the integral of `4/(1+x^2)` over `[0,1]` with a left-endpoint
Riemann sum: `N` is broadcast to every locality, each locality sums its own
contiguous block `[begin, end)` of the `N` intervals, and the partial sums
are combined with a `reduce`. It is an HPX-origin catalog row: one program
on four runtimes — `pi_hpx_hpx` is the HPX program,
`pi_hpx_arts_<variant>`, `pi_hpx_xsocr` and `pi_hpx_ocrvx` are the OCR
mirror (`benchmarks/apps/hpx_origin/pi_hpx.c`), one row in `hpx_apps.yaml`.
HPX primitives used: `hpx::collectives::broadcast`/`reduce` over the world
communicator, `hpx::init`/`hpx_main`. The mirror's mapping: one EDT per
rank computing its block into an 8-byte DB, wired directly into a
fixed-arity `reduce_edt` — the collectives become dependence edges instead
of blocking calls.

Because the sum is a left-endpoint rule (`i * h`, not a midpoint), its
truncation error is `O(h)` rather than `O(h^2)`; at `N = 1e8` that puts the
printed value, `3.14159266359028`, off from pi starting at the ninth
significant digit (the eighth decimal place). That is the algorithm's own value, not a rounding of
pi — see Sizing.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| positional `N` | number of Riemann-sum intervals | 1,000,000 | reachable — read on locality 0 in `hpx_main`, then broadcast to every locality; the mirror's `mainEdt` parses the same positional argument and passes it directly in every task's paramv |

A missing, non-numeric or zero `N` prints usage and calls `ocrShutdown()`
with status 0, where the origin's argument parsing throws or otherwise exits
non-zero; the missing `[APP_E2E]`/`pi:` marker fails the cell either way.

## Structure

`nl` = the number of ranks (localities). Per run:

| object | count |
|---|---|
| `block_edt` | `nl` — one per rank, each producing one 8-byte `double` DB |
| `reduce_edt` | 1, with `nl` RO dependences |

No separate argument DB is needed: `mainEdt` already knows `N` and every
rank's index before creating any task, so both travel as paramv words
instead of a broadcast payload.

## Wiring

Reply-by-dependence into a fixed `nl`-slot `reduce_edt` — one dependence
per block, no latch needed since the arity (`nl`) is fixed and known
before any block starts. HPX's equivalent is the world-communicator
`reduce`. `reduce_edt` destroys the `nl` partial blocks after summing them;
each has exactly one consumer, and nothing outlives the reduction.

## Flow

`mainEdt` validates `N` (or takes the default), computes `nl` via
`ocrAffinityCount`, creates `reduce_edt` with `nl` inbound RO slots hinted
at rank 0, then creates one `block_edt` per rank. When the last block's DB
satisfies `reduce_edt`, it sums the partial results, prints
`pi: <value>`, and shuts down.

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::collectives::broadcast(...)`, called on every locality | start wait — every locality blocks until it has `N` before computing | no wait needed: `mainEdt` already knows `N` and every rank's index, and passes both directly in `block_edt`'s paramv at creation |
| `hpx::collectives::reduce(...)`, called on every locality (only locality 0's return value is used) | end wait — the join of every locality's partial sum | `reduce_edt`, a fixed `nl`-slot dependence join hinted at rank 0 |

**Mid waits: 0.** Both sites above are start or end waits; no stage body
blocks on a future and then computes.

`[APP_E2E]` on the HPX side opens with `arts_hpx::run_clock` right after the
broadcast that seeds every locality with `N` and closes right after the
reduce that produces the final value — so the broadcast itself, the
program's own start-up collective, is excluded and the reduce is included.
`[HPX]` prints from `print_geometry()` before the broadcast. The mirror
stamps at the top of rank 0's block task, which `mainEdt` creates first, so
the creation and remote dispatch of every other rank's block task can fall
*inside* the mirror's interval while it never falls inside the HPX side's —
an asymmetry that stays small at the pinned size, where the block loop
dominates. The runtime's own `[E2E]` is still printed on both sides and kept
as a separate observation; `print_e2e`'s single-argument form makes
`[APP_E2E] == [E2E]` on the HPX side.

The `pi:` result line is a single `arts_hpx::write_stdout_line` write of a
pre-formatted buffer rather than a multi-piece `std::cout <<` sequence:
the origin's own `[E2E]` marker is an unbuffered write on the same rank,
and a buffered stream's write can otherwise be split across the piece
boundaries and interleaved with it under mpirun's demux — one write per
line is what removes the race.

## Placement

One task per locality by construction — the row is exempt from the width
rule and serves as the HPX entry's smallest consensus cell — so the origin
has no placement freedom beyond `broadcast`/`reduce`'s own world-communicator
membership. The mirror places each `block_edt` at
`ocrAffinityGetAt(AFFINITY_PD, rank)` for `rank` in `[0, nl)`, and
`reduce_edt` at rank 0: the same one-block-per-locality shape, expressed as
EDT affinity hints instead of communicator membership. The partial-sum block
carries an explicit home hint too — the rank that computes it, which is
where the origin's future value sits before it travels to the reduction — so
the placement is the program's statement rather than whatever the build's
no-hint DB policy (`ARTS_NOHINT_DB_HOME` / `ARTS_SHIM_NOHINT_DB_HOME`)
happens to be.

## Sizing

`N` is the only dial, and it scales work per locality (`N/nl` loop
iterations each), not object count — there is exactly one block per
locality regardless of `N`. The `hpx-gate` roster and the catalog's pinned
`args`/`expect_args` both run `100000000`; at that `N` both sides print
`pi: 3.14159266359028`, the left-endpoint rule's own truncation value —
not fifteen digits of pi — so the catalog's `expect` is pinned to what the
unmodified algorithm actually computes, and is not to be "corrected"
toward pi. Being exempt from the width rule, the row's size is `N` alone; the
calibrated value is at the end of this section.

**`N` must be a multiple of every node count the profile sweeps.** The
origin's block size is `N / num_localities` and the `N % num_localities`
trailing terms are simply dropped, which the mirror carries unchanged (A4:
the arithmetic is not repaired). A dropped term near `x = 1` is worth about
`4/N`, well above the catalog's `1e-09` tolerance, so a node count that does
not divide `N` moves the scalar off the pin on all four runtimes at once and
the row fails the expect check for a reason nothing in the failure names.
`100000000` divides every node count of the current profiles; a new node
count (3, 6, …) or a new `N` has to be chosen against this constraint
before it is measured.

**Calibrated arguments.** `72000000000` at every node count: a multiple of
32 × 10⁸, so the block `N / num_localities` drops no term at any node count of
the sweep (the constraint above). Derived to the 100 s window from the probe
through the linear law; the sizing pass measured 100.1 s on every ARTS arm and
223 s on HPX (the code-generation gap recorded under parity), 4.4 GB resident.
