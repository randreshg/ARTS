"""FAM-device mode: build contracts, entry selection, and once-per-cell
region setup around the fabric-attached-memory cells."""

import asyncio
import os
import subprocess
from dataclasses import replace
from pathlib import Path

import pytest

from artsrun.build import BuildError, ensure_build_dir, require_cxl
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import Profile
from artsrun.model.selection import Selection
from artsrun.paths import cxl_script
from artsrun.run.command import build_command, build_env, cxl_wrap, flush_log_path
from artsrun.run.flux import _launch as flux_launch
from artsrun.run.flux import job_script as flux_job_script
from artsrun.run.manifest import describe_command
from artsrun.run.slurm import _launch as slurm_launch
from artsrun.run.slurm import job_script as slurm_job_script
from test_command import _ssh_profile
from test_slurm import _cell, _local_profile, _slurm_profile

FAM = ["arts_excl_purge_fam_staged", "arts_excl_purge_fam_direct"]


def selection(cxl=True, entries=None):
    return Selection(profile="t", benchset="b", entries=entries or FAM,
                     apps={"nqueens": ["base"]}, node_counts=[1], cxl=cxl)


def cache(tree, **values):
    (tree / "CMakeCache.txt").write_text(
        "".join(f"{k}:STRING={v}\n" for k, v in values.items()))


def device(tmp_path):
    include = tmp_path / "device" / "include"
    include.mkdir(parents=True)
    library = tmp_path / "device" / "lib" / "libdevice.so"
    library.parent.mkdir(parents=True)
    library.touch()
    return include, library


def fam_cell(nodes, key="arts_excl_purge_fam_staged", cxl=True):
    return replace(_cell(RuntimeKind.ARTS, nodes),
                   entry=load_plane().entry(key), cxl=cxl)


def test_fam_device_requires_the_device_library_itself(tmp_path):
    include, library = device(tmp_path)
    opts = dict(ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="OFF",
                ARTS_FAM_DEVICE_INCLUDE_DIR=include,
                ARTS_FAM_DEVICE_LIBRARY=library)
    cache(tmp_path, **opts)
    replay = Selection.model_validate_json(selection().model_dump_json())
    require_cxl(tmp_path, replay)
    with pytest.raises(BuildError, match="only --cxl runs a DEVICE tree"):
        require_cxl(tmp_path, selection(False))
    cache(tmp_path, **(opts | {"ARTS_FAM_DEVICE_VENDORED": "ON"}))
    with pytest.raises(BuildError, match="ARTS_FAM_DEVICE_VENDORED"):
        require_cxl(tmp_path, replay)
    vendored = tmp_path / "third_party" / "fake_arts_cxl_lib" / "inc"
    vendored.mkdir(parents=True)
    cache(tmp_path, **(opts | {"ARTS_FAM_DEVICE_INCLUDE_DIR": vendored}))
    with pytest.raises(BuildError, match="fake_arts_cxl_lib"):
        require_cxl(tmp_path, replay)
    for backend in ("SHM", "OFF"):
        cache(tmp_path, **(opts | {"ARTS_FAM_BACKEND": backend}))
        with pytest.raises(BuildError, match="requires ARTS_FAM_BACKEND=DEVICE"):
            require_cxl(tmp_path, replay)
        require_cxl(tmp_path, selection(False))
    library.unlink()
    cache(tmp_path, **opts)
    with pytest.raises(BuildError, match="ARTS_FAM_DEVICE_LIBRARY"):
        require_cxl(tmp_path, replay)


def test_fam_device_tree_must_match_the_requested_paths(tmp_path):
    include, library = device(tmp_path)
    cache(tmp_path, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_INCLUDE_DIR=include,
          ARTS_FAM_DEVICE_LIBRARY=library)
    other = tmp_path / "other.so"
    other.touch()
    asked = selection().model_copy(update={"fam_device_library": str(other)})
    with pytest.raises(BuildError, match="ARTS_FAM_DEVICE_LIBRARY"):
        require_cxl(tmp_path, asked)


