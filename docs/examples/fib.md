# fib_example

*Recursive Fibonacci as a tree of EDTs: one fork EDT per call, one two-slot
join EDT per internal call, every result carried up as a slot value rather
than in a data block.*
Source: `examples/basics/fib.c`, written against the public `arts.h` alone.

## Overview

Computes `fib(n)` by binary recursion. Every call is a `fib_fork` EDT: a call
with `n < 2` delivers `n` to its parent at once; any other call creates a
`fib_join` EDT that will sum its two children, then the two child forks for
`n - 1` and `n - 2`. A join adds the two values it received and delivers the
sum to its own parent, so the tree unfolds top-down through the forks and
folds bottom-up through the joins until `fib_done` receives the root's value,
prints it and shuts the runtime down. The program creates no data block:
each result travels as the 64-bit value of a `DB_MODE_NULL` slot
satisfaction (`arts_edt_satisfy_slot`), which the receiving EDT reads back
from `depv[i].guid`.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`fib_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `argv[1]` | `n`, the Fibonacci argument | required: without it the program prints its usage and shuts down |

A join sums its two values as 32-bit unsigned integers, so the printed value
is `fib(n)` exactly up to `n = 47` and wraps modulo 2³² above it.

## Structure

For an argument `n`, with `C(n)` the number of calls of the recursion
(`C(n) = 2·fib(n + 1) − 1`):

| object | count |
|---|---|
| `fib_fork` | `C(n)` — one per call |
| `fib_join` | `(C(n) − 1) / 2` — one per call with `n ≥ 2` |
| `fib_done` | 1 |
| data blocks | none |

## Wiring

A fork has no dependence, so it is ready as soon as it is created; its three
parameters name the EDT to deliver to, the slot there, and its own `n`. A
join has two slots, one per child, and carries its fork's first two
parameters: where the join's own sum goes. `fib_done` has one slot, which the
root call fills. Every delivery satisfies exactly one slot of one EDT that
already exists — a leaf fork satisfies its parent's join, a join its
parent's join or `fib_done` — so no event and no latch is involved, and each
join becomes ready when its second child's value lands.

## Flow

`main_edt`, the runtime's start EDT on rank 0, reads `n`, creates `fib_done`
on rank 0, takes the start time, and creates the root fork. The forks then
unfold the call tree and the joins fold it back up, all concurrently; the
order in which subtrees finish is the schedule's and does not change the
result. `fib_done` prints

```
Fib <n>: <fib(n)> time: <elapsed> nodes: <ranks> workers: <workers>
```

and calls `arts_shutdown`. artsrun reads the value from that line (`Fib \d+:
(\d+) time:`) and times the run by the runtime's `[E2E]` stamp, not by the
printed elapsed time.

## Placement

`fib_done` and the root fork are placed on rank 0. A fork running on rank `r`
places its join on `r` and its two children on `(r + 1) mod N`, so the calls
at depth `d` of the tree run on rank `d mod N`, and every value a child
delivers to its join crosses to the previous rank. On one rank every EDT runs
on rank 0 and no message is sent; from two ranks on, every non-root call's
creation, and every delivery but the root's to `fib_done`, is remote.

## Sizing

`n` is the only knob: the EDT count grows as `φⁿ` (about `3·fib(n + 1)` EDTs
in all), and each EDT does a few additions, so a run is dominated by EDT
creation and, on more than one rank, by the message per call. The catalog row
runs `n = 20` (21 891 forks, 10 945 joins) and pins its answer, `6765`, at
every node count: a consensus workload that finishes in moments, never a
timing number.
