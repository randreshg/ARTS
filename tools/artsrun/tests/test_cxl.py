"""CXL build contracts and once-per-cell region setup."""

import asyncio
import os
import subprocess
from dataclasses import replace

import pytest

from artsrun.build import BuildError, ensure_build_dir, require_cxl
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import Profile
from artsrun.model.selection import Selection
from artsrun.paths import cxl_script
from artsrun.run.command import build_command, cxl_wrap
from artsrun.run.flux import _launch as flux_launch
from artsrun.run.manifest import describe_command
from artsrun.run.slurm import _launch as slurm_launch
from test_command import _ssh_profile
from test_slurm import _cell, _local_profile, _slurm_profile


def selection(cxl=True, entries=None):
    return Selection(profile="t", benchset="b", entries=entries or ["arts_excl_purge"],
                     apps={"nqueens": ["base"]}, node_counts=[1], cxl=cxl)


def cache(tree, **values):
    (tree / "CMakeCache.txt").write_text(
        "".join(f"{k}:STRING={v}\n" for k, v in values.items()))


def test_cxl_requires_real_library_and_headers(tmp_path):
    rapid = tmp_path / "rapid" / "inc"
    rapid.mkdir(parents=True)
    lib = tmp_path / "arts_cxl_lib"
    so = lib / "build/src/libarts_cxl_lib.so"
    so.parent.mkdir(parents=True)
    so.touch()
    opts = dict(ARTS_USE_CXL="ON", ARTS_USE_FAKE_CXL_LIB="OFF",
                ARTS_CXL_RAPID_INCLUDE_DIR=rapid, ARTS_CXL_LIB_DIR=lib,
                ARTS_MEMORY_MODEL="OCR", ARTS_COHERENCE_PROTOCOL="EXCL",
                ARTS_RELEASE_POLICY="PURGE", ARTS_WRITE_POLICY="WB")
    cache(tmp_path, **opts)
    replay = Selection.model_validate_json(selection().model_dump_json())
    require_cxl(tmp_path, replay)
    with pytest.raises(BuildError, match="requires ARTS_USE_CXL=OFF"):
        require_cxl(tmp_path, selection(False))
    cache(tmp_path, **(opts | {"ARTS_USE_FAKE_CXL_LIB": "ON"}))
    with pytest.raises(BuildError, match="fake_arts_cxl_lib"):
        require_cxl(tmp_path, replay)
    cache(tmp_path, **(opts | {"ARTS_USE_CXL": "OFF"}))
    with pytest.raises(BuildError, match="requires ARTS_USE_CXL=ON"):
        require_cxl(tmp_path, replay)
    so.unlink()
    cache(tmp_path, **opts)
    with pytest.raises(BuildError, match="libarts_cxl_lib.so"):
        require_cxl(tmp_path, replay)


@pytest.mark.parametrize("key,value", [("ARTS_COHERENCE_PROTOCOL", "VAL"),
                                     ("ARTS_RELEASE_POLICY", "RETAIN"),
                                     ("ARTS_WRITE_POLICY", "WT")])
def test_cxl_rejects_incompatible_build_axes(tmp_path, key, value):
    opts = dict(ARTS_USE_CXL="ON", ARTS_MEMORY_MODEL="OCR",
                ARTS_COHERENCE_PROTOCOL="EXCL", ARTS_RELEASE_POLICY="PURGE",
                ARTS_WRITE_POLICY="WB")
    cache(tmp_path, **(opts | {key: value}))
    with pytest.raises(BuildError, match=key):
        require_cxl(tmp_path, selection())


def test_cxl_selection_rejects_other_plane_entries():
    from artsrun.model.catalog import load_catalog

    plane = load_plane()
    assert plane.cxl_entry_keys == ["arts_excl_purge", "xsocr"]
    valid = selection(entries=["arts_excl_purge", "xsocr"])
    valid.validate_against(plane, load_catalog(), _local_profile())
    for entry in ("arts_excl_retain", "arts_val_wb", "ocrvx", "hpx"):
        with pytest.raises(ValueError, match="CXL requires EXCL \\+ PURGE"):
            selection(entries=["arts_excl_purge", entry]).validate_against(
                plane, load_catalog(), _local_profile())


def test_new_cxl_build_configures_excl_purge(tmp_path, monkeypatch):
    tree = tmp_path / "build"
    rapid = tmp_path / "rapid/include"
    rapid.mkdir(parents=True)
    lib = tmp_path / "arts_cxl_lib"
    so = lib / "build/src/libarts_cxl_lib.so"
    so.parent.mkdir(parents=True)
    so.touch()
    chosen = selection().model_copy(update={
        "cxl_rapid_include_dir": str(rapid), "cxl_lib_dir": str(lib),
    })
    commands = []

    class Configure:
        def __init__(self, argv, **_kwargs):
            commands.append(argv)
            tree.mkdir()
            (tree / "build.ninja").touch()
            cache(tree, ARTS_USE_CXL="ON", ARTS_USE_FAKE_CXL_LIB="OFF",
                  ARTS_MEMORY_MODEL="OCR", ARTS_COHERENCE_PROTOCOL="EXCL",
                  ARTS_RELEASE_POLICY="PURGE", ARTS_WRITE_POLICY="WB",
                  ARTS_CXL_RAPID_INCLUDE_DIR=rapid, ARTS_CXL_LIB_DIR=lib)
            self.stdout = iter(())

        def wait(self):
            return 0

    monkeypatch.setattr("artsrun.build.subprocess.Popen", Configure)
    ensure_build_dir(tree, bootstrap=True, selection=chosen)
    cmd = commands[0]
    assert "-DARTS_USE_CXL=On" in cmd
    assert "-DARTS_COHERENCE_PROTOCOL=EXCL" in cmd
    assert "-DARTS_RELEASE_POLICY=PURGE" in cmd
    assert "-DARTS_WRITE_POLICY=WB" in cmd


