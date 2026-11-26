"""The build plan: exactly what the campaign runs, read from the tree as it is.

Two things went wrong together once.  A source update registered new
programs and, on the same host, made the configure fail on a tool it could
not find; the failed configure left the tree's previous generation in place,
the plan read its target list from that generation, and the campaign
reported the new programs as "no target" with a hint about MPI compilers —
the real error never surfaced.  And a program the experiment does not enable
had its targets wanted anyway, without the row's own eligibility, so an
ARTS-only probe was asked for as an XSOCR binary.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from artsrun.build import (BuildError, available_targets, ensure_build_dir,
                           generator_stale, plan_targets)
from artsrun.model.experiment import Experiment, ExperimentApp
from artsrun.model.catalog import Section, Version, load_catalog
from artsrun.model.plane import load_plane
from artsrun.model.profile import Launcher, Profile, SlurmSettings
from artsrun.model.selection import Selection

# An ARTS-only probe, off by the catalog's default: a roster has to name it.
PROBE = "alternating_bomb_64k"
PROBE_BINARY = "alternating_bomb"


def _selection(entries: list[str], apps: dict) -> Selection:
    return Selection(profile="t", experiment="t", default_entries=[], entries=entries, apps=apps,
                     node_counts=[1], repeats=1)


def test_an_arts_only_row_wants_only_its_arts_targets(tmp_path):
    plane, catalog = load_plane(), load_catalog()
    plan = plan_targets(
        _selection(["arts_val_retain_owner", "xsocr"], {PROBE: [Version.BASE]}),
        plane, catalog, Experiment(entries=["arts_excl_purge_home"], name="t", apps={PROBE: ExperimentApp()}), tmp_path,
    )
    assert plan.targets == [f"{PROBE_BINARY}_arts_ocr_val_retain_owner"]


def test_a_cxl_entry_always_wants_its_target(tmp_path):
    # Every benchmark tree builds the CXL variants, so no launcher or tree
    # state drops them from the build.
    plane, catalog = load_plane(), load_catalog()
    sel = _selection(["arts_excl_purge_home", "arts_excl_purge_home_cxl_staged"],
                     {"nqueens": [Version.BASE]})
    bs = Experiment(entries=["arts_excl_purge_home"], name="t", apps={"nqueens": ExperimentApp()})
    local = Profile(name="p", launcher=Launcher.LOCAL, nodes=[1],
                    workers=2, progress=1)
    remote = Profile(name="p", launcher=Launcher.SLURM, nodes=[1],
                     workers=2, progress=1, ports=25000,
                     slurm=SlurmSettings())
    for prof in (local, remote, None):
        plan = plan_targets(sel, plane, catalog, bs, tmp_path, prof)
        assert plan.targets == ["nqueens_arts_ocr_excl_purge_home",
                                "nqueens_arts_ocr_excl_purge_home_cxl_staged"]


def test_a_row_the_experiment_does_not_enable_wants_no_target(tmp_path):
    # Expansion records such a row as skipped ("not enabled in the experiment"),
    # so no cell runs it and nothing of it needs building — least of all the
    # reference binaries an ARTS-only program never has.
    plane, catalog = load_plane(), load_catalog()
    plan = plan_targets(
        _selection(["arts_val_retain_owner", "xsocr"], {PROBE: [Version.BASE]}),
        plane, catalog, Experiment(entries=["arts_excl_purge_home"], name="t", apps={"fib_hpx": ExperimentApp()}),
        tmp_path,
    )
    assert plan.targets == []
    assert plan.ok


# --- the tree as it is ------------------------------------------------------
needs_cmake = pytest.mark.skipif(
    shutil.which("cmake") is None or shutil.which("ninja") is None,
    reason="cmake and ninja on PATH",
)

_PROJECT = (
    "cmake_minimum_required(VERSION 3.16)\n"
    "project(t C)\n"
    'file(WRITE a.c "int main(void){return 0;}")\n'
    "add_executable(old_app a.c)\n"
)


_GLOB_PROJECT = _PROJECT + (
    'file(GLOB parts CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/parts/*.txt")\n'
)


def _configured(tmp_path: Path, project: str = _PROJECT) -> Path:
    """A tree generated from the project as first written."""
    src = tmp_path / "src"
    src.mkdir()
    (src / "CMakeLists.txt").write_text(project)
    (src / "parts").mkdir()
    (src / "parts" / "a.txt").write_text("")
    build = tmp_path / "build"
    subprocess.run(
        ["cmake", "-GNinja", "-S", str(src), "-B", str(build),
         "-DCMAKE_BUILD_TYPE=Release"],
        check=True, capture_output=True,
    )
    return build


def _sources_move_on(tmp_path: Path, extra: str, project: str = _PROJECT) -> None:
    """The project changes after the tree was generated.

    The generation is aged rather than the source pushed into the future: a
    future-dated input stays newer than every regeneration, which is a
    manifest ninja can never bring current, not a tree that moved on.
    """
    (tmp_path / "src" / "CMakeLists.txt").write_text(project + extra)
    earlier = time.time() - 60
    for aged in (tmp_path / "build" / "build.ninja",
                 tmp_path / "build" / "CMakeFiles" / "cmake.verify_globs"):
        if aged.exists():
            os.utime(aged, (earlier, earlier))


@needs_cmake
def test_a_current_tree_is_left_alone(tmp_path):
    build = _configured(tmp_path)
    lines: list[str] = []
    ensure_build_dir(build, sections=list(Section), bootstrap=True,
                     on_line=lines.append)
    assert lines == []
    assert "old_app" in available_targets(build)


@needs_cmake
def test_a_source_update_reaches_the_target_list_before_it_is_read(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n")
    ensure_build_dir(build, sections=list(Section), bootstrap=True)
    assert "new_app" in available_targets(build)


@needs_cmake
def test_a_configure_that_now_fails_is_reported_as_the_reason(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n"
                     "find_program(NEEDED NAMES no-such-tool-anywhere REQUIRED)\n")
    with pytest.raises(BuildError, match="no-such-tool-anywhere"):
        ensure_build_dir(build, sections=list(Section), bootstrap=True)


@needs_cmake
def test_a_dry_run_regenerates_nothing_and_says_the_sources_moved(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n")
    lines: list[str] = []
    ensure_build_dir(build, sections=list(Section), bootstrap=False,
                     on_line=lines.append)
    assert any("changed since it was generated" in line for line in lines)
    # Dry means dry: the previous generation is what the tree still holds.
    assert "new_app" not in available_targets(build)


def _a_real_run_regenerates(build: Path) -> bool:
    out = subprocess.run(["ninja", "-C", str(build), "build.ninja"],
                         capture_output=True, text=True, check=False).stdout
    return "Re-running CMake" in out


@needs_cmake
def test_a_current_tree_with_glob_checks_is_not_stale(tmp_path):
    build = _configured(tmp_path, _GLOB_PROJECT)
    lines: list[str] = []
    ensure_build_dir(build, sections=list(Section), bootstrap=False,
                     on_line=lines.append)
    assert lines == []
    assert generator_stale(build) is False
    assert _a_real_run_regenerates(build) is False


@needs_cmake
def test_a_new_file_under_a_glob_makes_the_tree_stale(tmp_path):
    build = _configured(tmp_path, _GLOB_PROJECT)
    (tmp_path / "src" / "parts" / "b.txt").write_text("b")
    assert generator_stale(build) is True
    assert _a_real_run_regenerates(build) is True


@needs_cmake
@pytest.mark.parametrize("project", [_PROJECT, _GLOB_PROJECT])
def test_the_answer_is_a_real_runs_when_a_cmake_file_changed(tmp_path, project):
    build = _configured(tmp_path, project)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n", project=project)
    assert generator_stale(build) is True
    assert _a_real_run_regenerates(build) is True


@needs_cmake
def test_a_current_tree_without_glob_checks_is_not_stale(tmp_path):
    build = _configured(tmp_path)
    assert generator_stale(build) is False
    assert _a_real_run_regenerates(build) is False


@needs_cmake
def test_the_glob_stamp_alone_does_not_make_a_tree_stale(tmp_path):
    build = _configured(tmp_path, _GLOB_PROJECT)
    stamp = build / "CMakeFiles" / "cmake.verify_globs"
    assert stamp.exists()
    earlier = time.time() - 60
    os.utime(stamp, (earlier, earlier))
    assert generator_stale(build) is False


@needs_cmake
def test_a_missing_cmake_input_makes_the_tree_stale(tmp_path):
    project = _PROJECT + "include(${CMAKE_SOURCE_DIR}/parts/a.txt)\n"
    build = _configured(tmp_path, project)
    (tmp_path / "src" / "parts" / "a.txt").unlink()
    assert generator_stale(build) is True
    assert _a_real_run_regenerates(build) is True


@needs_cmake
def test_a_glob_check_that_cannot_run_is_a_build_error(tmp_path):
    build = _configured(tmp_path, _GLOB_PROJECT)
    cache = build / "CMakeCache.txt"
    cache.write_text(cache.read_text().replace(
        "CMAKE_COMMAND:INTERNAL=", "CMAKE_COMMAND:INTERNAL=/no/such/dir/"))
    with pytest.raises(BuildError, match="glob check"):
        generator_stale(build)
