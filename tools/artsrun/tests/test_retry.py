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
