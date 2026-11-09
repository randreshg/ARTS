"""Telling a mesh failure from a program failure, and naming the hosts to
avoid, from an attempt's log alone."""

from __future__ import annotations

import dataclasses
import glob
import re
import shutil
from dataclasses import dataclass
from pathlib import Path

from artsrun.model.plane import RuntimeKind
from artsrun.run.grace import CampaignRefusal
from artsrun.run.types import Cell, CellResult

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


def file_mesh_lines(path: Path) -> list[MeshLine]:
    """The mesh lines of a log, read line by line so none is lost to a
    truncated read of a large log."""
    out: list[MeshLine] = []
    try:
        with path.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "ARTS_MESH_FAILED rank=" in line:
                    out.extend(mesh_lines(line))
    except OSError:
        pass
    return out


def attempt_path(log_dir: Path, slug: str, attempt: int, suffix: str) -> Path:
    return log_dir / f"{slug}.attempt{attempt}{suffix}"


def kept_attempts(log_dir: Path, slug: str) -> int:
    pattern = re.compile(re.escape(slug) + r"\.attempt(\d+)\.(?:log|rc|sh)$")
    found = [int(m.group(1))
             for p in log_dir.glob(f"{glob.escape(slug)}.attempt*")
             if (m := pattern.fullmatch(p.name))]
    return max(found, default=0)


def keep(log_dir: Path, counters_root: Path | None, slug: str,
         attempt: int) -> Path:
    """Move one attempt's log, marker, script and counter folder aside."""
    for suffix in (".log", ".rc", ".sh"):
        src = log_dir / f"{slug}{suffix}"
        if src.exists():
            shutil.move(src, attempt_path(log_dir, slug, attempt, suffix))
    if counters_root is not None:
        folder = counters_root / slug
        if folder.exists():
            shutil.move(folder, counters_root / f"{slug}.attempt{attempt}")
    return attempt_path(log_dir, slug, attempt, ".log")


def supersede(log_dir: Path, counters_root: Path | None, cell: Cell) -> Cell:
    """Keep what an earlier session left under the cell's plain names."""
    left = any((log_dir / f"{cell.slug}{s}").exists()
               for s in (".log", ".rc", ".sh"))
    if left:
        keep(log_dir, counters_root, cell.slug,
             kept_attempts(log_dir, cell.slug) + 1)
    return cell


def _union(*groups) -> tuple[str, ...]:
    out: list[str] = []
    for group in groups:
        for name in group:
            if name not in out:
                out.append(name)
    return tuple(out)


def restore(log_dir: Path, cell: Cell, *, excluding: bool = True) -> Cell:
    """The cell's next attempt, rebuilt from the attempts kept on disk.

    Only a scheduler can be told to avoid hosts; with `excluding` false the
    exclusions stay as the cell has them."""
    kept = kept_attempts(log_dir, cell.slug)
    exclude = list(cell.exclude)
    for k in range(1, kept + 1 if excluding else 1):
        lines = file_mesh_lines(attempt_path(log_dir, cell.slug, k, ".log"))
        if _causes(lines):
            exclude = list(_union(exclude, map_suspects(
                suspects(lines, cell.nodes), lines)))
    return dataclasses.replace(cell, attempt=kept + 1, exclude=tuple(exclude))


class MeshRetry:
    """Decides, from an attempt's log alone, whether to relaunch it.

    A relaunch is budgeted per session: the attempts a cell made after the
    one it entered the session at.
    """

    def __init__(self, log_dir: Path, counters_root: Path | None = None,
                 max_retries: int = MAX_RETRIES, excluding: bool = True):
        self.excluding = excluding
        self.log_dir = log_dir
        self.counters_root = counters_root
        self.max_retries = max_retries
        self._entered: dict[str, int] = {}

    def __call__(self, result: CellResult) -> Cell | None:
        cell = result.cell
        entered = self._entered.setdefault(cell.slug, cell.attempt)
        if (cell.entry.kind is not RuntimeKind.ARTS or result.log_path is None
                or cell.attempt - entered >= self.max_retries):
            return None
        lines = file_mesh_lines(result.log_path)
        if not _causes(lines):
            return None
        named = map_suspects(suspects(lines, cell.nodes), lines)
        exclude = _union(cell.exclude, named) if self.excluding \
            else cell.exclude
        result.log_path = keep(self.log_dir, self.counters_root, cell.slug,
                               cell.attempt)
        result.extra["attempt"] = str(cell.attempt + 1)
        result.extra["suspects"] = ",".join(named)
        result.extra["exclude"] = ",".join(exclude)
        result.extra["log"] = str(result.log_path)
        return dataclasses.replace(cell, attempt=cell.attempt + 1,
                                   exclude=exclude)


def resolve_mesh_retries(asked: int | None, recorded: int | None) -> int:
    if asked is not None and asked < 0:
        raise CampaignRefusal(f"--mesh-retries must be 0 or more (got {asked})")
    if recorded is None:
        return MAX_RETRIES if asked is None else asked
    if asked is not None and asked != recorded:
        raise CampaignRefusal(
            f"--mesh-retries {asked} differs from the {recorded} this run "
            "recorded; a continuation keeps the recorded count")
    return recorded
