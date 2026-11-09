from __future__ import annotations

from artsrun.run.retry import (MAX_RETRIES, MeshLine, is_mesh_failure,
                               map_suspects, mesh_lines, mesh_note, suspects)


def L(rank, host, phase, missing="-", hosts="-", more=0):
    return (f"ARTS_MESH_FAILED rank={rank} host={host} phase={phase} "
            f"missing={missing} missing_hosts={hosts} more={more}")


def test_the_retry_budget_default_is_two():
    assert MAX_RETRIES == 2


def test_a_line_parses_into_its_fields():
    [m] = mesh_lines(L(1, "n1.site", "connect", "2,3", "n2,n3"))
    assert m == MeshLine(1, "n1.site", "connect", (2, 3), ("n2", "n3"), 0)


def test_a_line_behind_partial_stdout_is_found_and_garbage_ignored():
    text = "partial output" + L("-", "n4", "config") + "\nARTS_MESH_FAILED rank=x\n"
    assert [m.rank for m in mesh_lines(text)] == [None]


def test_hangup_only_is_not_a_mesh_failure():
    assert not is_mesh_failure(L(0, "n0", "hangup", "1", "n1"))
    assert is_mesh_failure(L(0, "n0", "hangup", "1", "n1") + "\n" +
                           L(2, "n2", "exchange", "1", "n1"))


def test_a_log_without_a_line_is_not_a_mesh_failure():
    assert not is_mesh_failure("[ERROR] something\n")


def test_connect_lines_vote_for_the_missing_host():
    lines = mesh_lines("\n".join(L(r, f"n{r}", "connect", "3", "n3")
                                 for r in range(3)))
    assert suspects(lines, nodes=4) == ["n3"]


def test_one_cause_line_among_hangups_still_names_the_host():
    text = "\n".join([L(0, "n0", "connect", "3", "n3"),
                      L(1, "n1", "hangup", "0", "n0"),
                      L(2, "n2", "hangup", "0", "n0")])
    assert suspects(mesh_lines(text), nodes=4) == ["n3"]


def test_an_isolated_reporter_votes_for_itself():
    lines = mesh_lines(L(3, "n3.a.b", "connect", "0,1,2", "n0,n1,n2"))
    assert suspects(lines, nodes=4) == ["n3"]


def test_names_compare_by_first_label_and_addresses_whole():
    lines = mesh_lines(L(0, "n0.site", "connect", "3", "n3.site") + "\n" +
                       L(3, "n3", "listen"))
    assert suspects(lines, nodes=4) == ["n3"]
    lines = mesh_lines(L(0, "10.9.0.1", "connect", "3", "10.9.0.4"))
    assert suspects(lines, nodes=4) == ["10.9.0.4"]


def test_a_line_without_listed_ranks_votes_for_its_reporter():
    for phase in ("config", "listen", "fabric", "connect", "exchange"):
        assert suspects(mesh_lines(L(5, "n5", phase)), nodes=8) == ["n5"]


def test_a_resolve_line_votes_for_the_names_it_could_not_resolve():
    lines = mesh_lines(L("-", "n1", "resolve", "3", "bad3") + "\n" +
                       L("-", "n2", "resolve", "3", "bad3"))
    assert suspects(lines, nodes=4) == ["bad3"]


def test_more_unlisted_ranks_count_toward_the_isolation_rule():
    lines = mesh_lines(L(0, "n0", "connect", "1", "n1", more=20))
    assert suspects(lines, nodes=32) == ["n0"]


def test_a_tie_over_every_reporter_singles_out_nobody():
    lines = mesh_lines("\n".join(L(r, f"n{r}", "listen") for r in range(4)))
    assert suspects(lines, nodes=4) == []


def test_a_tie_among_hosts_names_them_all():
    lines = mesh_lines(L(0, "n0", "connect", "2", "n2") + "\n" +
                       L(1, "n1", "connect", "3", "n3"))
    assert suspects(lines, nodes=4) == ["n2", "n3"]


def test_a_suspect_no_table_name_matches_is_dropped():
    lines = mesh_lines(L("-", "laptop", "config") + "\n" +
                       L(0, "n0", "connect", "2", "n2.site"))
    assert map_suspects(["laptop", "n2"], lines) == ["n2.site"]


