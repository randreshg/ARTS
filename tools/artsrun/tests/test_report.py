"""How a campaign's tables present a measurement whose time is process wall,
and a row whose workload changes along the node sweep."""

from __future__ import annotations

import csv
import os
from pathlib import Path

from rich.console import Console

from artsrun.check import vote
from artsrun.model.experiment import ResolvedApp
from artsrun.model.catalog import AppClass, ScalarKind, Version
from artsrun.model.plane import RuntimeKind, SelectionEntry
from artsrun.model.selection import Selection
from artsrun.report import scaling_table, write_results_csv, write_summary
from artsrun.run.types import Cell, CellResult, Status


def _app() -> ResolvedApp:
    return ResolvedApp(
        name="app", version=Version.BASE, binary="app", cls=AppClass.TASK,
        marker=r"RESULT", scalar_re=r"RESULT\s*=\s*([\d.]+)",
        scalar_kind=ScalarKind.FLOAT, args=["1"],
    )


def _result(entry_key: str, *, wall_s: float, e2e_s: float | None,
            nodes: int = 1, status: Status = Status.OK,
            note: str = "", app: ResolvedApp | None = None) -> CellResult:
    entry = SelectionEntry(key=entry_key, label=entry_key,
                           kind=RuntimeKind.ARTS, cell="VAL/RETAIN/WB",
                           variant=entry_key)
    app = app or _app()
    cell = Cell(entry=entry, app=app, nodes=nodes, repeat=1,
                binary=Path("/nonexistent"), args=app.args, timeout_s=10)
    r = CellResult(cell=cell, status=status, wall_s=wall_s, note=note)
    r.e2e_s = e2e_s
    r.scalar = "1.0"
    return r


def _text(table) -> str:
    console = Console(record=True, width=200, file=open(os.devnull, "w"))
    console.print(table)
    return console.export_text()


def test_the_scaling_table_reports_the_runtime_stamp(tmp_path):
    result = _result("a", wall_s=12.0, e2e_s=9.0)
    text = _text(scaling_table([result], ["a"]))
    assert "9.00" in text and "12.00" not in text and "e2e seconds" in text
    path = tmp_path / "results.csv"
    write_results_csv([result], path)
    row = next(csv.DictReader(path.open()))
    assert float(row["e2e_s"]) == 9.0
    assert "app_s" not in row and "timing_metric" not in row


def test_a_stamped_cell_carries_no_wall_only_mark(tmp_path):
    r = _result("a", wall_s=9.0, e2e_s=1.5)
    assert "1.50w" not in _text(scaling_table([r], ["a"]))
    write_results_csv([r], tmp_path / "results.csv")
    row = next(csv.DictReader((tmp_path / "results.csv").open()))
    assert row["note"] == ""


def test_a_cell_measured_by_wall_is_marked_in_the_table_and_the_note(tmp_path):
    r = _result("a", wall_s=9.0, e2e_s=None)
    assert "9.00w" in _text(scaling_table([r], ["a"]))
    write_results_csv([r], tmp_path / "results.csv")
    row = next(csv.DictReader((tmp_path / "results.csv").open()))
    assert "wall-only" in row["note"]


def test_the_mark_is_added_to_a_note_the_cell_already_had(tmp_path):
    r = _result("a", wall_s=9.0, e2e_s=None, note="teardown hang")
    write_results_csv([r], tmp_path / "results.csv")
    row = next(csv.DictReader((tmp_path / "results.csv").open()))
    assert row["note"].startswith("teardown hang")
    assert "wall-only" in row["note"]


def test_a_failed_cell_is_not_called_wall_only(tmp_path):
    # It measured nothing at all; saying "wall-only" of every failure would
    # bury the completed runs whose number is the weaker kind.
    r = _result("a", wall_s=9.0, e2e_s=None, status=Status.FAIL, note="rc 1")
    write_results_csv([r], tmp_path / "results.csv")
    row = next(csv.DictReader((tmp_path / "results.csv").open()))
    assert row["note"] == "rc 1"


def test_the_mark_follows_the_observation_the_table_shows():
    # Repeats collapse to the best observation, so the mark describes that
    # one rather than the group it came from.
    stamped_best = [_result("a", wall_s=9.0, e2e_s=1.0),
                    _result("a", wall_s=4.0, e2e_s=None)]
    text = _text(scaling_table(stamped_best, ["a"]))
    assert "1.00" in text and "1.00w" not in text
    wall_best = [_result("a", wall_s=9.0, e2e_s=1.0),
                 _result("a", wall_s=0.5, e2e_s=None)]
    assert "0.50w" in _text(scaling_table(wall_best, ["a"]))


def test_the_summary_explains_the_mark_only_when_one_appears(tmp_path):
    from artsrun.model.plane import load_plane

    selection = Selection(
        profile="t", experiment="b", entries=["a"],
        apps={"app": [Version.BASE]}, node_counts=[1],
    )
    plane = load_plane()
    walled = [_result("a", wall_s=9.0, e2e_s=None)]
    text = write_summary(walled, vote(walled), [], selection, plane,
                         tmp_path / "w.txt")
    assert "w after a time" in text
    stamped = [_result("a", wall_s=9.0, e2e_s=1.5)]
    text = write_summary(stamped, vote(stamped), [], selection, plane,
                         tmp_path / "s.txt")
    assert "w after a time" not in text


