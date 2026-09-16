"""Fixtures the tool synthesizes: generated when missing, reported when not."""

from __future__ import annotations

from artsrun.fixtures import stage


def test_a_known_fixture_is_generated_and_an_unknown_one_reported(tmp_path):
    known = tmp_path / "score-gate.txt"
    unknown = tmp_path / "mystery.bin"
    left = stage([str(known), str(unknown)])
    assert left == [str(unknown)]
    assert known.read_text() == "1176\n"
    assert not list(tmp_path.glob("*.staging"))


def test_an_existing_file_is_never_rewritten(tmp_path):
    fixture = tmp_path / "basicIO_test.dat"
    fixture.write_text("sentinel")
    assert stage([str(fixture)]) == []
    assert fixture.read_text() == "sentinel"