def test_an_own_host_suspect_with_a_known_rank_is_kept():
    lines = mesh_lines(L(5, "n5.site", "listen"))
    assert map_suspects(suspects(lines, nodes=8), lines) == ["n5.site"]


def test_the_note_names_phases_and_suspects():
    lines = mesh_lines(L(0, "n0", "connect", "3", "n3") + "\n" +
                       L(3, "n3", "listen"))
    assert mesh_note(lines, 4) == "mesh failure (listen,connect): suspect hosts n3"


def test_the_note_says_none_when_nobody_is_singled_out():
    lines = mesh_lines("\n".join(L(r, f"n{r}", "listen") for r in range(3)))
    assert mesh_note(lines, 3) == "mesh failure (listen): suspect hosts none"


FLUX_HOSTLIST_HANG = "\n".join([
    "ARTS_MESH_FAILED rank=3 host=10.9.0.4 phase=connect missing=2 "
    "missing_hosts=10.9.0.3 more=0",
    "ARTS_MESH_FAILED rank=0 host=10.9.0.1 phase=hangup missing=3 "
    "missing_hosts=10.9.0.4 more=0",
    "ARTS_MESH_FAILED rank=1 host=10.9.0.2 phase=hangup missing=3 "
    "missing_hosts=10.9.0.4 more=0",
    "ARTS_MESH_FAILED rank=- host=Alia phase=config missing=- "
    "missing_hosts=- more=0",
])


def test_a_uname_name_ties_the_vote_and_only_the_mapping_drops_it():
    lines = mesh_lines(FLUX_HOSTLIST_HANG)
    voted = suspects(lines, nodes=4)
    assert voted == ["10.9.0.3", "Alia"]
    assert map_suspects(voted, lines) == ["10.9.0.3"]
    assert mesh_note(lines, 4) == (
        "mesh failure (config,connect): suspect hosts 10.9.0.3")


def test_a_hangup_reporter_keeps_a_tie_from_covering_every_reporter():
    lines = mesh_lines("\n".join([L(0, "n0", "listen"), L(1, "n1", "listen"),
                                  L(2, "n2", "hangup", "0", "n0")]))
    assert suspects(lines, nodes=3) == ["n0", "n1"]


def test_isolation_needs_more_than_one_missing_and_over_half_the_peers():
    # five nodes: four peers, half is two
    one = L(0, "n0", "connect", "3", "n3")
    two = L(0, "n0", "connect", "3,4", "n3,n4")
    three = L(0, "n0", "connect", "2,3,4", "n2,n3,n4")
    assert suspects(mesh_lines(one), nodes=5) == ["n3"]
    assert suspects(mesh_lines(two), nodes=5) == ["n3", "n4"]
    assert suspects(mesh_lines(three), nodes=5) == ["n0"]
    # the unlisted count adds to the listed one
    more = L(0, "n0", "connect", "3", "n3", more=1)
    assert suspects(mesh_lines(more), nodes=5) == ["n3"]
    assert suspects(mesh_lines(more), nodes=4) == ["n0"]
    # one peer of one is never isolation
    assert suspects(mesh_lines(L(0, "n0", "connect", "1", "n1")),
                    nodes=2) == ["n1"]


def test_host_names_compare_without_case_and_keep_the_logs_spelling():
    lines = mesh_lines(L(0, "n0", "connect", "3", "N3.site") + "\n" +
                       L(1, "n1", "connect", "3", "n3.site"))
    assert suspects(lines, nodes=4) == ["N3"]
    assert map_suspects(["N3"], lines) == ["N3.site"]


def test_a_line_with_an_unknown_phase_is_not_a_cause():
    text = L(0, "n0", "warp", "1", "n1")
    assert mesh_lines(text) == []
    assert not is_mesh_failure(text)


# --- attempts kept on disk, and the relaunch decision ---------------------
import dataclasses  # noqa: E402
from pathlib import Path  # noqa: E402

import pytest  # noqa: E402

