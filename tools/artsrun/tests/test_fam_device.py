"""The FAM device library: named by the profile, the tree made to match it
or the campaign stopped, and the device's region setup around exactly the
FAM cells that run on the device library itself."""

import os
import subprocess
from dataclasses import replace
from pathlib import Path

import pytest

from artsrun.build import (BuildError, configure_fam_device, ensure_build_dir,
                           fam_device_mismatch, fam_device_options)
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import FamDevice, Profile
from artsrun.model.selection import Selection
from artsrun.paths import cxl_script
from artsrun.run.command import build_command, build_env, cxl_wrap, flush_log_path
from artsrun.run.flux import _launch as flux_launch
from artsrun.run.flux import job_script as flux_job_script
from artsrun.run.manifest import Manifest, describe_command, write_manifest
from artsrun.run.slurm import _launch as slurm_launch
from artsrun.run.slurm import job_script as slurm_job_script
from test_slurm import _cell

FAM = ["arts_excl_purge_fam_staged", "arts_excl_purge_fam_direct"]
INCLUDE = "/opt/device/include"
LIBRARY = "/opt/device/lib/libdevice.so"


def profile(launcher="local", **extra):
    data = {"name": "t", "launcher": launcher, "nodes": [1, 2],
            "workers": 15, "progress": 1}
    if launcher != "local":
        data["ports"] = [25000]
    if launcher == "slurm":
        data["slurm"] = {}
    if launcher == "flux":
        data["flux"] = {}
    if launcher == "ssh":
        data["ssh"] = {"budget": 2, "hosts": ["n01", "n02"]}
    return Profile.model_validate(data | extra)


def real(launcher="slurm", **extra):
    return profile(launcher, fam_device="real", fam_device_include_dir=INCLUDE,
                   fam_device_library=LIBRARY, **extra)


def cache(tree, **values):
    tree.mkdir(parents=True, exist_ok=True)
    (tree / "CMakeCache.txt").write_text(
        "".join(f"{k}:STRING={v}\n" for k, v in values.items()))


def fam_cell(nodes, device=FamDevice.REAL, key="arts_excl_purge_fam_staged"):
    return replace(_cell(RuntimeKind.ARTS, nodes),
                   entry=load_plane().entry(key), fam_device=device)


# -- the profile ---------------------------------------------------------------

def _check(prof, entries=FAM):
    """A campaign of these entries against this profile."""
    from artsrun.model.catalog import load_catalog

    Selection(profile="t", experiment="b", entries=entries,
              apps={"nqueens": ["base"]}, node_counts=[1]).validate_against(
        load_plane(), load_catalog(), prof)


def test_local_implies_fake_and_refuses_real():
    assert profile().fam_device is None
    assert profile().resolved_fam_device is FamDevice.FAKE
    assert profile(fam_device="fake").resolved_fam_device is FamDevice.FAKE
    with pytest.raises(ValueError, match="not allowed under launcher=local") as exc:
        profile(fam_device="real", fam_device_include_dir=INCLUDE,
                fam_device_library=LIBRARY)
    assert "local takes off or fake" in str(exc.value)


def test_a_local_profile_may_say_off():
    assert profile(fam_device="off").resolved_fam_device is FamDevice.OFF
    _check(profile(), ["arts_excl_purge"])
    _check(profile(fam_device="off"), ["arts_excl_purge", "xsocr"])


@pytest.mark.parametrize("launcher", ["local", "slurm", "flux", "ssh"])
def test_off_refuses_a_fam_entry_by_name(launcher):
    prof = profile(launcher, fam_device="off")
    with pytest.raises(ValueError, match="profile 't' has fam_device: off") as exc:
        _check(prof)
    assert "arts_excl_purge_fam_staged" in str(exc.value)


@pytest.mark.parametrize("launcher", ["slurm", "flux", "ssh"])
def test_every_other_launcher_is_off_unless_it_says_otherwise(launcher):
    assert profile(launcher).resolved_fam_device is FamDevice.OFF
    with pytest.raises(ValueError, match="fam_device: off"):
        _check(profile(launcher))
    # a campaign without a FAM entry never asks
    _check(profile(launcher), ["arts_excl_purge"])
    assert profile(launcher, fam_device="fake", nodes=[1]).resolved_fam_device \
        is FamDevice.FAKE
    assert real(launcher).resolved_fam_device is FamDevice.REAL


