"""The node profiles every test sees: the shipped ones plus the tests' own
(tests/profiles/), copied once per session, so a test never depends on one
site's profile and never writes a tracked one."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

_OWN_PROFILES = Path(__file__).parent / "profiles"


@pytest.fixture(scope="session")
def _profile_root(tmp_path_factory) -> Path:
    from artsrun.paths import profiles_dir

    root = tmp_path_factory.mktemp("profiles")
    for src in (*profiles_dir().glob("*.yaml"), *_OWN_PROFILES.glob("*.yaml")):
        shutil.copy(src, root / src.name)
    return root


@pytest.fixture(autouse=True)
def _profiles(_profile_root, monkeypatch):
    monkeypatch.setattr("artsrun.store.profiles_dir", lambda: _profile_root)
