"""Counter selection: what the runtime defines, what a set turns on, and the
file a build is configured against."""

from __future__ import annotations

import pathlib
import re

import pytest
from pydantic import ValidationError

from artsrun.model.counters import (
    Counterset, CounterSetting, Level, Mode, Reduce, load_counter_catalog,
)
from artsrun.paths import repo_root
from artsrun.render import render_counters


def _plan_targets_stub(*args, **kwargs):
    """Stand in for plan_targets while BINDING its real signature.

    A stub that accepts anything turns a signature change into a green
    run; binding makes the same change a loud TypeError here.
    """
    import inspect

    from artsrun.build import plan_targets

    inspect.signature(plan_targets).bind(*args, **kwargs)
    return "plan"


def test_the_plan_targets_stub_rejects_a_call_the_real_one_would():
    with pytest.raises(TypeError):
        _plan_targets_stub(1, 2, 3, 4, 5, 6, 7, 8, 9)
    with pytest.raises(TypeError):
        _plan_targets_stub(1, 2, 3, 4, 5, nonsense=True)


def test_the_catalog_mirrors_the_runtime_declaration_list():
    # counter.h's X-macro list is the authority; a name only the driver knows
    # would be silently ignored by the build's parser.
    header = (repo_root() /
              "libs/include/internal/arts/counter/counter.h").read_text()
    body = header.split("ARTS_COUNTER_LIST", 1)[1].split("// Generate enum")[0]
    declared = set(re.findall(r"X\(([A-Z0-9_]+)\)", body))
    assert set(load_counter_catalog().names) == declared


def test_the_shipped_default_cfg_mirrors_the_runtime_declaration_list():
    # configs/counters.cfg is the build's own default counter config. The
    # CMake parser silently defaults a name it has never seen to OFF with no
    # diagnostic, so a counter.h addition that never made it into this file
    # is unlistable from the stock cfg and nothing here says so.
    header = (repo_root() /
              "libs/include/internal/arts/counter/counter.h").read_text()
    body = header.split("ARTS_COUNTER_LIST", 1)[1].split("// Generate enum")[0]
    declared = set(re.findall(r"X\(([A-Z0-9_]+)\)", body))

    cfg = (repo_root() / "configs/counters.cfg").read_text()
    listed = set(re.findall(r"^([A-Z][A-Z0-9_]*)=", cfg, re.MULTILINE))

    missing_from_cfg = declared - listed
    stale_in_cfg = listed - declared
    assert not missing_from_cfg, (
        f"counter.h names absent from configs/counters.cfg: "
        f"{sorted(missing_from_cfg)}")
    assert not stale_in_cfg, (
        f"configs/counters.cfg names counter.h no longer declares: "
        f"{sorted(stale_in_cfg)}")


def test_every_counter_states_a_group_and_a_unit():
    for info in load_counter_catalog().counters.values():
        assert info.group
        assert info.unit


def test_a_set_may_not_name_a_counter_the_runtime_lacks():
    with pytest.raises(ValidationError, match="not counters this runtime"):
        Counterset(name="x", counters={"NUM_INVENTED": CounterSetting()})


def test_an_empty_set_compiles_nothing_in():
    assert Counterset(name="none").enabled == []


def test_the_rendered_file_lists_every_counter_including_the_off_ones():
    # The build's parser leaves an unmentioned counter at its own default, so
    # a set states what it turns off as plainly as what it turns on.
    cset = Counterset(name="one", counters={
        "NUM_EDT_FINISH": CounterSetting(mode=Mode.PERIODIC,
                                         level=Level.CLUSTER),
    })
    lines = [l for l in render_counters(cset).splitlines()
             if l and not l.startswith("#")]
    assert len(lines) == len(load_counter_catalog().names)
    assert "NUM_EDT_FINISH=PERIODIC,CLUSTER,SUM" in lines
    assert "NUM_EDT_CREATE=OFF" in lines


def test_the_rendered_syntax_is_what_the_build_parses():
    # Mirrors the regex in libs/include/internal/arts/counter/CMakeLists.txt.
    pattern = re.compile(
        r"^([A-Za-z0-9_]+)[ \t]*=[ \t]*(OFF|ONCE|PERIODIC)"
        r"(,[ \t]*(THREAD|NODE|CLUSTER))?(,[ \t]*(SUM|MAX|MIN|MASTER))?$"
    )
    cset = Counterset(name="mixed", counters={
        "TIME_EDT_EXEC": CounterSetting(mode=Mode.ONCE, level=Level.THREAD,
                                        reduce=Reduce.MAX),
        "NUM_EDT_FINISH": CounterSetting(mode=Mode.PERIODIC,
                                         level=Level.CLUSTER),
    })
    for line in render_counters(cset).splitlines():
        if line and not line.startswith("#"):
            assert pattern.match(line), line


