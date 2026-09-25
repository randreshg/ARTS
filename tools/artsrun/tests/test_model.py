from __future__ import annotations

import pytest
from pydantic import BaseModel, ValidationError

from artsrun.model import (
    AppEntry,
    Experiment,
    ExperimentApp,
    Catalog,
    Family,
    Kind,
    Profile,
    Release,
    RuntimeKind,
    Selection,
    Version,
    Write,
    load_catalog,
    load_plane,
)


# --- plane ----------------------------------------------------------------
def test_plane_has_twelve_positions_and_eight_configurations():
    plane = load_plane()
    assert len(plane.cells) == 12
    assert sum(c.buildable for c in plane.cells) == 8


def test_every_unbuildable_position_states_a_reason():
    plane = load_plane()
    for cell in plane.cells:
        if not cell.buildable:
            assert cell.reason
            if cell.family is Family.EXCL:
                assert cell.write is Write.WT
            else:
                assert cell.release is Release.PURGE


def test_two_configurations_offer_a_reference_and_the_rest_do_not():
    plane = load_plane()
    assert len(plane.entries) == 14
    refs = [e for e in plane.entries if e.is_reference and not e.is_external]
    assert {e.key for e in refs} == {"xsocr", "ocrvx"}
    assert plane.entry("xsocr").cell == "EXCL/PURGE/WB"
    assert plane.entry("ocrvx").cell == "INV/RETAIN/WB"


def test_hpx_is_selectable_but_sits_on_no_plane_position():
    plane = load_plane()
    hpx = plane.entry("hpx")
    assert hpx.kind is RuntimeKind.HPX
    assert hpx.is_reference and hpx.is_external
    assert hpx.cell is None
    assert all(hpx not in plane.entries_of(c) for c in plane.cells)


def test_the_db_wrf_section_offers_flush_and_draws_its_retired_arm():
    plane = load_plane()
    assert [m.model for m in plane.models] == ["DB_WRF"]
    db_wrf = plane.models[0]
    assert [c.arm for c in db_wrf.cells] == ["FLUSH", "VAL_WT_RETAIN"]
    flush, retired = db_wrf.cells
    assert flush.buildable and flush.variant == "wrf_flush"
    assert not retired.buildable and "retired" in retired.reason
    entry = plane.entry("arts_wrf_flush")
    assert entry.kind is RuntimeKind.ARTS and entry.model == "DB_WRF"
    assert entry.cell == "DB_WRF/FLUSH"
    assert plane.entries_of(flush) == [entry]
    assert plane.entries_of(retired) == []
    assert entry.binary("nqueens", hinted=False) == "nqueens_arts_wrf_flush"


def test_the_plane_orders_and_checks_a_named_entry_list():
    plane = load_plane()
    # Plane order, whatever order the list was written in.
    assert plane.ordered(["hpx", "arts_val_wb"], "x") == ["arts_val_wb", "hpx"]
    with pytest.raises(ValueError, match="x names unknown plane entries: nope"):
        plane.ordered(["arts_val_wb", "nope"], "x")
    # The standard entries: every OCR-model entry whose store is not CXL
    # memory.
    assert set(plane.entry_keys) - set(plane.standard_entries) == {
        "arts_wrf_flush"} | {e.key for e in plane.entries if e.is_cxl}


ARTS8 = ["arts_excl_purge", "arts_excl_retain", "arts_inv_wt_purge",
         "arts_inv_wt", "arts_inv_wb", "arts_val_wt_purge", "arts_val_wt",
         "arts_val_wb"]
SHIPPED_ENTRIES = {
    "paper-main": ARTS8 + ["xsocr", "ocrvx"],
    "paper-main-base": ARTS8 + ["xsocr", "ocrvx"],
    "paper-main-hinted": ARTS8 + ["xsocr", "ocrvx"],
    "paper-gate": ARTS8 + ["xsocr", "ocrvx"],
    "control-main": ARTS8 + ["hpx"],
    "control-gate": ARTS8 + ["hpx"],
    "control-fft": ARTS8 + ["hpx"],
    "trend": ARTS8 + ["xsocr", "ocrvx", "hpx"],
    "smoke": ARTS8 + ["xsocr", "ocrvx"],
}


def test_every_shipped_experiment_validates_and_states_its_entries():
    from artsrun import store

    plane = load_plane()
    assert store.list_experiments() == sorted(SHIPPED_ENTRIES)
    catalog = load_catalog()
    for name, want in SHIPPED_ENTRIES.items():
        x = store.load_experiment(name)
        assert x.name == name
        assert set(x.entries) == set(want), name
        assert x.entries == plane.ordered(want, name)
        # No shipped default reaches the DB-WRF model or a CXL store: those
        # are turned on per run.
        assert not any(plane.entry(k).is_cxl or plane.entry(k).model != "OCR"
                       for k in x.entries), name
        assert x.resolve(catalog), name
        # Every file says what it is for.
        head = store.experiment_path(name).read_text().splitlines()[0]
        assert head.startswith(f"# Experiment {name}:"), name
    assert store.load_experiment("trend").entries == plane.standard_entries


def test_smoke_runs_every_paper_gate_row_at_its_own_arguments():
    """smoke is paper-gate's roster shrunk again, not a different roster.

    Same app keys (every paper-gate row, base/hinted/restructured and the
    restructured tier's own rewrite-CLI entries alike) and the same per-key
    `versions` -- only the argument vectors are free to differ.
    """
    from artsrun import store

    gate, smoke = store.load_experiment("paper-gate"), store.load_experiment("smoke")
    assert set(smoke.apps) == set(gate.apps)
    for name, app in gate.apps.items():
        other = smoke.apps[name]
        assert other.versions == app.versions, name
        assert other.enabled == app.enabled, name
    # At least one row's vector actually shrank -- smoke is not a copy.
    assert any(smoke.apps[name].args != app.args for name, app in gate.apps.items())


def test_an_experiment_must_name_plane_entries():
    with pytest.raises(ValidationError, match="experiment names unknown plane "
                                              "entries: nope"):
        Experiment.model_validate({"name": "x", "entries": ["nope"]})
    with pytest.raises(ValidationError, match="entries"):
        Experiment.model_validate({"name": "x"})
    # Any plane entry may be a default, CXL and DB-WRF included.
    x = Experiment.model_validate({"name": "x", "entries": [
        "arts_wrf_flush", "arts_excl_purge_cxl_staged"]})
    assert x.entries == ["arts_excl_purge_cxl_staged", "arts_wrf_flush"]


