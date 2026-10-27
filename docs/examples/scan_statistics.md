# scan_statistics_example

*The largest scan statistic of a graph split into vertex blocks, one per
rank: for every vertex, one EDT on each rank intersects the vertex's
neighbour list with the rows of the neighbours that rank holds, a per-vertex
EDT adds the counts to the degree, and one EDT on rank 0 takes the maximum.*
Source: `examples/graph/scan_statistics.cpp` (C++17), written against the
public `arts.h` and `arts/graph.h` alone.

## Overview

The vertex range `[0, n)` is split into one block per rank, rank `r`
holding `[r·b, min((r + 1)·b, n))` with `b = ⌈n / N⌉` on `N` ranks, and each
block's edges are one CSR data block, the rank's partition, its rows sorted.
The statistic of a vertex `v` with neighbour list `N(v)` is `|N(v)|` plus,
over every `u` in `N(v)`, `|N(v) ∩ N(u)|` (`std::set_intersection` of the
two sorted rows); on an undirected simple graph that is `|N(v)| + 2t(v)`,
with `t(v)` the triangles through `v`. A vertex with no neighbour scores 1.
For each vertex, its rank asks every rank for the intersections with the
neighbours that rank holds, adds the answers to the degree, and sends the
vertex's statistic to `max_reducer` on rank 0, which prints `Max vertex_t:
<v> scan_stat: <s>`, `v` being the lowest vertex with the largest
statistic, and `Total execution time: <s> s`, from the first vertex's start
to the maximum.

This is a row of the ARTS-examples section: the ARTS entries run the native
program (`scan_statistics_example_arts_<variant>`), and no reference runtime
has a program for it.

## Parameters

| argument | meaning | reachability |
|---|---|---|
| `--file <path>` | the graph | required; a file that cannot be read or holds an invalid line, an endpoint at or above `--num-vertices` included, makes each loader print `Failed to load the graph on rank <r>` and shut down |
| `--num-vertices <n>` | the vertex count | required with `--num-edges`: without both, `main_edt` prints the usage and shuts down |
| `--num-edges <m>` | the edge count | recorded in the distribution, not checked against the file |
| `--flip` | add every edge's reverse | an edge list with each undirected edge once needs it |
| `--keep-self-loops` | keep an edge `v v` | dropped otherwise |
| `--csr-format` | read the adjacency format | the edge list otherwise |

The file formats are those of `arts/graph.h` (see `bfs_example`). Each
loader also prints `Node <r> argc <argc>`. The smoke test reads
`examples/inputs/graph_smoke.txt`, 16 vertices and 23 undirected edges with
`--flip`, whose largest statistic is vertex 3's: degree 6 and six triangles,
18.

## Structure

On `N` ranks, with `n₀` vertices that have no neighbour:

| object | count |
|---|---|
| partition blocks | `N`: a header, `b + 1` row offsets and the block's edges |
| `init_node` | `N`, the loaders |
| `main_continue`, `max_reducer` | 1 each; `max_reducer` has `n` slots |
| `visit_source` | `n` |
| `find_intersection` | `n − n₀`, `N` slots each |
| `visit_one_hop_neighbor_on_rank`, source-information blocks | `N · (n − n₀)` each; a block holds the vertex, its neighbour list and its `find_intersection`'s GUID |
| per-vertex blocks | `N · (n − n₀)` intersection counts, plus `n` statistics |
| finish events | 1: the loaders' |

## Wiring

`visit_source` takes a start value on slot 0 and its vertex's partition on
slot 1, RO. `visit_one_hop_neighbor_on_rank` takes a source-information
block on slot 0 and its rank's partition on slot 1, both RO, and sends its
count to slot `r` (its rank) of the vertex's `find_intersection`, which
takes the vertex's degree as a parameter. Each `find_intersection`, and
`visit_source` for a vertex with no neighbour, sends the statistic to
`max_reducer`'s slot of that vertex. Every block is created, filled and
released by one EDT before it is wired RO to its one reader, which destroys
it: `visit_one_hop_neighbor_on_rank` its source information,
`find_intersection` its `N` counts, `max_reducer` its `n` statistics. The
partitions, which `max_reducer` destroys, are released by their readers
before the satisfy that leads to it: `visit_source` once it has copied the
source's row for every rank (at once for a vertex with no neighbour),
`visit_one_hop_neighbor_on_rank` once its intersections are done. The
loaders are members of the one finish scope, on which `main_continue` waits
through its control slot. `max_reducer` is created under a GUID that
`main_edt` reserves and every `init_node` copies into its rank's memory, so an
EDT on any rank names it.

## Flow

`main_edt` on rank 0 refuses missing counts; otherwise it builds the
distribution from its arguments, reserving the partitions' GUIDs once,
reserves `max_reducer`'s GUID and creates the scope, one `init_node` per
rank, each given the distribution, `max_reducer`'s GUID and `main_edt`'s
arguments as parameters, and `main_continue`. Each `init_node` writes its
rank's copies of the distribution and `max_reducer`'s GUID, which every later
EDT on the rank reads, loads the partition homed on its rank, which it holds
until it ends, and, if the load failed, prints its message and shuts down.
`main_continue` on rank 0 creates `max_reducer`, records the start time and
creates every vertex's `visit_source`. `max_reducer` scans the statistics in
vertex order, prints the maximum and the time, destroys every partition and
shuts the runtime down.

## Placement

Partition `r` is homed on rank `r` and loaded there. A vertex's
`visit_source` and `find_intersection` run on the vertex's rank and
`visit_one_hop_neighbor_on_rank` `r` on rank `r`, so every partition read is
local. A vertex's `N` source-information blocks are created on its rank,
`N − 1` of them read elsewhere; each count is created on the intersecting
rank and read on the vertex's; every statistic travels to rank 0, where
`main_continue` and `max_reducer` run.

## Sizing

Each vertex with a neighbour costs `N + 1` EDTs besides its `visit_source`
and `2N + 1` blocks whatever its degree, so the task count grows with
`n · N`; the intersections cost about
`|N(v)| + |N(u)|` steps for each edge `v`–`u` in each direction.
`max_reducer`'s slot array grows with `n`: per vertex, a 24-byte `depv`
entry and the runtime's 24-byte acquire record. The maximum
does not depend on the node count: the catalog row pins 34 (at vertex 3874)
for the generated 4096-vertex graph with 16 668 undirected edges
(`fixtures.py`), and the smoke tests pin vertex 3 with 18 for the checked-in
graph at every rank count, both from `examples/inputs/graph_reference.py`.
