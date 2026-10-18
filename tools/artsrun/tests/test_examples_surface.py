"""The ARTS-examples section: native programs the ARTS entries alone run.

A row of the section is named `<stem>_example`, its binary is that name, it
has one tier, and its section — not a row flag — says that no reference
runtime has a program for it: pairing it with xsocr, ocrvx or hpx is not a
cell and not a skip, and the campaign records that once for the section.
Its binaries come from the tree's examples directory, its document from the
example's page in the documentation, and selecting it asks for a tree that
builds the section.
"""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest
from pydantic import ValidationError

from artsrun.build import (plan_targets, section_mismatch, section_options)
from artsrun.model.catalog import AppEntry, Kind, Section, Version, load_catalog
from artsrun.model.experiment import Experiment, ExperimentApp
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import Launcher, Profile
from artsrun.model.selection import Selection
from artsrun.run.plan import expand, not_applicable

ROW = "fib_example"
EVERY_KIND = ["arts_val_wb", "xsocr", "ocrvx", "hpx"]


@pytest.fixture(autouse=True)
def _first_threads_first(tmp_path, monkeypatch):
    """A host that numbers every core's first SMT thread first, so whether a
    reference can be placed here is not what these tests see."""
    from artsrun.model import selection

    root = tmp_path / "cpu"
    for cpu in range(64):
        d = root / f"cpu{cpu}" / "topology"
        d.mkdir(parents=True)
        (d / "thread_siblings_list").write_text(f"{cpu % 32},{cpu % 32 + 32}\n")
    monkeypatch.setattr(selection, "SYSFS_CPU_ROOT", root)


def _roster(*names: str) -> Experiment:
    return Experiment(entries=["arts_val_wb"], name="t",
                      apps={n: ExperimentApp() for n in (names or (ROW,))})


def _profile(**over) -> Profile:
    return Profile.model_validate({
        "name": "t", "launcher": "local", "nodes": [1, 2],
        "workers": 15, "progress": 1, **over,
    })


def _selection(entries: list[str], *names: str) -> Selection:
    return Selection(profile="t", experiment="t", default_entries=[],
                     entries=entries,
                     apps={n: [Version.BASE] for n in (names or (ROW,))},
                     node_counts=[1, 2], repeats=1)


def _entry(**over) -> AppEntry:
    base = {"name": "x_example", "binary": "x_example", "class": "task",
            "marker": "M", "section": "examples"}
    base.update(over)
    return AppEntry.model_validate(base)


# --- the section -----------------------------------------------------------
def test_the_section_is_its_own_file_of_native_rows():
    catalog = load_catalog()
    rows = catalog.rows_in(Section.EXAMPLES)
    assert ROW in {a.name for a in rows}
    for row in rows:
        assert row.name.endswith("_example") and row.binary == row.name
        assert row.own_versions == [Version.BASE]
        # The section says the references have no program; the row does not.
        assert not row.arts_only
        assert not row.default_enabled
        assert row.provenance
    for kind in Kind:
        assert not any(a.section is Section.EXAMPLES
                       for a in catalog.rows_of(kind))
    assert [k for k in RuntimeKind if Section.EXAMPLES.runs(k)] == [
        RuntimeKind.ARTS]


def test_a_row_is_named_by_the_suffix_and_has_one_tier():
    with pytest.raises(ValidationError, match="_example"):
        _entry(name="x", binary="x")
    with pytest.raises(ValidationError, match="_example"):
        _entry(binary="x")
    with pytest.raises(ValidationError, match="one tier"):
        _entry(hinted=True)
    with pytest.raises(ValidationError, match="one tier"):
        _entry(restructured_as="y")
    # The suffix names this section, so no other section's row carries it.
    with pytest.raises(ValidationError, match="_example"):
        _entry(section="ocr")


def test_a_row_may_not_restate_the_section_as_a_flag():
    with pytest.raises(ValidationError, match="arts_only"):
        _entry(arts_only=True)


# --- cells -------------------------------------------------------------------
def test_only_the_arts_entries_make_cells_and_nothing_is_skipped(tmp_path):
    plane, catalog = load_plane(), load_catalog()
    configs = {n: {"arts": tmp_path / f"arts_{n}.cfg"} for n in (1, 2)}
    cells, skipped = expand(_selection(EVERY_KIND), plane, catalog, _roster(),
                            _profile(), configs, build_dir=tmp_path)
    assert not skipped
    assert {(c.entry.key, c.nodes) for c in cells} == {
        ("arts_val_wb", 1), ("arts_val_wb", 2)}
    for c in cells:
        assert c.binary == tmp_path / "examples" / f"{ROW}_arts_ocr_val_wb"