def test_a_node_profile_that_lists_entries_is_refused_naming_the_move():
    with pytest.raises(ValidationError, match="no longer lists entries.*moved "
                                              "to the experiment"):
        Profile.model_validate(_local(entries=["arts_excl_purge"]))
    from artsrun import store

    for name in store.list_profiles():
        assert "entries" not in store.load_profile(name).model_dump(), name


def test_a_selection_saved_under_a_benchset_replays_as_its_experiment():
    base = {"profile": "p", "entries": ["arts_val_wb"],
            "apps": {"nqueens": ["base"]}, "node_counts": [1]}
    for old, new in (("main-gate", "paper-gate"),
                     ("paper-controls", "control-main"),
                     ("controls-gate", "control-gate"),
                     ("paper-main", "paper-main")):
        sel = Selection.model_validate(base | {"benchset": old})
        assert sel.experiment == new
        assert sel.default_entries is None


def test_everything_starts_from_the_experiment_entries():
    from artsrun import store

    plane, catalog = load_plane(), load_catalog()
    x = store.load_experiment("control-gate")
    sel = Selection.everything(plane, catalog, x, Profile.model_validate(_local()))
    assert sel.experiment == "control-gate"
    assert sel.entries == sel.default_entries == x.entries


def _model_keys(model: type[BaseModel], prefix: str = "") -> set[str]:
    out: set[str] = set()
    for name, info in model.model_fields.items():
        inner = [a for a in getattr(info.annotation, "__args__", ())
                 if isinstance(a, type) and issubclass(a, BaseModel)]
        if inner:
            out |= _model_keys(inner[0], f"{prefix}{name}.")
        else:
            out.add(f"{prefix}{name}")
    return out


def _orphan_form_keys(specs, model) -> set[str]:
    """Form keys that name no field of the model (dotted for nested)."""
    return {spec.key for spec in specs} - _model_keys(model)


def test_every_profile_field_is_one_the_form_carries():
    """The screens rebuild a profile from the form's fields, so a field the
    form does not know is silently reset to its default in every campaign
    started from them; a form field that names no `Profile` field either
    raises `ValidationError` at rebuild time or is silently dropped,
    depending on where it is consumed."""
    from artsrun.model.profile import Profile
    from artsrun.tui import form

    carried = {spec.key for spec in form.PROFILE_FIELDS} | {"name", "nodes"}
    assert _model_keys(Profile) - carried == set()
    assert _orphan_form_keys(form.PROFILE_FIELDS, Profile) == set()
    bogus = [*form.PROFILE_FIELDS,
             form.FieldSpec("not_a_profile_field", "x", "int", "x",
                            section="run")]
    assert _orphan_form_keys(bogus, Profile) == {"not_a_profile_field"}


def test_grid_entries_belong_to_the_ocr_model():
    plane = load_plane()
    assert {e.model for e in plane.entries if e.cell and "/" in e.cell and e.cell.count("/") == 2} == {"OCR"}


def test_binary_names_follow_the_build_convention():
    plane = load_plane()
    arts = plane.entry("arts_val_wb")
    assert arts.binary("nqueens", hinted=False) == "nqueens_arts_ocr_val_wb"
    assert arts.binary("nqueens", hinted=True) == "nqueens_hinted_arts_ocr_val_wb"
    assert plane.entry("xsocr").binary("nqueens", hinted=False) == "nqueens_xsocr"
    assert plane.entry("ocrvx").kind is RuntimeKind.OCRVX


def test_the_excl_purge_wb_cell_offers_two_cxl_entries_beside_arts_and_xsocr():
    plane = load_plane()
    cell = plane.cell(Family.EXCL, Release.PURGE, Write.WB)
    assert [e.label for e in plane.entries_of(cell)] == [
        "arts_excl_purge", "ARTS-CXL-STAGED", "ARTS-CXL-DIRECT", "XSOCR"]
    staged = plane.entry("arts_excl_purge_cxl_staged")
    assert staged.kind is RuntimeKind.ARTS and staged.cell == cell.key
    assert staged.variant == "ocr_excl_purge_cxl_staged"
    assert not staged.is_reference and not staged.is_external
    assert staged.is_cxl
    assert staged.binary("nqueens", hinted=False) == \
        "nqueens_arts_ocr_excl_purge_cxl_staged"
    direct = plane.entry("arts_excl_purge_cxl_direct")
    assert direct.is_cxl
    assert direct.binary("nqueens", hinted=True) == \
        "nqueens_hinted_arts_ocr_excl_purge_cxl_direct"
    assert not plane.entry("xsocr").is_cxl
    assert not plane.entry("arts_excl_purge").is_cxl


def test_every_other_buildable_cell_still_offers_at_most_two_entries():
    plane = load_plane()
    cxl_cell = plane.cell(Family.EXCL, Release.PURGE, Write.WB).key
    for cell in plane.cells:
        if cell.buildable and cell.key != cxl_cell:
            assert cell.entries == []
            assert len(plane.entries_of(cell)) <= 2


# --- catalog --------------------------------------------------------------
def test_catalog_rows_exclude_rewrites():
    catalog = load_catalog()
    rewrites = {a.name for a in catalog.apps.values() if a.restructured_from}
    assert rewrites
    assert not rewrites & {a.name for a in catalog.rows}


def test_every_unordered_writes_reason_cites_a_source_line():
    import re
    for app in load_catalog().apps.values():
        if app.unordered_writes:
            assert re.search(r"\.c:\d+", app.unordered_writes), app.name


def test_a_restructured_version_resolves_to_the_rewrite_target():
    # Whichever application offers one — naming a specific application here
    # makes the test fail when that application's rewrite is held back, which
    # says nothing about the resolution being tested.
    catalog = load_catalog()
    named = next(a for a in catalog.rows if a.restructured_as)
    source, stem = catalog.resolve(named.name, Version.RESTRUCTURED)
    assert source.name == named.restructured_as
    assert stem == catalog.apps[named.restructured_as].binary


def test_optimized_version_resolves_to_the_opt_target():
    catalog = load_catalog()
    _, stem = catalog.resolve("fft", Version.HINTED)
    assert stem.endswith("_hinted")


def test_optimized_is_refused_where_the_source_has_no_hint_layer():
    # Whichever application has no layer — naming one here makes the test fail
    # when that application later gains one, which says nothing about the
    # refusal being tested.
    catalog = load_catalog()
    bare = next(a for a in catalog.rows if not a.hinted)
    with pytest.raises(KeyError):
        catalog.resolve(bare.name, Version.HINTED)