def test_fam_device_selection_is_the_fam_entries():
    from artsrun.model.catalog import load_catalog

    plane = load_plane()
    assert plane.fam_entry_keys == FAM
    assert not hasattr(plane, "cxl_entry_keys")
    selection().validate_against(plane, load_catalog(), _local_profile())
    for entry in ("arts_excl_purge", "xsocr", "arts_val_wb", "ocrvx", "hpx"):
        with pytest.raises(ValueError, match="FAM-device mode"):
            selection(entries=[*FAM, entry]).validate_against(
                plane, load_catalog(), _local_profile())


def test_new_fam_device_build_configures_the_device_backend(tmp_path, monkeypatch):
    tree = tmp_path / "build"
    include, library = device(tmp_path)
    chosen = selection().model_copy(update={
        "fam_device_include_dir": str(include), "fam_device_library": str(library),
    })
    commands = []

    class Configure:
        def __init__(self, argv, **_kwargs):
            commands.append(argv)
            tree.mkdir()
            (tree / "build.ninja").touch()
            cache(tree, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="OFF",
                  ARTS_FAM_DEVICE_INCLUDE_DIR=include,
                  ARTS_FAM_DEVICE_LIBRARY=library)
            self.stdout = iter(())

        def wait(self):
            return 0

    monkeypatch.setattr("artsrun.build.subprocess.Popen", Configure)
    ensure_build_dir(tree, bootstrap=True, selection=chosen)
    cmd = commands[0]
    assert "-DARTS_FAM_BACKEND=DEVICE" in cmd
    assert "-DARTS_FAM_DEVICE_VENDORED=OFF" in cmd
    assert f"-DARTS_FAM_DEVICE_INCLUDE_DIR={include}" in cmd
    assert f"-DARTS_FAM_DEVICE_LIBRARY={library}" in cmd
    assert not any("ARTS_USE_CXL" in a or "COHERENCE_PROTOCOL" in a for a in cmd)


def test_new_fam_device_build_needs_both_paths(tmp_path):
    with pytest.raises(BuildError, match="--fam-device-library"):
        ensure_build_dir(tmp_path / "build", bootstrap=True, selection=selection())


def test_fam_device_wraps_whole_launch_once():
    script = str(cxl_script())
    fam = fam_cell(2)
    assert fam.fam_device
    assert cxl_wrap(build_command(fam, _local_profile()), fam) == [
        "bash", script, "/opt/bin/app", "12", "4"]
    # Neither a non-FAM ARTS cell nor a FAM cell outside the mode is wrapped.
    plain = replace(_cell(RuntimeKind.ARTS, 2), cxl=True)
    assert not plain.fam_device
    assert cxl_wrap(build_command(plain, _local_profile()), plain) == \
        build_command(plain, _local_profile())
    outside = fam_cell(2, cxl=False)
    assert cxl_wrap(build_command(outside, _local_profile()), outside) == \
        build_command(outside, _local_profile())
    assert slurm_launch(fam, _slurm_profile()).startswith(
        f"bash {script} srun --ntasks-per-node=1 -N 2")
    flux = Profile.model_validate({"name": "t", "launcher": "flux", "nodes": [2],
                                   "workers": 15, "progress": 1,
                                   "ports": [25000], "flux": {}})
    assert flux_launch(fam, flux).startswith(f"bash {script} flux run -N 2")


def test_ssh_manifest_shows_the_wrapped_command():
    fam = fam_cell(2, key="arts_excl_purge_fam_direct")
    recorded = describe_command(fam, _ssh_profile(), cxl_script())
    assert recorded == {
        "command": f"timeout -k 1 60 bash {cxl_script()} /opt/bin/app 12 4",
        "script": None,
    }


