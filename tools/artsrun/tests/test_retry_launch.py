"""A mesh failure relaunched through the real backends, with fake launchers
on PATH: the attempts a campaign keeps, the exclusions each scheduler is
handed, the track's retry events and the report's attempts column."""

from __future__ import annotations

import csv
import json
import stat
from pathlib import Path
from types import SimpleNamespace

import pytest

from artsrun.campaign import Campaign
from artsrun.model.catalog import AppClass, ScalarKind, Version, load_catalog
from artsrun.model.experiment import Experiment, ResolvedApp
from artsrun.model.plane import RuntimeKind, SelectionEntry, load_plane
from artsrun.model.profile import Profile
from artsrun.model.selection import Selection
from artsrun.run.types import Cell

from launcher_shims import install

MESH = ("ARTS_MESH_FAILED rank=1 host=n1 phase=connect missing=3 "
        "missing_hosts=n3 more=0\n")
FAIL = {"out": MESH, "rc": 75}
GOOD = {"out": "DONE 1\n[E2E] 0.5\n", "rc": 0}


def _cell(nodes: int, binary: Path = Path("/opt/bin/app")) -> Cell:
    app = ResolvedApp(name="app", version=Version.BASE, binary="app",
                      cls=AppClass.TASK, marker="DONE",
                      scalar_re=r"DONE (\d+)", scalar_kind=ScalarKind.INT)
    entry = SelectionEntry(key="arts_val_retain_owner",
                           label="arts_val_retain_owner",
                           kind=RuntimeKind.ARTS, cell="VAL/RETAIN/OWNER",
                           variant="arts_val_retain_owner")
    return Cell(entry=entry, app=app, nodes=nodes, repeat=1, binary=binary,
                args=[], timeout_s=30, cpu_width=4)


def _profile(launcher: str, nodes: int) -> Profile:
    data = {"name": "p", "launcher": launcher, "nodes": [nodes],
            "workers": 3, "progress": 1}
    if launcher in ("slurm", "flux"):
        data.update(ports=25000, **{launcher: {}})
    return Profile.model_validate(data)


def _campaign(tmp_path, monkeypatch, launcher, nodes, *, cell=None,
              retries=None) -> tuple[Campaign, Cell]:
    cell = cell or _cell(nodes)
    for mod in ("artsrun.run.slurm", "artsrun.run.flux", "artsrun.run.local",
                "artsrun.run.manifest"):
        monkeypatch.setattr(f"{mod}.scratch_dir", lambda: tmp_path / "scratch")
    monkeypatch.setattr("artsrun.campaign.wall_cache_path",
                        lambda: tmp_path / "wall.json")
    monkeypatch.setattr(Campaign, "build_plan",
                        lambda self, **kw: SimpleNamespace(targets=[]))
    monkeypatch.setattr("artsrun.campaign.build", lambda *a, **k: None)
    monkeypatch.setattr(Campaign, "cells", lambda self: ([cell], []))
    monkeypatch.setattr(Campaign, "not_applicable", lambda self: [])
    sel = Selection(profile="p", experiment="b", default_entries=[],
                    entries=[cell.entry.key], apps={"app": ["base"]},
                    node_counts=[nodes])
    camp = Campaign(selection=sel, plane=load_plane(), catalog=load_catalog(),
                    experiment=Experiment(entries=[cell.entry.key], name="b",
                                          apps={}),
                    profile=_profile(launcher, nodes),
                    build_dir=tmp_path / "build", run_dir=tmp_path / "run",
                    **({} if retries is None else {"mesh_retries": retries}))
    return camp, cell


def _track(run_dir: Path) -> list[dict]:
    return [json.loads(x) for x in
            (run_dir / "track.jsonl").read_text().splitlines()]


def _csv(run_dir: Path) -> list[dict]:
    return list(csv.DictReader((run_dir / "results.csv").open()))


@pytest.mark.parametrize("launcher, flag", [
    ("slurm", "--exclude=n3"), ("flux", "--requires=-host:n3")])
