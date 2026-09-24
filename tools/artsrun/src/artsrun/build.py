"""Resolve a selection to build targets and compile them in one pass.

The whole selection is compiled before any cell runs, so a campaign never
interleaves building with measuring.  Ninja is the cache: a target that is
already current costs nothing to ask for again.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

from artsrun.model.benchset import Benchset
from artsrun.model.catalog import Catalog
from artsrun.model.plane import Plane, RuntimeKind
from artsrun.model.profile import Profile
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


# Where the vendored emulation of the device library lives in the checkout.
_VENDORED_DEVICE = "third_party/fake_arts_cxl_lib"


def require_cxl(build_dir: Path, selection: Selection) -> None:
    """Reject a tree whose fabric-attached-memory backend differs from this
    campaign's mode.

    A FAM-device campaign (`--cxl`) runs on the device library itself: the
    tree must name ARTS_FAM_BACKEND=DEVICE with both device paths real, and
    the vendored emulation of the library, which is for development runs,
    cannot stand in for a measurement.  A DEVICE tree is equally refused to
    any other campaign, whose cells would run outside the device's region
    setup.
    """
    def is_on(key: str) -> bool:
        return (_cache_value(build_dir, key) or "OFF").upper() in ("ON", "TRUE", "YES", "1")

    backend = fam_backend_of(build_dir) or "OFF"
    if selection.cxl != (backend == "DEVICE"):
        want = ("ARTS_FAM_BACKEND=DEVICE" if selection.cxl
                else "a backend other than DEVICE (only --cxl runs a DEVICE tree)")
        raise BuildError(
            f"{build_dir} has ARTS_FAM_BACKEND={backend}; this campaign requires "
            f"{want}. Use a build tree configured for this mode."
        )
    if not selection.cxl:
        return
    include = _cache_value(build_dir, "ARTS_FAM_DEVICE_INCLUDE_DIR")
    library = _cache_value(build_dir, "ARTS_FAM_DEVICE_LIBRARY")
    if is_on("ARTS_FAM_DEVICE_VENDORED") or any(
            _VENDORED_DEVICE in str(Path(p).resolve()) for p in (include, library) if p):
        raise BuildError(
            f"{build_dir}: a measured campaign needs the device library itself; "
            f"ARTS_FAM_DEVICE_VENDORED / {_VENDORED_DEVICE} is for development runs")
    if not include or not Path(include).is_dir() or not library or not Path(library).is_file():
        raise BuildError(
            f"{build_dir} needs real ARTS_FAM_DEVICE_INCLUDE_DIR and "
            "ARTS_FAM_DEVICE_LIBRARY. Configure with -DARTS_FAM_BACKEND=DEVICE "
            "-DARTS_FAM_DEVICE_VENDORED=OFF and both paths."
        )
    for key, want, have in (("ARTS_FAM_DEVICE_INCLUDE_DIR", selection.fam_device_include_dir, include),
                            ("ARTS_FAM_DEVICE_LIBRARY", selection.fam_device_library, library)):
        if want and Path(want).resolve() != Path(have).resolve():
            raise BuildError(f"{build_dir} has {key}={have}, but this campaign requested {want}")


def counter_config_of(build_dir: Path) -> str | None:
    """Which counter file this tree was configured against."""
    return _cache_value(build_dir, "ARTS_COUNTER_CONFIG")


def fam_backend_of(build_dir: Path) -> str | None:
    """Which ARTS_FAM_BACKEND this tree was configured with.

    None for a tree that predates the option or names none, which the
    eligibility gate reads the same way as OFF.
    """
    return _cache_value(build_dir, "ARTS_FAM_BACKEND")


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
                     selection: Selection | None = None) -> None:
    """Configure a tree that never was; verify one that already is.

    The experiment tree is fully determined — Release, benchmarks on — so a
    missing one is a first run rather than an error, and the configure is
    simply run.  A tree that EXISTS is only verified, never reconfigured
    behind its owner's back: a Debug or no-benchmark tree is somebody's
    deliberate configuration, and the counter reconfigure elsewhere changes
    exactly one option for the same reason.  A dry run configures nothing —
    dry means dry — and reports what a real run would do instead.
    """
    fresh = False
    if not (build_dir / "build.ninja").is_file():
        if not bootstrap:
            raise BuildError(
                f"{build_dir} is not a configured build tree; a real run "
                f"configures it first (Release, benchmarks on)"
            )
        fresh = True
        if shutil.which("cmake") is None:
            raise BuildError("cmake not found on PATH")
        from artsrun.paths import repo_root

        say = on_line or (lambda _msg: None)
        say(f"{build_dir} does not exist yet — configuring it "
            "(the first build also compiles the vendored dependencies)")
        # The fabric-attached entries are benchmark variants that carry their
        # own protocol, so a FAM-device tree names only the backend and the
        # library; the tree's own protocol stays at its default.
        fam_opts = []
        if selection and selection.cxl:
            if not selection.fam_device_include_dir or not selection.fam_device_library:
                raise BuildError("a new FAM-device build requires "
                                 "--fam-device-include-dir and --fam-device-library "
                                 "(the device library itself)")
            fam_opts = ["-DARTS_FAM_BACKEND=DEVICE", "-DARTS_FAM_DEVICE_VENDORED=OFF",
                        f"-DARTS_FAM_DEVICE_INCLUDE_DIR={selection.fam_device_include_dir}",
                        f"-DARTS_FAM_DEVICE_LIBRARY={selection.fam_device_library}"]
        proc = subprocess.Popen(
            [*(prefix or []), "cmake", "-S", str(repo_root()), "-GNinja",
             f"-B{build_dir}", "-DCMAKE_BUILD_TYPE=Release", *fam_opts],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        tail: list[str] = []
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip("\n")
            tail.append(line)
            del tail[:-25]
            say(line)
        if proc.wait() != 0:
            raise BuildError("configure failed:\n" + "\n".join(tail))
    if selection is not None:
        require_cxl(build_dir, selection)
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
    benchset: Benchset,
    build_dir: Path,
    profile: Profile | None = None,
    fam_backend: str | None = None,
) -> BuildPlan:
    """Every executable this campaign will run, deduplicated.

    The same rows and the same eligibility the expansion applies: a name the
    benchset does not enable runs no cell, so it needs nothing built, and a
    row's own exclusions (ARTS-only, no ocr-vx, outside DB-WRF) hold for the
    build exactly as they hold for the run — as does an entry this tree has
    no backend for or a reference this host cannot place, each reported
    with its reason rather than as a target that is missing.
    """
    from artsrun.run.plan import fam_ineligible, reference_ineligible

    entries = [plane.entry(k) for k in selection.entries]
    resolved = {a.key: a for a in benchset.resolve(catalog)}

    wanted: list[str] = []
    for name, versions in selection.apps.items():
        for version in versions:
            app = resolved.get(f"{name}:{version.value}")
            if app is None:
                continue
            for entry in entries:
                if profile is not None and (
                        fam_ineligible(entry, profile, fam_backend)
                        or reference_ineligible(entry, profile)):
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
