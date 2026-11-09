"""Telling a mesh failure from a program failure, and naming the hosts to
avoid, from an attempt's log alone."""

from __future__ import annotations

import re
from dataclasses import dataclass

MAX_RETRIES = 2

_LINE_RE = re.compile(
    r"ARTS_MESH_FAILED rank=(\S+) host=(\S+) phase=(\S+) missing=(\S+) "
    r"missing_hosts=(\S+) more=(\d+)\s*$")
_IPV4_RE = re.compile(r"\d+\.\d+\.\d+\.\d+")
# A line with a phase outside this list is not parsed: an unknown line is
# never a cause, so a relaunch is only ever triggered by a recognised one.
_PHASES = ("config", "resolve", "listen", "connect", "fabric", "exchange",
           "hangup")


@dataclass(frozen=True)
class MeshLine:
    rank: int | None
    host: str
    phase: str
    missing: tuple[int, ...]
    missing_hosts: tuple[str, ...]
    more: int


def _parse(match: re.Match) -> MeshLine | None:
    rank, host, phase, missing, hosts, more = match.groups()
    if phase not in _PHASES:
        return None
    try:
        return MeshLine(
            None if rank == "-" else int(rank), host, phase,
            () if missing == "-" else tuple(int(r) for r in missing.split(",")),
            () if hosts == "-" else tuple(hosts.split(",")),
            int(more))
    except ValueError:
        return None


def mesh_lines(text: str) -> list[MeshLine]:
    out = []
    for line in text.splitlines():
        at = line.find("ARTS_MESH_FAILED rank=")
        m = _LINE_RE.match(line, at) if at >= 0 else None
        parsed = _parse(m) if m else None
        if parsed is not None:
            out.append(parsed)
    return out


def _causes(lines: list[MeshLine]) -> list[MeshLine]:
    return [m for m in lines if m.phase != "hangup"]


def is_mesh_failure(text: str) -> bool:
    return bool(_causes(mesh_lines(text)))


def _label(name: str) -> str:
    return name if _IPV4_RE.fullmatch(name) else name.split(".")[0]


def _key(name: str) -> str:
    return _label(name).lower()


def suspects(lines: list[MeshLine], nodes: int) -> list[str]:
    votes: dict[str, int] = {}
    spelling: dict[str, str] = {}
    for m in _causes(lines):
        total = len(m.missing) + m.more
        if m.missing_hosts and not (total > 1 and 2 * total > nodes - 1):
            named = m.missing_hosts
        else:
            named = (m.host,)
        for h in named:
            spelling.setdefault(_key(h), _label(h))
        for key in {_key(h) for h in named}:
            votes[key] = votes.get(key, 0) + 1
    if not votes:
        return []
    top = max(votes.values())
    best = {k for k, v in votes.items() if v == top}
    reporters = {_key(m.host) for m in lines}
    if len(reporters) >= 2 and reporters <= best:
        return []
    return [spelling[k] for k in sorted(best)]


def map_suspects(names: list[str], lines: list[MeshLine]) -> list[str]:
    table: dict[str, str] = {}
    for m in lines:
        for h in m.missing_hosts:
            table.setdefault(_key(h), h)
        if m.rank is not None:
            table.setdefault(_key(m.host), m.host)
    out: list[str] = []
    for name in names:
        mapped = table.get(_key(name))
        if mapped is not None and mapped not in out:
            out.append(mapped)
    return out


def mesh_note(lines: list[MeshLine], nodes: int) -> str:
    seen = {m.phase for m in _causes(lines)}
    phases = ",".join(p for p in _PHASES if p in seen)
    hosts = ",".join(map_suspects(suspects(lines, nodes), lines)) or "none"
    return f"mesh failure ({phases}): suspect hosts {hosts}"