def test_the_shipped_sets_load_and_enable_something():
    from artsrun import store

    for name in store.list_countersets():
        cset = store.load_counterset(name)
        assert cset.enabled or cset.allow_empty, (
            f"{name} turns nothing on and does not say it means to"
        )
        assert cset.capture_interval >= 1


def _tree_with_counters(build_dir, settings):
    """A build tree carrying the header the configure step would generate."""
    header = build_dir / "libs/include/internal/arts/counter/Preamble.h"
    header.parent.mkdir(parents=True, exist_ok=True)
    modes = {"OFF": 0, "ONCE": 1, "PERIODIC": 2}
    levels = {"THREAD": 0, "NODE": 1, "CLUSTER": 2}
    reducers = {"SUM": 0, "MAX": 1, "MIN": 2, "MASTER": 3}
    out = []
    for name, (mode, level, reduce) in settings.items():
        out.append(f"#define ENABLE_{name} {0 if mode == 'OFF' else 1}")
        out.append(f"#define COUNTER_MODE_{name} {modes[mode]}")
        out.append(f"#define COUNTER_LEVEL_{name} {levels[level]}")
        out.append(f"#define COUNTER_REDUCE_{name} {reducers[reduce]}")
    header.write_text("\n".join(out) + "\n")
    return build_dir


def test_what_a_tree_compiled_is_read_from_its_generated_header(tmp_path):
    # Not from the file it was configured against: a set is values that get
    # rendered afresh per campaign, so the file they land in is incidental.
    from artsrun.build import compiled_counters

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    })
    assert compiled_counters(build_dir) == {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    }


def test_a_tree_already_carrying_the_set_needs_no_reconfigure(tmp_path):
    from artsrun.build import counter_mismatch
    from artsrun.model.counters import Counterset, CounterSetting, Level, Mode, Reduce

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
    })
    same = Counterset(name="x", counters={"NUM_EDT_CREATE": CounterSetting(
        mode=Mode.PERIODIC, level=Level.CLUSTER, reduce=Reduce.SUM)})
    assert counter_mismatch(build_dir, same) == []


def test_a_tree_carrying_other_counters_is_named_counter_by_counter(tmp_path):
    # What has to change is a counter, not a path, so that is what is reported.
    from artsrun.build import counter_mismatch
    from artsrun.model.counters import Counterset, CounterSetting, Level, Mode

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    })
    other = Counterset(name="x", counters={
        "NUM_DB_CREATE": CounterSetting(mode=Mode.ONCE, level=Level.NODE),
    })
    # NUM_EDT_CREATE is on in the tree and unnamed by the set; NUM_DB_CREATE
    # differs in mode.  Both have to move.
    assert counter_mismatch(build_dir, other) == ["NUM_DB_CREATE", "NUM_EDT_CREATE"]


def test_the_sampling_interval_reaches_the_runtime_configuration():
    # Interval and folder are runtime keys, so they move without a rebuild.
    from artsrun.render import render_arts
    from artsrun.store import load_profile

    text = render_arts(load_profile("ferrari-local"), 2,
                       counter_folder="/runs/c", capture_interval=250)
    assert "counter_capture_interval=250" in text
    assert "counter_folder=/runs/c" in text


def test_a_set_that_turns_everything_off_names_the_counters_still_on(tmp_path):
    from artsrun.build import counter_mismatch
    from artsrun.model.counters import Counterset

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    })
    empty = Counterset(name="e2e", counters={}, allow_empty=True)
    assert counter_mismatch(build_dir, empty) == ["NUM_EDT_CREATE"]


def test_an_all_off_set_still_reconfigures_a_counting_tree(tmp_path, monkeypatch):
    # Selecting a named empty set pledges the tree to no instrumentation;
    # the reconfigure is what redeems that pledge.  Gating the check on the
    # set enabling something let a timing campaign measure on whatever an
    # earlier campaign left compiled in.
    from types import SimpleNamespace

    import artsrun.campaign as campaign_mod
    from artsrun.campaign import Campaign
    from artsrun.model.counters import Counterset
    from artsrun.model.profile import Launcher

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
    })
    wanted = tmp_path / "cfg" / "counters_e2e.cfg"
    reconfigured = []
    monkeypatch.setattr(campaign_mod, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "write_counter_config", lambda cs, d: wanted)
    monkeypatch.setattr(campaign_mod, "configure_counters",
                        lambda bd, w, **k: reconfigured.append((bd, w)))
    monkeypatch.setattr(campaign_mod, "plan_targets", _plan_targets_stub)

    c = Campaign(
        selection=SimpleNamespace(cxl_entries=lambda _plane: []),
        plane=None, catalog=None, experiment=None,
        profile=SimpleNamespace(launcher=Launcher.LOCAL),
        build_dir=build_dir, run_dir=tmp_path / "run",
        counterset=Counterset(name="e2e", counters={}, allow_empty=True),
    )
    said = []
    assert c.build_plan(on_line=said.append) == "plan"
    # A dry run never writes the tree: it only says what a real run would do.
    assert reconfigured == []
    assert any("a real run reconfigures it first" in line for line in said)
    assert c.build_plan(bootstrap=True) == "plan"
    # The tree is configured from its own copy, never from the run's.
    tree_cfg = (build_dir / "counters" / "counters_e2e.cfg").resolve()
    assert reconfigured == [(build_dir, tree_cfg)]
    assert tree_cfg.is_file()
    # Nothing is on, so there is no counter output to read back through.
    assert c.counters_cfg is None


