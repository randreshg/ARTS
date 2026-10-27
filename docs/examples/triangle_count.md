# triangle_count_example

*Triangle counting over a graph split into vertex blocks, one per rank: each
rank counts the triangles whose smallest vertex it holds by merging sorted
neighbour lists, ships a vertex's list to every other rank that holds one of
its larger neighbours, and a two-level reduction sums the counts on rank 0.*
Source: `examples/graph/triangle_count.c`, written against the public
`arts.h` and `arts/graph.h` alone.

## Overview

The vertex range `[0, n)` is split into one block per rank, rank `r`
holding `[r·b, min((r + 1)·b, n))` with `b = ⌈n / N⌉` on `N` ranks, and each
block's edges are one CSR data block, the rank's partition, its rows sorted.
A triangle `i < k < j` is counted once, at `i`: for each neighbour `j` of
`i` after the first neighbour at or above `i`, the program merges the part
of `i`'s row from that first neighbour up to `j` with the part of `j`'s row
in `[i, j]` and counts the common vertices. That needs the adjacency to hold both directions of every
edge: an edge list with each edge once is read with `--flip`.

When `j` is in the rank's own block, its row is read from the partition.
Otherwise, the first time vertex `i` meets a neighbour held by rank `o` (a
64-bit mask per vertex, so at most 64 ranks: on more, `main_edt` prints so and
shuts down), it copies its row into a data
block and starts a remote `visit_vertex` on `o`, which runs the same loop for
the neighbours `o` holds. Each rank adds its counts to rank-local counters;
once every count has finished, each rank prints `Local: <pairs merged
locally> Remote: <lists shipped> Incoming: <pairs merged for other ranks>`
and sends its count to rank 0, which prints `Triangle Count: <count> Time:
<ns>`, the time running from the start of the counting to the sum.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`triangle_count_example_arts_<variant>`), and no reference runtime
has a program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `--file <path>` | the graph | required; a file that cannot be read or holds an invalid line, an endpoint at or above `--num-vertices` included, makes each loader print `Failed to load the graph on rank <r>` and shut down |
| `--num-vertices <n>` | the vertex count | required with `--num-edges`: without both, `main_edt` prints the usage and shuts down |
| `--num-edges <m>` | the edge count | recorded in the distribution, not checked against the file |
| `--flip` | add every edge's reverse | needed for an edge list with each edge once |
| `--keep-self-loops` | keep an edge `v v` | dropped otherwise; a self loop or a repeated edge makes the count no triangle count |
| `--csr-format` | read the adjacency format | the edge list otherwise |

The file formats are those of `arts/graph.h` (see `bfs_example`). The smoke
test reads `examples/inputs/graph_smoke.txt`, 16 vertices and 23 undirected
edges with `--flip`: ten triangles, four of which have the last vertex of a
four-rank block (3, 7 or 11) as their smallest vertex.

## Structure

On `N` ranks, with `W` workers in all:

| object | count |
|---|---|
| partition blocks | `N`: a header, `b + 1` row offsets and the block's edges |
| `init_node`, `start_work`, `local_reduce` | `N` each |
| `main_continue`, `start_reduce`, `final_reduce` | 1 each; `final_reduce` has `N` slots |
| local `visit_vertex` | per rank, one per chunk of `max(1, ⌊(bᵣ − 1) / 16W⌋)` vertices, `bᵣ` the rank's own block size |
| remote `visit_vertex`, neighbour-list blocks | one each per vertex and other rank that holds a neighbour of it past its first at or above it |
| finish events | 2: the loaders' and the counting's |

## Wiring

A local `visit_vertex` takes its rank's partition on slot 0, RO. A remote
one takes the neighbour-list copy on slot 0 and its rank's partition on
slot 1, both RO; the two forms are told apart by their slot count. The
sender creates the copy, fills it and releases it before it wires it, and
the remote form destroys it after its merge. The loaders are members of the
first finish scope and the `start_work` EDTs of the second; every
`visit_vertex` joins its creator's scope, so the second fires once the last
count has finished, and `start_reduce`'s control slot is wired on it.
`start_reduce` creates one `local_reduce` per rank, each of which sends its
rank's count to `final_reduce`'s slot of that rank as a slot value.
`start_reduce` and `final_reduce` are created under GUIDs that `main_edt`
reserves and every `init_node` copies into its rank's memory, so a
`local_reduce` on any rank names `final_reduce`. The counters are C11 atomic
words in each rank's memory, added to with relaxed adds by that rank's
`visit_vertex` EDTs and read by its `local_reduce` after the second scope
has fired.

## Flow

`main_edt` on rank 0 refuses missing counts; otherwise it builds the
distribution from its arguments, reserving the partitions' GUIDs once,
reserves the two reducers' GUIDs and creates the first scope, one `init_node`
per rank, each given the distribution, the reducers' GUIDs and `main_edt`'s
arguments as parameters, and `main_continue`. Each `init_node` writes its
rank's copies of the distribution and the reducers' GUIDs, which every later
EDT on the rank reads, loads the partition homed on its rank, which it holds
until it ends, sets its rank's vertex range and chunk size and, if the load
failed, prints its message and shuts down. `main_continue` on rank 0 starts
the timer, creates the two reducers, the second scope with one `start_work`
per rank in it, and wires `start_reduce`. Each `start_work` creates its
rank's local `visit_vertex` EDTs over the whole of its block. `final_reduce`
sums the counts, prints the result, destroys every partition, whose readers
have all finished, and shuts the runtime down.

## Placement

Partition `r` is homed on rank `r` and loaded there. `start_work` `r`, its
local `visit_vertex` EDTs and `local_reduce` `r` run on rank `r`; a remote
`visit_vertex` runs on the rank that holds the neighbours it merges, so
every partition read is local. A neighbour-list copy is created with no
hint, so the build's no-hint data-block policy homes it, on the sender's
rank by default, and the remote rank reads it. `main_continue`, `start_reduce` and `final_reduce` run on
rank 0.

## Sizing

The work is one merge per vertex and later neighbour, each as long as the
two row segments, so it grows with the squares of the degrees. Each rank
splits its block into about `16W` chunks, or one vertex each when the
block is smaller. A vertex ships its whole row once
to each other rank that holds a neighbour it merges with, at most `N − 1`
copies. The count does not depend on the node count: the catalog row pins
2754 for the generated 4096-vertex graph with 16 668 undirected edges
(`fixtures.py`), and the smoke tests pin 10 for the checked-in graph at
every rank count, both from `examples/inputs/graph_reference.py`.
