import ast
import json
import subprocess
import sys
from pathlib import Path

import pytest

from artsrun.reduce import (MixedRunError, ReduceError, reduce_folder,
                            write_record)


def put(folder: Path, rank: int, counters: dict, *, run="00000000000000aa",
        sync="rtt", **meta):
    doc = {"metadata": {"rank": rank, "run": run, "clock_sync": sync,
                        "t0_realtime_ns": 1000 + rank * 7,
                        "t0_synced_ns": 500 + rank * 3, **meta},
           "counters": counters}
    (folder / f"n{rank}.json").write_text(json.dumps(doc))


def c(value, history=None, method="SUM"):
    out = {"captureMode": "ONCE", "reduce_method": method, "value": value}
    if history is not None:
        out["captureHistory"] = history
    return out


METHODS = {"NUM_EDT_CREATE": "SUM", "NUM_EDT_FINISH": "MAX",
           "NUM_DB_CREATE": "MIN", "NUM_REMOTE_SEND": "MASTER"}


def test_each_reduce_method_tells_master_from_min(tmp_path):
    # Rank 0 holds the larger value, so MASTER (5) differs from MIN (3).
    put(tmp_path, 0, {n: c(5, method=m) for n, m in METHODS.items()})
    put(tmp_path, 1, {n: c(3, method=m) for n, m in METHODS.items()})
    got = reduce_folder(tmp_path)["counters"]
    assert got["NUM_EDT_CREATE"]["value"] == 8
    assert got["NUM_EDT_FINISH"]["value"] == 5
    assert got["NUM_DB_CREATE"]["value"] == 3
    assert got["NUM_REMOTE_SEND"]["value"] == 5


def test_min_and_master_histories(tmp_path):
    names = {"NUM_DB_CREATE": "MIN", "NUM_REMOTE_SEND": "MASTER"}
    for rank, h in ((0, [[0, 9], [1, 8]]), (1, [[0, 4], [1, 7], [2, 1]])):
        put(tmp_path, rank, {n: c(0, h, m) for n, m in names.items()})
    got = reduce_folder(tmp_path)["counters"]
    assert got["NUM_DB_CREATE"]["captureHistory"] == [[0, 4], [1, 7], [2, 1]]
    assert got["NUM_REMOTE_SEND"]["captureHistory"] == [[0, 9], [1, 8]]


def test_an_entry_without_a_method_is_refused(tmp_path):
    entry = c(1)
    del entry["reduce_method"]
    put(tmp_path, 0, {"X": entry})
    with pytest.raises(ReduceError, match="X in rank 0.*no reduce_method"):
        reduce_folder(tmp_path)


def test_an_unknown_method_is_refused(tmp_path):
    put(tmp_path, 0, {"X": c(1, method="AVG")})
    with pytest.raises(ReduceError, match="reduce_method 'AVG'"):
        reduce_folder(tmp_path)


def test_ranks_that_disagree_on_a_method_are_refused(tmp_path):
    put(tmp_path, 0, {"X": c(1, method="SUM")})
    put(tmp_path, 1, {"X": c(1, method="MAX")})
    with pytest.raises(ReduceError, match="disagree.*X.*rank 0 SUM"):
        reduce_folder(tmp_path)


def test_master_without_rank_zero_is_null_and_flagged(tmp_path):
    put(tmp_path, 1, {"X": c(7, [[0, 7]], method="MASTER")})
    got = reduce_folder(tmp_path, nodes=2)
    assert got["missing"] == [0]
    assert got["origin"] is None
    x = got["counters"]["X"]
    assert x["value"] is None and x["partial"] is True
    assert x["captureHistory"] == []


def test_pre_arm_rank_zero_counts_as_missing(tmp_path):
    put(tmp_path, 0, {"X": c(1)}, run="")
    put(tmp_path, 1, {"X": c(2)})
    got = reduce_folder(tmp_path, nodes=2)
    assert got["pre_arm"] == ["n0.json"] and got["missing"] == [0]