def test_a_campaign_without_a_set_measures_on_an_uninstrumented_tree(tmp_path):
    # The timing default is no instrumentation at all, so a tree that already
    # says so is exactly the tree a counterless campaign wants.
    from artsrun.build import require_default_counters

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("OFF", "NODE", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    })
    require_default_counters(build_dir)  # no refusal


def test_a_counterless_campaign_refuses_an_instrumented_tree(tmp_path,
                                                             monkeypatch):
    # Instrumentation is compiled in, so a tree an earlier campaign left
    # counting measures every later timing campaign through those counters
    # with nothing said.  The refusal names the file the cache points at,
    # because that is what the reconfigure has to replace.
    from types import SimpleNamespace

    import artsrun.campaign as campaign_mod
    from artsrun.build import BuildError
    from artsrun.campaign import Campaign
    from artsrun.model.profile import Launcher

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
        "NUM_DB_CREATE": ("OFF", "NODE", "SUM"),
    })
    (build_dir / "CMakeCache.txt").write_text(
        "ARTS_COUNTER_CONFIG:FILEPATH=/somewhere/counters_census.cfg\n")
    monkeypatch.setattr(campaign_mod, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "plan_targets", _plan_targets_stub)

    c = Campaign(
        selection=SimpleNamespace(cxl_entries=lambda _plane: []),
        plane=None, catalog=None, experiment=None,
        profile=SimpleNamespace(launcher=Launcher.LOCAL),
        build_dir=build_dir, run_dir=tmp_path / "run", counterset=None,
    )
    with pytest.raises(BuildError) as excinfo:
        c.build_plan()
    msg = str(excinfo.value)
    assert "NUM_EDT_CREATE" in msg
    assert "/somewhere/counters_census.cfg" in msg
    assert "configs/counters_off.cfg" in msg
    assert f"ninja -C {build_dir}" in msg


def test_a_selected_set_the_tree_already_carries_still_runs(tmp_path,
                                                            monkeypatch):
    # The refusal is for campaigns that asked for nothing; -c is unchanged.
    from types import SimpleNamespace

    import artsrun.campaign as campaign_mod
    from artsrun.campaign import Campaign
    from artsrun.model.counters import Counterset, CounterSetting, Level, Mode
    from artsrun.model.profile import Launcher

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
    })
    wanted = tmp_path / "cfg" / "counters_x.cfg"
    reconfigured = []
    monkeypatch.setattr(campaign_mod, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "write_counter_config", lambda cs, d: wanted)
    monkeypatch.setattr(campaign_mod, "configure_counters",
                        lambda bd, w, **k: reconfigured.append((bd, w)))
    monkeypatch.setattr(campaign_mod, "plan_targets", _plan_targets_stub)

    c = Campaign(
        selection=SimpleNamespace(cxl_entries=lambda _plane: []),
        plane=None, catalog=None, experiment=None,
        profile=SimpleNamespace(launcher=Launcher.LOCAL),
        build_dir=build_dir, run_dir=tmp_path / "run",
        counterset=Counterset(name="x", counters={
            "NUM_EDT_CREATE": CounterSetting(mode=Mode.PERIODIC,
                                             level=Level.CLUSTER)}),
    )
    assert c.build_plan() == "plan"
    assert reconfigured == []