def test_real_requires_both_absolute_paths_and_fake_forbids_them():
    with pytest.raises(ValueError, match="requires fam_device_library"):
        profile("slurm", fam_device="real", fam_device_include_dir=INCLUDE)
    with pytest.raises(ValueError, match="fam_device_include_dir and fam_device_library"):
        profile("slurm", fam_device="real")
    with pytest.raises(ValueError, match="must be absolute"):
        profile("slurm", fam_device="real", fam_device_include_dir="inc",
                fam_device_library=LIBRARY)
    for launcher in ("local", "slurm"):
        with pytest.raises(ValueError, match="belong to fam_device: real"):
            profile(launcher, fam_device="fake", fam_device_library=LIBRARY,
                    nodes=[1])
    with pytest.raises(ValueError, match="belong to fam_device: real"):
        profile("slurm", fam_device_library=LIBRARY)


@pytest.mark.parametrize("launcher", ["slurm", "flux", "ssh"])
def test_fake_off_the_local_launcher_runs_on_one_host_only(launcher):
    # The vendored library's pool is one host's shared memory.
    with pytest.raises(ValueError, match=r"requires\s+nodes: \[1\].*one\s+host's shared memory"):
        profile(launcher, fam_device="fake")
    assert profile(launcher, fam_device="fake", nodes=[1]).nodes == [1]
    # local ranks share one host at any count; real reaches every node
    assert profile(fam_device="fake").nodes == [1, 2]
    assert real(launcher).nodes == [1, 2]


def test_fam_strict_is_on_by_default_under_fake_and_may_be_turned_off():
    assert profile().fam_strict is None
    assert profile().resolved_fam_strict is True
    assert profile(fam_strict=True).resolved_fam_strict is True
    assert profile(fam_strict=False).resolved_fam_strict is False
    for launcher in ("slurm", "flux", "ssh"):
        fake = profile(launcher, fam_device="fake", nodes=[1])
        assert fake.resolved_fam_strict is True
        assert profile(launcher, fam_device="fake", nodes=[1],
                       fam_strict=False).resolved_fam_strict is False


@pytest.mark.parametrize("value", [True, False])
def test_fam_strict_is_refused_beside_real_and_off(value):
    with pytest.raises(ValueError, match="fam_strict is a setting of "
                                         "fam_device: fake only") as exc:
        real(fam_strict=value)
    assert "fam_device is real" in str(exc.value)
    # off, stated or implied by a launcher other than local
    with pytest.raises(ValueError, match="fam_device is off"):
        profile(fam_device="off", fam_strict=value)
    with pytest.raises(ValueError, match="fam_device is off"):
        profile("slurm", fam_strict=value)
    assert real().resolved_fam_strict is None
    assert profile(fam_device="off").resolved_fam_strict is None


def test_the_rendered_cfg_carries_fam_strict_only_under_fake():
    from artsrun.render import render_arts

    assert "fam_strict=1" in render_arts(profile(), 2, fam=True)
    assert "fam_strict=0" in render_arts(profile(fam_strict=False), 2, fam=True)
    assert "fam_strict=1" in render_arts(profile(fam_strict=True), 2, fam=True)
    assert "fam_strict" not in render_arts(real(), 2, fam=True)
    assert "fam_strict" not in render_arts(profile(fam_device="off"), 2, fam=True)
    # a campaign that runs no FAM entry leaves the key unstated
    assert "fam_strict" not in render_arts(profile(), 2)


def test_shipped_profiles_name_their_library():
    from artsrun import store

    for name in store.list_profiles():
        prof = store.load_profile(name)
        assert prof.resolved_fam_device in FamDevice, name


def test_crete_validates_with_its_placeholder_paths():
    from artsrun import store

    crete = store.load_profile("crete")
    assert crete.fam_device is FamDevice.REAL
    assert crete.fam_device_include_dir == "/PATH/TO/device-library/inc"
    assert crete.fam_device_library == \
        "/PATH/TO/device-library/lib/libarts_cxl_lib.so"
    for name in ("dane", "dane-p1", "dane-p8", "junction", "tuolumne"):
        prof = store.load_profile(name)
        assert prof.fam_device is None, name
        assert prof.resolved_fam_device is FamDevice.OFF, name
    for name in ("ferrari-dane", "ferrari-local"):
        assert store.load_profile(name).resolved_fam_device is FamDevice.FAKE
    assert store.load_profile("local-fam").fam_device is FamDevice.FAKE


def test_a_campaign_entry_list_is_checked_against_the_profile():
    slurm = profile("slurm")
    with pytest.raises(ValueError, match="profile 't' has fam_device: off"):
        _check(slurm)
    with pytest.raises(ValueError, match="fam_device: off"):
        _check(profile(fam_device="off"))
    _check(profile())
    _check(real())


