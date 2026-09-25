"""The CXL entries: which library a cell links, how the tree is made to match,
and how a real cell's launch is wrapped.  Three profile paths, all or none."""

import subprocess
from dataclasses import replace
from pathlib import Path

import pytest

from artsrun.build import (BuildError, arena_bytes, configure_cxl, cxl_mismatch,
                           cxl_options, ensure_build_dir, fake_region_bytes)
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import CxlLibrary, Profile
from artsrun.model.selection import Selection
from artsrun.run.command import build_command, build_env, cxl_wrap, flush_log_path
from artsrun.run.flux import _launch as flux_launch
from artsrun.run.flux import job_script as flux_job_script
from artsrun.run.manifest import Manifest, describe_command, write_manifest
from artsrun.run.slurm import _launch as slurm_launch
from artsrun.run.slurm import job_script as slurm_job_script
from artsrun.run.types import Cell
from test_slurm import _cell

CXL = ["arts_excl_purge_cxl_staged", "arts_excl_purge_cxl_direct"]
WRAPPER = "/site/run.py"


def profile(**kw) -> Profile:
    base = dict(name="p", launcher="local", nodes=[1], workers=2, progress=1)
    base.update(kw)
    launcher = base["launcher"]
    if launcher != "local":
        base.setdefault("ports", [25000])
    if launcher == "slurm":
        base.setdefault("slurm", {})
    if launcher == "flux":
        base.setdefault("flux", {})
    if launcher == "ssh":
        base.setdefault("ssh", {"budget": 2, "hosts": ["n01", "n02"]})
    return Profile(**base)


REAL = dict(cxl_include_dir="/dev/inc", cxl_library="/dev/libarts_cxl_lib.so",
            cxl_launch_wrapper=WRAPPER)


def cxl_cell(nodes=2, cxl=CxlLibrary.REAL, key="arts_excl_purge_cxl_staged",
             region=None) -> Cell:
    if region is None and cxl is CxlLibrary.FAKE:
        region = 2 * 5_000_000_000 + (128 << 20)
    return replace(_cell(RuntimeKind.ARTS, nodes),
                   entry=load_plane().entry(key), cxl=cxl,
                   cxl_region_bytes=region)


def cache(tree, **values):
    tree.mkdir(parents=True, exist_ok=True)
    (tree / "CMakeCache.txt").write_text(
        "".join(f"{k}:STRING={v}\n" for k, v in values.items()))


REAL_CACHE = dict(ARTS_CXL_REAL="ON", ARTS_CXL_RAPID_INCLUDE_DIR="/dev/inc",
                  ARTS_CXL_LIB="/dev/libarts_cxl_lib.so")


# -- the profile ---------------------------------------------------------------

def test_no_paths_is_fake_and_all_three_is_real():
    assert profile().cxl_library_kind is CxlLibrary.FAKE
    assert profile(**REAL).cxl_library_kind is CxlLibrary.REAL


@pytest.mark.parametrize("drop", ["cxl_include_dir", "cxl_library", "cxl_launch_wrapper"])
def test_real_needs_all_three_paths(drop):
    two = {k: v for k, v in REAL.items() if k != drop}
    with pytest.raises(ValueError, match=drop):
        profile(**two)


def test_paths_must_be_absolute():
    with pytest.raises(ValueError, match="absolute"):
        profile(**{**REAL, "cxl_library": "rel/lib.so"})


def test_fake_off_the_local_launcher_runs_on_one_host_only():
    multi = profile(launcher="slurm", nodes=[1, 2], ports=[25000])   # loads: it is Dane's shape
    with pytest.raises(ValueError, match="nodes"):
        multi.check_cxl(["arts_excl_purge_cxl_staged"])
    multi.check_cxl([])                                              # no CXL entry: fine
    profile(launcher="slurm", nodes=[1], ports=[25000]).check_cxl(["arts_excl_purge_cxl_staged"])
    profile(launcher="slurm", nodes=[1, 4], ports=[25000], **REAL).check_cxl(["arts_excl_purge_cxl_staged"])
    # local ranks share one host at any count
    profile(nodes=[1, 2]).check_cxl(["arts_excl_purge_cxl_staged"])


def test_real_under_local_is_allowed():
    assert profile(**REAL).cxl_library_kind is CxlLibrary.REAL