def test_a_real_run_configures_a_missing_build_tree(tmp_path, monkeypatch):
    # The experiment tree is fully determined (Release, benchmarks on), so a
    # missing one is a first run, not an error to hand back to the user.
    from artsrun import build as build_mod

    calls = {}
    tree = tmp_path / "bt"

    class FakeProc:
        stdout = iter(())

        def wait(self):
            return 0

    def fake_popen(cmd, **_kw):
        calls["cmd"] = cmd
        tree.mkdir(parents=True, exist_ok=True)
        (tree / "build.ninja").write_text("")
        return FakeProc()

    monkeypatch.setattr(build_mod.subprocess, "Popen", fake_popen)
    monkeypatch.setattr(build_mod.shutil, "which", lambda _n: "/usr/bin/cmake")
    build_mod.ensure_build_dir(tree, bootstrap=True)
    assert "-GNinja" in calls["cmd"]
    assert f"-B{tree}" in calls["cmd"]
    assert "-DCMAKE_BUILD_TYPE=Release" in calls["cmd"]


def test_a_dry_run_configures_nothing(tmp_path):
    from artsrun import build as build_mod

    with pytest.raises(build_mod.BuildError, match="real run configures"):
        build_mod.ensure_build_dir(tmp_path / "bt", bootstrap=False)
    assert not (tmp_path / "bt").exists()


def test_the_old_hinted_name_still_parses_as_optimized():
    # The version was recorded as "hinted" before the rename; selections and
    # experiments written under that name must replay unchanged.
    assert Version("hinted") is Version.HINTED
    selection = Selection.model_validate({
        "profile": "p", "experiment": "b", "entries": ["arts_val_wb"],
        "apps": {"nqueens": ["hinted"]}, "node_counts": [1],
    })
    assert selection.apps["nqueens"] == [Version.HINTED]
    bench = Experiment.model_validate(
        {"name": "b", "entries": ["arts_val_wb"], "apps": {"nqueens": {"versions": ["hinted"]}}})
    assert bench.apps["nqueens"].versions == [Version.HINTED]


# --- profile --------------------------------------------------------------
def _local(**over):
    base = dict(
        name="t", launcher="local", nodes=[1, 2], workers=3, progress=1,
    )
    base.update(over)
    return base


def test_local_profile_rejects_ports():
    with pytest.raises(ValidationError, match="ports must not be set"):
        Profile.model_validate(_local(ports=[25000]))


def test_the_local_cxl_smoke_profile_loads_and_fits_the_host(monkeypatch,
                                                              tmp_path):
    from artsrun import store
    from artsrun.model.profile import CxlLibrary, Launcher

    # The rule is judged against a half-split eight-core topology, not the
    # machine the suite happens to run on.
    _use_topology(monkeypatch, tmp_path,
                  {c: f"{c},{c + 8}" for c in range(8)}
                  | {c + 8: f"{c},{c + 8}" for c in range(8)})
    plane = load_plane()
    profile = store.load_profile("local-cxl")
    assert profile.launcher is Launcher.LOCAL
    assert profile.cxl_library_kind is CxlLibrary.FAKE
    # the smoke's entries are named for the run: one cell's worth
    entries = plane.ordered(["xsocr", "arts_excl_purge",
                             "arts_excl_purge_cxl_staged",
                             "arts_excl_purge_cxl_direct"], "-e")
    assert entries == [
        "arts_excl_purge", "arts_excl_purge_cxl_staged",
        "arts_excl_purge_cxl_direct", "xsocr",
    ]
    # The profile's geometry must satisfy the driver's own reference-geometry
    # rule (a reference entry's colocated local ranks are each pinned to one
    # physical core), not a hand-computed bound against the logical CPU
    # count — so this exercises the real check a campaign runs into, rather
    # than reimplementing it and drifting from it.
    selection = Selection(
        profile=profile.name, experiment="b", entries=entries,
        apps={"nqueens": [Version.BASE]}, node_counts=profile.nodes,
    )
    selection.validate_against(plane, load_catalog(), profile)


def test_a_geometry_wider_than_the_machine_is_not_the_tool_s_call():
    # ARTS does not oversubscribe and says so at startup; the profile only
    # records the shape asked for.
    profile = Profile.model_validate(_local(nodes=[1, 2, 4, 8], workers=63))
    assert profile.threads_per_node == 64


def test_remote_profile_requires_ports():
    with pytest.raises(ValidationError, match="ports is required"):
        Profile.model_validate(_local(launcher="ssh"))


def test_profile_refuses_unknown_keys():
    # A typoed key must refuse rather than vanish: slurm.mpi is the only
    # handle against an otherwise-silent PMI failure, so a key that "took"
    # while doing nothing is the worst outcome.
    with pytest.raises(ValidationError, match="extra_forbidden|Extra inputs"):
        Profile.model_validate(_local(typoed_key=1))
    with pytest.raises(ValidationError, match="extra_forbidden|Extra inputs"):
        Profile.model_validate(_local(
            launcher="slurm", ports=[25000], slurm={"budget": 32},
        ))


def test_ssh_hosts_must_be_distinct():
    with pytest.raises(ValidationError, match="distinct"):
        Profile.model_validate(_local(
            launcher="ssh", ports=[25000],
            ssh={"budget": 2, "hosts": ["n01", "n01"]},
        ))


def test_slurm_needs_no_budget_and_defaults_its_build_slot():
    # Scheduling the queue is Slurm's whole purpose: every cell is submitted
    # up front, so a profile carries no admission budget — only where the
    # cells and the build work go, and how wide the build slot is.
    profile = Profile.model_validate(_local(
        launcher="slurm", nodes=[1, 2, 4, 8], ports=[25000],
        slurm={"partition": "pbatch", "build_partition": "pdebug"},
    ))
    assert profile.slurm.partition == "pbatch"
    assert profile.slurm.build_partition == "pdebug"
    assert profile.slurm.build_cpus == 8


# --- experiment -----------------------------------------------------------
def test_experiment_falls_through_to_the_catalog():
    catalog = load_catalog()
    resolved = {a.key: a for a in Experiment(entries=["arts_excl_purge"], name="empty").resolve(catalog)}
    assert resolved["nqueens:base"].args == catalog.apps["nqueens"].args
    assert not resolved["nqueens:base"].args_overridden


def test_experiment_override_marks_the_argument_source():
    catalog = load_catalog()
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={"nqueens": ExperimentApp(args=["8", "2"])})
    resolved = {a.key: a for a in bs.resolve(catalog)}
    assert resolved["nqueens:base"].args == ["8", "2"]
    assert resolved["nqueens:base"].args_overridden