def test_the_section_records_its_entries_without_a_program_once():
    plane, catalog = load_plane(), load_catalog()
    record, = not_applicable(_selection(EVERY_KIND), plane, catalog,
                             _roster())
    assert record.section == "examples"
    assert record.entries == ("xsocr", "ocrvx", "hpx")
    assert record.apps == (f"{ROW}:base",)
    assert record.reason == Section.EXAMPLES.reach
    assert Section.EXAMPLES.label in record.line


def test_the_build_wants_only_the_arts_targets(tmp_path):
    plane, catalog = load_plane(), load_catalog()
    plan = plan_targets(_selection(EVERY_KIND), plane, catalog, _roster(),
                        tmp_path, _profile())
    assert plan.targets == [f"{ROW}_arts_ocr_val_wb"]


def test_the_campaign_size_counts_only_the_pairs_with_a_program():
    plane, catalog = load_plane(), load_catalog()
    sel = _selection(EVERY_KIND, ROW, "nqueens")
    # nqueens on the ARTS entry and both OCR references; the example on the
    # ARTS entry alone; neither on hpx.
    assert sel.cell_count(catalog, plane) == (3 + 1) * 2
    assert "ARTS examples: 1 entries x 1 app-versions" in sel.cell_formula(
        catalog, plane)


def test_a_name_the_experiment_does_not_enable_counts_and_configures_nothing(
        tmp_path):
    # quicksort is an OCR row the examples-only roster does not enable: it
    # runs no cell, so it adds neither cells nor a section to configure.
    plane, catalog = load_plane(), load_catalog()
    sel = _selection(EVERY_KIND, ROW, "quicksort")
    roster = _roster()
    assert sel.sections(catalog, roster) == [Section.EXAMPLES]
    assert section_options(sel.sections(catalog, roster)) == section_options(
        [Section.EXAMPLES])
    assert Section.OCR in sel.sections(catalog)
    alone = _selection(EVERY_KIND)
    assert sel.cell_count(catalog, plane, roster) == alone.cell_count(
        catalog, plane, roster) == alone.cell_count(catalog, plane)
    assert "OCR applications" not in sel.cell_formula(catalog, plane, roster)
    plan = plan_targets(sel, plane, catalog, roster, tmp_path, _profile())
    assert plan.targets == [f"{ROW}_arts_ocr_val_wb"]


def test_a_reference_running_no_selected_section_places_nothing():
    # An unpinned profile cannot host a reference; an examples-only campaign
    # with xsocr checked places no reference cell, so it is not refused.
    plane, catalog = load_plane(), load_catalog()
    unpinned = _profile(pin=False)
    _selection(EVERY_KIND).validate_against(plane, catalog, unpinned,
                                            _roster())
    with pytest.raises(ValueError, match="pin: true"):
        _selection(EVERY_KIND, ROW, "nqueens").validate_against(
            plane, catalog, unpinned, _roster(ROW, "nqueens"))


def test_the_row_pin_answers_at_the_row_arguments():
    row = load_catalog().apps[ROW]
    assert row.expect and row.expect_args == row.args


# --- the tree --------------------------------------------------------------
def test_a_new_tree_is_configured_with_exactly_the_selected_sections():
    assert section_options([Section.EXAMPLES]) == [
        "-DARTS_BUILD_OCR_APPS=OFF", "-DARTS_BUILD_HPX_APPS=OFF",
        "-DARTS_BUILD_EXAMPLES=ON"]


def _tree(path: Path, **options: str) -> Path:
    path.mkdir(parents=True)
    (path / "CMakeCache.txt").write_text("".join(
        f"{key}:BOOL={value}\n" for key, value in options.items()))
    return path


def test_a_tree_lacking_a_selected_section_is_the_mismatch(tmp_path):
    tree = _tree(tmp_path / "b", ARTS_BUILD_OCR_APPS="ON",
                 ARTS_BUILD_EXAMPLES="OFF")
    assert section_mismatch(tree, [Section.OCR, Section.EXAMPLES]) == [
        Section.EXAMPLES]
    # An option the cache lacks is created at its default by the next
    # configure, which builds the section.
    assert section_mismatch(tree, [Section.HPX]) == []


