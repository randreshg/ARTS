"""The end grace G: B per cell, the campaign's G, its refusals and records."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path

import pytest

from artsrun.model.catalog import AppClass, Version
from artsrun.model.counters import (Counterset, CounterSetting, Level, Mode,
                                    load_counter_catalog)
from artsrun.model.experiment import ResolvedApp
from artsrun.model.plane import RuntimeKind, SelectionEntry
from artsrun.model.profile import Profile
from artsrun.run.command import build_env
from artsrun.run.grace import (EndGraceError, apply_grace, cell_bound_s,
                               reconcile, record_bytes, startup_budget_s)
from artsrun.run.manifest import Manifest, write_manifest
from artsrun.run.types import Cell


def _cell(kind: RuntimeKind = RuntimeKind.ARTS, timeout_s: int = 600) -> Cell:
    entry = SelectionEntry(key=f"{kind.value}_e", label=kind.value, kind=kind,
                           cell=kind.value)
    app = ResolvedApp(name="app", version=Version.BASE, binary="app",
                      cls=AppClass.TASK, marker="X", scalar_re="X")
    return Cell(entry=entry, app=app, nodes=2, repeat=1,
                binary=Path("/opt/bin/app"), args=[], timeout_s=timeout_s,
                cpu_width=16)


def _profile() -> Profile:
    return Profile.model_validate({
        "name": "t", "launcher": "local", "nodes": [1, 2],
        "workers": 4, "progress": 1})


def _counters(level: Level, n: int = 28) -> Counterset:
    names = load_counter_catalog().names[:n]
    assert len(names) == n
    return Counterset(
        name="s", capture_interval=100,
        counters={x: CounterSetting(mode=Mode.PERIODIC, level=level)
                  for x in names})


def test_defaults_give_b_30_and_g_30():
    cell = _cell()
    assert cell_bound_s(cell, {}) == 30
    [out] = apply_grace([cell], 30, False, {})
    assert out.end_grace_s == 30 and out.end_grace_raised_from is None


def test_node_periodic_record_estimate_leaves_g_at_30():
    cell, cs = _cell(), _counters(Level.NODE)
    assert record_bytes(cell, cs, 112, 100) == 6_730_816
    assert cell_bound_s(cell, {}, counterset=cs, threads=112) == 30
    [out] = apply_grace([cell], 30, False, {}, counterset=cs, threads=112)
    assert out.end_grace_s == 30


def test_thread_periodic_cell_raises_g_and_notes_it():
    cell, cs = _cell(), _counters(Level.THREAD)
    assert record_bytes(cell, cs, 112, 100) == 753_401_216
    assert cell_bound_s(cell, {}, counterset=cs, threads=112) == 192
    [out] = apply_grace([cell], 30, False, {}, counterset=cs, threads=112)
    assert out.end_grace_s == 192 and out.end_grace_raised_from == 30


def test_an_explicit_value_below_b_is_refused_naming_cell_and_b():
    cell, cs = _cell(), _counters(Level.THREAD)
    with pytest.raises(EndGraceError) as err:
        apply_grace([cell], 120, True, {}, counterset=cs, threads=112)
    assert cell.key in str(err.value) and "192" in str(err.value)
    [out] = apply_grace([cell], 192, True, {}, counterset=cs, threads=112)
    assert out.end_grace_s == 192 and out.end_grace_raised_from is None


def test_a_startup_budget_in_the_environment_raises_b():
    cell = _cell()
    env = {"ARTS_STARTUP_TIMEOUT_S": "60"}
    assert cell_bound_s(cell, env) == 70
    [out] = apply_grace([cell], 30, False, env)
    assert out.end_grace_s == 70


@pytest.mark.parametrize("raw", ["9", "86401", "10s", "0", "+20", " 20", "",
                                 "\u0661\u0660"])
def test_a_startup_budget_outside_the_runtime_rule_is_refused(raw):
    with pytest.raises(EndGraceError, match="ARTS_STARTUP_TIMEOUT_S"):
        startup_budget_s({"ARTS_STARTUP_TIMEOUT_S": raw})


def test_the_budget_floor_ceiling_and_default():
    assert startup_budget_s({"ARTS_STARTUP_TIMEOUT_S": "10"}) == 10
    assert startup_budget_s({"ARTS_STARTUP_TIMEOUT_S": "86400"}) == 86400
    assert startup_budget_s({}) == 20


def test_reference_cells_carry_no_grace_and_no_bound_check():
    cell = _cell(RuntimeKind.XSOCR)
    [out] = apply_grace([cell], 1, True, {}, counterset=_counters(Level.THREAD))
    assert out is cell
    assert "ARTS_END_GRACE_S" not in build_env(out, _profile())


def test_every_arts_cell_environment_carries_its_grace():
    [cell] = apply_grace([_cell()], 45, False, {})
    assert build_env(cell, _profile())["ARTS_END_GRACE_S"] == "45"


def test_g_is_range_checked():
    for bad in (0, 86401):
        with pytest.raises(EndGraceError):
            reconcile(bad, None)
    assert reconcile(None, None) == (30, False)
    assert reconcile(7, None) == (7, True)


def test_a_resume_keeps_the_recorded_value_and_refuses_another():
    assert reconcile(None, (60, True)) == (60, True)
    assert reconcile(60, (60, True)) == (60, True)
    assert reconcile(None, (30, False)) == (30, False)
    with pytest.raises(EndGraceError):
        reconcile(90, (60, True))
    with pytest.raises(EndGraceError):
        reconcile(90, (30, False))


def test_manifest_records_g_whether_given_and_the_raise(tmp_path):
    cs = _counters(Level.THREAD)
    cells = apply_grace([_cell(), _cell(RuntimeKind.XSOCR)], 30, False, {},
                        counterset=cs, threads=112)
    path = write_manifest(tmp_path, cells, [], _profile(), tmp_path,
                          not_applicable=[], end_grace_s=30,
                          end_grace_given=False)
    doc = json.loads(path.read_text())
    assert doc["end_grace_s"] == 30 and doc["end_grace_given"] is False
    rows = {r["entry"]: r for r in doc["cells"]}
    assert rows["arts_e"]["end_grace_s"] == 192
    assert rows["arts_e"]["end_grace_raised_from"] == 30
    assert rows["xsocr_e"]["end_grace_s"] is None
    manifest = Manifest.load(tmp_path)
    assert manifest.recorded_end_grace == (30, False)
    arts = [c for c in manifest.cells if c.entry.kind is RuntimeKind.ARTS][0]
    assert arts.end_grace_s == 192 and arts.end_grace_raised_from == 30


def test_an_explicit_value_equal_to_the_recorded_default_turns_given_on():
    assert reconcile(30, (30, False)) == (30, True)
    cell, cs = _cell(), _counters(Level.THREAD)
    g, given = reconcile(30, (30, False))
    with pytest.raises(EndGraceError):
        apply_grace([cell], g, given, {}, counterset=cs, threads=112)


def test_a_resume_under_another_startup_budget_is_refused():
    from artsrun.run.grace import check_budget

    assert check_budget(None, {}) == 20
    assert check_budget(60, {"ARTS_STARTUP_TIMEOUT_S": "60"}) == 60
    with pytest.raises(EndGraceError) as err:
        check_budget(20, {"ARTS_STARTUP_TIMEOUT_S": "60"})
    assert "60" in str(err.value) and "20" in str(err.value)


def test_the_manifest_records_the_startup_budget(tmp_path):
    path = write_manifest(tmp_path, [_cell()], [], _profile(), tmp_path,
                          not_applicable=[], startup_budget_s=60)
    assert json.loads(path.read_text())["startup_budget_s"] == 60
    assert Manifest.load(tmp_path).startup_budget_s == 60


def test_the_epoch_interval_is_the_one_the_rendered_cfg_carries():
    import math
    import re

    from artsrun.render import render_arts

    cs = _counters(Level.THREAD).model_copy(update={"capture_interval": 250})
    text = render_arts(_profile(), 2, counter_folder="/runs/c",
                       capture_interval=cs.capture_interval)
    cfg = int(re.search(r"counter_capture_interval=(\d+)", text).group(1))
    cell = _cell()
    size = record_bytes(cell, cs, 112, cfg)
    assert size != record_bytes(cell, cs, 112, 100)
    assert cell_bound_s(cell, {}, counterset=cs, threads=112) == max(
        12 + math.ceil(size / (4 << 20)), 30)