def _catalog_with_a_node_laddered_row() -> tuple[Catalog, AppEntry]:
    # The shipped catalog no longer ships any row with args_by_nodes (the
    # attack suite was the only user, and it derives population from the
    # rank count instead); this general roster-override mechanic needs its
    # own fixture rather than fishing one out of load_catalog().
    row = AppEntry.model_validate({
        "name": "r", "binary": "r", "class": "spmd", "marker": "DONE",
        "args": ["9"], "args_by_nodes": {1: ["8"], 2: ["7"]},
    })
    return Catalog(apps={"r": row}), row


def test_a_roster_args_override_replaces_the_catalogs_per_node_editions():
    # A row's catalog arguments may come as one list plus per-node editions.
    # A roster that gives `args` alone has replaced the whole surface: one
    # list serves every geometry, and the catalog's editions must not answer
    # for any node count -- otherwise the override is dead exactly where
    # the catalog is most specific.
    catalog, row = _catalog_with_a_node_laddered_row()
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={row.name: ExperimentApp(args=["1", "2"])})
    got = {a.key: a for a in bs.resolve(catalog)}[f"{row.name}:base"]
    assert got.args_overridden
    assert got.args_by_nodes == {}
    for n in {1, *row.args_by_nodes}:
        assert got.args_for(n) == ["1", "2"]


def test_a_roster_keeps_its_own_per_node_editions_beside_its_args():
    catalog, row = _catalog_with_a_node_laddered_row()
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={row.name: ExperimentApp(
        args=["1", "2"], args_by_nodes={2: ["3", "4"]})})
    got = {a.key: a for a in bs.resolve(catalog)}[f"{row.name}:base"]
    assert got.args_for(2) == ["3", "4"]
    assert got.args_for(1) == ["1", "2"]
    assert got.args_for(8) == ["1", "2"]


def test_a_rewrites_override_replaces_its_per_node_editions_too():
    catalog = Catalog(apps={
        "r": AppEntry.model_validate({
            "name": "r", "binary": "r", "class": "spmd", "marker": "DONE",
            "restructured_as": "r_dist"}),
        "r_dist": AppEntry.model_validate({
            "name": "r_dist", "binary": "r_dist", "class": "spmd",
            "marker": "DONE", "args": ["9"],
            "args_by_nodes": {1: ["8"], 2: ["7"]}}),
    })
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={
        "r": ExperimentApp(versions=[Version.RESTRUCTURED]),
        "r_dist": ExperimentApp(args=["1"]),
    })
    got = {a.key: a for a in bs.resolve(catalog)}["r:restructured"]
    assert got.args_overridden
    assert got.args_for(1) == ["1"] and got.args_for(2) == ["1"]


def test_experiment_overrides_a_restructured_row_by_the_rewrites_name():
    catalog = load_catalog()
    row = next(a for a in catalog.rows if a.restructured_as)
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={
        row.name: ExperimentApp(versions=[Version.RESTRUCTURED]),
        row.restructured_as: ExperimentApp(args=["7", "3"]),
    })
    resolved = {a.key: a for a in bs.resolve(catalog)}
    got = resolved[f"{row.name}:restructured"]
    assert got.args == ["7", "3"]
    assert got.args_overridden


def test_a_row_override_never_follows_into_the_rewrites_cli():
    catalog = load_catalog()
    row = next(a for a in catalog.rows if a.restructured_as)
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={
        row.name: ExperimentApp(versions=[Version.RESTRUCTURED],
                                args=["9", "9"]),
    })
    resolved = {a.key: a for a in bs.resolve(catalog)}
    got = resolved[f"{row.name}:restructured"]
    assert got.args == catalog.apps[row.restructured_as].args
    assert not got.args_overridden


def test_a_rows_override_reaches_its_own_tiers_and_stops_at_the_rewrite():
    # One roster entry can carry both: the row's own name sizes the tiers
    # that share its CLI, the rewrite's name sizes the rewrite.  Neither
    # override may cross into the other's arguments.
    catalog = load_catalog()
    row = next(a for a in catalog.rows if a.restructured_as and a.hinted)
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={
        row.name: ExperimentApp(args=["4", "4"]),
        row.restructured_as: ExperimentApp(args=["7", "3"]),
    })
    resolved = {a.key: a for a in bs.resolve(catalog)}
    assert resolved[f"{row.name}:base"].args == ["4", "4"]
    assert resolved[f"{row.name}:hinted"].args == ["4", "4"]
    assert resolved[f"{row.name}:restructured"].args == ["7", "3"]


def test_experiment_disable_removes_every_version():
    catalog = load_catalog()
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={"nqueens": ExperimentApp(enabled=False)})
    assert not [a for a in bs.resolve(catalog) if a.name == "nqueens"]


def test_a_version_an_application_lacks_is_dropped_and_said_out_loud(capsys):
    # A roster keeps naming a version the catalog has withdrawn, so it comes
    # back on its own when the catalog restores it; what must not happen is
    # the campaign running as though it had measured it.
    catalog = load_catalog()
    bare = next(a for a in catalog.rows if not a.hinted)
    bs = Experiment(entries=["arts_excl_purge"], name="o", apps={bare.name: ExperimentApp(
        versions=[Version.BASE, Version.HINTED])})
    got = bs.resolve(catalog)
    assert [a.key for a in got] == [f"{bare.name}:base"]
    assert "no hinted version" in capsys.readouterr().err


# --- selection ------------------------------------------------------------
def test_selection_rejects_a_node_count_outside_the_profile_sweep():
    plane, catalog = load_plane(), load_catalog()
    profile = Profile.model_validate(_local())
    sel = Selection(
        profile="t", experiment="b", entries=["arts_val_wb"],
        apps={"nqueens": [Version.BASE]}, node_counts=[8],
    )
    with pytest.raises(ValueError, match="not in profile"):
        sel.validate_against(plane, catalog, profile)


# --- declared width -------------------------------------------------------
def _width_catalog(cls: str, width: int | None) -> Catalog:
    spec = {"name": "w", "binary": "w", "class": cls, "marker": "DONE"}
    if width is not None:
        spec["width_max"] = width
    return Catalog(apps={"w": AppEntry.model_validate(spec)})


def _width_selection() -> Selection:
    return Selection(
        profile="t", experiment="b", entries=["arts_val_wb"],
        apps={"w": [Version.BASE]}, node_counts=[1, 2],
    )


