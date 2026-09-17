"""Campaign output: a replayable selection, machine-readable results, and the
tables a person reads."""

from __future__ import annotations

import csv
import json
from dataclasses import asdict
from pathlib import Path

from rich.console import Console
from rich.table import Table

from artsrun.check import Group, Verdict, minority_report
from artsrun.model.plane import Plane
from artsrun.model.selection import Selection
from artsrun.run.types import CellResult, Skipped, Status

RESULT_COLUMNS = [
    "app", "version", "nodes", "entry", "kind", "repeat",
    "status", "rc", "wall_s", "e2e_s", "app_s", "timing_metric",
    "timing_contract", "scalar", "note", "log",
    "teardown_hang", "maxrss_kb", "minflt",
]

# Display metadata for runtime series, carried into the report so a later
# figure tool inherits one naming and colour convention instead of inventing
# its own.  Keyed by the SELECTION-ENTRY form (what every live caller passes);
# rt_key() below folds build suffixes and pre-promotion comb keys onto it.
# Every label spells all three axes {family} x {write policy} x {release
# policy} — an elided axis reads as "no such axis" to anyone outside the
# build system (EXCL's WB and INV/VAL's RETAIN are requirements, but the
# NAME still carries them).
RT_LABEL = {
    "arts_excl_purge": "EXCL·WB·PURGE",
    "arts_excl_retain": "EXCL·WB·RETAIN",
    "arts_inv_wt": "INV·WT·RETAIN",
    "arts_inv_wb": "INV·WB·RETAIN",
    "arts_inv_wt_purge": "INV·WT·PURGE",
    "arts_val_wt": "VAL·WT·RETAIN",
    "arts_val_wb": "VAL·WB·RETAIN",
    "arts_val_wt_purge": "VAL·WT·PURGE",
    # The non-combining ablation twins are build variants, not plane entries;
    # they surface only in sweeps and ablation figures.
    "arts_val_wt_nocomb": "VAL·WT·RETAIN nocomb",
    "arts_val_wb_nocomb": "VAL·WB·RETAIN nocomb",
    "arts_val_wt_purge_nocomb": "VAL·WT·PURGE nocomb",
    "xsocr": "XSOCR",
    # OCR-vx ships three runtime families; the one built here is the
    # distributed-memory one, which is what the label should name.
    "ocrvx": "OCR-Vdm",
    "hpx": "HPX",
}
RT_COLOR = {
    "arts_excl_purge": "#4C72B0",
    "arts_excl_retain": "#7BA3D8",
    "arts_inv_wt": "#DD8452",
    "arts_inv_wb": "#F0B08A",
    # A third, deeper tone of the family's own hue — WT+PURGE is a third
    # point in this family, not a fourth family, so it stays on the same
    # hue as WT/WB rather than drawing a new one.
    "arts_inv_wt_purge": "#B65924",
    "arts_val_wt": "#55A868",
    "arts_val_wb": "#8CCB9B",
    "arts_val_wt_purge": "#3D794B",
    # Ablation twins stay on the VAL hue, desaturated: same family, with the
    # combining window compiled out.
    "arts_val_wt_nocomb": "#7E9E87",
    "arts_val_wb_nocomb": "#A9C4B0",
    "arts_val_wt_purge_nocomb": "#5C7A64",
    "xsocr": "#8172B3",
    "ocrvx": "#937860",
    "hpx": "#C44E52",
}


def rt_key(key: str) -> str:
    """Fold any spelling of a runtime series onto the display key.

    Callers hold either a selection-entry key (arts_*/xsocr/ocrvx — possibly a
    pre-promotion comb spelling from an old run) or a raw build suffix
    (ocr_val_wt, from a sweep that addresses arms directly).
    """
    from artsrun.model.plane import modern_entry_key

    if key.startswith("ocr_"):
        alt = "arts_" + key.removeprefix("ocr_")
        return alt if alt in RT_LABEL else key
    return modern_entry_key(key)


# What a completed cell's time is when the runtime printed no end-to-end
# stamp: process wall, which includes runtime init and teardown and is
# therefore not comparable with a stamped cell's number.  Such a cell is
# marked wherever its time is shown rather than silently mixed in.
WALL_ONLY_NOTE = "wall-only: no [E2E] stamp, time includes init and teardown"


