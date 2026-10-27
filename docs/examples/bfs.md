# bfs_example

*An asynchronous label-correcting breadth-first search over a graph split
into vertex blocks, one per rank: a `relax` EDT per message lowers one
vertex's level by compare-and-swap and, when it wins, messages every
neighbour; a finish scope tells when no message is left.*
Source: `examples/graph/bfs.c`, written against the public `arts.h` and
`arts/graph.h` alone.

## Overview

The vertex range `[0, n)` is split into one block per rank, rank `r`
holding `[r·b, min((r + 1)·b, n))` with `b = ⌈n / N⌉` on `N` ranks, and each
block's edges are one CSR data block, the rank's partition, its rows sorted.
Every rank keeps a level per vertex of its block, all starting at
`2⁶⁴ − 1` (unreached). The search starts by sending level 0 to the source. A
`relax` EDT for vertex `v` with level `L` runs on `v`'s rank: while `L` is
below `v`'s level it tries to swap `L` in, and if a swap of its own
succeeded it sends `v`'s current level plus one to every neighbour of `v`,
each as a new `relax`. A vertex whose level is
lowered again by a shorter path's later message sends again, so the levels
settle at the BFS distances whatever order the messages run in. When no
`relax` is left, `exit_program` on rank 0 prints rank 0's levels.

Every `relax` prints `calling relax` and every message `sending u=<u>,
level= <L>`; the start prints `Kick off`; the end prints `Printing vertex
levels....` and then `Local vertex : <i>, Level : <L>` for each of rank 0's
vertices, `i` being the local index, which on rank 0 is the vertex id.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`bfs_example_arts_<variant>`), and no reference runtime has a
program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `--file <path>` | the graph | required; a file that cannot be read or holds an invalid line, an endpoint at or above `--num-vertices` included, makes each loader print `Failed to load the graph on rank <r>` and shut down |
| `--num-vertices <n>` | the vertex count | required with `--num-edges`: without both, `main_edt` prints the usage and shuts down |
| `--num-edges <m>` | the edge count | recorded in the distribution, not checked against the file |
| `--source <v>` | the vertex the search starts from, default 0 | must be below `n`, or the usage and a shutdown |
| `--flip` | add every edge's reverse | an edge list with each undirected edge once needs it for an undirected search |
| `--keep-self-loops` | keep an edge `v v` | dropped otherwise |
| `--csr-format` | read the adjacency format | the edge list otherwise |

The edge list holds one `source target` pair per line, 0-based unless
`%` lines open the file (MatrixMarket, KONECT), with `#` lines as comments;
the adjacency format is a `<vertices> <edges>` header and then the 1-based
targets of each vertex on its own line (`arts/graph.h`). The smoke test
reads `examples/inputs/graph_smoke.txt`, 16 vertices and 23 undirected
edges with `--flip`, from vertex 0.

## Structure

On `N` ranks:

| object | count |
|---|---|
| partition blocks | `N`: a header, `b + 1` row offsets and the block's edges |
| level arrays | `N`, one per rank in its own memory, `b` words each |
| `init_node` | `N`, the loaders |
| `main_continue`, `kickoff_termination`, `exit_program` | 1 each |
| `relax` | one per message, plus the start: a vertex messages all its neighbours each time its level is lowered, at least once per vertex the search reaches |
| finish events | 2: the loaders' and the search's |

## Wiring

Every `relax` takes the partition that holds its vertex on slot 0, RO, and
is created on that partition's rank. The loaders are members of the first
finish scope, which `main_edt` creates on rank 0; `main_continue` waits on it
through its one control slot. `main_continue` creates the second scope,
wires `exit_program`'s control slot on it and creates `kickoff_termination`
as its member; every `relax` is created by `kickoff_termination` or by
another `relax` with no scope of its own named, so it joins its creator's,
and the scope fires only when the last `relax` has finished. The level
arrays are no data blocks: each is an array of C11 atomic words in its
rank's memory, lowered only by that rank's `relax` EDTs, each with a relaxed
compare-and-swap loop (a level is a value and orders nothing), and read by
`exit_program` after the second scope has fired.

## Flow

`main_edt` on rank 0 refuses missing counts; otherwise it builds the
distribution from its arguments, reserving the partitions' GUIDs once, and
creates the first scope, one `init_node` per rank, each given the
distribution and `main_edt`'s arguments as parameters, and `main_continue`.
Each `init_node` writes its rank's copy of the distribution, which every
later EDT on the rank reads, and loads the partition homed on its rank, which
it holds until it ends, so its completion publishes the partition before the
scope fires; it allocates its rank's level array and, if the load failed,
prints its message and shuts down. `main_continue` reads `--source` from
`main_edt`'s arguments, refuses an out-of-range source, and starts the
search. `exit_program` prints rank 0's levels, frees rank 0's array, destroys
every partition, whose readers have all finished, and shuts the runtime down.

## Placement

Partition `r` is homed on rank `r` and loaded by `init_node` `r` there. A
`relax` runs on its vertex's rank, so every partition read is local and a
message to a vertex of another rank is a remote EDT create. `main_continue`,
`kickoff_termination` and `exit_program` run on rank 0.

## Sizing

The work is the messages: at least one per adjacency entry of every reached
vertex, and again each time a later message lowers a vertex's level. Nothing
orders the messages by distance, so on a graph with long paths the lowerings
grow far faster than the graph: on the generated graphs of `fixtures.py`,
between 6·10⁴ and 4·10⁵ `relax` EDTs at 512 vertices, as the schedule
orders them, and of the order of 10⁷ at 4096. Every message prints a line and
the `relax` it starts another, so the output grows with them. A rank holds its
partition, `8·(b + 1 + local edges)` bytes plus a header, and `b` level
words. The levels are the graph's, but which vertices are printed depends on
the node count, so the catalog row pins no answer: it runs the generated
512-vertex graph with 2074 undirected edges from vertex 0, and its marker is
the `Printing vertex levels` line. The single-node smoke test pins all sixteen
levels of the checked-in graph from vertex 0 (vertex 15 is unreachable and
prints `18446744073709551615`), values from
`examples/inputs/graph_reference.py`; its multinode variants check the
completion line.