def _validate_width(cls: str, width: int | None) -> None:
    # nodes [1, 2] x 3 workers: the machine is 6 workers wide at its widest.
    profile = Profile.model_validate(_local())
    _width_selection().validate_against(
        load_plane(), _width_catalog(cls, width), profile)


def test_a_row_that_declares_no_width_is_not_checked():
    _validate_width("task", None)


def test_a_width_below_the_widest_geometry_is_refused_by_name():
    with pytest.raises(ValueError, match=r"w: width_max=5 is below"):
        _validate_width("task", 5)


def test_a_task_width_needs_only_to_cover_the_machine():
    # A task decomposition drains its excess; only the floor is structural.
    _validate_width("task", 6)
    _validate_width("task", 7)


def test_a_team_width_must_divide_the_machine_evenly():
    for cls in ("spmd", "mw"):
        _validate_width(cls, 6)
        _validate_width(cls, 12)
        with pytest.raises(ValueError, match="not a whole multiple"):
            _validate_width(cls, 7)


def test_a_roster_override_suspends_the_width_check_and_says_so(capsys):
    # width_max describes the catalog's own arguments; a roster that replaces
    # them is running a different workload, so the number no longer describes
    # the cell.  Refusing would break the smoke rosters shrinking a row is
    # for, so the campaign is told instead.
    profile = Profile.model_validate(_local())
    catalog = _width_catalog("mw", 7)          # would be refused as declared
    bs = Experiment(entries=["arts_excl_purge"], name="smoke", apps={"w": ExperimentApp(args=["2"])})
    _width_selection().validate_against(load_plane(), catalog, profile, bs)
    err = capsys.readouterr().err
    assert "width_max=7" in err and "smoke" in err


def test_a_roster_that_leaves_the_arguments_alone_is_still_checked():
    profile = Profile.model_validate(_local())
    catalog = _width_catalog("mw", 7)
    bs = Experiment(entries=["arts_excl_purge"], name="full", apps={"w": ExperimentApp()})
    with pytest.raises(ValueError, match="not a whole multiple"):
        _width_selection().validate_against(
            load_plane(), catalog, profile, bs)


def test_cell_count_is_the_product_of_the_three_surfaces():
    sel = Selection(
        profile="t", experiment="b", entries=["arts_val_wb", "xsocr"],
        apps={"nqueens": [Version.BASE, Version.HINTED]},
        node_counts=[1, 2], repeats=3,
    )
    assert sel.cell_count == 2 * 2 * 2 * 3


def test_an_experiment_that_names_applications_defines_the_roster():
    # A short experiment is a short campaign, not the whole catalog with three
    # entries annotated.
    catalog = load_catalog()
    bs = Experiment(entries=["arts_excl_purge"], name="small", apps={"nqueens": ExperimentApp()})
    names = {a.name for a in bs.resolve(catalog)}
    assert names == {"nqueens"}


def test_an_empty_experiment_defers_to_the_catalog_defaults():
    catalog = load_catalog()
    names = {a.name for a in Experiment(entries=["arts_excl_purge"], name="empty").resolve(catalog)}
    assert names == {a.name for a in catalog.rows if a.default_enabled}


# --- ports and hosts ------------------------------------------------------
def test_a_local_run_states_a_connection_count_but_no_ports():
    profile = Profile.model_validate(_local(port_count=4))
    assert profile.port_count == 4
    assert profile.ports == []


def test_a_remote_port_list_must_match_the_connection_count():
    with pytest.raises(ValidationError, match="must name exactly that many"):
        Profile.model_validate(_local(
            launcher="slurm", ports=[25000], port_count=2,
            slurm={},
        ))


def test_a_matching_port_list_is_accepted():
    profile = Profile.model_validate(_local(
        launcher="slurm", ports=[25000, 25001], port_count=2,
        slurm={},
    ))
    assert len(profile.ports) == profile.port_count


def test_ssh_hosts_must_number_exactly_the_node_budget():
    with pytest.raises(ValidationError, match="name one host per node"):
        Profile.model_validate(_local(
            launcher="ssh", ports=[25000],
            ssh={"budget": 4, "hosts": ["n01", "n02"]},
        ))


def test_ssh_budget_below_the_widest_cell_is_refused():
    with pytest.raises(ValidationError, match="nowhere to run"):
        Profile.model_validate(_local(
            launcher="ssh", nodes=[1, 4], ports=[25000],
            ssh={"budget": 2, "hosts": ["n01", "n02"]},
        ))


def test_a_consistent_ssh_profile_is_accepted():
    profile = Profile.model_validate(_local(
        launcher="ssh", nodes=[1, 2, 4], ports=[25000],
        ssh={"budget": 4, "hosts": ["n01", "n02", "n03", "n04"]},
    ))
    assert profile.hosts == ["n01", "n02", "n03", "n04"]


def test_columns_group_by_write_policy_then_split_by_release():
    plane = load_plane()
    labelled = [(plane.write_label(w), plane.release_label(r))
                for r, w in plane.columns()]
    assert labelled == [
        ("Write Through", "Purge"), ("Write Through", "Retain"),
        ("Write Back", "Purge"), ("Write Back", "Retain"),
    ]


def test_the_write_policy_is_spelled_out():
    plane = load_plane()
    assert plane.write_label(Write.WT) == "Write Through"
    assert plane.write_label(Write.WB) == "Write Back"


def test_the_policies_read_as_words_and_the_families_as_acronyms():
    plane = load_plane()
    assert plane.release_label(Release.PURGE) == "Purge"
    assert plane.release_label(Release.RETAIN) == "Retain"
    assert plane.family_labels[Family.EXCL] == "EXCL"


# --- application vs attack probe vs toy -----------------------------------
def test_the_catalog_separates_apps_probes_and_toys():
    catalog = load_catalog()
    apps = catalog.rows_of(Kind.APP)
    attacks = catalog.rows_of(Kind.ATTACK)
    toys = catalog.rows_of(Kind.TOY)
    hpx = catalog.hpx_rows
    assert apps and attacks and toys and hpx
    grouped = {a.name for a in (*apps, *attacks, *toys, *hpx)}
    assert grouped == {a.name for a in catalog.rows}
    assert len(apps) + len(attacks) + len(toys) + len(hpx) == len(catalog.rows)


def test_the_old_probe_spelling_still_parses():
    # The probe group was spelled "microbench" before its rename.
    assert Kind("microbench") is Kind.ATTACK