def test_a_scheduler_relaunch_excludes_the_suspect(
        tmp_path, monkeypatch, launcher, flag):
    shims = install(tmp_path, monkeypatch, [FAIL, GOOD])
    camp, cell = _campaign(tmp_path, monkeypatch, launcher, 4)
    camp.run()
    submit = shims.argv("sbatch") if launcher == "slurm" else \
        shims.argv("flux", "batch")
    assert len(submit) == 2
    assert flag not in submit[0] and flag in submit[1]
    cells = tmp_path / "run" / "cells"
    assert (cells / f"{cell.slug}.attempt1.log").is_file()
    assert (cells / f"{cell.slug}.log").is_file()
    [row] = _csv(tmp_path / "run")
    assert row["status"] == "ok" and row["attempts"] == "2"
    retries = [r for r in _track(tmp_path / "run") if r["event"] == "retry"]
    assert len(retries) == 1
    assert retries[0]["extra"]["attempt"] == 2
    assert retries[0]["extra"]["exclude"] == ["n3"]
    assert json.loads((tmp_path / "run" / "manifest.json").read_text()
                      )["mesh_retries"] == 2


def test_zero_retries_leaves_the_failure_alone(tmp_path, monkeypatch):
    shims = install(tmp_path, monkeypatch, [FAIL, GOOD])
    camp, cell = _campaign(tmp_path, monkeypatch, "slurm", 4, retries=0)
    camp.run()
    assert len(shims.argv("sbatch")) == 1
    [row] = _csv(tmp_path / "run")
    assert row["status"] == "fail" and row["attempts"] == "1"
    assert row["note"].startswith("mesh failure (connect)")


def test_a_local_relaunch_runs_in_place(tmp_path, monkeypatch):
    marker = tmp_path / "ran-once"
    script = tmp_path / "app.sh"
    script.write_text(
        "#!/bin/sh\n"
        f'if [ ! -e {marker} ]; then touch {marker}; printf %s "{MESH}"; '
        "exit 75; fi\n"
        "echo DONE 1; echo '[E2E] 0.5'\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    camp, cell = _campaign(tmp_path, monkeypatch, "local", 1,
                           cell=_cell(1, script))
    camp.run()
    cells = tmp_path / "run" / "cells"
    assert (cells / f"{cell.slug}.attempt1.log").is_file()
    [row] = _csv(tmp_path / "run")
    assert row["status"] == "ok" and row["attempts"] == "2"
    retry = [r for r in _track(tmp_path / "run") if r["event"] == "retry"]
    assert retry[0]["extra"]["exclude"] == []


def test_a_resume_after_a_kill_between_keep_and_submit_continues(
        tmp_path, monkeypatch):
    shims = install(tmp_path, monkeypatch, [GOOD])
    camp, cell = _campaign(tmp_path, monkeypatch, "slurm", 4)
    cells = tmp_path / "run" / "cells"
    cells.mkdir(parents=True)
    (cells / f"{cell.slug}.attempt1.log").write_text(MESH)
    (cells / f"{cell.slug}.attempt1.rc").write_text("75 1.0 2.0\n")
    camp.run(resume=True)
    [submit] = shims.argv("sbatch")
    assert "--exclude=n3" in submit
    [row] = _csv(tmp_path / "run")
    assert row["status"] == "ok" and row["attempts"] == "2"
    assert (cells / f"{cell.slug}.log").is_file()


def test_a_resume_keeps_the_recorded_retry_count_and_refuses_another(
        tmp_path, monkeypatch):
    install(tmp_path, monkeypatch, [GOOD])
    camp, cell = _campaign(tmp_path, monkeypatch, "slurm", 4, retries=1)
    camp.run()
    from artsrun.run.manifest import Manifest

    assert Manifest.load(tmp_path / "run").mesh_retries == 1
    from artsrun.run.retry import resolve_mesh_retries

    assert resolve_mesh_retries(None, 1) == 1
    with pytest.raises(ValueError):
        resolve_mesh_retries(2, 1)
