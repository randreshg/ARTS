"""Resolve a selection to build targets and compile them in one pass.

The whole selection is compiled before any cell runs, so a campaign never
interleaves building with measuring.  Ninja is the cache: a target that is
already current costs nothing to ask for again.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from artsrun.model.experiment import Experiment
from artsrun.model.catalog import Catalog
from artsrun.model.plane import Plane, RuntimeKind
from artsrun.model.profile import FamDevice, Profile
from artsrun.model.selection import Selection


class BuildError(RuntimeError):
    pass


@dataclass
class BuildPlan:
    build_dir: Path
    targets: list[str] = field(default_factory=list)
    missing: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.missing


def _cache_value(build_dir: Path, key: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    pattern = re.compile(rf"^{re.escape(key)}:[^=]*=(.*)$")
    for line in cache.read_text(errors="ignore").splitlines():
        m = pattern.match(line)
        if m:
            return m.group(1).strip()
    return None


def fam_device_options(profile: Profile) -> list[str]:
    """The cache values a tree needs to run this profile's fabric-attached
    entries on the library it names.

    The entries are benchmark variants that carry their own protocol, so
    only the backend and the library are named; the tree's own protocol
    stays what it is.  Under `fake` the two paths are cleared: the vendored
    option shadows them, and a stale path left in the cache is exactly the
    kind of state that makes a later configure mean something else.
    """
    if profile.resolved_fam_device is FamDevice.REAL:
        return ["-DARTS_FAM_BACKEND=DEVICE", "-DARTS_FAM_DEVICE_VENDORED=OFF",
                f"-DARTS_FAM_DEVICE_INCLUDE_DIR={profile.fam_device_include_dir}",
                f"-DARTS_FAM_DEVICE_LIBRARY={profile.fam_device_library}"]
    return ["-DARTS_FAM_BACKEND=DEVICE", "-DARTS_FAM_DEVICE_VENDORED=ON",
            "-DARTS_FAM_DEVICE_INCLUDE_DIR=", "-DARTS_FAM_DEVICE_LIBRARY="]


def _is_on(value: str | None) -> bool:
    return (value or "OFF").upper() in ("ON", "TRUE", "YES", "Y", "1")


def fam_device_mismatch(build_dir: Path, profile: Profile) -> list[str]:
    """How this tree's cache differs from the library the profile names,
    one `KEY: have X, want Y` line per differing value; empty when it
    matches."""
    device = profile.resolved_fam_device
    have = {k: _cache_value(build_dir, k) for k in (
        "ARTS_FAM_BACKEND", "ARTS_FAM_DEVICE_VENDORED",
        "ARTS_FAM_DEVICE_INCLUDE_DIR", "ARTS_FAM_DEVICE_LIBRARY")}
    out = []
    if have["ARTS_FAM_BACKEND"] != "DEVICE":
        out.append(f"ARTS_FAM_BACKEND: have {have['ARTS_FAM_BACKEND'] or 'OFF'}, "
                   "want DEVICE")
    vendored = _is_on(have["ARTS_FAM_DEVICE_VENDORED"])
    if device is FamDevice.REAL:
        if vendored:
            out.append("ARTS_FAM_DEVICE_VENDORED: have ON, want OFF")
        for key, want in (("ARTS_FAM_DEVICE_INCLUDE_DIR", profile.fam_device_include_dir),
                          ("ARTS_FAM_DEVICE_LIBRARY", profile.fam_device_library)):
            got = have[key]
            if not got or os.path.normpath(got) != os.path.normpath(want):
                out.append(f"{key}: have {got or '(unset)'}, want {want}")
    elif not vendored:
        out.append("ARTS_FAM_DEVICE_VENDORED: have OFF, want ON")
    return out


def _cmake_error(text: str) -> str:
    """cmake's own error report, from its first `CMake Error` on; the tail
    of the output when it printed none."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith("CMake Error"):
            return "\n".join(lines[i:i + 40])
    return "\n".join(lines[-25:])


def fam_device_configure_command(build_dir: Path, profile: Profile, *,
                                 prefix: list[str] | None = None) -> list[str]:
    from artsrun.paths import repo_root

    # -S for the same reason as the counter reconfigure: without it cmake
    # takes the source directory from the caller's cwd.
    return [*(prefix or []), "cmake", "-S", str(repo_root()), "-B", str(build_dir),
            *fam_device_options(profile)]