def test_no_probe_or_toy_is_enabled_by_default():
    # A toy is a regression check and an attack row runs in the
    # control-main roster; neither belongs in a fresh comparison campaign.
    # Every row of every section is held to this, so a new section inherits
    # the rule.
    catalog = load_catalog()
    offenders = [a.name for a in catalog.rows
                 if a.default_enabled and a.kind in (Kind.TOY, Kind.ATTACK)]
    assert not offenders, f"toys or probes enabled by default: {offenders}"


ATTACK_ROWS = {
    "own_reread_64k": "own_reread", "own_reread_16m": "own_reread",
    "own_rewrite_64k": "own_rewrite", "own_rewrite_16m": "own_rewrite",
    "one_home_funnel_4m": "read_funnel", "spread_home_funnel_4m": "read_funnel",
    "audienceless_publish_16m": "audienceless_publish",
    "freerun_read_heavy_64k": "freerun_mix", "freerun_grain_200us_64k": "freerun_mix",
    "freerun_grain_2ms_64k": "freerun_mix",
    "pipeline_converge_1m": "pipeline_converge",
    "alternating_bomb_64k": "alternating_bomb", "bomb_narrow_sharers_64k": "alternating_bomb",
    "resident_mill_1m": "resident_mill", "sparse_mill_1m": "resident_mill",
    "slow_churn_dial_16m": "resident_mill",
    "migration_gauntlet_64k": "migration_gauntlet", "migration_gauntlet_16m": "migration_gauntlet",
    "handoff_control_64k": "handoff_lattice", "handoff_long_read_64k": "handoff_lattice",
    "serial_rounds": "serial_rounds",
}


def test_every_attack_row_is_one_program_with_one_argument_vector():
    catalog = load_catalog()
    assert {row.name for row in catalog.rows_of(Kind.ATTACK)} == set(ATTACK_ROWS)
    for row, binary in ATTACK_ROWS.items():
        app = catalog.apps[row]
        assert app.kind is Kind.ATTACK, row
        assert app.binary == binary, row
        assert not app.args_by_nodes, f"{row}: populations are the program's, not a ladder"
        assert app.marker.startswith(binary.upper() + " OK"), row
    assert not {"rwmix", "rwsteady", "rwpriv", "rwhandoff", "rwrounds"} & set(catalog.apps)


def test_the_suite_core_is_made_of_applications():
    catalog = load_catalog()
    for name in ("graph500", "hpcg_intel", "CoMD_sdsc2", "quicksort",
                 "nekbone", "hpgmg", "npb_cg", "cholesky_blas"):
        assert catalog.apps[name].kind is Kind.APP, name


def test_fixtures_and_library_drivers_are_toys():
    # Upstream files these under kernels/ or examples/, or their own README
    # calls them a driver for one library.
    catalog = load_catalog()
    for name in ("printf", "testlibs", "basicIO", "highbw", "prodcon",
                 "dbctrl", "reduction_intel", "xeonNumaSize"):
        assert catalog.apps[name].kind is Kind.TOY, name


def test_an_idiom_study_series_is_a_mechanism_probe():
    # The david stencil1D set shares a directory with the PRK Stencil ports
    # but neither the kernel nor the provenance: its own README presents it
    # as a comparison of event-passing styles on a hand-written solver, and
    # its default arguments were tuned to CI execution time, not to a
    # problem anyone cites — that is exercising one runtime mechanism, not
    # a variant of the published benchmark.
    catalog = load_catalog()
    for name in ("stencil1D_once", "stencil1D_oncePI", "stencil1D_sticky",
                 "stencil1D_stickyLG", "stencil1D_guid", "stencil1D_guidPI",
                 "stencil1D_channel"):
        entry = catalog.apps[name]
        assert entry.kind is Kind.TOY, name
        assert not entry.default_enabled, name


def test_operations_named_after_themselves_are_toys():
    # "globalsum" is the name of a sum, not of a benchmark anyone cites.
    catalog = load_catalog()
    for name in ("globalsum_cgShim", "globalsum_cgNoShim", "globalsum_pcg",
                 "curvefit", "dbcreate_matrix"):
        assert catalog.apps[name].kind is Kind.TOY, name


def test_cited_benchmarks_are_applications():
    catalog = load_catalog()
    for name in ("graph500", "hpcg_intel", "CoMD_sdsc2", "XSBench_intel",
                 "stream", "nekbone", "smithwaterman", "npb_cg"):
        assert catalog.apps[name].kind is Kind.APP, name


def test_every_catalog_entry_states_where_it_came_from():
    catalog = load_catalog()
    missing = [a.name for a in catalog.apps.values() if not a.provenance]
    assert not missing, f"no provenance for: {', '.join(sorted(missing))}"


def test_sar_is_one_application_and_its_restructured_tier():
    # sar_tiny/small/medium/large were rungs of one program's shipped
    # parameter ladder, carried as if they were different applications.  A
    # rung is an argument, so the roster keeps the one row and gives it the
    # restructured tier instead.
    catalog = load_catalog()
    for rung in ("sar_tiny", "sar_small", "sar_medium", "sar_large"):
        assert rung not in catalog.apps, f"{rung} is a rung, not an application"
    assert "sar_pss" in catalog.apps
    assert catalog.apps["sar_dist"].restructured_from == "sar_pss"
    # Both rows answer for themselves: each runs a rung of its own, so each
    # carries its own pinned answer.
    for name in ("sar_pss", "sar_dist"):
        assert catalog.apps[name].expect, f"{name} has no pinned answer"


# --- continuing a run -----------------------------------------------------
def _track(run_dir, rows):
    import json as _json

    run_dir.mkdir(parents=True, exist_ok=True)
    (run_dir / "track.jsonl").write_text(
        "\n".join(_json.dumps(r) for r in rows) + "\n"
    )


def test_a_continuation_carries_the_earlier_results_into_the_report(tmp_path):
    # Consensus is a vote across the configurations that ran one application,
    # so a report covering only the cells run after the interruption would be
    # voting with half a ballot.
    from artsrun.campaign import recorded_results
    from artsrun.run.types import Status

    class FakeCell:
        def __init__(self, key):
            self.key = key
            self.log_name = f"{key}.log"
            self.slug = key

    cells = [FakeCell("a"), FakeCell("b"), FakeCell("c")]
    _track(tmp_path, [
        {"event": "finished", "cell": "a", "status": "ok", "rc": 0, "wall_s": 1.5},
        {"event": "finished", "cell": "b", "status": "fail", "rc": 1, "wall_s": 0.5},
        {"event": "submitted", "cell": "c", "status": "submitted"},
    ])
    got = {r.cell.key: r for r in recorded_results(tmp_path, cells)}
    assert set(got) == {"a", "b"}          # "c" never finished
    assert got["a"].status is Status.OK and got["a"].wall_s == 1.5
    assert got["b"].status is Status.FAIL