from artsrun.model.catalog import AppClass, Version  # noqa: E402
from artsrun.model.experiment import ResolvedApp  # noqa: E402
from artsrun.model.plane import RuntimeKind, SelectionEntry  # noqa: E402
from artsrun.run.retry import (MeshRetry, attempt_path, keep,  # noqa: E402
                               kept_attempts, resolve_mesh_retries, restore,
                               supersede)
from artsrun.run.types import Cell, CellResult, Status  # noqa: E402


def _cell(nodes: int = 4, kind: RuntimeKind = RuntimeKind.ARTS) -> Cell:
    app = ResolvedApp(name="app", version=Version.BASE, binary="app",
                      cls=AppClass.TASK, marker="X", scalar_re="X")
    entry = SelectionEntry(key="e", label="e", kind=kind, cell="VAL/RETAIN/OWNER",
                           variant="e")
    return Cell(entry=entry, app=app, nodes=nodes, repeat=1,
                binary=Path("/nonexistent"), args=[], timeout_s=5, cpu_width=4)


def _attempt(tmp_path, cell, text, rc=75, counters=True):
    """Leave the files one finished attempt of `cell` leaves."""
    logs = tmp_path / "cells"
    logs.mkdir(exist_ok=True)
    (logs / f"{cell.slug}.log").write_text(text)
    (logs / f"{cell.slug}.rc").write_text(f"{rc} 1.0 2.0\n")
    (logs / f"{cell.slug}.sh").write_text("#!/bin/bash\n")
    if counters:
        folder = tmp_path / "counters" / cell.slug
        folder.mkdir(parents=True, exist_ok=True)
        (folder / "n0.json").write_text("{}")
    return logs, tmp_path / "counters"


def _result(cell, tmp_path, status=Status.FAIL) -> CellResult:
    return CellResult(cell=cell, status=status, rc=75,
                      log_path=tmp_path / "cells" / f"{cell.slug}.log")


def test_the_kept_name_follows_the_slug():
    assert attempt_path(Path("/l"), "s.1n.r1", 2, ".log") == Path(
        "/l/s.1n.r1.attempt2.log")


def test_keep_moves_every_file_and_the_counter_folder(tmp_path):
    cell = _cell()
    logs, counters = _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    kept = keep(logs, counters, cell.slug, 1)
    assert kept == logs / f"{cell.slug}.attempt1.log"
    for suffix in (".log", ".rc", ".sh"):
        assert (logs / f"{cell.slug}.attempt1{suffix}").is_file()
        assert not (logs / f"{cell.slug}{suffix}").exists()
    assert (counters / f"{cell.slug}.attempt1" / "n0.json").is_file()
    assert not (counters / cell.slug).exists()
    assert kept_attempts(logs, cell.slug) == 1


def test_keep_without_a_counter_folder_or_root_is_fine(tmp_path):
    cell = _cell()
    logs, _ = _attempt(tmp_path, cell, "x", counters=False)
    assert keep(logs, None, cell.slug, 1).is_file()


def _hook(tmp_path, cell, text, **kw):
    logs, counters = _attempt(tmp_path, cell, text)
    return MeshRetry(logs, counters, **kw), logs, counters


def test_mesh_retry_relaunches_twice_then_gives_up(tmp_path):
    cell = _cell()
    hook, logs, counters = _hook(tmp_path, cell,
                                 L(1, "n1", "connect", "3", "n3"))
    first = hook(_result(cell, tmp_path))
    assert (first.attempt, first.exclude) == (2, ("n3",))
    assert first.key == cell.key and first.slug == cell.slug
    _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    second = hook(_result(first, tmp_path))
    assert (second.attempt, second.exclude) == (3, ("n3",))
    _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    assert hook(_result(second, tmp_path)) is None