def configure_fam_device(build_dir: Path, profile: Profile, *, on_line=None,
                         prefix: list[str] | None = None) -> None:
    """Reconfigure a tree to the device library the profile names, or die.

    Only the FAM options are passed; everything else stays in the cache.
    The configure is the check: when the named library cannot be built or
    found, cmake's own error ends the campaign, since a FAM cell must run
    on the library its profile states or not at all.
    """
    import shlex

    say = on_line or (lambda _msg: None)
    if shutil.which("cmake") is None:
        raise BuildError("cmake not found on PATH")
    cmd = fam_device_configure_command(build_dir, profile, prefix=prefix)
    say(f"fam_device: {profile.resolved_fam_device} — reconfiguring {build_dir} "
        "to that library (this rebuilds everything):")
    say(f"  $ {shlex.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise BuildError(
            f"could not configure {build_dir} for fam_device: "
            f"{profile.resolved_fam_device}; cmake reported:\n"
            + _cmake_error((proc.stdout or "") + "\n" + (proc.stderr or "")))
    left = fam_device_mismatch(build_dir, profile)
    if left:
        raise BuildError(
            f"{build_dir} was reconfigured but its cache still differs from "
            "the profile's device library:\n  " + "\n  ".join(left))


def counter_config_of(build_dir: Path) -> str | None:
    """Which counter file this tree was configured against."""
    return _cache_value(build_dir, "ARTS_COUNTER_CONFIG")


# How the configure step encodes a counter's settings into the header it
# generates.  Reading them back is what lets a tree be checked against a set
# rather than against the file it happened to be configured from.
_MODES = {0: "OFF", 1: "ONCE", 2: "PERIODIC"}
_LEVELS = {0: "THREAD", 1: "NODE", 2: "CLUSTER"}
_REDUCERS = {0: "SUM", 1: "MAX", 2: "MIN", 3: "MASTER"}

_PREAMBLE = "libs/include/internal/arts/counter/Preamble.h"


def compiled_counters(build_dir: Path) -> dict[str, tuple[str, str, str]] | None:
    """What this tree actually compiled, counter by counter.

    The generated header is the only authority: the selection is turned into
    macros at configure time, so a build carries its counters no matter which
    file they arrived in or whether that file still exists.
    """
    header = build_dir / _PREAMBLE
    if not header.is_file():
        return None
    text = header.read_text(errors="ignore")

    def read(prefix: str) -> dict[str, int]:
        return {
            m.group(1): int(m.group(2))
            for m in re.finditer(rf"^#define {prefix}([A-Z0-9_]+) (\d+)$", text, re.M)
        }

    enabled = read("ENABLE_")
    modes, levels = read("COUNTER_MODE_"), read("COUNTER_LEVEL_")
    reducers = read("COUNTER_REDUCE_") or read("REDUCE_METHOD_")
    if not enabled:
        return None
    return {
        name: (
            _MODES.get(modes.get(name, 0), "OFF") if on else "OFF",
            _LEVELS.get(levels.get(name, 1), "NODE"),
            _REDUCERS.get(reducers.get(name, 0), "SUM"),
        )
        for name, on in enabled.items()
    }


def counter_mismatch(build_dir: Path, counterset) -> list[str]:
    """Counters whose compiled settings differ from what a set asks for.

    What the tree compiled is read back out of its generated header, not out
    of the file it was configured from.  A counter set is values that get
    rendered afresh for every campaign, so the file they land in is
    incidental: a tree configured from a hand-written file may hold exactly
    the wanted selection, and two renderings of one set differ in path only.
    """
    have = compiled_counters(build_dir)
    if have is None or counterset is None:
        return []
    default = ("OFF", "NODE", "SUM")
    want = {
        name: (s.mode.value, s.level.value,
               (s.reduce.value if s.reduce else "SUM"))
        for name, s in counterset.counters.items()
    }
    return sorted(
        name for name in set(have) | set(want)
        if have.get(name, default) != want.get(name, default)
    )


# The state a tree measures in when nothing asked for counters: the build's
# own default, every counter compiled out.
DEFAULT_COUNTER_CONFIG = "configs/counters_off.cfg"


def instrumented_counters(build_dir: Path) -> list[str]:
    """Counters this tree compiled in, whatever file they arrived in."""
    have = compiled_counters(build_dir)
    if have is None:
        return []
    return sorted(name for name, (mode, _lvl, _red) in have.items()
                  if mode != "OFF")


def require_default_counters(build_dir: Path) -> None:
    """Refuse to measure on a tree an earlier campaign left instrumented.

    A campaign that names no counter set is asking for a timing measurement,
    and instrumentation is compiled in: whatever the last reconfigure left in
    the tree is what the cells would measure through, silently.  Counters are
    opted into, so anything on without being asked for is the tree
    contradicting the campaign, and the only honest answer is to stop.
    """
    from artsrun.paths import repo_root

    on = instrumented_counters(build_dir)
    if not on:
        return
    shown = ", ".join(on[:6]) + (f", and {len(on) - 6} more" if len(on) > 6 else "")
    configured_from = counter_config_of(build_dir) or "(not recorded in the cache)"
    off = repo_root() / DEFAULT_COUNTER_CONFIG
    raise BuildError(
        f"{build_dir} has {len(on)} counter(s) compiled in ({shown}) but this "
        f"campaign selected no counter set — a timing run measures on an "
        f"uninstrumented tree.\n"
        f"  the tree is configured from: {configured_from}\n"
        f"  select that set with -c to measure with it, or restore the default:\n"
        f"    cmake -S . -B {build_dir} -DARTS_COUNTER_CONFIG={off} && "
        f"ninja -C {build_dir}"
    )


