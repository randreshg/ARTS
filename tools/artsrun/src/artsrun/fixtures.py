"""Fixture staging: declared inputs the tool knows how to synthesize.

Most fixtures are cheap deterministic files (a seeded sequence, a counting
file); regenerating them on the machine that needs them beats carrying them
around or dropping the cell.  An input an application can derive from its own
arguments is not a fixture at all — it is generated inside the run, by the
tasks that own the data — so it never reaches this registry.  Anything not in
the registry stays a missing input and is reported the usual way — a generated
file is written atomically so a crashed generator never leaves a half-file that
later runs mistake for the fixture.
"""

from __future__ import annotations

from pathlib import Path


def _acgt_pair(path: Path, which: int, lens, seed, names) -> None:
    # The two alignment inputs are one seeded ACGT stream split at lens[0];
    # regenerating either file alone must not restart the stream, so one call
    # derives both and writes the sibling (atomically) when absent.
    import random

    rng = random.Random(seed)
    both = ["".join(rng.choice("ACGT") for _ in range(n)) for n in lens]
    path.write_text(both[which])
    sibling = path.parent / names[1 - which]
    if not sibling.exists():
        tmp = sibling.with_name(sibling.name + ".staging")
        tmp.write_text(both[1 - which])
        tmp.replace(sibling)


def _counting_file(path: Path) -> None:
    # basicIO reads u64 values one per line; 0..9 gives a fixed checksum.
    path.write_text("\n".join(str(i) for i in range(10)) + "\n")


_SW_HUGE  = ("string1-huge.txt", "string2-huge.txt")
_SW_CAL   = ("string1-cal.txt", "string2-cal.txt")
_SW_TREND = ("string1-trend.txt", "string2-trend.txt")
_SW_SWD   = ("string1-swd.txt", "string2-swd.txt")
_SW_GATE  = ("string1-gate.txt", "string2-gate.txt")
_SW_MID   = ("string1-mid.txt", "string2-mid.txt")
_SW_CMP   = ("string1-cmp.txt", "string2-cmp.txt")

GENERATORS = {
    # Alignment pairs, each a seeded ACGT stream split in two.  The scores are
    # the expected global alignment of the pair (border = gap*position,
    # match 2 / transition -2 / transversion -4 / gap -1, no zero clamp): the
    # huge, cal, trend and gate scores come from an independent sequential
    # reference of the same DP; the swd, mid and cmp scores are the value the
    # program itself printed in agreement across runtimes and node counts,
    # since those pairs are past what the sequential reference computes.
    "string1-huge.txt":  lambda p: _acgt_pair(p, 0, (515000, 517000), 20260819, _SW_HUGE),
    "string2-huge.txt":  lambda p: _acgt_pair(p, 1, (515000, 517000), 20260819, _SW_HUGE),
    "score-huge.txt":    lambda p: p.write_text("318128\n"),
    "string1-cal.txt":   lambda p: _acgt_pair(p, 0, (140000, 140400), 20260819, _SW_CAL),
    "string2-cal.txt":   lambda p: _acgt_pair(p, 1, (140000, 140400), 20260819, _SW_CAL),
    "score-cal.txt":     lambda p: p.write_text("86360\n"),
    "string1-trend.txt": lambda p: _acgt_pair(p, 0, (70000, 70000), 20260828, _SW_TREND),
    "string2-trend.txt": lambda p: _acgt_pair(p, 1, (70000, 70000), 20260828, _SW_TREND),
    "score-trend.txt":   lambda p: p.write_text("43068\n"),
    "string1-swd.txt":   lambda p: _acgt_pair(p, 0, (800000, 800000), 20260830, _SW_SWD),
    "string2-swd.txt":   lambda p: _acgt_pair(p, 1, (800000, 800000), 20260830, _SW_SWD),
    "score-swd.txt":     lambda p: p.write_text("493680\n"),
    "string1-gate.txt":  lambda p: _acgt_pair(p, 0, (2000, 2000), 20260902, _SW_GATE),
    "string2-gate.txt":  lambda p: _acgt_pair(p, 1, (2000, 2000), 20260902, _SW_GATE),
    "score-gate.txt":    lambda p: p.write_text("1176\n"),
    "string1-mid.txt":   lambda p: _acgt_pair(p, 0, (178000, 178400), 20260828, _SW_MID),
    "string2-mid.txt":   lambda p: _acgt_pair(p, 1, (178000, 178400), 20260828, _SW_MID),
    "score-mid.txt":     lambda p: p.write_text("110024\n"),
    "string1-cmp.txt":   lambda p: _acgt_pair(p, 0, (200000, 200000), 20260828001, _SW_CMP),
    "string2-cmp.txt":   lambda p: _acgt_pair(p, 1, (200000, 200000), 20260828001, _SW_CMP),
    "score-cmp.txt":     lambda p: p.write_text("123196\n"),
    "basicIO_test.dat": _counting_file,
}


def stage(paths: list[str]) -> list[str]:
    """Generate whatever is missing and known; return what is still missing."""
    left: list[str] = []
    for name in paths:
        path = Path(name)
        if path.exists():
            continue
        generator = GENERATORS.get(path.name)
        if generator is None:
            left.append(name)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".staging")
        generator(tmp)
        tmp.replace(path)
    return left