def test_a_key_no_field_takes_is_refused():
    with pytest.raises(ValueError, match="cxl_device"):
        profile(cxl_device="real")


def test_shipped_profiles_name_their_library():
    from artsrun import store

    crete = store.load_profile("crete")
    assert crete.cxl_library_kind is CxlLibrary.REAL
    assert crete.cxl_include_dir == "/PATH/TO/device-sdk/include"
    assert crete.cxl_library == "/PATH/TO/glue/libarts_cxl_lib.so"
    assert crete.cxl_launch_wrapper == "/PATH/TO/site/run.py"
    for name in store.list_profiles():
        if name != "crete":
            assert store.load_profile(name).cxl_library_kind is CxlLibrary.FAKE, name


def _check(prof, entries=CXL):
    """A campaign of these entries against this profile."""
    from artsrun.model.catalog import load_catalog

    Selection(profile="t", experiment="b", entries=entries,
              apps={"nqueens": ["base"]}, node_counts=[1]).validate_against(
        load_plane(), load_catalog(), prof)


def test_a_campaign_entry_list_is_checked_against_the_profile():
    slurm = profile(launcher="slurm", nodes=[1, 2])
    with pytest.raises(ValueError, match="vendored fake"):
        _check(slurm)
    _check(slurm, ["arts_excl_purge"])
    _check(profile(nodes=[1, 2]))
    _check(profile(launcher="slurm", nodes=[1, 2], **REAL))


def test_selections_saved_in_the_old_campaign_mode_are_refused():
    base = {"profile": "t", "experiment": "b", "entries": CXL,
            "apps": {"nqueens": ["base"]}, "node_counts": [1]}
    for old in ({"cxl": True}, {"cxl_rapid_include_dir": "/x"},
                {"cxl_lib_dir": "/x"}):
        with pytest.raises(ValueError, match="the profile names it now"):
            Selection.model_validate(base | old)
    with pytest.raises(ValueError, match="cxl_device"):
        Selection.model_validate(base | {"cxl_device": "/x"})
    assert Selection.model_validate(base | {"cxl": False}).entries == CXL


# -- the tree follows the profile ----------------------------------------------

def test_the_options_are_exactly_the_profile_library():
    assert cxl_options(profile()) == [
        "-DARTS_CXL_REAL=OFF", "-DARTS_CXL_RAPID_INCLUDE_DIR=", "-DARTS_CXL_LIB="]
    assert cxl_options(profile(**REAL)) == [
        "-DARTS_CXL_REAL=ON", "-DARTS_CXL_RAPID_INCLUDE_DIR=/dev/inc",
        "-DARTS_CXL_LIB=/dev/libarts_cxl_lib.so"]


def test_the_decision_reads_the_tree_against_the_profile(tmp_path):
    (tmp_path / "CMakeCache.txt").write_text(
        "ARTS_CXL_REAL:BOOL=ON\nARTS_CXL_RAPID_INCLUDE_DIR:PATH=/old/inc\n"
        "ARTS_CXL_LIB:FILEPATH=/dev/libarts_cxl_lib.so\n")
    assert cxl_mismatch(tmp_path, profile(**REAL)) == [
        "ARTS_CXL_RAPID_INCLUDE_DIR: have /old/inc, want /dev/inc"]
    assert cxl_mismatch(tmp_path, profile()) == [
        "ARTS_CXL_REAL: have ON, want OFF",
        "ARTS_CXL_RAPID_INCLUDE_DIR: have /old/inc, want (unset)",
        "ARTS_CXL_LIB: have /dev/libarts_cxl_lib.so, want (unset)"]
    (tmp_path / "CMakeCache.txt").write_text("ARTS_CXL_REAL:BOOL=OFF\n")
    assert cxl_mismatch(tmp_path, profile()) == []
    (tmp_path / "CMakeCache.txt").write_text("")    # a tree that predates the option
    assert cxl_mismatch(tmp_path, profile()) == []
    assert cxl_mismatch(tmp_path, profile(**REAL))[0] == \
        "ARTS_CXL_REAL: have OFF, want ON"