def wall_only(r: CellResult) -> bool:
    """Whether this cell's measurement falls back to process wall.

    Only a completed cell can be one: a cell that failed has no measurement
    to qualify, and saying so of every failure would bury the cases where a
    successful run's number is the weaker kind.
    """
    return (r.status is Status.OK and r.cell.app.timing_metric == "e2e_s"
            and r.e2e_s is None)


# What a row whose arguments change with the node count is: a curve at fixed
# per-rank load, not a strong-scaling curve.  Its times are still comparable
# ACROSS runtimes at one node count — that is the comparison such a row exists
# for — but never along the row, so the row carries a mark of its own.
VARIED_ARGS_NOTE = (
    "arguments change with the node count (fixed per-rank load): compare "
    "across runtimes at one node count, not along the row"
)


def varied_args(results: list[CellResult]) -> set[str]:
    """The application keys whose arguments differ across the node counts run."""
    seen: dict[str, set[tuple[str, ...]]] = {}
    for r in results:
        # Only the observations the table shows, so the mark and the rows it
        # explains can never disagree.
        if r.status is not Status.OK:
            continue
        app = r.cell.app
        seen.setdefault(app.key, set()).add(tuple(app.args_for(r.cell.nodes)))
    return {k for k, v in seen.items() if len(v) > 1}


def _note(r: CellResult) -> str:
    if not wall_only(r):
        return r.note
    return f"{r.note}; {WALL_ONLY_NOTE}" if r.note else WALL_ONLY_NOTE


def _row(r: CellResult) -> dict:
    return {
        "app": r.cell.app.name,
        "version": r.cell.app.version.value,
        "nodes": r.cell.nodes,
        "entry": r.cell.entry.key,
        "kind": r.cell.entry.kind.value,
        "repeat": r.cell.repeat,
        "status": r.status.value,
        "rc": r.rc,
        "wall_s": round(r.wall_s, 3),
        "e2e_s": round(r.e2e_s, 3) if r.e2e_s is not None else "",
        "app_s": r.app_s if r.app_s is not None else "",
        "timing_metric": r.cell.app.timing_metric,
        "timing_contract": r.cell.app.timing_contract or "",
        "scalar": r.scalar or "",
        "note": _note(r),
        "log": str(r.log_path) if r.log_path else "",
        "teardown_hang": "true" if r.teardown_hang else "",
        "maxrss_kb": r.extra.get("maxrss_kb", ""),
        "minflt": r.extra.get("minflt", ""),
    }


def write_results_csv(results: list[CellResult], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=RESULT_COLUMNS)
        writer.writeheader()
        for r in results:
            writer.writerow(_row(r))


def write_report_json(
    results: list[CellResult],
    groups: list[Group],
    skipped: list[Skipped],
    selection: Selection,
    path: Path,
) -> None:
    payload = {
        "selection": selection.model_dump(mode="json"),
        "runtimes": {
            key: {"label": RT_LABEL.get(rt_key(key), key),
                  "color": RT_COLOR.get(rt_key(key))}
            for key in selection.entries
        },
        "cells": [_row(r) for r in results],
        "consensus": [
            {
                "app": g.app_key,
                "nodes": g.nodes,
                "value": g.consensus,
                "verdicts": {k: v.value for k, v in g.verdicts.items()},
                "unanimous": g.unanimous,
            }
            for g in groups
        ],
        "skipped": [asdict(s) for s in skipped],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2))


def consensus_table(groups: list[Group], plane: Plane, entries: list[str]) -> Table:
    table = Table(title="Consensus by application and node count", expand=False)
    table.add_column("app", style="bold")
    table.add_column("n", justify="right")
    table.add_column("value")
    for key in entries:
        table.add_column(RT_LABEL.get(rt_key(key), key), justify="center")

    style = {
        Verdict.OK: "[green]OK[/green]",
        Verdict.DISAGREE: "[red]DIFF[/red]",
        Verdict.FAIL: "[red]FAIL[/red]",
        Verdict.NA: "[dim]--[/dim]",
        Verdict.EXPECT_FAIL: "[yellow]EXP![/yellow]",
        Verdict.LONE: "[magenta]LONE[/magenta]",
    }
    for g in groups:
        cells = []
        for k in entries:
            verdict = g.verdicts.get(k, Verdict.NA)
            text = style.get(verdict, "?")
            # A dagger, not a new verdict: the cell voted OK (or is the sole
            # survivor of its group), only the way it got there is worth a
            # reader's second look.
            if verdict in (Verdict.OK, Verdict.LONE) and g.teardown_hang.get(k):
                text = text.replace("[/", "†[/", 1)
            cells.append(text)
        table.add_row(g.app_key, str(g.nodes), g.consensus or "-", *cells)
    return table


