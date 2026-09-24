"""The cross-model HPX entry: it runs the HPX-origin section of the catalog.

An HPX-origin row is one program on four runtimes: the HPX original is the
row's `_hpx` target and the OCR mirrors of it are the row's ARTS, XSOCR and
OCR-vx targets — one stem, the runtime tag appended uniformly.  The row lives in its own catalog file, is named with
the `_hpx` suffix, has one tier, and shares no name with an OCR-origin row.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from pydantic import ValidationError

from artsrun.build import plan_targets
from artsrun.model.benchset import Benchset, BenchsetEntry
from artsrun.model.catalog import AppEntry, Kind, Origin, Version, load_catalog
from artsrun.model.plane import RuntimeKind, load_plane
from artsrun.model.profile import Profile
from artsrun.model.selection import Selection
from artsrun.run.command import build_command
from artsrun.run.plan import expand

ROW = "fib_hpx"


@pytest.fixture(autouse=True)
def _first_threads_first(tmp_path, monkeypatch):
    """A host that numbers every core's first SMT thread first.

    Whether a reference can be placed on the host running the suite is a
    property of that host, not of the HPX surface under test here.
    """
    from artsrun.model import selection

    root = tmp_path / "cpu"
    for cpu in range(64):
        d = root / f"cpu{cpu}" / "topology"
        d.mkdir(parents=True)
        (d / "thread_siblings_list").write_text(f"{cpu % 32},{cpu % 32 + 32}\n")
    monkeypatch.setattr(selection, "SYSFS_CPU_ROOT", root)


def _roster(*names: str) -> Benchset:
    """The rows named outright, the way a roster names them.

    The section's rows are toys and so are off by the catalog's default
    (`test_no_probe_or_toy_is_enabled_by_default`); a roster that wants one
    says so, and that is the path these tests exercise.
    """
    return Benchset(name="t", apps={n: BenchsetEntry() for n in (names or (ROW,))})


def _profile() -> Profile:
    return Profile.model_validate({
        "name": "t", "launcher": "local", "nodes": [1, 2],
        "workers": 15, "progress": 1,
    })


def _selection(entries: list[str], apps=None) -> Selection:
    return Selection(
        profile="t", benchset="paper-main", entries=entries,
        apps=apps or {ROW: [Version.BASE]},
        node_counts=[1, 2], repeats=1,
    )


def _entry(**over) -> AppEntry:
    base = {"name": "x", "binary": "x", "class": "task", "marker": "M"}
    base.update(over)
    return AppEntry.model_validate(base)


# --- the section ---------------------------------------------------------------
def test_the_section_is_its_own_file_and_origin():
    catalog = load_catalog()
    names = {a.name for a in catalog.hpx_rows}
    assert names and all(n.endswith("_hpx") for n in names)
    assert all(catalog.apps[n].origin is Origin.HPX for n in names)
    for kind in Kind:
        assert not any(a.origin is Origin.HPX for a in catalog.rows_of(kind))


def test_an_hpx_origin_row_names_its_hpx_program_by_the_runtime_tag():
    e = _entry(name="x_hpx", binary="x_hpx", origin="hpx")
    assert e.hpx_target(Version.BASE) == "x_hpx_hpx"
    assert e.hpx_target(Version.HINTED) is None
    assert e.hpx_versions == [Version.BASE]


def test_an_ocr_origin_row_has_no_hpx_program():
    e = _entry()
    assert e.origin is Origin.OCR
    assert e.hpx_target(Version.BASE) is None
    assert e.hpx_versions == []


def test_the_retired_fields_are_loud_errors():
    with pytest.raises(ValidationError, match="hpx_apps.yaml"):
        _entry(hpx=["base"])
    with pytest.raises(ValidationError, match="hpx_apps.yaml"):
        _entry(hpx_tier="hinted")


def test_an_hpx_origin_row_is_one_tier_named_with_the_suffix():
    with pytest.raises(ValidationError, match="_hpx"):
        _entry(name="x", binary="x", origin="hpx")
    with pytest.raises(ValidationError, match="one tier"):
        _entry(name="x_hpx", binary="x_hpx", origin="hpx", hinted=True)
    with pytest.raises(ValidationError, match="one tier"):
        _entry(name="x_hpx", binary="x_hpx", origin="hpx", restructured_as="y")


# --- resolution ----------------------------------------------------------------
def test_the_row_binds_the_hpx_program_and_the_mirror_stems():
    catalog = load_catalog()
    resolved = {a.key: a for a in _roster().resolve(catalog)}
    row = resolved[f"{ROW}:base"]
    assert row.hpx_binary == f"{ROW}_hpx" and row.binary == ROW
    assert row.hpx_versions == [Version.BASE]
    plane = load_plane()
    assert plane.entry("arts_val_wb").binary(row.binary, hinted=False) == f"{ROW}_arts_ocr_val_wb"
    assert plane.entry("xsocr").binary(row.binary, hinted=False) == f"{ROW}_xsocr"


def test_hpx_cells_exist_for_the_row_and_skip_nothing(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    cells, skipped = expand(
        _selection(["hpx"]), plane, catalog, _roster(), _profile(),
        tmp_path / "apps", {1: {}, 2: {}},
    )
    assert {(c.app.key, c.nodes) for c in cells} == {(f"{ROW}:base", 1), (f"{ROW}:base", 2)}
    assert not skipped
    for c in cells:
        assert c.binary == tmp_path / "hpx" / f"{ROW}_hpx"
        assert c.cfg is None


def test_an_ocr_origin_row_is_ineligible_for_the_hpx_entry(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    sel = _selection(["hpx"], {"fft": [Version.BASE]})
    cells, skipped = expand(sel, plane, catalog, _roster("fft"),
                            _profile(), tmp_path / "apps", {1: {}})
    assert not cells
    assert {s.reason for s in skipped} == {
        "no HPX program: an OCR-origin row (the HPX entry runs the HPX-origin section)"}


def test_expansion_records_a_skip_once_however_many_repeats(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    sel = Selection(profile="t", benchset="paper-main", entries=["hpx"],
                    apps={"fft": [Version.BASE]}, node_counts=[1], repeats=3)
    cells, skipped = expand(sel, plane, catalog, _roster("fft"),
                            _profile(), tmp_path / "apps", {1: {}})
    assert not cells and len(skipped) == 1


def test_the_build_plan_wants_the_program_and_the_mirrors(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    plan = plan_targets(
        _selection(["hpx", "arts_val_wb", "xsocr"]), plane, catalog,
        _roster(), tmp_path,
    )
    assert plan.targets.count(f"{ROW}_hpx") == 1
    assert f"{ROW}_arts_ocr_val_wb" in plan.targets
    assert f"{ROW}_xsocr" in plan.targets


def test_an_hpx_cell_launches_like_a_reference(monkeypatch, tmp_path):
    monkeypatch.setattr("artsrun.run.command._mpi_probe", lambda: "mpich")
    plane = load_plane()
    catalog = load_catalog()
    cells, _ = expand(
        _selection(["hpx"]), plane, catalog,
        _roster(), _profile(), tmp_path / "apps", {1: {}, 2: {}},
    )
    by_nodes = {c.nodes: c for c in cells}
    two = build_command(by_nodes[2], _profile())
    assert two[:3] == ["mpirun", "-bind-to", "none"]
    assert "rank" in two
    assert str(by_nodes[2].binary) in two
    one = build_command(by_nodes[1], _profile())
    assert one[0] == "bash" and one[1].endswith("envelope.sh")
    assert "mpirun" not in one


def test_hpx_never_derives_a_binary_from_a_version_stem():
    plane = load_plane()
    with pytest.raises(ValueError):
        plane.entry("hpx").binary(ROW, hinted=False)


def test_cells_carry_the_cpu_block_they_were_granted(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    configs = {n: {"arts": tmp_path / f"arts_{n}.cfg", "ocr": tmp_path / f"ocr_{n}.cfg"}
               for n in (1, 2)}
    cells, _ = expand(_selection(["hpx", "arts_val_wb"]), plane, catalog,
                      _roster(), _profile(), tmp_path / "apps",
                      configs)
    assert cells and all(c.cpu_width == 16 for c in cells)


def _hpx_cell(tmp_path):
    plane = load_plane()
    catalog = load_catalog()
    cells, _ = expand(_selection(["hpx"]), plane, catalog,
                      _roster(), _profile(), tmp_path / "apps",
                      {1: {}, 2: {}})
    return cells[0]


def test_colocated_hpx_ranks_reach_each_other_over_loopback_tcp(tmp_path):
    from artsrun.run.command import build_env

    env = build_env(_hpx_cell(tmp_path), _profile())
    assert env["UCX_TLS"] == "tcp,self"
    assert env["UCX_NET_DEVICES"] == "lo"


@pytest.mark.parametrize("launcher, extra", [
    ("slurm", {"slurm": {"partition": "p"}}),
    ("flux", {"flux": {"queue": "q"}}),
    ("ssh", {"ssh": {"budget": 2, "hosts": ["n01", "n02"]}}),
])
def test_a_scheduled_hpx_cell_keeps_the_site_mpi_defaults(tmp_path, launcher,
                                                          extra):
    from artsrun.run.command import build_env

    remote = Profile.model_validate({
        "name": "t", "launcher": launcher, "nodes": [1, 2],
        "workers": 15, "progress": 1, "ports": [20000], **extra,
    })
    env = build_env(_hpx_cell(tmp_path), remote)
    assert "UCX_TLS" not in env and "UCX_NET_DEVICES" not in env


def test_an_arts_cell_never_carries_the_transport_override(tmp_path):
    from artsrun.run.command import build_env

    plane = load_plane()
    catalog = load_catalog()
    configs = {n: {"arts": tmp_path / f"arts_{n}.cfg"} for n in (1, 2)}
    cells, _ = expand(_selection(["arts_val_wb"]), plane,
                      catalog, _roster(), _profile(),
                      tmp_path / "apps", configs)
    env = build_env(cells[0], _profile())
    assert "UCX_TLS" not in env and "UCX_NET_DEVICES" not in env


def test_the_struct_marker_is_forwarded_only_when_set(monkeypatch):
    from artsrun.run.command import build_env
    plane = load_plane()
    catalog = load_catalog()
    cells, _ = expand(_selection(["hpx"]), plane, catalog,
                      _roster(), _profile(), Path("/apps"), {1: {}, 2: {}})
    monkeypatch.delenv("ARTS_STRUCT_MARKER", raising=False)
    assert "ARTS_STRUCT_MARKER" not in build_env(cells[0], _profile())
    monkeypatch.setenv("ARTS_STRUCT_MARKER", "1")
    assert build_env(cells[0], _profile())["ARTS_STRUCT_MARKER"] == "1"


# --- cli -----------------------------------------------------------------------
def test_the_apps_listing_shows_the_hpx_origin_section_as_its_own_group(monkeypatch):
    """No test in this suite drives the CLI through typer's CliRunner (no such
    pattern exists anywhere under tests/), so this calls the listing command's
    underlying function directly and captures what it prints."""
    import io

    from rich.console import Console

    from artsrun import cli

    buf = io.StringIO()
    monkeypatch.setattr(cli, "console", Console(file=buf, width=200))
    cli.show_apps(name=None, benchset=None, enabled_only=False)
    out = buf.getvalue()
    heading = out.find("HPX-origin applications")
    row = out.find(ROW)
    assert heading != -1 and row != -1
    assert heading < row


def test_the_section_is_the_four_compared_rows():
    # The section admits an origin only if it never suspends a started
    # parallel task (continuation style, or a bounded number of driver-thread
    # phase joins) and lets the problem set its parallel width; the rows that
    # failed either test are archived, not annotated, so every row here is in
    # the comparison and has its HPX program.
    catalog = load_catalog()
    compared = {"stencil1d_hpx", "fib_hpx", "network_storage_hpx", "fft_hpx"}
    hpx_rows = {name for name, app in catalog.apps.items() if app.origin is Origin.HPX}
    assert hpx_rows == compared
    for name in compared:
        assert catalog.apps[name].hpx_target(Version.BASE) is not None
