"""Reduce one run's per-rank counter files into a single record.

Each rank writes `n<rank>.json` at its own teardown; nothing in the runtime
combines them.  The reduction is a pure function of the files, so it can be
repeated on a finished run directory and gives the same record.
"""

from __future__ import annotations

import json
import re
import sys
from enum import StrEnum
from pathlib import Path

_RANK_FILE = re.compile(r"n(\d+)\.json$")


class Reduce(StrEnum):
    """How a counter's values combine, across a rank's threads in the
    runtime and across ranks here: MASTER keeps thread 0's, then rank 0's.

    Defined here, with the standard library alone, so that the reducer runs
    on a stock Python without artsrun's dependencies; the counter model
    imports it from this module."""

    SUM = "SUM"
    MAX = "MAX"
    MIN = "MIN"
    MASTER = "MASTER"


class ReduceError(ValueError):
    """The folder cannot be reduced; the message names why."""


class MixedRunError(ReduceError):
    """The folder holds files of more than one run."""


def _combine(method: Reduce, values: list[int | float]) -> int | float:
    if method is Reduce.SUM:
        return sum(values)
    if method is Reduce.MAX:
        return max(values)
    if method is Reduce.MIN:
        return min(values)
    raise ValueError(f"{method} is not a combining reduction")


def _method(name: str, entries: dict[int, dict]) -> Reduce:
    """The reduction the run compiled, as every rank's file states it."""
    stated = {}
    for rank in sorted(entries):
        text = entries[rank].get("reduce_method")
        if not text:
            raise ReduceError(
                f"{name} in rank {rank}'s file names no reduce_method")
        try:
            stated[rank] = Reduce(text)
        except ValueError as exc:
            raise ReduceError(
                f"{name} in rank {rank}'s file names reduce_method "
                f"{text!r}") from exc
    if len(set(stated.values())) > 1:
        raise ReduceError(
            f"ranks disagree on the reduce_method of {name}: "
            + ", ".join(f"rank {r} {m.value}" for r, m in stated.items()))
    return next(iter(stated.values()))


def _reduce_value(method: Reduce, entries: dict[int, dict]):
    if method is Reduce.MASTER:
        entry = entries.get(0)
        return None if entry is None else entry.get("value")
    values = [e["value"] for e in entries.values() if "value" in e]
    return _combine(method, values) if values else None


def _reduce_history(method: Reduce, entries: dict[int, dict]) -> list:
    by_epoch: dict[int, dict[int, int]] = {}
    for rank, entry in entries.items():
        for epoch, value in entry.get("captureHistory", []):
            by_epoch.setdefault(epoch, {})[rank] = value
    merged = []
    for epoch in sorted(by_epoch):
        at = by_epoch[epoch]
        if method is Reduce.MASTER:
            if 0 in at:
                merged.append([epoch, at[0]])
        else:
            merged.append([epoch, _combine(method, list(at.values()))])
    return merged


def _load(path: Path) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ReduceError(f"{path.name} is unreadable: {exc}") from exc
    if (not isinstance(doc, dict)
            or not isinstance(doc.get("metadata"), dict)
            or not isinstance(doc.get("counters", {}), dict)):
        raise ReduceError(f"{path.name} is not a counter file")
    return doc


def reduce_folder(folder: Path, nodes: int | None = None) -> dict:
    """`nodes` is the rank count the run was launched with.  A rank that
    wrote no usable file is then listed under `missing` whichever it is, and
    every counter of the record is marked `partial`: the surviving ranks'
    values are kept for diagnosis, but a SUM over fewer ranks is not the
    run's total and must not be read as one."""
    files: dict[int, dict] = {}
    pre_arm: list[str] = []
    for path in sorted(Path(folder).glob("n*.json")):
        match = _RANK_FILE.fullmatch(path.name)
        if not match:
            continue
        doc = _load(path)
        if not doc["metadata"].get("run", ""):
            pre_arm.append(path.name)
            continue
        rank = int(match.group(1))
        stated = doc["metadata"].get("rank")
        if stated is not None and stated != rank:
            raise ReduceError(
                f"{path.name} states rank {stated}, not the rank its name "
                "gives")
        files[rank] = doc

    runs = sorted({d["metadata"]["run"] for d in files.values()})
    if len(runs) > 1:
        raise MixedRunError(
            f"mixed run ids in {folder}: {' and '.join(runs)}")

    ranks = sorted(files)
    span = nodes if nodes is not None else (ranks[-1] + 1 if ranks else 0)
    missing = [r for r in range(span) if r not in files]
    meta = {r: files[r]["metadata"] for r in ranks}

    synced = bool(ranks) and all(m.get("clock_sync") == "rtt"
                                 for m in meta.values())
    key = "t0_synced_ns" if synced else "t0_realtime_ns"
    offsets: dict[str, int] = {}
    stamped = [r for r in ranks if key in meta[r]]
    if stamped:
        base = meta[0][key] if 0 in stamped else meta[stamped[0]][key]
        offsets = {str(r): meta[r][key] - base for r in stamped}

    names: dict[str, None] = {}
    for r in ranks:
        for name in files[r].get("counters", {}):
            names.setdefault(name)
    counters = {}
    for name in names:
        entries = {r: files[r]["counters"][name] for r in ranks
                   if name in files[r].get("counters", {})}
        method = _method(name, entries)
        out = {"reduce": method.value,
               "value": _reduce_value(method, entries)}
        if any("captureHistory" in e for e in entries.values()):
            out["captureHistory"] = _reduce_history(method, entries)
        if missing:
            out["partial"] = True
        counters[name] = out

    return {
        "run": runs[0] if runs else "",
        "ranks": ranks,
        "missing": missing,
        "pre_arm": pre_arm,
        "origin": meta[0].get("origin") if 0 in meta else None,
        "synced": synced,
        "t0_offset_ns": offsets,
        "counters": counters,
    }


def write_record(folder: Path, nodes: int | None = None) -> dict:
    """A refusal removes the c.json an earlier attempt left, so a file that
    is there always describes the files beside it."""
    target = Path(folder) / "c.json"
    try:
        record = reduce_folder(folder, nodes)
    except ReduceError:
        target.unlink(missing_ok=True)
        raise
    target.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("usage: python3 -m artsrun.reduce <folder>", file=sys.stderr)
        return 2
    try:
        record = reduce_folder(Path(args[0]))
    except ReduceError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    print(json.dumps(record, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