def test_cxl_wraps_whole_launch_once():
    script = str(cxl_script())
    arts = replace(_cell(RuntimeKind.ARTS, 2), cxl=True)
    ref = replace(_cell(RuntimeKind.XSOCR, 2), cxl=True)
    assert cxl_wrap(build_command(arts, _local_profile()), arts) == [
        "bash", script, "/opt/bin/app", "12", "4"]
    assert cxl_wrap(["srun", *build_command(ref, _slurm_profile())], ref) == [
        "srun", *build_command(ref, _slurm_profile())]
    assert slurm_launch(arts, _slurm_profile()).startswith(
        f"bash {script} srun --ntasks-per-node=1 -N 2")
    flux = Profile.model_validate({"name": "t", "launcher": "flux", "nodes": [2],
                                   "workers": 15, "progress": 1,
                                   "ports": [25000], "flux": {}})
    assert flux_launch(arts, flux).startswith(f"bash {script} flux run -N 2")


def test_ssh_manifest_shows_the_real_cxl_command():
    arts = replace(_cell(RuntimeKind.ARTS, 2), cxl=True)
    recorded = describe_command(arts, _ssh_profile(), cxl_script())
    assert recorded == {
        "command": f"timeout -k 1 60 bash {cxl_script()} /opt/bin/app 12 4",
        "script": None,
    }


def test_wrapper_uses_absolute_prep_path_and_cleans_up_after_failure(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    events = tmp_path / "events"
    (bin_dir / "rapidutil").write_text('#!/bin/sh\necho "clean $*" >> "$EVENTS"\n')
    (bin_dir / "python3").write_text('#!/bin/sh\necho "prep $*" >> "$EVENTS"\n')
    (bin_dir / "app").write_text('#!/bin/sh\necho "app $*" >> "$EVENTS"\nexit 7\n')
    for path in bin_dir.iterdir():
        path.chmod(0o755)
    proc = subprocess.run(
        ["bash", str(cxl_script()), str(bin_dir / "app"), "a b"],
        cwd=tmp_path, env=os.environ | {"PATH": f"{bin_dir}:{os.environ['PATH']}",
                                     "EVENTS": str(events)}, check=False,
    )
    assert proc.returncode == 7
    assert events.read_text().splitlines() == [
        "clean remove -r shared", "clean remove -r fam_ranks",
        f"prep {cxl_script().parent / 'script/run.py'} --exe /usr/bin/pwd",
        "app a b", "clean remove -r shared", "clean remove -r fam_ranks",
    ]


def test_run_screen_records_cxl_and_new_build_paths(tmp_path):
    from textual.widgets import Input, Switch, TabbedContent
    from artsrun.tui.app import ArtsRunApp
    from artsrun.tui.panels import PlanePanel

    async def check():
        app = ArtsRunApp()
        async with app.run_test() as pilot:
            app.query_one(TabbedContent).active = "tab-run"
            await pilot.pause()
            toggle = app.query_one("#run-cxl", Switch)
            rapid = app.query_one("#run-cxl-rapid", Input)
            lib = app.query_one("#run-cxl-lib", Input)
            assert not toggle.value and rapid.disabled and lib.disabled
            plane = app.query_one("#plane", PlanePanel)
            before = plane.selected()
            app.query_one("#run-build-dir", Input).value = str(tmp_path / "build")
            toggle.value = True
            await pilot.pause()
            assert not rapid.disabled and not lib.disabled
            assert plane.selected() == app.plane.cxl_entry_keys
            assert all(t.disabled for t in plane.toggles
                       if t.ident not in app.plane.cxl_entry_keys)
            plane.toggle_all()
            assert plane.selected() == []
            plane.toggle_all()
            assert plane.selected() == app.plane.cxl_entry_keys
            rapid.value = str(tmp_path / "rapid")
            lib.value = str(tmp_path / "lib")
            chosen = app.build_selection()
            assert chosen is not None
            assert chosen.cxl and chosen.build_dir == str(tmp_path / "build")
            assert chosen.cxl_rapid_include_dir == str(tmp_path / "rapid")
            assert chosen.cxl_lib_dir == str(tmp_path / "lib")
            replay = Selection.model_validate_json(chosen.model_dump_json())
            assert replay.cxl and replay.cxl_lib_dir == chosen.cxl_lib_dir
            toggle.value = False
            await pilot.pause()
            assert rapid.disabled and lib.disabled
            assert plane.selected() == before
            assert all(not t.disabled for t in plane.toggles)
            assert app.build_selection().cxl_rapid_include_dir is None

    asyncio.run(check())
