# reduction_example

*A one-step global sum: every rank contributes a one-integer block holding
its rank id, and one EDT on rank 0 with one slot per rank adds them.*
Source: `examples/basics/reduction.c`, written against the public `arts.h`
alone.

## Overview

`main_edt` reserves the GUID of the `reduction` EDT on rank 0, creates
`shut_down` on rank 0, creates `reduction` under the reserved GUID with one
slot per rank and `shut_down`'s GUID as its parameter, and creates one
`node_setup` EDT on every rank, passing it its rank id and the reduction's
GUID. Each `node_setup` creates a one-integer block, writes its rank id into
it, releases it and hands it to the reduction's slot numbered by its rank.
When the last block arrives, `reduction` adds the values, destroying each
block once it has read it, and delivers the total to `shut_down` as a slot
value; `shut_down` prints it and shuts the runtime down. The printed total
is the sum of the rank ids, `n(n − 1)/2` on `n` ranks.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`reduction_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

None: the rank count is the program's only input.

## Structure

On `n` ranks:

| object | count |
|---|---|
| `node_setup` | `n`, one per rank |
| `reduction` | 1, with `n` slots |
| `shut_down` | 1 |
| data blocks | `n`, one integer each, every one destroyed by `reduction` |

## Wiring

The reduction's GUID is reserved before any `node_setup` exists and travels
to each of them as a parameter, so a `node_setup` on any rank names it; a
satisfy that reaches rank 0 before the reduction is created there is held
until it is. Rank `r`'s block reaches the reduction's slot `r` read-only,
after its creator's release. The total reaches `shut_down`'s one slot as the
64-bit value of a `DB_MODE_NULL` satisfaction, which `shut_down` reads back
from `depv[0].guid`. No event is involved.

## Flow

`main_edt` creates the three kinds of EDT; the `node_setup` EDTs then run on
their ranks concurrently, and the reduction becomes ready when the last block
lands. `shut_down` prints

```
Result <sum of rank ids>
```

and calls `arts_shutdown`. artsrun reads the value from that line (`Result
(\d+)`); the smoke tests pin it per rank count (0 on one rank, 1 on two, 6 on
four).

## Placement

`reduction` and `shut_down` run on rank 0 and `node_setup` `r` on rank `r`.
A block is created with no hint, so its home is chosen by the build's
no-hint data-block policy, the creating rank by default; on `n` ranks the
reduction then acquires `n − 1` of its blocks from other ranks.

## Sizing

There is no knob: the work is `n` blocks of one integer, so a run measures
only the latency of one gather to rank 0. The printed value depends on the
node count, so the catalog row pins no answer and the consensus compares the
entries at one node count. The smoke test pins `0` on one rank, and `1` or
`6` on its two- and four-rank variants.
