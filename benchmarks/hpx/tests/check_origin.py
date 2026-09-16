#!/usr/bin/env python3
"""Every difference between an HPX-origin program and its pristine origin is
the program's committed origin.patch — nothing more, nothing less.

usage: check_origin.py <program-dir> <origin-root>
       check_origin.py --write <program-dir> <origin-root>
       check_origin.py --selftest

<program-dir>/ORIGIN.md carries a `files:` table of `origin-path -> ours`
lines; each origin path is relative to <origin-root>.  The right-hand side of
that table is also the program's complete source list: a source file under
<program-dir> that the table does not name is built but undisclosed, and a
named one that is absent is a disclosure with nothing behind it.  The diff is
regenerated with fixed labels (so the patch is byte-stable across hosts) and
must equal origin.patch exactly.  Exit 0 on match, 1 on mismatch or on a file
that is missing or unlisted, 2 on usage or on an ORIGIN.md that lists no
files at all.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FILES_RE = re.compile(r"^\s*-\s*`([^`]+)`\s*->\s*`([^`]+)`\s*$")

# What a compiler would take from the program directory; the disclosure has
# to account for exactly these.
SOURCE_SUFFIXES = frozenset({".cpp", ".hpp", ".h", ".cxx"})


class CheckFailed(Exception):
    def __init__(self, message: str, code: int = 1) -> None:
        super().__init__(message)
        self.code = code


def file_pairs(origin_md: Path) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    in_files = False
    for line in origin_md.read_text().splitlines():
        if line.strip() == "files:":
            in_files = True
            continue
        if in_files:
            m = FILES_RE.match(line)
            if m:
                pairs.append((m.group(1), m.group(2)))
            elif line.strip() and not line.startswith(" ") and not line.startswith("-"):
                break
    return pairs


def sources_present(program: Path) -> set[str]:
    return {
        p.relative_to(program).as_posix()
        for p in program.rglob("*")
        if p.is_file() and p.suffix in SOURCE_SUFFIXES
    }


def check_file_set(program: Path, pairs: list[tuple[str, str]]) -> None:
    listed = {dst for _, dst in pairs}
    present = sources_present(program)
    for name in sorted(present - listed):
        raise CheckFailed(
            f"{program.name}: {name} is a source of this program but no "
            f"ORIGIN.md `files:` line names it")
    for name in sorted(listed - present):
        raise CheckFailed(
            f"{program.name}: ORIGIN.md names {name} but no such file exists")


def regenerate(program: Path, origin_root: Path) -> str:
    out: list[str] = []
    for src, dst in file_pairs(program / "ORIGIN.md"):
        a = origin_root / src
        b = program / dst
        if not a.exists():
            raise FileNotFoundError(f"origin file missing: {a}")
        if not b.exists():
            raise FileNotFoundError(f"program file missing: {b}")
        proc = subprocess.run(
            ["diff", "-u", "--label", f"origin/{src}", "--label", f"ours/{dst}",
             str(a), str(b)],
            capture_output=True, text=True, check=False)
        if proc.returncode not in (0, 1):
            raise RuntimeError(proc.stderr)
        out.append(proc.stdout)
    return "".join(out)


def check(program: Path, origin_root: Path) -> None:
    pairs = file_pairs(program / "ORIGIN.md")
    if not pairs:
        raise CheckFailed(
            f"{program.name}: ORIGIN.md has no `files:` entry, so nothing is "
            f"disclosed", code=2)
    check_file_set(program, pairs)
    fresh = regenerate(program, origin_root)
    committed = program / "origin.patch"
    if not committed.exists():
        raise CheckFailed(f"{committed} missing")
    if committed.read_text() != fresh:
        raise CheckFailed(
            f"{program.name}: origin.patch is stale; regenerate with\n"
            f"  python3 {Path(__file__).name} --write {program} {origin_root}")


def run(program: Path, origin_root: Path) -> int:
    try:
        check(program, origin_root)
    except CheckFailed as e:
        print(f"check_origin: {e}")
        return e.code
    except (FileNotFoundError, RuntimeError) as e:
        print(f"check_origin: {e}")
        return 1
    return 0


def selftest() -> int:
    """Each exit code the checker owes its caller, produced from a fixture."""
    cases: list[tuple[str, int]] = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        origin = root / "origin"
        (origin / "sub").mkdir(parents=True)
        (origin / "sub" / "a.cpp").write_text("int a() { return 1; }\n")

        def fixture(name: str, origin_md: str, files: dict[str, str]) -> Path:
            p = root / name
            p.mkdir()
            (p / "ORIGIN.md").write_text(origin_md)
            for rel, text in files.items():
                q = p / rel
                q.parent.mkdir(parents=True, exist_ok=True)
                q.write_text(text)
            return p

        listing = "files:\n- `sub/a.cpp` -> `a.cpp`\n"
        edited = "int a() { return 2; }\n"

        good = fixture("good", listing, {"a.cpp": edited})
        (good / "origin.patch").write_text(regenerate(good, origin))
        cases.append(("in sync", run(good, origin)))

        stale = fixture("stale", listing, {"a.cpp": edited})
        shutil.copyfile(good / "origin.patch", stale / "origin.patch")
        (stale / "a.cpp").write_text("int a() { return 3; }\n")
        cases.append(("stale patch", run(stale, origin)))

        missing = fixture("missing", listing, {})
        (missing / "origin.patch").write_text("")
        cases.append(("listed source absent", run(missing, origin)))

        extra = fixture("extra", listing, {"a.cpp": edited, "b.hpp": "int b;\n"})
        shutil.copyfile(good / "origin.patch", extra / "origin.patch")
        cases.append(("unlisted source present", run(extra, origin)))

        nofiles = fixture("nofiles", "# nothing disclosed\n", {"a.cpp": edited})
        (nofiles / "origin.patch").write_text("")
        cases.append(("no files: entry", run(nofiles, origin)))

    expected = [0, 1, 1, 1, 2]
    failures = 0
    for (name, got), want in zip(cases, expected, strict=True):
        if got != want:
            print(f"check_origin selftest: {name}: expected {want}, got {got}")
            failures += 1
    print(f"check_origin selftest: {len(cases) - failures}/{len(cases)} cases")
    return 1 if failures else 0


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) == 4 and argv[1] == "--write":
        p, r = Path(argv[2]), Path(argv[3])
        (p / "origin.patch").write_text(regenerate(p, r))
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2
    return run(Path(argv[1]), Path(argv[2]))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