def configure_counters(build_dir: Path, wanted: Path, *, on_line=None,
                       prefix: list[str] | None = None) -> None:
    """Point an existing tree at a counter configuration and reconfigure it.

    Counter selection is compiled in, so making a tree match is a build step
    rather than something to hand back to the caller: asking to build with a
    counter set IS asking for the tree that carries it.  Only the counter
    option is passed — everything else stays in the cache, so this cannot
    quietly change the configuration in any other respect.  The rebuild that
    follows is a full one, and says so.
    """
    from artsrun.paths import repo_root

    say = on_line or (lambda _msg: None)
    if shutil.which("cmake") is None:
        raise BuildError("cmake not found on PATH")
    say(f"counters changed — reconfiguring {build_dir} (this rebuilds everything)")
    # -S is required even for an existing cache: without it cmake derives the
    # source directory from the CALLER'S cwd, and a campaign launched from
    # anywhere but the repo root reconfigures against the wrong source.
    proc = subprocess.run(
        [*(prefix or []), "cmake", "-S", str(repo_root()), "-B", str(build_dir),
         f"-DARTS_COUNTER_CONFIG={wanted}"],
        capture_output=True, text=True,
    )
    for line in proc.stdout.splitlines():
        if "Counter configuration:" in line or "error" in line.lower():
            say(f"  {line.strip()}")
    if proc.returncode != 0:
        tail = "\n".join((proc.stderr or proc.stdout).splitlines()[-15:])
        raise BuildError(
            f"could not reconfigure {build_dir} for the counter set:\n{tail}"
        )


def ensure_build_dir(build_dir: Path, *, bootstrap: bool = False,
                     on_line=None, prefix: list[str] | None = None,
                     fam_options: list[str] | None = None) -> None:
    """Configure a tree that never was; verify one that already is.

    The experiment tree is fully determined — Release, benchmarks on — so a
    missing one is a first run rather than an error, and the configure is
    simply run.  A tree that EXISTS is only verified, never reconfigured
    behind its owner's back: a Debug or no-benchmark tree is somebody's
    deliberate configuration, and the counter and FAM-device reconfigures
    elsewhere change only the options they own for the same reason.  A dry run configures nothing —
    dry means dry — and reports what a real run would do instead.

    `fam_options` are the device-library values a campaign that runs the
    fabric-attached entries needs; a new tree is configured with them.
    """
    from artsrun.paths import repo_root

    fresh = False
    configure = [*(prefix or []), "cmake", "-S", str(repo_root()), "-GNinja",
                 f"-B{build_dir}", "-DCMAKE_BUILD_TYPE=Release",
                 *(fam_options or [])]
    if not (build_dir / "build.ninja").is_file():
        if not bootstrap:
            import shlex

            raise BuildError(
                f"{build_dir} is not a configured build tree; a real run "
                f"configures it first:\n  $ {shlex.join(configure)}"
            )
        fresh = True
        if shutil.which("cmake") is None:
            raise BuildError("cmake not found on PATH")

        say = on_line or (lambda _msg: None)
        say(f"{build_dir} does not exist yet — configuring it "
            "(the first build also compiles the vendored dependencies)")
        proc = subprocess.Popen(
            configure,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        output: list[str] = []
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip("\n")
            output.append(line)
            say(line)
        if proc.wait() != 0:
            raise BuildError("configure failed:\n" + _cmake_error("\n".join(output)))
    if (_cache_value(build_dir, "ARTS_BUILD_BENCHMARKS") or "ON") == "OFF":
        raise BuildError(
            f"{build_dir} was configured with ARTS_BUILD_BENCHMARKS=OFF; the "
            f"application targets do not exist there"
        )
    build_type = _cache_value(build_dir, "CMAKE_BUILD_TYPE") or ""
    if build_type and build_type.lower() != "release":
        raise BuildError(
            f"{build_dir} is a {build_type} tree; measurements must come from a "
            f"Release build (reconfigure with -DCMAKE_BUILD_TYPE=Release)"
        )
    # A tree generated a moment ago is current by construction.
    if fresh:
        return
    if bootstrap:
        regenerate(build_dir, prefix=prefix)
    elif generator_stale(build_dir):
        say = on_line or (lambda _msg: None)
        say(f"{build_dir}: its CMake files changed since it was generated; a "
            "real run re-runs the configure first, and until then the target "
            "check reads the previous generation")


def generator_stale(build_dir: Path) -> bool:
    """Whether the tree's generation predates its CMake files."""
    if shutil.which("ninja") is None:
        return False
    out = subprocess.run(
        ["ninja", "-C", str(build_dir), "-n", "build.ninja"],
        capture_output=True, text=True, check=False,
    ).stdout
    return "Re-running CMake" in out


def regenerate(build_dir: Path, *, prefix: list[str] | None = None) -> None:
    """Bring an existing tree's generation up to date with its CMake files.

    This is the tree's own first build step, run early: ninja re-runs the
    configure from the tree's cache — no option changed — before it builds
    anything, so doing it before the target list is read is not a
    reconfiguration, it is reading the list the build would use.  Without
    it a source update that registers new programs is invisible until the
    build, and a configure that now fails leaves the previous generation
    in place, so the targets it did not know are reported as missing and
    the failure itself is never shown.
    """
    if shutil.which("ninja") is None:
        raise BuildError("ninja not found on PATH")
    proc = subprocess.run(
        [*(prefix or []), "ninja", "-C", str(build_dir), "build.ninja"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
    )
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.splitlines()[-25:])
        raise BuildError(
            f"{build_dir}: its CMake files changed since it was generated and "
            f"the configure now fails, so nothing in it can be built until "
            f"that is resolved:\n{tail}"
        )


def available_targets(build_dir: Path) -> set[str]:
    out = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "targets", "all"],
        capture_output=True, text=True, check=False,
    ).stdout
    names = set()
    for line in out.splitlines():
        target = line.split(":", 1)[0].strip()
        if target:
            names.add(Path(target).name)
    return names