def _campaign(tree: Path, monkeypatch, sections: list[Section]):
    import artsrun.campaign as campaign_mod
    from artsrun.campaign import Campaign

    monkeypatch.setattr(campaign_mod, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "require_default_counters",
                        lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "missing_counter_config",
                        lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "plan_targets", lambda *a, **k: "plan")
    monkeypatch.setattr("artsrun.build.platform.machine", lambda: "x86_64")
    return Campaign(
        selection=SimpleNamespace(cxl_entries=lambda _plane: [],
                                  sections=lambda *_a: sections,
                                  entries=["arts_val_wb"]),
        plane=None, catalog=None, experiment=None,
        profile=SimpleNamespace(launcher=Launcher.LOCAL),
        build_dir=tree, run_dir=tree / "run", counterset=None,
    )


def test_selecting_a_section_turns_it_on_and_never_turns_one_off(
        tmp_path, monkeypatch):
    tree = _tree(tmp_path / "b", ARTS_BUILD_OCR_APPS="OFF",
                 ARTS_BUILD_HPX_APPS="OFF", ARTS_BUILD_EXAMPLES="OFF")
    commands = []

    def cmake(cmd, **_kw):
        commands.append(cmd)
        (tree / "CMakeCache.txt").write_text(
            "ARTS_BUILD_OCR_APPS:BOOL=OFF\nARTS_BUILD_HPX_APPS:BOOL=OFF\n"
            "ARTS_BUILD_EXAMPLES:BOOL=ON\n")
        return SimpleNamespace(returncode=0, stdout="", stderr="")

    monkeypatch.setattr("artsrun.build.subprocess.run", cmake)
    monkeypatch.setattr("artsrun.build.shutil.which", lambda _n: "/usr/bin/x")
    campaign = _campaign(tree, monkeypatch, [Section.EXAMPLES])
    said: list[str] = []
    # A dry run changes nothing and says what a real run would pass.
    assert campaign.build_plan(on_line=said.append) == "plan"
    assert commands == []
    assert any("-DARTS_BUILD_EXAMPLES=ON" in line for line in said)
    assert campaign.build_plan(bootstrap=True) == "plan"
    cmd, = commands
    assert "-DARTS_BUILD_EXAMPLES=ON" in cmd
    assert not any(a.endswith("=OFF") for a in cmd)
    assert not any("OCR_APPS" in a or "HPX_APPS" in a for a in cmd)


def test_a_dry_run_shows_the_targets_of_an_off_section_as_pending(tmp_path):
    plane, catalog = load_plane(), load_catalog()
    tree = _tree(tmp_path / "b", ARTS_BUILD_EXAMPLES="OFF")
    (tree / "build.ninja").write_text("")
    wanted = f"{ROW}_arts_ocr_val_wb"
    import artsrun.build as build_mod

    real = build_mod.available_targets
    build_mod.available_targets = lambda _d: {"other"}
    try:
        now = plan_targets(_selection(["arts_val_wb"]), plane, catalog,
                           _roster(), tree)
        later = plan_targets(_selection(["arts_val_wb"]), plane, catalog,
                             _roster(), tree, pending=[Section.EXAMPLES])
    finally:
        build_mod.available_targets = real
    assert now.missing == [wanted] and now.pending == []
    assert later.missing == [] and later.pending == [wanted]


def test_the_section_options_are_the_root_cmake_options_defaulting_on():
    import re

    from artsrun.paths import repo_root

    text = (repo_root() / "CMakeLists.txt").read_text()
    for section in Section:
        option = section.spec.option
        assert re.search(rf'option\(\s*{option}\s+"[^"]*"\s+ON\s*\)', text), (
            f"{option} is not declared ON in the root CMakeLists.txt")


def test_a_tree_that_builds_the_section_is_left_alone(tmp_path, monkeypatch):
    tree = _tree(tmp_path / "b", ARTS_BUILD_EXAMPLES="ON")
    monkeypatch.setattr("artsrun.build.subprocess.run",
                        lambda *a, **k: pytest.fail("reconfigured"))
    assert _campaign(tree, monkeypatch, [Section.EXAMPLES]).build_plan(
        bootstrap=True) == "plan"


# --- the experiment ----------------------------------------------------------
def test_the_examples_experiment_runs_every_row_at_its_own_arguments():
    from artsrun import store

    plane, catalog = load_plane(), load_catalog()
    x = store.load_experiment("examples")
    assert x.entries == plane.ordered(
        [k for k in plane.standard_entries
         if plane.entry(k).kind is RuntimeKind.ARTS], "examples")
    resolved = {a.name: a for a in x.resolve(catalog)}
    assert set(resolved) == {a.name for a in catalog.rows_in(Section.EXAMPLES)}
    # An override would leave a row's pin behind.
    assert not any(a.args_overridden for a in resolved.values())