def test_a_fake_cell_states_the_region_size_from_the_trees_arena(tmp_path):
    (tmp_path / "CMakeCache.txt").write_text("ARTS_CXL_DB_ARENA_SIZE_BYTES:STRING=268435456\n")
    assert arena_bytes(tmp_path) == 268435456
    assert fake_region_bytes(tmp_path) == 2 * 268435456 + (128 << 20)
    (tmp_path / "CMakeCache.txt").write_text("")
    assert arena_bytes(tmp_path) == 5000000000


def _fake_cmake(monkeypatch, tree, *, rc=0, then=None, output=""):
    calls = []

    def run(argv, **_kwargs):
        calls.append(argv)
        if rc == 0 and then is not None:
            cache(tree, **then)
        return subprocess.CompletedProcess(argv, rc, stdout=output, stderr="")

    monkeypatch.setattr("artsrun.build.subprocess.run", run)
    monkeypatch.setattr("artsrun.build.shutil.which", lambda _n: "/usr/bin/cmake")
    return calls


def test_a_mismatched_tree_is_reconfigured_and_says_how(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_CXL_REAL="OFF")
    calls = _fake_cmake(monkeypatch, tmp_path, then=REAL_CACHE)
    said = []
    configure_cxl(tmp_path, profile(**REAL), on_line=said.append, prefix=["srun"])
    assert calls[0][:2] == ["srun", "cmake"]
    assert calls[0][-3:] == cxl_options(profile(**REAL))
    assert any("cxl: real — reconfiguring" in line for line in said)
    assert any("$ srun cmake -S" in line for line in said)


def test_a_failed_configure_ends_the_campaign_with_cmakes_text(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_CXL_REAL="OFF")
    _fake_cmake(monkeypatch, tmp_path, rc=1, output=(
        "-- noise\nCMake Error at CMakeLists.txt:668 (message):\n"
        "  ARTS_CXL_REAL=ON needs ARTS_CXL_LIB to name an existing file\n"))
    with pytest.raises(BuildError) as exc:
        configure_cxl(tmp_path, profile(**REAL))
    assert "for the CXL library real" in str(exc.value)
    assert "needs ARTS_CXL_LIB to name an existing file" in str(exc.value)
    assert "noise" not in str(exc.value)


def test_a_configure_that_leaves_the_cache_different_is_an_error(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_CXL_REAL="OFF")
    _fake_cmake(monkeypatch, tmp_path, then={"ARTS_CXL_REAL": "ON"})
    with pytest.raises(BuildError, match="still differs"):
        configure_cxl(tmp_path, profile())


def _campaign(tmp_path, prof):
    from artsrun.campaign import Campaign
    from artsrun.model.catalog import load_catalog
    from artsrun.model.experiment import Experiment

    sel = Selection(profile="t", experiment="b", entries=["arts_excl_purge", *CXL],
                    apps={"nqueens": ["base"]}, node_counts=[1])
    return Campaign(selection=sel, plane=load_plane(), catalog=load_catalog(),
                    experiment=Experiment(entries=["arts_excl_purge"], name="b", apps={}),
                    profile=prof, build_dir=tmp_path, run_dir=tmp_path / "run")


def test_a_dry_run_shows_the_reconfigure_and_changes_nothing(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_CXL_REAL="ON")
    monkeypatch.setattr("artsrun.build.subprocess.run",
                        lambda *a, **k: pytest.fail("a dry run ran cmake"))
    said = []
    _campaign(tmp_path, profile())._match_cxl(said.append, [], dry=True)
    assert "a real run reconfigures it first" in said[0]
    assert "-DARTS_CXL_REAL=OFF" in said[0]
    cache(tmp_path, ARTS_CXL_REAL="OFF")
    said.clear()
    _campaign(tmp_path, profile())._match_cxl(said.append, [], dry=True)
    assert said[0] == (f"{tmp_path}: ARTS_CXL_REAL=OFF — the tree links the "
                       "profile's CXL library (fake)")


def test_a_real_run_reconfigures(tmp_path, monkeypatch):
    cache(tmp_path, ARTS_CXL_REAL="ON")
    calls = _fake_cmake(monkeypatch, tmp_path, then={"ARTS_CXL_REAL": "OFF"})
    _campaign(tmp_path, profile())._match_cxl(None, [], dry=False)
    assert calls and calls[0][-3:] == cxl_options(profile())


