# fib_db_example

*Recursive Fibonacci as a tree of EDTs whose every value travels in a data
block: each call's argument and each partial sum is a one-integer block that
its creator fills, releases and hands on, and that its last reader destroys.*
Source: `examples/basics/fib_db.c`, written against the public `arts.h` alone.

## Overview

Computes `fib(n)` by the binary recursion of `fib_example`, with one
difference: every value is a data block instead of a slot value. A `fib_fork`
EDT receives its argument as a one-integer block on its single slot,
read-only. A call with `n < 2` hands that same block on to its parent's join.
Any other call destroys its block, creates a `fib_join` that will sum its two
children, creates two blocks holding `n - 1` and `n - 2`, releases them, and
hands each to a child fork. A join reads its two values, destroys both
blocks, creates a block for their sum, releases it and hands it to its own
parent. `fib_done` reads the root's block, destroys it, prints the value and
shuts the runtime down.

Every hand-off follows two rules. A creator releases its block before the
satisfy that exposes it, so the reader acquires the written value on every
coherence protocol. A block is destroyed by the EDT that reads it last, which
runs after every other holder's release: a leaf fork releases the block it
holds read-only before it forwards it to the join that destroys it.

`main_edt` on rank 0 creates `fib_done`, the input block and the root fork,
and releases the input block once it is filled, before it hands the block to
the root fork, by the first rule.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`fib_db_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `argv[1]` | `n`, the Fibonacci argument, read with `atoll` | required: without it rank 0 prints its usage and shuts down |

A value is stored in an `int` and summed as a 32-bit unsigned integer, so the
printed value is `fib(n)` exactly up to `n = 47` and wraps modulo 2³² above
it.

## Structure

For an argument `n ≥ 2`, with `C(n)` the number of calls of the recursion
(`C(n) = 2·fib(n + 1) − 1`):

| object | count |
|---|---|
| `fib_fork` | `C(n)`, one per call |
| `fib_join` | `(C(n) − 1) / 2`, one per call with `n ≥ 2` |
| `fib_done` | 1 |
| data blocks | `1 + 3·(C(n) − 1) / 2`: the input block, two argument blocks per join and one result block per join, each of one integer and each destroyed by its last reader |

## Wiring

A fork has one read-only slot, which takes its argument block; its two
parameters name the EDT to deliver its result to and the slot there. A join
has two read-only slots, one per child, and carries its fork's two
parameters: where the join's own sum goes. `fib_done` has one read-only slot,
which the root call fills. Every delivery is
`arts_add_dependence(block, edt, slot, DB_MODE_RO)` into an EDT that already
exists: an argument block into a child fork, a leaf's forwarded block or a
result block into a parent's join or `fib_done`. No event and no latch is
involved; each EDT is ready once its blocks have arrived, and acquires them
then.

## Flow

Every process prints `Starting ArtsRT with DB-based Fibonacci` before the
runtime starts. `main_edt` on rank 0 prints `Using default DBs`, reads `n`,
creates `fib_done` on rank 0, creates, fills and releases the input block,
takes the start time, creates the root fork on rank 0 and hands it the input
block. The forks then unfold the call tree and the joins fold it back up, all
concurrently; the order in which subtrees finish is the schedule's and does
not change the result. `fib_done` prints

```
Fib <n>: <fib(n)> time: <elapsed> nodes: <ranks> workers: <workers>
```

and calls `arts_shutdown`. artsrun reads the value from that line (`Fib \d+:
(\d+) time:`) and times the run by the runtime's `[E2E]` stamp, not by the
printed elapsed time.

## Placement

`fib_done` and the root fork are placed on rank 0. A fork running on rank `r`
places its join on `r` and its two children on `(r + 1) mod N`, so the calls
at depth `d` run on rank `d mod N`. A block is created with no hint, so its
home is chosen by the build's no-hint data-block policy, the creating rank by
default: an argument block is homed on its parent's rank and read by the
child on the next rank, a result block is homed on its join's rank and read
by the parent's join on the previous rank, and a leaf's forwarded block is
read by its parent's join on the block's own home. On one rank everything is
local; from two ranks on, every argument block and every non-root result block
is acquired from another rank, while the input block and the root's result
stay on rank 0.

## Sizing

`n` is the only knob: the EDT and block counts grow as `φⁿ` (about
`3·fib(n + 1)` EDTs and `3·fib(n + 1)` blocks in all), and each EDT does a few
additions, so a run is dominated by EDT creation and the block life cycle
(create, release, acquire, destroy), and on more than one rank by moving one
small block per call. The smoke test runs `n = 15` (2 959 blocks); the
catalog row runs `n = 20` and pins its answer, `6765`, at every node count: a
consensus workload that finishes in moments, never a timing number.
