"""The end grace G of every ARTS cell, and the bound B it must cover.

A launcher that ends a run when its first rank exits must wait for the
others: a rank still writing its record when the launcher kills it loses
the record and can fail a run that would have ended by itself.  B is the
longest a rank can spend ending once the run has begun to end: the probe
latency, the budget of the end's own waits, and the time to write the
largest record the cell's counters can produce.  G is at least B.
"""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import replace

from artsrun.model.counters import Counterset, Level, Mode
from artsrun.run.types import DEFAULT_END_GRACE_S, Cell

PROBE_LATENCY_S = 1
END_BOUND_S = 10
WATCHDOG_LINE_S = 1
STARTUP_BUDGET_DEFAULT_S = 20
RECORD_RATE_BYTES_S = 4 << 20

BASE_RECORD_BYTES = 4096
COUNTER_BYTES = 160
ENTRY_BYTES = 40

END_GRACE_MIN_S = 1
END_GRACE_MAX_S = 86400


class EndGraceError(ValueError):
    pass


def check_value(value: int, what: str = "--end-grace") -> int:
    if not END_GRACE_MIN_S <= value <= END_GRACE_MAX_S:
        raise EndGraceError(
            f"{what} must lie in [{END_GRACE_MIN_S}, {END_GRACE_MAX_S}] "
            f"seconds, not {value}")
    return value


def startup_budget_s(env: Mapping[str, str]) -> int:
    raw = env.get("ARTS_STARTUP_TIMEOUT_S")
    if raw is None or raw == "":
        return STARTUP_BUDGET_DEFAULT_S
    try:
        value = int(raw)
    except ValueError:
        value = 0
    if value < 1:
        raise EndGraceError(
            f"ARTS_STARTUP_TIMEOUT_S={raw!r} is not a positive whole "
            "number of seconds")
    return value


def record_bytes(cell: Cell, counterset: Counterset | None,
                 threads: int, capture_interval_ms: int) -> int:
    """The largest record one rank writes: the fixed part, 160 B per enabled
    counter (and per thread for a THREAD one), and 40 B per captured
    PERIODIC entry.  A NODE counter keeps one node-reduced history of one
    entry per epoch, a THREAD counter one history per thread."""
    if counterset is None:
        return BASE_RECORD_BYTES
    epochs = -(-cell.timeout_s * 1000 // capture_interval_ms) + 2
    size = BASE_RECORD_BYTES
    for setting in counterset.counters.values():
        if not setting.enabled:
            continue
        histories = threads if setting.level is Level.THREAD else 1
        size += COUNTER_BYTES + (COUNTER_BYTES * threads
                                 if setting.level is Level.THREAD else 0)
        if setting.mode is Mode.PERIODIC:
            size += ENTRY_BYTES * epochs * histories
    return size


def cell_bound_s(cell: Cell, env: Mapping[str, str], *,
                 counterset: Counterset | None = None,
                 threads: int = 1) -> int:
    """B = max(lambda + b + the watchdog line's second + ceil(s_max / 4 MiB/s),
    budget + b), whole seconds."""
    interval = counterset.capture_interval if counterset else 100
    s_max = record_bytes(cell, counterset, threads, interval)
    write_s = math.ceil(s_max / RECORD_RATE_BYTES_S)
    return max(PROBE_LATENCY_S + END_BOUND_S + WATCHDOG_LINE_S + write_s,
               startup_budget_s(env) + END_BOUND_S)


def apply_grace(cells: list[Cell], campaign_s: int, given: bool,
                env: Mapping[str, str], *,
                counterset: Counterset | None = None,
                threads: int = 1) -> list[Cell]:
    """Each ARTS cell with its own G: the campaign's, raised to the cell's B
    when B is larger.  An explicit value below a cell's B is refused, naming
    the cell and B, before anything runs.  Reference cells are left alone."""
    from artsrun.model.plane import RuntimeKind

    out: list[Cell] = []
    for cell in cells:
        if cell.entry.kind is not RuntimeKind.ARTS:
            out.append(cell)
            continue
        bound = cell_bound_s(cell, {**env, **cell.env},
                             counterset=counterset, threads=threads)
        if bound > campaign_s and given:
            raise EndGraceError(
                f"--end-grace {campaign_s} is below the bound B = {bound} s "
                f"of cell {cell.key}: a rank could be killed while it "
                f"still ends by itself")
        if bound > campaign_s:
            out.append(replace(cell, end_grace_s=bound,
                               end_grace_raised_from=campaign_s))
        else:
            out.append(replace(cell, end_grace_s=campaign_s))
    return out


def reconcile(given: int | None, recorded: tuple[int, bool] | None
              ) -> tuple[int, bool]:
    """The campaign's (G, given) from the flag and what the run's manifest
    recorded: a continuation keeps the recorded value and refuses another."""
    if given is not None:
        check_value(given)
    if recorded is None:
        return (given, True) if given is not None else (DEFAULT_END_GRACE_S, False)
    value, was_given = recorded
    if given is None:
        return value, was_given
    if given != value:
        raise EndGraceError(
            f"--end-grace {given} differs from the {value} this run "
            "started with; a continuation keeps its value")
    return value, True


def check_budget(recorded: int | None, env: Mapping[str, str]) -> int:
    """The startup budget B was computed with: a continuation keeps the one
    its run recorded and refuses a changed environment, so no cell's G is
    recomputed from it."""
    current = startup_budget_s(env)
    if recorded is not None and recorded != current:
        raise EndGraceError(
            f"ARTS_STARTUP_TIMEOUT_S gives a startup budget of {current} s "
            f"but this run started with {recorded} s; a continuation keeps "
            "its value")
    return current