def test_a_real_run_refuses_a_missing_wrapper(tmp_path, monkeypatch):
    # The wrapper must exist before any cell starts; a dry run renders the
    # placeholder and says nothing.
    wrapper = tmp_path / "absent.py"
    prof = profile(launcher="slurm", nodes=[1, 2],
                   **{**REAL, "cxl_launch_wrapper": str(wrapper)})
    cache(tmp_path, **REAL_CACHE)
    monkeypatch.setattr("artsrun.build.subprocess.run",
                        lambda *a, **k: pytest.fail("a matching tree ran cmake"))
    with pytest.raises(BuildError, match="cxl_launch_wrapper does not exist"):
        _campaign(tmp_path, prof)._match_cxl(None, [], dry=False)
    _campaign(tmp_path, prof)._match_cxl(None, [], dry=True)
    wrapper.touch()
    _campaign(tmp_path, prof)._match_cxl(None, [], dry=False)


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
    ensure_build_dir(tree, bootstrap=True, cxl_options=cxl_options(profile(**REAL)))
    cmd = commands[0]
    for opt in cxl_options(profile(**REAL)):
        assert opt in cmd
    assert not any("COHERENCE_PROTOCOL" in a for a in cmd)
    assert not any("RESIDENCY" in a or "USE_CXL" in a for a in cmd)


def test_a_dry_run_on_a_missing_tree_names_the_configure(tmp_path):
    with pytest.raises(BuildError) as exc:
        ensure_build_dir(tmp_path / "none", cxl_options=cxl_options(profile()))
    assert "-DARTS_CXL_REAL=OFF" in str(exc.value)


# -- the cells -----------------------------------------------------------------

def test_expansion_stamps_the_profile_library_on_cxl_cells_only(tmp_path):
    from artsrun.model.catalog import Version, load_catalog
    from artsrun.model.experiment import Experiment, ExperimentApp
    from artsrun.run.plan import expand

    tree = tmp_path / "build"
    cache(tree, ARTS_CXL_DB_ARENA_SIZE_BYTES="268435456")
    region = 2 * 268435456 + (128 << 20)
    sel = Selection(profile="t", experiment="t", entries=["arts_excl_purge", *CXL],
                    apps={"nqueens": [Version.BASE]}, node_counts=[1])
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={"nqueens": ExperimentApp()})
    for prof, want, size in ((profile(), CxlLibrary.FAKE, region),
                             (profile(**REAL), CxlLibrary.REAL, None)):
        cells, skipped = expand(sel, load_plane(), load_catalog(), bs, prof,
                                tmp_path / "apps",
                                {1: {"arts": tmp_path / "arts_1.cfg"}},
                                build_dir=tree)
        assert not skipped
        assert {c.entry.key: (c.cxl, c.cxl_region_bytes) for c in cells} == {
            "arts_excl_purge": (None, None), CXL[0]: (want, size),
            CXL[1]: (want, size)}


def test_only_a_real_cxl_cell_runs_inside_the_wrapper():
    real = cxl_cell(cxl=CxlLibrary.REAL)
    assert real.region_setup
    assert cxl_wrap(["bin", "x"], real, profile(**REAL)) == [
        "python3", "/site/run.py", "bin", "x"]
    assert cxl_wrap(["bin", "x"], cxl_cell(cxl=CxlLibrary.FAKE), profile()) == ["bin", "x"]
    assert cxl_wrap(["bin", "x"], cxl_cell(cxl=None), profile()) == ["bin", "x"]
    for other in (cxl_cell(cxl=CxlLibrary.FAKE), _cell(RuntimeKind.ARTS, 2)):
        assert not other.region_setup
    local = profile(nodes=[1, 2], **REAL)
    assert cxl_wrap(build_command(real, local), real, local) == [
        "python3", WRAPPER, "/opt/bin/app", "12", "4"]
    slurm = profile(launcher="slurm", nodes=[1, 2], **REAL)
    assert slurm_launch(real, slurm).startswith(
        f"python3 {WRAPPER} srun --ntasks-per-node=1 -N 2")
    flux = profile(launcher="flux", nodes=[1, 2], **REAL)
    assert flux_launch(real, flux).startswith(f"python3 {WRAPPER} flux run -N 2")
    assert not slurm_launch(cxl_cell(cxl=CxlLibrary.FAKE),
                            profile(launcher="slurm", nodes=[1])
                            ).startswith("python3")