def test_selections_saved_in_the_old_campaign_mode_are_refused():
    base = {"profile": "t", "experiment": "b", "entries": FAM,
            "apps": {"nqueens": ["base"]}, "node_counts": [1]}
    with pytest.raises(ValueError, match="profile setting now"):
        Selection.model_validate(base | {"cxl": True})
    with pytest.raises(ValueError, match="fam_device"):
        Selection.model_validate(base | {"cxl": False, "fam_device_library": LIBRARY})
    assert Selection.model_validate(base | {"cxl": False}).entries == FAM


# -- the tree follows the profile ----------------------------------------------

def test_the_decision_reads_the_tree_against_the_profile(tmp_path):
    fake = profile()
    cache(tmp_path, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="ON")
    assert fam_device_mismatch(tmp_path, fake) == []
    assert fam_device_mismatch(tmp_path, real()) == [
        "ARTS_FAM_DEVICE_VENDORED: have ON, want OFF",
        f"ARTS_FAM_DEVICE_INCLUDE_DIR: have (unset), want {INCLUDE}",
        f"ARTS_FAM_DEVICE_LIBRARY: have (unset), want {LIBRARY}",
    ]
    cache(tmp_path, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="OFF",
          ARTS_FAM_DEVICE_INCLUDE_DIR=INCLUDE + "/", ARTS_FAM_DEVICE_LIBRARY=LIBRARY)
    assert fam_device_mismatch(tmp_path, real()) == []
    assert fam_device_mismatch(tmp_path, fake) == [
        "ARTS_FAM_DEVICE_VENDORED: have OFF, want ON"]
    cache(tmp_path, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="OFF",
          ARTS_FAM_DEVICE_INCLUDE_DIR=INCLUDE, ARTS_FAM_DEVICE_LIBRARY="/other.so")
    assert fam_device_mismatch(tmp_path, real()) == [
        f"ARTS_FAM_DEVICE_LIBRARY: have /other.so, want {LIBRARY}"]
    for backend in ("SHM", "OFF"):
        cache(tmp_path, ARTS_FAM_BACKEND=backend)
        assert fam_device_mismatch(tmp_path, fake)[0] == \
            f"ARTS_FAM_BACKEND: have {backend}, want DEVICE"
    cache(tmp_path)                           # a tree that predates the option
    assert "have OFF" in fam_device_mismatch(tmp_path, fake)[0]


def test_the_options_are_exactly_the_profile_library():
    assert fam_device_options(profile()) == [
        "-DARTS_FAM_BACKEND=DEVICE", "-DARTS_FAM_DEVICE_VENDORED=ON",
        "-DARTS_FAM_DEVICE_INCLUDE_DIR=", "-DARTS_FAM_DEVICE_LIBRARY="]
    assert fam_device_options(real()) == [
        "-DARTS_FAM_BACKEND=DEVICE", "-DARTS_FAM_DEVICE_VENDORED=OFF",
        f"-DARTS_FAM_DEVICE_INCLUDE_DIR={INCLUDE}",
        f"-DARTS_FAM_DEVICE_LIBRARY={LIBRARY}"]


def _fake_cmake(monkeypatch, tree, *, rc=0, then=None, output=""):
    calls = []

    def run(argv, **_kwargs):
        calls.append(argv)
        if rc == 0 and then:
            cache(tree, **then)
        return subprocess.CompletedProcess(argv, rc, stdout=output, stderr="")

    monkeypatch.setattr("artsrun.build.subprocess.run", run)
    monkeypatch.setattr("artsrun.build.shutil.which", lambda _n: "/usr/bin/cmake")
    return calls


def test_a_mismatched_tree_is_reconfigured_and_says_how(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_FAM_BACKEND="OFF")
    calls = _fake_cmake(monkeypatch, tmp_path, then={
        "ARTS_FAM_BACKEND": "DEVICE", "ARTS_FAM_DEVICE_VENDORED": "OFF",
        "ARTS_FAM_DEVICE_INCLUDE_DIR": INCLUDE, "ARTS_FAM_DEVICE_LIBRARY": LIBRARY})
    said = []
    configure_fam_device(tmp_path, real(), on_line=said.append, prefix=["srun"])
    assert calls[0][:2] == ["srun", "cmake"]
    assert calls[0][-4:] == fam_device_options(real())
    assert any("$ srun cmake -S" in line for line in said)