def plan_targets(
    selection: Selection,
    plane: Plane,
    catalog: Catalog,
    experiment: Experiment,
    build_dir: Path,
    profile: Profile | None = None,
) -> BuildPlan:
    """Every executable this campaign will run, deduplicated.

    The same rows and the same eligibility the expansion applies: a name the
    experiment does not enable runs no cell, so it needs nothing built, and a
    row's own exclusions (ARTS-only, no ocr-vx, outside DB-WRF) hold for the
    build exactly as they hold for the run — as does a reference this host
    cannot place, reported with its reason rather than as a target that is
    missing.
    """
    from artsrun.run.plan import reference_ineligible

    entries = [plane.entry(k) for k in selection.entries]
    resolved = {a.key: a for a in experiment.resolve(catalog)}

    wanted: list[str] = []
    for name, versions in selection.apps.items():
        for version in versions:
            app = resolved.get(f"{name}:{version.value}")
            if app is None:
                continue
            for entry in entries:
                if profile is not None and reference_ineligible(entry, profile):
                    continue
                if entry.kind is RuntimeKind.HPX:
                    # The HPX program is the row's _hpx target; nothing is
                    # derived from a version stem.
                    if app.hpx_binary:
                        wanted.append(app.hpx_binary)
                    continue
                if entry.kind.value == "ocrvx" and app.ocrvx_skip:
                    continue
                if entry.model == "DB_WRF" and app.unordered_writes:
                    continue
                if entry.kind.value != "arts" and app.arts_only:
                    continue
                # The hint layer is already folded into the resolved stem.
                wanted.append(entry.binary(app.binary, hinted=False))

    targets = sorted(set(wanted))
    have = available_targets(build_dir)
    missing = [t for t in targets if t not in have] if have else []
    return BuildPlan(build_dir=build_dir, targets=targets, missing=missing)


def build(plan: BuildPlan, *, jobs: int | None = None, on_line=None,
          prefix: list[str] | None = None) -> None:
    """Compile the whole plan in a single ninja invocation."""
    if not plan.ok:
        raise BuildError(
            "the build tree has no target for: " + ", ".join(plan.missing[:10])
            + ("…" if len(plan.missing) > 10 else "")
            + "\n(the tree's generation is current, so either the application "
            "is not registered in CMake, its sources are not in this checkout "
            "-- the configure creates no target for such a registration -- or "
            "the tree skipped its runtime: the xsocr/ocrvx references and the "
            "HPX-origin programs are skipped on a host without an MPI compiler "
            "and under -DARTS_BUILD_HPX=OFF; the configure output's 'Skipping' "
            "and References/HPX lines say which)"
        )
    if shutil.which("ninja") is None:
        raise BuildError("ninja not found on PATH")
    cmd = [*(prefix or []), "ninja", "-C", str(plan.build_dir)]
    if jobs:
        cmd += ["-j", str(jobs)]
    cmd += plan.targets
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    tail: list[str] = []
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip("\n")
        tail.append(line)
        del tail[:-40]
        if on_line:
            on_line(line)
    if proc.wait() != 0:
        raise BuildError("build failed:\n" + "\n".join(tail))
