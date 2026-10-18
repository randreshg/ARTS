"""A tree that targets a processor other than AMD64 has no benchmark tree, so
a campaign on it stops before it configures or builds anything, with one
message naming the tree's processor and every entry it selected."""

from pathlib import Path

import pytest

from artsrun.build import AMD64_PROCESSOR, BuildError, tree_processor
from artsrun.model.catalog import load_catalog
from artsrun.model.experiment import Experiment
from artsrun.model.plane import load_plane
from artsrun.model.profile import Profile
from artsrun.model.selection import Selection

ENTRIES = ["arts_excl_purge", "arts_excl_purge_cxl_staged",
           "arts_excl_purge_cxl_direct"]


def _cache_tree(tree: Path, processor: str | None) -> None:
    """A configured native tree: the cache names no processor, and CMake's
    record of the tree's system does."""
    tree.mkdir(parents=True, exist_ok=True)
    (tree / "CMakeCache.txt").write_text(
        "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3\n"
        "CMAKE_CACHE_MINOR_VERSION:INTERNAL=31\n"
        "CMAKE_CACHE_PATCH_VERSION:INTERNAL=6\n"
        + (f"CMAKE_SYSTEM_PROCESSOR:STRING={processor}\n" if processor else ""))


def _named_in_cache(tree: Path, monkeypatch) -> None:
    _cache_tree(tree, "aarch64")
    monkeypatch.setattr("artsrun.build.platform.machine", lambda: "x86_64")


def _recorded_by_cmake(tree: Path, monkeypatch) -> None:
    _cache_tree(tree, None)
    # CMake names the directory by its full version, a release candidate's
    # suffix included.
    system = tree / "CMakeFiles" / "3.31.6-rc2" / "CMakeSystem.cmake"
    system.parent.mkdir(parents=True)
    system.write_text('set(CMAKE_HOST_SYSTEM_PROCESSOR "x86_64")\n'
                      'set(CMAKE_SYSTEM_PROCESSOR "aarch64")\n')
    monkeypatch.setattr("artsrun.build.platform.machine", lambda: "x86_64")


def _host_of_an_unconfigured_tree(tree: Path, monkeypatch) -> None:
    monkeypatch.setattr("artsrun.build.platform.machine", lambda: "aarch64")


def _campaign(tree: Path):
    from artsrun.campaign import Campaign

    prof = Profile(name="p", launcher="local", nodes=[1], workers=2,
                   progress=1)
    sel = Selection(profile="p", experiment="b", default_entries=[],
                    entries=list(ENTRIES), apps={"nqueens": ["base"]},
                    node_counts=[1])
    return Campaign(selection=sel, plane=load_plane(), catalog=load_catalog(),
                    experiment=Experiment(entries=["arts_excl_purge"],
                                          name="b", apps={}),
                    profile=prof, build_dir=tree, run_dir=tree / "run")


@pytest.mark.parametrize("source", [_named_in_cache, _recorded_by_cmake,
                                    _host_of_an_unconfigured_tree])
@pytest.mark.parametrize("bootstrap", [False, True])
def test_a_non_amd64_tree_stops_the_campaign_before_any_build(
        tmp_path, monkeypatch, source, bootstrap):
    tree = tmp_path / "build"
    source(tree, monkeypatch)
    assert tree_processor(tree) == "aarch64"
    for name in ("ensure_build_dir", "configure_sections", "configure_cxl",
                 "configure_counters", "plan_targets"):
        monkeypatch.setattr(f"artsrun.campaign.{name}",
                            lambda *a, _n=name, **k: pytest.fail(
                                f"{_n} ran on a tree with no benchmark variant"))
    with pytest.raises(BuildError) as exc:
        _campaign(tree).build_plan(bootstrap=bootstrap)
    message = str(exc.value)
    assert f"{tree} targets aarch64" in message
    assert "this tree has no benchmark variant" in message
    assert f"({', '.join(ENTRIES)})" in message


def test_an_amd64_tree_passes_the_processor_check(tmp_path, monkeypatch):
    tree = tmp_path / "build"
    _cache_tree(tree, None)
    monkeypatch.setattr("artsrun.build.platform.machine", lambda: "x86_64")
    assert tree_processor(tree) == "x86_64"
    planned = []
    monkeypatch.setattr("artsrun.campaign.ensure_build_dir",
                        lambda *a, **k: planned.append("ensure"))
    monkeypatch.setattr("artsrun.campaign.Campaign._match_cxl",
                        lambda *a, **k: None)
    monkeypatch.setattr("artsrun.campaign.require_default_counters",
                        lambda *a, **k: None)
    monkeypatch.setattr("artsrun.campaign.missing_counter_config",
                        lambda *a, **k: None)
    monkeypatch.setattr("artsrun.campaign.plan_targets",
                        lambda *a, **k: "plan")
    assert _campaign(tree).build_plan() == "plan"
    assert planned == ["ensure"]


def test_the_amd64_expression_is_the_one_cmake_applies():
    import re

    from artsrun.paths import repo_root

    text = (repo_root() / "CMakeLists.txt").read_text()
    m = re.search(r'if\(CMAKE_SYSTEM_PROCESSOR MATCHES "([^"]+)"\)\s*\n'
                  r'\s*set\(ARTS_TARGET_AMD64 ON\)', text)
    assert m is not None
    assert m.group(1) == AMD64_PROCESSOR.pattern