def test_a_failed_configure_ends_the_campaign_with_cmakes_text(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_FAM_BACKEND="OFF")
    _fake_cmake(monkeypatch, tmp_path, rc=1, output=(
        "-- noise\nCMake Error at CMakeLists.txt:668 (message):\n"
        "  ARTS_FAM_BACKEND=DEVICE needs the device library itself\n"))
    with pytest.raises(BuildError) as exc:
        configure_fam_device(tmp_path, real())
    assert "needs the device library itself" in str(exc.value)
    assert "noise" not in str(exc.value)


def test_a_configure_that_leaves_the_cache_different_is_an_error(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_FAM_BACKEND="OFF")
    _fake_cmake(monkeypatch, tmp_path, then={"ARTS_FAM_BACKEND": "SHM"})
    with pytest.raises(BuildError, match="still differs"):
        configure_fam_device(tmp_path, profile())


def _campaign(tmp_path, prof):
    from artsrun.campaign import Campaign
    from artsrun.model.experiment import Experiment
    from artsrun.model.catalog import load_catalog

    sel = Selection(profile="t", experiment="b", entries=["arts_excl_purge", *FAM],
                    apps={"nqueens": ["base"]}, node_counts=[1])
    return Campaign(selection=sel, plane=load_plane(), catalog=load_catalog(),
                    experiment=Experiment(entries=["arts_excl_purge"], name="b", apps={}), profile=prof,
                    build_dir=tmp_path, run_dir=tmp_path / "run")


def test_a_dry_run_shows_the_reconfigure_and_changes_nothing(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_FAM_BACKEND="OFF")
    monkeypatch.setattr("artsrun.build.subprocess.run",
                        lambda *a, **k: pytest.fail("a dry run ran cmake"))
    said = []
    _campaign(tmp_path, profile())._match_fam_device(said.append, [], dry=True)
    assert "a real run reconfigures it first" in said[0]
    assert "-DARTS_FAM_DEVICE_VENDORED=ON" in said[0]
    cache(tmp_path, ARTS_FAM_BACKEND="DEVICE", ARTS_FAM_DEVICE_VENDORED="ON")
    said.clear()
    _campaign(tmp_path, profile())._match_fam_device(said.append, [], dry=True)
    assert "matches the profile" in said[0]


def test_a_real_run_reconfigures(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_FAM_BACKEND="SHM")
    calls = _fake_cmake(monkeypatch, tmp_path, then={
        "ARTS_FAM_BACKEND": "DEVICE", "ARTS_FAM_DEVICE_VENDORED": "ON"})
    _campaign(tmp_path, profile())._match_fam_device(None, [], dry=False)
    assert calls and calls[0][-4:] == fam_device_options(profile())


def test_a_new_tree_is_configured_on_the_profile_library(tmp_path, monkeypatch):
    tree = tmp_path / "build"
    commands = []

    class Configure:
        def __init__(self, argv, **_kwargs):
            commands.append(argv)
            (tree / "build.ninja").parent.mkdir(parents=True, exist_ok=True)
            (tree / "build.ninja").touch()
            self.stdout = iter(())

        def wait(self):
            return 0

    monkeypatch.setattr("artsrun.build.subprocess.Popen", Configure)
    ensure_build_dir(tree, bootstrap=True, fam_options=fam_device_options(real()))
    cmd = commands[0]
    for opt in fam_device_options(real()):
        assert opt in cmd
    assert not any("COHERENCE_PROTOCOL" in a for a in cmd)


def test_a_dry_run_on_a_missing_tree_names_the_configure(tmp_path):
    with pytest.raises(BuildError) as exc:
        ensure_build_dir(tmp_path / "none", fam_options=fam_device_options(profile()))
    assert "-DARTS_FAM_BACKEND=DEVICE -DARTS_FAM_DEVICE_VENDORED=ON" in str(exc.value)


# -- the cells -----------------------------------------------------------------

def test_expansion_stamps_the_profile_library_on_fam_cells_only(tmp_path):
    from artsrun.model.experiment import Experiment, ExperimentApp
    from artsrun.model.catalog import Version, load_catalog
    from artsrun.run.plan import expand

    sel = Selection(profile="t", experiment="t", entries=["arts_excl_purge", *FAM],
                    apps={"nqueens": [Version.BASE]}, node_counts=[1])
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={"nqueens": ExperimentApp()})
    for prof, want in ((profile(), FamDevice.FAKE), (real(), FamDevice.REAL)):
        cells, skipped = expand(sel, load_plane(), load_catalog(), bs, prof,
                                tmp_path / "apps",
                                {1: {"arts": tmp_path / "arts_1.cfg"}})
        assert not skipped
        assert {c.entry.key: c.fam_device for c in cells} == {
            "arts_excl_purge": None, FAM[0]: want, FAM[1]: want}