def test_ssh_manifest_shows_the_wrapped_command(tmp_path):
    cell = cxl_cell(key="arts_excl_purge_cxl_direct")
    recorded = describe_command(cell, profile(launcher="ssh", nodes=[1, 2], **REAL),
                                tmp_path / "cell.log")
    assert recorded == {
        "command": f"timeout -k 1 60 python3 {WRAPPER} /opt/bin/app 12 4",
        "script": None,
    }


def test_the_manifest_records_each_cell_library(tmp_path):
    cells = [cxl_cell(1, CxlLibrary.FAKE, region=12345),
             replace(_cell(RuntimeKind.ARTS, 1), repeat=2)]
    write_manifest(tmp_path, cells, [], profile(), tmp_path / "build")
    back = Manifest.load(tmp_path)
    assert [(c.cxl, c.cxl_region_bytes) for c in back.cells] == [
        (CxlLibrary.FAKE, 12345), (None, None)]


@pytest.mark.parametrize("library", [CxlLibrary.FAKE, CxlLibrary.REAL])
def test_one_rank_flush_trace_lands_beside_the_cell_log(tmp_path, library):
    cell = cxl_cell(1, library)
    env = build_env(cell, profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == str(flush_log_path(tmp_path, cell))
    assert "ARTS_FLUSH_LOG" not in build_env(_cell(RuntimeKind.ARTS, 1),
                                             profile(), tmp_path)
    marker = tmp_path / f"{cell.slug}.rc"
    trace = f"export ARTS_FLUSH_LOG={flush_log_path(tmp_path, cell)}"
    assert trace in slurm_job_script(cell, profile(launcher="slurm", **REAL), marker)
    assert trace in flux_job_script(cell, profile(launcher="flux", **REAL), marker)


def test_a_fake_cell_hands_its_region_size_to_the_library(tmp_path):
    # The library sizes its region when it loads, before any cfg is read.
    cell = cxl_cell(1, CxlLibrary.FAKE, region=777 << 20)
    assert build_env(cell, profile(), tmp_path)["ARTS_FAKE_CXL_REGION_SIZE"] == \
        str(777 << 20)
    assert "ARTS_FAKE_CXL_REGION_SIZE" not in build_env(
        cxl_cell(1, CxlLibrary.REAL), profile(**REAL), tmp_path)
    assert "ARTS_FAKE_CXL_REGION_SIZE" not in build_env(
        _cell(RuntimeKind.ARTS, 1), profile(), tmp_path)
    marker = tmp_path / f"{cell.slug}.rc"
    assert f"export ARTS_FAKE_CXL_REGION_SIZE={777 << 20}" in \
        slurm_job_script(cell, profile(launcher="slurm"), marker)


def test_multi_rank_flush_trace_is_discarded(tmp_path):
    cell = cxl_cell(2)
    env = build_env(cell, profile(), tmp_path)
    assert env["ARTS_FLUSH_LOG"] == "/dev/null"
    marker = tmp_path / f"{cell.slug}.rc"
    assert "export ARTS_FLUSH_LOG=/dev/null" in slurm_job_script(
        cell, profile(launcher="slurm", nodes=[1, 2], **REAL), marker)
    assert not flush_log_path(tmp_path, cell).exists()


# -- the command line ----------------------------------------------------------

@pytest.mark.parametrize("args", [
    ["--cxl"], ["--cxl-rapid-include-dir", "/x"], ["--cxl-lib-dir", "/x"],
])
def test_the_deleted_options_name_the_profile_fields(args):
    from typer.testing import CliRunner

    from artsrun.cli import app

    result = CliRunner().invoke(app, ["run", "-p", "local-cxl", *args, "--dry-run"])
    assert result.exit_code != 0
    for field in ("cxl_include_dir", "cxl_library", "cxl_launch_wrapper"):
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
            assert not app.query("#run-cxl")
            assert not app.query("#run-cxl-include")
            app.query_one("#run-build-dir", Input).value = str(tmp_path / "build")
            chosen = app.build_selection()
            assert chosen is not None
            assert chosen.build_dir == str(tmp_path / "build")

    asyncio.run(check())
