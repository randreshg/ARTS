"""The build plan: exactly what the campaign runs, read from the tree as it is.

Two things went wrong together once.  A source update registered new
programs and, on the same host, made the configure fail on a tool it could
not find; the failed configure left the tree's previous generation in place,
the plan read its target list from that generation, and the campaign
reported the new programs as "no target" with a hint about MPI compilers —
the real error never surfaced.  And a program the benchset does not enable
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

from artsrun.build import BuildError, available_targets, ensure_build_dir, plan_targets
from artsrun.model.benchset import Benchset, BenchsetEntry
from artsrun.model.catalog import Version, load_catalog
from artsrun.model.plane import load_plane
from artsrun.model.profile import Launcher, Profile, SlurmSettings
from artsrun.model.selection import Selection

# An ARTS-only probe, off by the catalog's default: a roster has to name it.
PROBE = "alternating_bomb_64k"
PROBE_BINARY = "alternating_bomb"


def _selection(entries: list[str], apps: dict) -> Selection:
    return Selection(profile="t", benchset="t", entries=entries, apps=apps,
                     node_counts=[1], repeats=1)


def test_an_arts_only_row_wants_only_its_arts_targets(tmp_path):
    plane, catalog = load_plane(), load_catalog()
    plan = plan_targets(
        _selection(["arts_val_wb", "xsocr"], {PROBE: [Version.BASE]}),
        plane, catalog, Benchset(name="t", apps={PROBE: BenchsetEntry()}), tmp_path,
    )
    assert plan.targets == [f"{PROBE_BINARY}_arts_ocr_val_wb"]


def test_a_fam_entry_always_wants_its_target(tmp_path):
    # The tree is made to carry the FAM variants before the plan is read,
    # so no launcher or tree state drops them from the build.
    plane, catalog = load_plane(), load_catalog()
    sel = _selection(["arts_excl_purge", "arts_excl_purge_fam_staged"],
                     {"nqueens": [Version.BASE]})
    bs = Benchset(name="t", apps={"nqueens": BenchsetEntry()})
    local = Profile(name="p", launcher=Launcher.LOCAL, nodes=[1],
                    workers=2, progress=1)
    remote = Profile(name="p", launcher=Launcher.SLURM, nodes=[1], fam_device="fake",
                     workers=2, progress=1, ports=[25000],
                     slurm=SlurmSettings())
    for prof in (local, remote, None):
        plan = plan_targets(sel, plane, catalog, bs, tmp_path, prof)
        assert plan.targets == ["nqueens_arts_ocr_excl_purge",
                                "nqueens_arts_ocr_excl_purge_fam_staged"]


def test_a_row_the_benchset_does_not_enable_wants_no_target(tmp_path):
    # Expansion records such a row as skipped ("not enabled in the benchset"),
    # so no cell runs it and nothing of it needs building — least of all the
    # reference binaries an ARTS-only program never has.
    plane, catalog = load_plane(), load_catalog()
    plan = plan_targets(
        _selection(["arts_val_wb", "xsocr"], {PROBE: [Version.BASE]}),
        plane, catalog, Benchset(name="t", apps={"fib_hpx": BenchsetEntry()}),
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


def _configured(tmp_path: Path) -> Path:
    """A tree generated from the project as first written."""
    src = tmp_path / "src"
    src.mkdir()
    (src / "CMakeLists.txt").write_text(_PROJECT)
    build = tmp_path / "build"
    subprocess.run(
        ["cmake", "-GNinja", "-S", str(src), "-B", str(build),
         "-DCMAKE_BUILD_TYPE=Release"],
        check=True, capture_output=True,
    )
    return build


def _sources_move_on(tmp_path: Path, extra: str) -> None:
    """The project changes after the tree was generated.

    The generation is aged rather than the source pushed into the future: a
    future-dated input stays newer than every regeneration, which is a
    manifest ninja can never bring current, not a tree that moved on.
    """
    (tmp_path / "src" / "CMakeLists.txt").write_text(_PROJECT + extra)
    manifest = tmp_path / "build" / "build.ninja"
    earlier = time.time() - 60
    os.utime(manifest, (earlier, earlier))


@needs_cmake
def test_a_current_tree_is_left_alone(tmp_path):
    build = _configured(tmp_path)
    lines: list[str] = []
    ensure_build_dir(build, bootstrap=True, on_line=lines.append)
    assert lines == []
    assert "old_app" in available_targets(build)


@needs_cmake
def test_a_source_update_reaches_the_target_list_before_it_is_read(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n")
    ensure_build_dir(build, bootstrap=True)
    assert "new_app" in available_targets(build)


@needs_cmake
def test_a_configure_that_now_fails_is_reported_as_the_reason(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n"
                     "find_program(NEEDED NAMES no-such-tool-anywhere REQUIRED)\n")
    with pytest.raises(BuildError, match="no-such-tool-anywhere"):
        ensure_build_dir(build, bootstrap=True)


@needs_cmake
def test_a_dry_run_regenerates_nothing_and_says_the_sources_moved(tmp_path):
    build = _configured(tmp_path)
    _sources_move_on(tmp_path, "add_executable(new_app a.c)\n")
    lines: list[str] = []
    ensure_build_dir(build, bootstrap=False, on_line=lines.append)
    assert any("changed since it was generated" in line for line in lines)
    # Dry means dry: the previous generation is what the tree still holds.
    assert "new_app" not in available_targets(build)