def test_a_later_attempt_supersedes_the_earlier_one(tmp_path):
    from artsrun.campaign import recorded_results
    from artsrun.run.types import Status

    class FakeCell:
        def __init__(self, key):
            self.key = key
            self.log_name = f"{key}.log"
            self.slug = key

    _track(tmp_path, [
        {"event": "finished", "cell": "a", "status": "fail", "rc": 1},
        {"event": "finished", "cell": "a", "status": "ok", "rc": 0},
    ])
    got = recorded_results(tmp_path, [FakeCell("a")])
    assert len(got) == 1 and got[0].status is Status.OK


def test_a_run_with_no_selection_is_not_offered_for_continuing(tmp_path, monkeypatch):
    # Without it there is nothing to measure against, so it is not a campaign
    # anyone can carry on.
    import artsrun.campaign as mod

    monkeypatch.setattr(mod, "logs_root", lambda: tmp_path)
    (tmp_path / "20260101-000000").mkdir()
    _track(tmp_path / "20260101-000000", [
        {"event": "finished", "cell": "a", "status": "ok"},
    ])
    assert mod.past_runs() == []


def test_a_dry_run_leaves_nothing_under_the_campaign_log_root(tmp_path, monkeypatch):
    # The configurations a dry run renders are what a campaign would write,
    # but they record nothing: a dry run must not leave a campaign directory.
    import artsrun.campaign as mod
    from artsrun import store
    from artsrun.model.catalog import load_catalog
    from artsrun.model.plane import load_plane

    exp = tmp_path / "exp"
    exp.mkdir()
    monkeypatch.setattr(mod, "logs_root", lambda: exp)
    plane, catalog = load_plane(), load_catalog()
    prof = store.load_profile("ferrari-local")
    bs = store.default_experiment()
    selection = Selection(
        profile=prof.name, experiment=bs.name, entries=["arts_val_wb"],
        apps={"nqueens": [Version.BASE]}, node_counts=[1], repeats=1,
    )
    with mod.scratch_run_dir() as scratch:
        campaign = mod.Campaign.prepare(
            selection, plane, catalog, bs, prof,
            build_dir=tmp_path / "build", run_dir=scratch,
        )
        cells, _ = campaign.cells()
        assert cells and (scratch / "cfg").is_dir()
    assert not scratch.exists()
    assert list(exp.iterdir()) == []


def test_flux_requires_its_section():
    with pytest.raises(ValidationError, match="requires a flux section"):
        Profile.model_validate(_local(launcher="flux", ports=[25000]))


def test_flux_profile_requires_ports_like_every_remote_launcher():
    with pytest.raises(ValidationError, match="ports is required"):
        Profile.model_validate(_local(launcher="flux", flux={}))


def test_flux_defaults_state_the_llnl_posture():
    profile = Profile.model_validate(_local(
        launcher="flux", ports=[25000],
        flux={"queue": "pbatch", "bank": "guests"},
    ))
    # mpibind is on by default at the sites this exists for; build work
    # follows the cell queue (a debug queue's short cap kills a build).
    assert profile.flux.mpibind is True
    assert profile.flux.build_cpus == 8
    assert profile.flux.build_queue is None
    assert profile.flux.poll_interval_s == 10.0


def test_sched_settings_reads_the_launchers_own_section():
    # Build sizing and poll cadence go through one accessor, so a consumer
    # wired to profile.slurm cannot silently ignore a flux profile's values.
    slurm = Profile.model_validate(_local(
        launcher="slurm", ports=[25000], slurm={"build_cpus": 4}))
    flux = Profile.model_validate(_local(
        launcher="flux", ports=[25000], flux={"build_cpus": 12}))
    local = Profile.model_validate(_local())
    assert slurm.sched_settings.build_cpus == 4
    assert flux.sched_settings.build_cpus == 12
    assert local.sched_settings is None


def test_arts_only_probe_excludes_every_reference():
    """A probe built for the ARTS variants alone has no reference target: its
    xsocr/ocr-vx cells are structurally ineligible, never a build error."""
    from artsrun.model.catalog import load_catalog
    from artsrun.model.plane import RuntimeKind
    from artsrun.run.plan import _ineligible

    cat = load_catalog()
    flagged = {k for k, a in cat.apps.items() if a.arts_only}
    assert flagged == set(ATTACK_ROWS)
    assert {cat.apps[k].binary for k in flagged} <= set(ATTACK_ROWS.values())

    class _Entry:
        def __init__(self, kind):
            self.kind = kind
            self.model = "OCR"

        @property
        def is_reference(self):
            return self.kind is not RuntimeKind.ARTS

    class _App:
        unsupported = None
        multinode_skip = None
        ocrvx_skip = False
        arts_only = True
        unordered_writes = None
        hpx_binary = None
        hpx_versions = []
        fixtures = []

    for kind in (RuntimeKind.XSOCR, RuntimeKind.OCRVX):
        assert _ineligible(_Entry(kind), _App(), 1) == "probe is built for the ARTS variants alone"
    assert _ineligible(_Entry(RuntimeKind.ARTS), _App(), 1) is None


def test_a_row_outside_db_wrf_is_dropped_on_the_wrf_flush_entry_only():
    from artsrun.run.plan import _ineligible
    plane = load_plane()
    catalog = load_catalog()
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={})
    resolved = {a.key: a for a in bs.resolve(catalog)}
    app = resolved["quicksort:base"]
    assert app.unordered_writes
    assert _ineligible(plane.entry("arts_wrf_flush"), app, 2).startswith("program is outside DB-WRF")
    assert _ineligible(plane.entry("arts_val_wb"), app, 2) is None
    ok = resolved["nqueens:base"]
    assert ok.unordered_writes is None
    assert _ineligible(plane.entry("arts_wrf_flush"), ok, 2) is None