def test_only_a_real_fam_cell_runs_inside_the_region_setup():
    local = profile()
    script = str(cxl_script())
    fam = fam_cell(2)
    assert fam.region_setup
    assert cxl_wrap(build_command(fam, local), fam) == [
        "bash", script, "/opt/bin/app", "12", "4"]
    for other in (fam_cell(2, FamDevice.FAKE), _cell(RuntimeKind.ARTS, 2)):
        assert not other.region_setup
        assert cxl_wrap(build_command(other, local), other) == \
            build_command(other, local)
    assert slurm_launch(fam, real("slurm")).startswith(
        f"bash {script} srun --ntasks-per-node=1 -N 2")
    assert flux_launch(fam, real("flux")).startswith(f"bash {script} flux run -N 2")
    assert not slurm_launch(fam_cell(2, FamDevice.FAKE),
                            profile("slurm", fam_device="fake", nodes=[1])
                            ).startswith("bash")


def test_ssh_manifest_shows_the_wrapped_command():
    fam = fam_cell(2, key="arts_excl_purge_fam_direct")
    recorded = describe_command(fam, real("ssh"), cxl_script())
    assert recorded == {
        "command": f"timeout -k 1 60 bash {cxl_script()} /opt/bin/app 12 4",
        "script": None,
    }


def test_the_manifest_records_each_cell_library(tmp_path):
    cells = [fam_cell(1, FamDevice.FAKE), replace(_cell(RuntimeKind.ARTS, 1), repeat=2)]
    write_manifest(tmp_path, cells, [], profile(), tmp_path / "build")
    back = Manifest.load(tmp_path)
    assert [c.fam_device for c in back.cells] == [FamDevice.FAKE, None]


@pytest.mark.parametrize("device", [FamDevice.FAKE, FamDevice.REAL])
def test_one_rank_flush_trace_lands_beside_the_cell_log(tmp_path, device):
    fam = fam_cell(1, device)
    env = build_env(fam, profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == str(flush_log_path(tmp_path, fam))
    assert "ARTS_FLUSH_LOG" not in build_env(_cell(RuntimeKind.ARTS, 1),
                                             profile(), tmp_path)
    marker = tmp_path / f"{fam.slug}.rc"
    trace = f"export ARTS_FLUSH_LOG={flush_log_path(tmp_path, fam)}"
    assert trace in slurm_job_script(fam, real("slurm"), marker)
    assert trace in flux_job_script(fam, real("flux"), marker)


def test_multi_rank_flush_trace_is_discarded(tmp_path):
    fam = fam_cell(2)
    env = build_env(fam, profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == "/dev/null"
    marker = tmp_path / f"{fam.slug}.rc"
    assert "export ARTS_FLUSH_LOG=/dev/null" in slurm_job_script(
        fam, real("slurm"), marker)
    assert not flush_log_path(tmp_path, fam).exists()


# -- the command line ----------------------------------------------------------

@pytest.mark.parametrize("args", [
    ["--cxl"], ["--fam-device-include-dir", "/x"], ["--fam-device-library", "/x"],
    ["--cxl-rapid-include-dir", "/x"], ["--cxl-lib-dir", "/x"],
])
def test_the_deleted_options_name_the_profile_fields(args):
    from typer.testing import CliRunner

    from artsrun.cli import app

    result = CliRunner().invoke(app, ["run", "-p", "local-fam", *args, "--dry-run"])
    assert result.exit_code != 0
    for field in ("fam_device", "fam_device_include_dir", "fam_device_library"):
        assert field in result.output


def test_the_run_screen_has_no_device_switch(tmp_path):
    import asyncio

    from textual.widgets import Input, TabbedContent

    from artsrun.tui.app import ArtsRunApp

    async def check():
        app = ArtsRunApp()
        async with app.run_test() as pilot:
            app.query_one(TabbedContent).active = "tab-run"
            await pilot.pause()
            assert not app.query("#run-fam-device")
            assert not app.query("#run-fam-include")
            app.query_one("#run-build-dir", Input).value = str(tmp_path / "build")
            chosen = app.build_selection()
            assert chosen is not None
            assert chosen.build_dir == str(tmp_path / "build")

    asyncio.run(check())


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