def test_a_missing_highest_rank_is_reported_and_partial(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    got = reduce_folder(tmp_path, nodes=2)
    assert got["missing"] == [1]
    assert got["counters"]["X"]["partial"] is True
    full = reduce_folder(tmp_path, nodes=1)
    assert full["missing"] == [] and "partial" not in full["counters"]["X"]


def test_truncated_file_is_a_named_refusal(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    (tmp_path / "n1.json").write_text('{"metadata": {"run": "a')
    with pytest.raises(ReduceError, match="n1.json is unreadable"):
        reduce_folder(tmp_path)


def test_a_non_counter_document_is_refused(tmp_path):
    (tmp_path / "n0.json").write_text("[1, 2]")
    with pytest.raises(ReduceError, match="n0.json is not a counter file"):
        reduce_folder(tmp_path)


def test_file_name_and_stated_rank_must_agree(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    (tmp_path / "n3.json").write_text((tmp_path / "n0.json").read_text())
    with pytest.raises(ReduceError, match="states rank 0"):
        reduce_folder(tmp_path)


def test_a_file_without_a_stated_rank_is_accepted(tmp_path):
    doc = {"metadata": {"run": "aa"}, "counters": {"X": c(1)}}
    (tmp_path / "n0.json").write_text(json.dumps(doc))
    assert reduce_folder(tmp_path)["ranks"] == [0]


def test_a_refusal_removes_a_stale_record(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    write_record(tmp_path)
    assert (tmp_path / "c.json").is_file()
    (tmp_path / "n1.json").write_text("{")
    with pytest.raises(ReduceError):
        write_record(tmp_path)
    assert not (tmp_path / "c.json").exists()


def test_module_refuses_a_truncated_file_with_status_2(tmp_path):
    (tmp_path / "n0.json").write_text("{")
    p = subprocess.run([sys.executable, "-m", "artsrun.reduce",
                        str(tmp_path)], capture_output=True, text=True)
    assert p.returncode == 2 and "unreadable" in p.stderr


def test_the_campaign_hook_reports_a_refusal_and_never_raises(tmp_path):
    from types import SimpleNamespace

    from artsrun.campaign import Campaign

    folder = tmp_path / "counters" / "cellA"
    folder.mkdir(parents=True)
    put(folder, 0, {"X": c(1)})
    (folder / "n1.json").write_text("{")
    camp = Campaign.__new__(Campaign)
    camp.run_dir = tmp_path
    camp.counterset = SimpleNamespace(enabled=["NUM_EDT_CREATE"])
    cell = SimpleNamespace(slug="cellA", key="k", nodes=2)
    said = []
    camp._reduce_counters(cell, said.append)
    assert said and "unreadable" in said[0]
    (folder / "n1.json").unlink()
    camp._reduce_counters(cell, said.append)
    rec = json.loads((folder / "c.json").read_text())
    assert rec["missing"] == [1]


def test_cli_reduce_uses_the_manifest_and_bare_counters_still_lists(tmp_path):
    from typer.testing import CliRunner

    from artsrun.cli import app

    folder = tmp_path / "counters" / "cellA"
    folder.mkdir(parents=True)
    put(folder, 0, {"NUM_EDT_CREATE": c(1, method="MAX")})
    (tmp_path / "manifest.json").write_text(json.dumps({
        "cells": [{"slug": "cellA", "nodes": 2}]}))
    runner = CliRunner()
    out = runner.invoke(app, ["counters", "reduce", str(tmp_path)])
    assert out.exit_code == 0 and "missing [1]" in out.output
    rec = json.loads((folder / "c.json").read_text())
    assert rec["missing"] == [1] and rec["counters"]["NUM_EDT_CREATE"]["reduce"] == "MAX"
    (folder / "n0.json").write_text("{")
    out = runner.invoke(app, ["counters", "reduce", str(tmp_path)])
    assert out.exit_code == 2 and "unreadable" in out.output
    assert not (folder / "c.json").exists()
    out = runner.invoke(app, ["counters", "--enabled"])
    assert out.exit_code == 0 and "Counters" in out.output


def test_the_files_method_is_the_reduction(tmp_path):
    put(tmp_path, 0, {"X": c(3, method="MAX")})
    put(tmp_path, 1, {"X": c(9, method="MAX")})
    assert reduce_folder(tmp_path)["counters"]["X"]["value"] == 9


def test_epochs_merge_with_a_missing_epoch(tmp_path):
    put(tmp_path, 0, {"X": c(0, [[0, 1], [1, 2], [2, 4]])})
    put(tmp_path, 1, {"X": c(0, [[0, 10], [2, 40]])})
    hist = reduce_folder(tmp_path)["counters"]["X"]["captureHistory"]
    assert hist == [[0, 11], [1, 2], [2, 44]]


def test_missing_rank_is_reported(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    put(tmp_path, 2, {"X": c(1)})
    got = reduce_folder(tmp_path)
    assert got["ranks"] == [0, 2] and got["missing"] == [1]


def test_mixed_run_ids_refused(tmp_path):
    put(tmp_path, 0, {"X": c(1)}, run="00000000000000aa")
    put(tmp_path, 1, {"X": c(1)}, run="00000000000000bb")
    with pytest.raises(MixedRunError, match="mixed run ids.*aa.*bb"):
        reduce_folder(tmp_path)


def test_file_without_a_run_id_is_pre_arm_not_mixed(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    put(tmp_path, 1, {"X": c(7)}, run="")
    got = reduce_folder(tmp_path)
    assert got["pre_arm"] == ["n1.json"]
    assert got["ranks"] == [0]
    assert got["counters"]["X"]["value"] == 1


def test_thread_files_are_not_read(tmp_path):
    put(tmp_path, 0, {"X": c(1)})
    (tmp_path / "n0_t3.json").write_text("not json")
    assert reduce_folder(tmp_path)["ranks"] == [0]


def test_offsets_synced_only_when_every_clock_is_rtt(tmp_path):
    put(tmp_path, 0, {"X": c(1)}, origin="shutdown")
    put(tmp_path, 1, {"X": c(1)})
    got = reduce_folder(tmp_path)
    assert got["synced"] is True
    assert got["t0_offset_ns"] == {"0": 0, "1": 3}
    assert got["origin"] == "shutdown"
    put(tmp_path, 1, {"X": c(1)}, sync="timeout")
    got = reduce_folder(tmp_path)
    assert got["synced"] is False
    assert got["t0_offset_ns"] == {"0": 0, "1": 7}


def test_module_refuses_mixed_runs_with_status_2(tmp_path):
    put(tmp_path, 0, {"X": c(1)}, run="00000000000000aa")
    put(tmp_path, 1, {"X": c(1)}, run="00000000000000bb")
    p = subprocess.run([sys.executable, "-m", "artsrun.reduce",
                        str(tmp_path)], capture_output=True, text=True)
    assert p.returncode == 2 and "mixed run ids" in p.stderr


def test_the_reducer_imports_the_standard_library_alone():
    # It runs on whatever Python 3 a build tree found, which need not have
    # artsrun's dependencies, so it imports no other part of artsrun either.
    import artsrun.reduce as reducer
    tree = ast.parse(Path(reducer.__file__).read_text(encoding="utf-8"))
    roots = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            roots |= {a.name.split(".")[0] for a in node.names}
        elif isinstance(node, ast.ImportFrom):
            roots.add((node.module or "").split(".")[0])
    assert roots <= set(sys.stdlib_module_names) | {"__future__"}, roots


def test_write_record_writes_c_json(tmp_path):
    put(tmp_path, 0, {"X": c(4)})
    write_record(tmp_path)
    assert json.loads((tmp_path / "c.json").read_text())["counters"]["X"][
        "value"] == 4


def test_cli_reduce_names_an_unusable_manifest(tmp_path):
    from typer.testing import CliRunner

    from artsrun.cli import app

    (tmp_path / "counters" / "cellA").mkdir(parents=True)
    (tmp_path / "manifest.json").write_text("{not json")
    out = CliRunner().invoke(app, ["counters", "reduce", str(tmp_path)])
    assert out.exit_code == 2 and "not usable" in " ".join(out.output.split())
    (tmp_path / "manifest.json").write_text('{"cells": [{"slug": "a"}]}')
    out = CliRunner().invoke(app, ["counters", "reduce", str(tmp_path)])
    assert out.exit_code == 2 and "not usable" in " ".join(out.output.split())


def test_a_rebuild_equals_the_campaigns_record_whatever_the_tree_now_holds(
        tmp_path):
    from types import SimpleNamespace

    from typer.testing import CliRunner

    from artsrun.campaign import Campaign
    from artsrun.cli import app

    folder = tmp_path / "counters" / "cellA"
    folder.mkdir(parents=True)
    put(folder, 0, {"NUM_EDT_CREATE": c(5, method="MAX")})
    put(folder, 1, {"NUM_EDT_CREATE": c(3, method="MAX")})
    camp = Campaign.__new__(Campaign)
    camp.run_dir = tmp_path
    camp.counterset = SimpleNamespace(enabled=["NUM_EDT_CREATE"])
    camp._reduce_counters(SimpleNamespace(slug="cellA", key="k", nodes=2),
                          lambda _m: None)
    first = (folder / "c.json").read_text()
    # The tree is reconfigured afterwards to reduce the counter by SUM.
    header = tmp_path / "tree/libs/include/internal/arts/counter/Preamble.h"
    header.parent.mkdir(parents=True)
    header.write_text("#define ENABLE_NUM_EDT_CREATE 1\n"
                      "#define COUNTER_MODE_NUM_EDT_CREATE 1\n"
                      "#define COUNTER_LEVEL_NUM_EDT_CREATE 1\n"
                      "#define COUNTER_REDUCE_NUM_EDT_CREATE 0\n")
    (tmp_path / "manifest.json").write_text(json.dumps({
        "build_dir": str(tmp_path / "tree"),
        "cells": [{"slug": "cellA", "nodes": 2}]}))
    out = CliRunner().invoke(app, ["counters", "reduce", str(tmp_path)])
    assert out.exit_code == 0
    assert (folder / "c.json").read_text() == first
    assert json.loads(first)["counters"]["NUM_EDT_CREATE"]["value"] == 5