def test_a_relaunch_points_the_result_at_the_kept_log(tmp_path):
    cell = _cell()
    hook, logs, _ = _hook(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    result = _result(cell, tmp_path)
    hook(result)
    assert result.log_path == logs / f"{cell.slug}.attempt1.log"
    assert result.extra["attempt"] == "2"
    assert result.extra["suspects"] == "n3"
    assert result.extra["exclude"] == "n3"


def test_mesh_retry_with_zero_relaunches_nothing(tmp_path):
    cell = _cell()
    hook, logs, _ = _hook(tmp_path, cell, L(1, "n1", "connect", "3", "n3"),
                          max_retries=0)
    assert hook(_result(cell, tmp_path)) is None
    assert (logs / f"{cell.slug}.log").is_file()


def test_a_program_failure_is_never_relaunched(tmp_path):
    cell = _cell()
    hook, logs, _ = _hook(tmp_path, cell, "[ERROR] boom\n")
    assert hook(_result(cell, tmp_path)) is None
    assert (logs / f"{cell.slug}.log").is_file()


def test_a_reference_cell_is_never_relaunched(tmp_path):
    cell = _cell(kind=RuntimeKind.XSOCR)
    hook, _, _ = _hook(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    assert hook(_result(cell, tmp_path)) is None


def test_a_tie_relaunches_in_place(tmp_path):
    cell = _cell()
    text = "\n".join(L(r, f"n{r}", "listen") for r in range(4))
    hook, _, _ = _hook(tmp_path, cell, text)
    again = hook(_result(cell, tmp_path))
    assert (again.attempt, again.exclude) == (2, ())


def test_a_suspect_the_log_never_named_is_not_excluded(tmp_path):
    cell = _cell()
    text = L("-", "laptop", "config") + "\n" + L(0, "n0", "connect", "2", "n2")
    hook, _, _ = _hook(tmp_path, cell, text)
    assert hook(_result(cell, tmp_path)).exclude == ("n2",)


def test_restore_rebuilds_numbering_and_exclusions_from_disk(tmp_path):
    cell = _cell()
    logs, counters = _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    keep(logs, counters, cell.slug, 1)
    _attempt(tmp_path, cell, L(1, "n1", "connect", "5", "n5"))
    keep(logs, counters, cell.slug, 2)
    again = restore(logs, cell)
    assert (again.attempt, again.exclude) == (3, ("n3", "n5"))


def test_restore_ignores_a_kept_program_failure(tmp_path):
    cell = _cell()
    logs, counters = _attempt(tmp_path, cell, "[ERROR] boom\n")
    keep(logs, counters, cell.slug, 1)
    again = restore(logs, cell)
    assert (again.attempt, again.exclude) == (2, ())


def test_supersede_keeps_what_an_earlier_session_left(tmp_path):
    cell = _cell()
    logs, counters = _attempt(tmp_path, cell, "[ERROR] boom\n")
    assert supersede(logs, counters, cell) == cell
    assert kept_attempts(logs, cell.slug) == 1
    assert not (logs / f"{cell.slug}.log").exists()
    assert restore(logs, cell).attempt == 2


def test_a_resumed_cell_gets_a_fresh_budget(tmp_path):
    cell = dataclasses.replace(_cell(), attempt=3)
    hook, _, _ = _hook(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    four = hook(_result(cell, tmp_path))
    assert four.attempt == 4
    _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    five = hook(_result(four, tmp_path))
    assert five.attempt == 5
    _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    assert hook(_result(five, tmp_path)) is None


def test_resolve_mesh_retries():
    assert resolve_mesh_retries(None, None) == MAX_RETRIES
    assert resolve_mesh_retries(0, None) == 0
    assert resolve_mesh_retries(None, 1) == 1
    assert resolve_mesh_retries(1, 1) == 1
    with pytest.raises(ValueError):
        resolve_mesh_retries(3, 1)
    with pytest.raises(ValueError):
        resolve_mesh_retries(-1, None)


def test_restore_for_a_launcher_that_cannot_exclude_keeps_no_exclusions(
        tmp_path):
    cell = _cell()
    logs, counters = _attempt(tmp_path, cell, L(1, "n1", "connect", "3", "n3"))
    keep(logs, counters, cell.slug, 1)
    again = restore(logs, cell, excluding=False)
    assert (again.attempt, again.exclude) == (2, ())


def test_a_slug_with_glob_characters_finds_only_its_own_attempts(tmp_path):
    cell = _cell()
    slug = cell.slug + "[1]"
    (tmp_path / f"{slug}.attempt2.log").write_text("x")
    (tmp_path / f"{cell.slug}1.attempt7.log").write_text("x")
    assert kept_attempts(tmp_path, slug) == 2