def _counted_campaign(tmp_path, monkeypatch, build_dir, counterset):
    """A campaign whose reconfigures are recorded instead of run."""
    from types import SimpleNamespace

    import artsrun.campaign as campaign_mod
    from artsrun.campaign import Campaign
    from artsrun.model.profile import Launcher

    reconfigured = []
    monkeypatch.setattr(campaign_mod, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(campaign_mod, "configure_counters",
                        lambda bd, w, **k: reconfigured.append((bd, w)))
    monkeypatch.setattr(campaign_mod, "plan_targets", _plan_targets_stub)
    c = Campaign(
        selection=SimpleNamespace(cxl_entries=lambda _plane: []),
        plane=None, catalog=None, experiment=None,
        profile=SimpleNamespace(launcher=Launcher.LOCAL),
        build_dir=build_dir, run_dir=tmp_path / "run", counterset=counterset,
    )
    return c, reconfigured


def _census_like():
    from artsrun.model.counters import Counterset, CounterSetting, Level, Mode

    return Counterset(name="x", counters={
        "NUM_EDT_CREATE": CounterSetting(mode=Mode.PERIODIC, level=Level.CLUSTER)})


def test_a_tree_configured_from_a_removed_run_file_is_repointed(tmp_path,
                                                                 monkeypatch):
    # The tree compiles exactly the set, but its cache names a file that
    # lived with an earlier campaign's logs.  The file is an input of the
    # configure, so once it is gone the next build cannot regenerate; the
    # campaign points the tree at its own copy instead of trusting a match.
    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
    })
    gone = tmp_path / "logs/exp/20260925-082512/cfg/counters_x.cfg"
    (build_dir / "CMakeCache.txt").write_text(
        f"ARTS_COUNTER_CONFIG:FILEPATH={gone}\n")
    c, reconfigured = _counted_campaign(tmp_path, monkeypatch, build_dir,
                                        _census_like())
    said = []
    assert c.build_plan(on_line=said.append) == "plan"
    tree_cfg = (build_dir / "counters" / "counters_x.cfg").resolve()
    assert reconfigured == [] and not tree_cfg.exists()
    assert any(str(gone) in line and str(tree_cfg) in line for line in said)
    assert c.build_plan(bootstrap=True) == "plan"
    assert reconfigured == [(build_dir, tree_cfg)]
    from artsrun.render import render_counters
    assert tree_cfg.read_text() == render_counters(_census_like())


def test_a_tree_already_on_its_own_copy_is_left_alone(tmp_path, monkeypatch):
    # Matching counters and the tree's own file: nothing to reconfigure, so
    # nothing that would rewrite the generated header and rebuild everything.
    from artsrun.build import write_tree_counter_config

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("PERIODIC", "CLUSTER", "SUM"),
    })
    tree_cfg = write_tree_counter_config(build_dir, _census_like())
    stamp = tree_cfg.stat().st_mtime_ns
    (build_dir / "CMakeCache.txt").write_text(
        f"ARTS_COUNTER_CONFIG:FILEPATH={tree_cfg}\n")
    c, reconfigured = _counted_campaign(tmp_path, monkeypatch, build_dir,
                                        _census_like())
    assert c.build_plan(bootstrap=True) == "plan"
    assert reconfigured == []
    # Unchanged contents are not rewritten: a newer configure input would
    # make the next build regenerate.
    assert tree_cfg.stat().st_mtime_ns == stamp


def test_a_counterless_campaign_repoints_a_removed_file_to_the_default(
        tmp_path, monkeypatch):
    # An uninstrumented tree whose file is gone takes the build's default,
    # which compiles the same nothing; an instrumented one is still refused.
    from artsrun.paths import repo_root

    build_dir = _tree_with_counters(tmp_path / "build", {
        "NUM_EDT_CREATE": ("OFF", "NODE", "SUM"),
    })
    gone = tmp_path / "logs/exp/old/cfg/counters_e2e.cfg"
    (build_dir / "CMakeCache.txt").write_text(
        f"ARTS_COUNTER_CONFIG:FILEPATH={gone}\n")
    c, reconfigured = _counted_campaign(tmp_path, monkeypatch, build_dir, None)
    said = []
    assert c.build_plan(on_line=said.append) == "plan"
    assert reconfigured == []
    assert any(str(gone) in line for line in said)
    assert c.build_plan(bootstrap=True) == "plan"
    assert reconfigured == [(build_dir, repo_root() / "configs/counters_off.cfg")]


def test_the_shipped_off_cfg_turns_every_declared_counter_off():
    # The CMake default this refusal is written against: the parser leaves an
    # unmentioned counter at its own default, so the file states every one.
    header = (repo_root() /
              "libs/include/internal/arts/counter/counter.h").read_text()
    body = header.split("ARTS_COUNTER_LIST", 1)[1].split("// Generate enum")[0]
    declared = set(re.findall(r"X\(([A-Z0-9_]+)\)", body))

    cfg = (repo_root() / "configs/counters_off.cfg").read_text()
    settings = dict(re.findall(r"^([A-Z][A-Z0-9_]*)=(\S+)$", cfg, re.MULTILINE))
    assert set(settings) == declared
    assert set(settings.values()) == {"OFF"}


def test_the_build_defaults_to_the_all_off_counter_file():
    # A fresh experiment tree measures; the profiling example is opted into.
    text = (repo_root() / "CMakeLists.txt").read_text()
    block = text.split("set(ARTS_COUNTER_CONFIG", 1)[1].split(")", 1)[0]
    assert "configs/counters_off.cfg" in block
