#!/usr/bin/env python3
"""Reference answers for the graph examples, computed without them.

    graph_reference.py <edge list> --num-vertices <n> [--source <vertex>]

The edge list is the examples' format as their pinned runs read it: one
"source target" pair per line, 0-based, '#' lines and blank lines ignored.
It must describe a simple undirected graph, each edge once, which the
examples see in full when run with --flip; a self loop, a repeated edge or an
endpoint outside [0, n) is refused, since the examples would then see a
different graph.

Printed:
  triangles: the number of vertex triples joined by all three edges;
  scan maximum: the scan statistic as scan_statistics defines it — for a
    vertex v with neighbours N(v), |N(v)| plus, over every u in N(v),
    |N(v) & N(u)|, which is |N(v)| + 2 t(v) with t(v) the triangles through
    v; 1 for a vertex with no neighbour — and the lowest vertex attaining it;
  with --source, every vertex's BFS level from it, 2^64 - 1 when unreachable.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path

UNREACHED = 2**64 - 1


def read_graph(path: Path, n: int) -> list[set[int]]:
    adj: list[set[int]] = [set() for _ in range(n)]
    for number, line in enumerate(path.read_text().splitlines(), 1):
        fields = line.split()
        if not fields or line.startswith("#"):
            continue
        if line.startswith("%") or len(fields) < 2:
            raise SystemExit(f"{path}:{number}: not a 0-based edge line")
        u, v = int(fields[0]), int(fields[1])
        if not (0 <= u < n and 0 <= v < n):
            raise SystemExit(f"{path}:{number}: an endpoint is outside [0, {n})")
        if u == v or v in adj[u]:
            raise SystemExit(f"{path}:{number}: a self loop or a repeated edge")
        adj[u].add(v)
        adj[v].add(u)
    return adj


def triangles_through(adj: list[set[int]]) -> list[int]:
    """t(v) for every v, each triangle {a < b < c} found once from its
    lowest edge and credited to its three vertices."""
    per_vertex = [0] * len(adj)
    for a, na in enumerate(adj):
        for b in na:
            if b <= a:
                continue
            for c in na & adj[b]:
                if c > b:
                    per_vertex[a] += 1
                    per_vertex[b] += 1
                    per_vertex[c] += 1
    return per_vertex


def scan_statistic(adj: list[set[int]], t: list[int]) -> list[int]:
    stats = []
    for v, nv in enumerate(adj):
        if not nv:
            stats.append(1)
            continue
        direct = len(nv) + sum(len(nv & adj[u]) for u in nv)
        assert direct == len(nv) + 2 * t[v]
        stats.append(direct)
    return stats


def bfs_levels(adj: list[set[int]], source: int) -> list[int]:
    level = [UNREACHED] * len(adj)
    level[source] = 0
    queue = deque([source])
    while queue:
        u = queue.popleft()
        for w in adj[u]:
            if level[w] == UNREACHED:
                level[w] = level[u] + 1
                queue.append(w)
    return level


def reference(path: Path, n: int) -> dict[str, int]:
    adj = read_graph(path, n)
    t = triangles_through(adj)
    stats = scan_statistic(adj, t)
    best = max(stats)
    return {
        "edges": sum(len(s) for s in adj) // 2,
        "triangles": sum(t) // 3,
        "scan_vertex": stats.index(best),
        "scan_stat": best,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("edge_list", type=Path)
    parser.add_argument("--num-vertices", type=int, required=True)
    parser.add_argument("--source", type=int)
    args = parser.parse_args()
    ref = reference(args.edge_list, args.num_vertices)
    print(f"edges {ref['edges']}")
    print(f"triangles {ref['triangles']}")
    print(f"scan maximum: vertex {ref['scan_vertex']} "
          f"scan_stat {ref['scan_stat']}")
    if args.source is not None:
        adj = read_graph(args.edge_list, args.num_vertices)
        for v, lvl in enumerate(bfs_levels(adj, args.source)):
            print(f"level {v} {lvl}")


if __name__ == "__main__":
    main()
