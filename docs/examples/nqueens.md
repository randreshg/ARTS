# nqueens_example

*N-Queens by exhaustive search as a tree of EDTs: each partial board is a
data block handed to one fork EDT per safe square, and a join per fork sums
its children's solution counts.*
Source: `examples/basics/nqueens.c`, written against the public `arts.h`
alone.

## Overview

A `fork_nqueens` EDT receives a partial board, queens placed in rows
`0 .. row − 1`, as a block on its one read-only slot. A complete board
(`row == n`) reports 1 and a board whose row is out of range reports 0.
Otherwise the fork lists the columns of the next row that no placed queen
attacks: with none it reports 0, and with some it creates a `join_nqueens`
with one slot per safe column and, per column, a child board (the placed
rows plus the new queen), which it releases and hands to a child fork. A
fork destroys its own board once its children's boards exist, since they
never read it, and a leaf destroys its board before it reports. A join sums
its children's counts as a 64-bit integer and reports to its parent; the
root's count reaches `final_nqueens`, which prints the count, the elapsed
time and, for `n < 19`, the known count and a verdict.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`nqueens_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `argv[1]` | `n`, the board size, 1 to 20 | required: without it the program prints its usage and shuts down; a value outside 1..20 prints the bound and shuts down |

The program's table of known counts covers `n ≤ 18`, so the verdict line
appears for `n < 19` only.

## Structure

For a board size `n`, with `B(n)` the number of boards the search visits (the
empty board plus one per safe placement) and `J(n)` the number of those with
at least one safe column in their next row:

| object | count |
|---|---|
| `fork_nqueens` | `B(n)`, one per board |
| `join_nqueens` | `J(n)` |
| `final_nqueens` | 1 |
| data blocks | `B(n)`, one board each, every one destroyed by the fork that received it |

`B(8) = 2 057`, `J(8) = 1 321`; `B(10) = 35 539`, `J(10) = 22 765`.

## Wiring

A fork's board arrives on its slot read-only, after its creator released it.
Counts travel as slot values (`DB_MODE_NULL`): a leaf fork or a join
delivers its count to slot `i` of its parent's join, `i` being the index of
its column among the parent's safe columns, or the root's count to
`final_nqueens`' one slot. No event is involved: every delivery fills one
slot of an EDT that already exists.

## Flow

`main_edt` checks `n`, prints `Solving <n>-Queens problem` and `Using ARTS`,
records the start time (handed to `final_nqueens` as a parameter), creates
`final_nqueens` and the root fork on rank 0, and creates, fills, releases and
hands over the empty board. The search then unfolds and folds back
concurrently. `final_nqueens` prints

```
Results:
Execution time: <seconds> seconds
Number of solutions: <count>
Expected solutions for <n>-queens: <known count>
✓ Result verified!
```

with `✗ Result incorrect!` in place of the last line when the count differs
from the table, and without the last two lines for `n ≥ 19`; then it calls
`arts_shutdown`. artsrun reads the count from `Number of solutions: (\d+)`.

## Placement

`final_nqueens` and the root fork run on rank 0. A fork on rank `c` places
its join on `c`, and the child for its `i`-th safe column on
`(c + i) mod N` while the child's row is at most 2, on `c` below that: the
first two levels of the tree spread over the ranks and every deeper subtree
stays on the rank of its row-2 ancestor. A board is created with no hint, so
its home is chosen by the build's no-hint data-block policy, the creating
rank by default, and only boards of rows 1 and 2 are read from another rank.

## Sizing

`n` is the only knob, and the search grows faster than exponentially in it
(`B(12) = 856 189`). Each fork does at most `n²` comparisons and creates at
most `n` small blocks, so a run is dominated by EDT and block traffic. The
smoke test runs `n = 8` (92 solutions); the catalog row runs `n = 10` and
pins its answer, `724`, at every node count.