def _laddered_app() -> ResolvedApp:
    """A row whose arguments follow the node count.  Synthetic: attack rows
    run in the control-main roster at one argument vector, so no shipped
    row actually ladders like this; the fixture exists only to exercise the
    scaling table's marking rule."""
    return ResolvedApp(
        name="attack", version=Version.BASE, binary="freerun_mix",
        cls=AppClass.MW, marker=r"RESULT", scalar_re=r"RESULT\s*=\s*([\d.]+)",
        scalar_kind=ScalarKind.FLOAT, args=["465"],
        args_by_nodes={1: ["15"], 2: ["15"], 4: ["45"]},
    )


def test_a_row_whose_arguments_follow_the_node_count_is_marked():
    app = _laddered_app()
    rs = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=n, app=app)
          for n in (1, 4)]
    assert "attack:base ~" in _text(scaling_table(rs, ["a"]))


def test_a_fixed_workload_row_is_not_marked():
    rs = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=n) for n in (1, 4)]
    assert "~" not in _text(scaling_table(rs, ["a"]))


def test_two_node_counts_running_the_same_arguments_are_not_marked():
    # The laddered row's 1n and 2n editions are equal, so a sweep over just
    # those two shows one workload and must not be marked.
    app = _laddered_app()
    rs = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=n, app=app)
          for n in (1, 2)]
    assert "~" not in _text(scaling_table(rs, ["a"]))


def test_a_failed_cell_does_not_by_itself_mark_a_row():
    # The mark explains rows the table shows; a cell that never produced an
    # observation contributes no column to explain.
    app = _laddered_app()
    rs = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=1, app=app),
          _result("a", wall_s=9.0, e2e_s=None, nodes=4, status=Status.FAIL,
                  app=app)]
    assert "~" not in _text(scaling_table(rs, ["a"]))


def test_the_wrf_flush_entry_has_a_label_and_a_colour_of_its_own():
    from artsrun.report import RT_COLOR, RT_LABEL, rt_key
    assert RT_LABEL["arts_wrf_flush"] == "DB-WRF·FLUSH"
    assert RT_COLOR["arts_wrf_flush"] not in {v for k, v in RT_COLOR.items() if k != "arts_wrf_flush"}
    assert rt_key("wrf_flush") == "arts_wrf_flush"


def test_the_fam_entries_name_their_design_point_and_have_colours_of_their_own():
    from artsrun.report import RT_COLOR, RT_LABEL, rt_key

    assert RT_LABEL["arts_excl_purge_fam_staged"] == "EXCL·WB·PURGE FAM-STAGED"
    assert RT_LABEL["arts_excl_purge_fam_direct"] == "EXCL·WB·PURGE FAM-DIRECT"
    for key in ("arts_excl_purge_fam_staged", "arts_excl_purge_fam_direct"):
        assert RT_COLOR[key] not in {v for k, v in RT_COLOR.items() if k != key}
        assert rt_key(key) == key


def test_the_summary_explains_the_ladder_mark_only_when_one_appears(tmp_path):
    from artsrun.model.plane import load_plane

    plane = load_plane()
    app = _laddered_app()
    selection = Selection(
        profile="t", experiment="b", entries=["a"],
        apps={"app": [Version.BASE]}, node_counts=[1, 4],
    )
    laddered = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=n, app=app)
                for n in (1, 4)]
    text = write_summary(laddered, vote(laddered), [], selection, plane,
                         tmp_path / "l.txt")
    assert "~ after an application" in text
    fixed = [_result("a", wall_s=9.0, e2e_s=1.0, nodes=n) for n in (1, 4)]
    text = write_summary(fixed, vote(fixed), [], selection, plane,
                         tmp_path / "f.txt")
    assert "~ after an application" not in text


def test_the_apps_table_marks_each_tier_by_the_row_that_supplies_it():
    # The DB-WRF mark on a terminal is its own style, not the entry's figure
    # tone; the restructured column is judged by the rewrite's row.
    from artsrun.cli import _tier_mark
    from artsrun.model.catalog import Version, load_catalog
    from artsrun.report import RT_COLOR, WRF_ELIGIBLE_STYLE

    assert WRF_ELIGIBLE_STYLE not in RT_COLOR.values()
    catalog = load_catalog()
    split = [a for a in catalog.rows
             if a.restructured_as and not a.unsupported
             and (a.unordered_writes is None)
             != (catalog.apps[a.restructured_as].unordered_writes is None)]
    if not split:
        pytest.skip("no row whose rewrite verdict differs from its own")
    row = split[0]
    base = _tier_mark(catalog, row, Version.BASE)
    rewrite = _tier_mark(catalog, row, Version.RESTRUCTURED)
    assert base != rewrite
    inside, outside = (base, rewrite) if row.unordered_writes is None else (rewrite, base)
    assert inside == f"[{WRF_ELIGIBLE_STYLE}]✓[/]"
    assert outside == "[green]✓[/green]"
    assert _tier_mark(catalog, row, Version.HINTED) in (base, "[dim]·[/dim]")