def test_a_restructured_row_carries_the_rewrites_own_unordered_writes():
    """`unordered_writes` is a per-row annotation (base/hinted share one
    program; a rewrite is a separate program with its own), so a restructured
    ResolvedApp must read it from the rewrite the catalog resolved to, not
    from the row head it was dispatched under."""
    from artsrun.run.plan import _ineligible
    plane = load_plane()
    catalog = load_catalog()
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={})
    resolved = {a.key: a for a in bs.resolve(catalog)}

    # fft's base row is annotated; its rewrite (fft_dist) is not.
    assert resolved["fft:base"].unordered_writes
    restructured = resolved["fft:restructured"]
    assert restructured.unordered_writes is None
    assert _ineligible(plane.entry("arts_wrf_flush"), restructured, 2) is None

    # npb_cg's base row is unannotated; its rewrite (npb_cg_dist) is.
    assert resolved["npb_cg:base"].unordered_writes is None
    restructured = resolved["npb_cg:restructured"]
    assert restructured.unordered_writes
    assert _ineligible(plane.entry("arts_wrf_flush"), restructured, 2).startswith(
        "program is outside DB-WRF")


def test_the_two_section_loader_rejects_a_name_in_both_sections():
    """A row is addressed by name alone downstream, so the merge that builds
    the catalog refuses a name two sections both claim rather than letting
    section order decide which one a roster gets."""
    from artsrun.model.catalog import Origin, merge_sections

    ocr = {"twin": {"class": "task", "provenance": "p", "kind": "toy",
                    "marker": "x", "args": []}}
    hpx = {"twin": {"class": "task", "provenance": "p", "kind": "toy",
                    "marker": "x", "args": []}}
    # Either section alone is accepted.
    assert set(merge_sections([(ocr, Origin.OCR)], "/repo")) == {"twin"}
    with pytest.raises(ValueError, match="named in two catalog sections"):
        merge_sections([(ocr, Origin.OCR), (hpx, Origin.HPX)], "/repo")


# --- reference envelopes of absolute cpu ids vs the host's SMT numbering ----
# Two numbering schemes of one 4-core, 2-thread host: sibling-adjacent puts a
# core's threads next to each other (0-1, 2-3, ...), half-split numbers every
# core's first thread before any second one (0,4  1,5  ...).

SIBLING_ADJACENT = {c: f"{c - c % 2}-{c - c % 2 + 1}" for c in range(8)}
HALF_SPLIT = {c: f"{c % 4},{c % 4 + 4}" for c in range(8)}


def _cpu_topology(root, siblings: dict[int, str]):
    for cpu, lst in siblings.items():
        d = root / f"cpu{cpu}" / "topology"
        d.mkdir(parents=True, exist_ok=True)
        (d / "thread_siblings_list").write_text(lst + "\n")
    return root


def _use_topology(monkeypatch, tmp_path, siblings):
    from artsrun.model import selection

    root = _cpu_topology(tmp_path / f"cpu-{len(siblings)}-{id(siblings)}",
                         siblings)
    monkeypatch.setattr(selection, "SYSFS_CPU_ROOT", root, raising=False)


def _reference_fixture():
    from artsrun.model.profile import Launcher, Profile, SlurmSettings

    plane = load_plane()
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={})
    app = {a.key: a for a in bs.resolve(load_catalog())}["nqueens:base"]
    local = Profile(name="p", launcher=Launcher.LOCAL, nodes=[1, 2],
                    workers=2, progress=1)
    remote = Profile(name="p", launcher=Launcher.SLURM, nodes=[1],
                     workers=2, progress=1, ports=[25000],
                     slurm=SlurmSettings())
    return plane, app, local, remote


def _hpx_row():
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={})
    return {a.key: a for a in bs.resolve(load_catalog())}["stencil1d_hpx:base"]


def test_first_sibling_cpus_reads_both_numbering_schemes(tmp_path, monkeypatch):
    from artsrun.model import selection

    _use_topology(monkeypatch, tmp_path, SIBLING_ADJACENT)
    assert selection.first_sibling_cpus() == [0, 2, 4, 6]
    assert selection.siblings_interleaved() is True
    assert selection._first_sibling_cpu_count() == 4

    _use_topology(monkeypatch, tmp_path, HALF_SPLIT)
    assert selection.first_sibling_cpus() == [0, 1, 2, 3]
    assert selection.siblings_interleaved() is False
    assert selection._first_sibling_cpu_count() == 4

    monkeypatch.setattr(selection, "SYSFS_CPU_ROOT", tmp_path / "absent")
    assert selection.first_sibling_cpus() is None
    assert selection.siblings_interleaved() is False


def test_a_reference_is_skipped_where_siblings_interleave(
        tmp_path, monkeypatch):
    from artsrun.run.plan import _ineligible

    plane, app, local, remote = _reference_fixture()
    hpx_app = _hpx_row()
    _use_topology(monkeypatch, tmp_path, SIBLING_ADJACENT)

    for key, row in (("xsocr", app), ("ocrvx", app), ("hpx", hpx_app)):
        for nodes in (1, 2):
            why = _ineligible(plane.entry(key), row, nodes, local)
            assert why is not None and "absolute cpu id" in why, key
        # a remote rank owns a host whose numbering this host cannot see
        assert _ineligible(plane.entry(key), row, 1, remote) is None
    # the runtime under test pins itself from the topology and is untouched
    assert _ineligible(plane.entry("arts_excl_purge"), app, 2, local) is None


def test_a_reference_stays_eligible_on_a_half_split_host(
        tmp_path, monkeypatch):
    from artsrun.run.plan import _ineligible

    plane, app, local, _ = _reference_fixture()
    hpx_app = _hpx_row()
    _use_topology(monkeypatch, tmp_path, HALF_SPLIT)
    for key, row in (("xsocr", app), ("ocrvx", app), ("hpx", hpx_app)):
        for nodes in (1, 2):
            assert _ineligible(plane.entry(key), row, nodes, local) is None


def test_the_build_plan_skips_the_reference_the_expansion_skips(
        tmp_path, monkeypatch):
    from artsrun.build import plan_targets
    from artsrun.model.experiment import ExperimentApp
    from artsrun.model.selection import Selection

    plane, _, local, _ = _reference_fixture()
    sel = Selection(profile="p", experiment="t",
                    entries=["arts_excl_purge", "xsocr"],
                    apps={"nqueens": [Version.BASE]}, node_counts=[1, 2],
                    repeats=1)
    bs = Experiment(entries=["arts_excl_purge"], name="t", apps={"nqueens": ExperimentApp()})

    _use_topology(monkeypatch, tmp_path, SIBLING_ADJACENT)
    adjacent = plan_targets(sel, plane, load_catalog(), bs, tmp_path, local)
    assert adjacent.targets == ["nqueens_arts_ocr_excl_purge"]

    _use_topology(monkeypatch, tmp_path, HALF_SPLIT)
    split = plan_targets(sel, plane, load_catalog(), bs, tmp_path, local)
    assert "nqueens_xsocr" in split.targets