def test_one_rank_flush_trace_lands_beside_the_cell_log(tmp_path):
    fam = fam_cell(1)
    env = build_env(fam, _local_profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == str(flush_log_path(tmp_path, fam))
    assert Path(env["ARTS_FLUSH_LOG"]).parent == tmp_path
    for other in (fam_cell(1, cxl=False), replace(_cell(RuntimeKind.ARTS, 1), cxl=True)):
        assert "ARTS_FLUSH_LOG" not in build_env(other, _local_profile(), tmp_path)
    marker = tmp_path / f"{fam.slug}.rc"
    trace = f"export ARTS_FLUSH_LOG={flush_log_path(tmp_path, fam)}"
    assert trace in slurm_job_script(fam, _slurm_profile(), marker)
    flux = Profile.model_validate({"name": "t", "launcher": "flux", "nodes": [1],
                                   "workers": 15, "progress": 1,
                                   "ports": [25000], "flux": {}})
    assert trace in flux_job_script(fam, flux, marker)


def test_multi_rank_flush_trace_is_discarded(tmp_path):
    fam = fam_cell(2)
    env = build_env(fam, _local_profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == "/dev/null"
    marker = tmp_path / f"{fam.slug}.rc"
    assert "export ARTS_FLUSH_LOG=/dev/null" in slurm_job_script(
        fam, _slurm_profile(), marker)
    assert not flush_log_path(tmp_path, fam).exists()


def test_old_device_option_spellings_name_the_rename(tmp_path):
    from typer.testing import CliRunner

    from artsrun.cli import app

    runner = CliRunner()
    for old, new in (("--cxl-rapid-include-dir", "--fam-device-include-dir"),
                     ("--cxl-lib-dir", "--fam-device-library")):
        result = runner.invoke(app, ["run", "-p", "t", "--cxl", old, str(tmp_path)])
        assert result.exit_code != 0
        assert new in result.output


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


def test_run_screen_records_fam_device_mode_and_new_build_paths(tmp_path):
    from textual.widgets import Input, Switch, TabbedContent
    from artsrun.tui.app import ArtsRunApp
    from artsrun.tui.panels import PlanePanel

    async def check():
        app = ArtsRunApp()
        async with app.run_test() as pilot:
            app.query_one(TabbedContent).active = "tab-run"
            await pilot.pause()
            toggle = app.query_one("#run-fam-device", Switch)
            include = app.query_one("#run-fam-include", Input)
            library = app.query_one("#run-fam-library", Input)
            assert not toggle.value and include.disabled and library.disabled
            plane = app.query_one("#plane", PlanePanel)
            before = plane.selected()
            app.query_one("#run-build-dir", Input).value = str(tmp_path / "build")
            toggle.value = True
            await pilot.pause()
            assert not include.disabled and not library.disabled
            assert plane.selected() == app.plane.fam_entry_keys
            assert all(t.disabled for t in plane.toggles
                       if t.ident not in app.plane.fam_entry_keys)
            plane.toggle_all()
            assert plane.selected() == []
            plane.toggle_all()
            assert plane.selected() == app.plane.fam_entry_keys
            include.value = str(tmp_path / "include")
            library.value = str(tmp_path / "libdevice.so")
            chosen = app.build_selection()
            assert chosen is not None
            assert chosen.cxl and chosen.build_dir == str(tmp_path / "build")
            assert chosen.fam_device_include_dir == str(tmp_path / "include")
            assert chosen.fam_device_library == str(tmp_path / "libdevice.so")
            replay = Selection.model_validate_json(chosen.model_dump_json())
            assert replay.cxl and replay.fam_device_library == chosen.fam_device_library
            toggle.value = False
            await pilot.pause()
            assert include.disabled and library.disabled
            assert plane.selected() == before
            assert all(not t.disabled for t in plane.toggles)
            assert app.build_selection().fam_device_include_dir is None

    asyncio.run(check())