def scaling_table(results: list[CellResult], entries: list[str]) -> Table:
    """Measured time per node count, one row per (application, configuration).

    The application's declared metric selects its interval. Runtime timing
    retains a marked wall-time fallback for legacy observations; application
    intervals require their own stamp and never substitute another interval.

    A row is a scaling curve only where the workload is the same at every
    node count, so an application whose arguments vary along the sweep is
    marked "~" on its name: the numbers on that row are a per-rank-load
    series and reading them as speedup would be reading a different job at
    every column.
    """
    node_counts = sorted({r.cell.nodes for r in results})
    contracts: dict[str, tuple[str, str | None]] = {}
    for r in results:
        app = r.cell.app
        contract = (app.timing_metric, app.timing_contract)
        if contracts.setdefault(app.key, contract) != contract:
            raise ValueError(f"{app.key}: cannot combine different timing contracts")
    metrics = {r.cell.app.timing_metric for r in results}
    label = "app seconds" if metrics == {"app_s"} else (
        "e2e seconds" if metrics <= {"e2e_s"} else "declared metric, seconds")
    table = Table(title=f"Strong scaling ({label})", expand=False)
    table.add_column("app", style="bold")
    table.add_column("configuration")
    for n in node_counts:
        table.add_column(f"{n}n", justify="right")

    by_key: dict[tuple[str, str], dict[int, tuple[float, bool]]] = {}
    for r in results:
        if r.status is not Status.OK:
            continue
        measured = r.measured_s
        if measured is None:
            if r.cell.app.timing_metric == "app_s":
                continue
            measured = r.wall_s
        key = (r.cell.app.key, r.cell.entry.key)
        walls = by_key.setdefault(key, {})
        # Repeats collapse to their best observation, and the mark follows
        # the observation shown rather than the group it came from.
        best = walls.get(r.cell.nodes)
        if best is None or measured < best[0]:
            walls[r.cell.nodes] = (measured, wall_only(r))

    varied = varied_args(results)
    for (app_key, entry_key) in sorted(by_key):
        walls = by_key[(app_key, entry_key)]
        row = [f"{walls[n][0]:.2f}{'w' if walls[n][1] else ''}"
               if n in walls else "-" for n in node_counts]
        name = f"{app_key} ~" if app_key in varied else app_key
        table.add_row(name, RT_LABEL.get(rt_key(entry_key), entry_key), *row)
    return table


def write_summary(
    results: list[CellResult],
    groups: list[Group],
    skipped: list[Skipped],
    selection: Selection,
    plane: Plane,
    path: Path,
) -> str:
    console = Console(record=True, width=200, file=open("/dev/null", "w"))
    console.print(consensus_table(groups, plane, selection.entries))
    if any(any(g.teardown_hang.values()) for g in groups):
        console.print(
            "[dim]† on a verdict: reaped after a completed, measured run "
            "(teardown hang)[/dim]"
        )
    console.print()
    console.print(scaling_table(results, selection.entries))
    if any(wall_only(r) for r in results):
        console.print(f"[dim]w after a time: {WALL_ONLY_NOTE}[/dim]")
    if varied_args(results):
        console.print(f"[dim]~ after an application: {VARIED_ARGS_NOTE}[/dim]")

    minority = minority_report(groups)
    console.print()
    if minority:
        console.print("[bold]MINORITY REPORT[/bold]")
        for g in minority:
            dissent = ", ".join(
                f"{k}={v.value}" for k, v in g.verdicts.items()
                if v in (Verdict.DISAGREE, Verdict.EXPECT_FAIL, Verdict.FAIL, Verdict.LONE)
            )
            console.print(f"  {g.app_key} @ {g.nodes}n consensus={g.consensus} -> {dissent}")
    else:
        console.print("[bold]No disagreement; every completed cell was corroborated.[/bold]")

    if skipped:
        console.print()
        console.print(f"[bold]Not run ({len(skipped)} cells)[/bold]")
        seen = set()
        for s in skipped:
            if s.reason in seen:
                continue
            seen.add(s.reason)
            console.print(f"  {s.app_key}: {s.reason}")

    text = console.export_text()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return text
