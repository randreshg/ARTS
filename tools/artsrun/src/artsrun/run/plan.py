"""Expand a selection into the cells it runs, and record what it cannot.

A cell that is dropped is dropped for a structural reason — the application
cannot run multinode, or a reference runtime cannot run it at all.  The reason
is carried through to the report; nothing is dropped silently, and nothing is
dropped because a run once failed.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import replace
from pathlib import Path

from artsrun.model.benchset import Benchset, ResolvedApp
from artsrun.model.catalog import Catalog
from artsrun.model.plane import Plane, RuntimeKind, SelectionEntry
from artsrun.model.profile import Launcher, Profile
from artsrun.model.selection import Selection
from artsrun.run.types import Cell, Skipped


def _missing_inputs(app: ResolvedApp) -> list[str]:
    """Declared inputs the application needs that this machine does not have.

    Cheap deterministic fixtures are synthesized on the spot (fixtures.stage);
    the rest are too expensive to regenerate on every host and are staged onto
    it instead.  A cell whose input is absent has nothing to measure, and
    running it would report a failure of the runtime for what is a property of
    the machine — so it is dropped the way a structurally ineligible cell is.

    The paths are declared rather than inferred from the arguments: an
    application is handed the paths it writes as well as the ones it reads,
    and only the catalog knows which is which.
    """
    from artsrun.fixtures import stage

    return stage(app.fixtures)


def fam_ineligible(
    entry: SelectionEntry, profile: Profile, fam_backend: str | None
) -> str | None:
    """Why a fabric-attached-memory entry cannot run here.

    The backend is a property of the build tree — one tree builds both
    residencies of whichever backend it was configured with — so this
    never depends on the application, and the build planner asks it the
    same question the expansion does.
    """
    if not entry.is_fam:
        return None
    if fam_backend in (None, "OFF"):
        return ("this build tree names no ARTS_FAM_BACKEND, so the "
                "fabric-attached-memory entries have no binaries in it")
    if fam_backend == "SHM" and profile.launcher is not Launcher.LOCAL:
        return ("the SHM backend's pool is one machine's shared "
                "mapping, so it runs only under launcher: local")
    if fam_backend == "DEVICE" and profile.fam_strict:
        return ("fam_strict is an oracle for a second coherency domain, "
                "which the DEVICE backend refuses at config load")
    return None


def reference_ineligible(entry: SelectionEntry, profile: Profile) -> str | None:
    """Why a reference cannot be placed one thread per core on this host.

    Every reference rank runs inside an envelope of absolute CPU ids — the
    block r*width..(r+1)*width-1 for colocated rank r — that must consist of
    per-core first SMT threads.  Where the host numbers a core's threads
    adjacently, no block of width >= 2 does, so the cells are dropped rather
    than failed by the envelope or run on different hardware from the other
    entries.  xsocr has a second reason of its own: it pins thread j of rank
    r to the absolute id j + width*r, replacing the mask it inherited.
    Local only: a remote rank's host is not the one whose topology is
    visible here, and the envelope still checks it there.
    """
    from artsrun.model.selection import siblings_interleaved

    if not entry.is_reference:
        return None
    if profile.launcher is not Launcher.LOCAL or profile.threads_per_node < 2:
        return None
    if not siblings_interleaved():
        return None
    return ("the reference envelope binds absolute cpu id blocks, which "
            "cannot be one thread per core on a host whose numbering "
            "interleaves SMT siblings; its cells run on a host that numbers "
            "every core's first thread first")


def _ineligible(
    entry: SelectionEntry, app: ResolvedApp, nodes: int,
    profile: Profile | None = None, fam_backend: str | None = None,
) -> str | None:
    # Belt over the selection surfaces' braces: a replayed selection.yaml can
    # predate the catalog marking an application unsupported.
    if app.unsupported:
        return f"application is unsupported: {app.unsupported}"
    if nodes > 1 and app.multinode_skip:
        return f"application cannot run multinode: {app.multinode_skip}"
    if entry.kind is RuntimeKind.OCRVX and app.ocrvx_skip:
        return "application uses OCR extensions this reference does not implement"
    if entry.is_reference and app.arts_only:
        return "probe is built for the ARTS variants alone"
    if entry.model == "DB_WRF" and app.unordered_writes:
        return f"program is outside DB-WRF: {app.unordered_writes}"
    if entry.kind is RuntimeKind.HPX and app.hpx_binary is None:
        return ("no HPX program: an OCR-origin row (the HPX entry runs the "
                "HPX-origin section)")
    if profile is not None:
        why = (fam_ineligible(entry, profile, fam_backend)
               or reference_ineligible(entry, profile))
        if why:
            return why
    absent = _missing_inputs(app)
    if absent:
        return "input not staged on this machine: " + ", ".join(absent)
    return None


def expand(
    selection: Selection,
    plane: Plane,
    catalog: Catalog,
    benchset: Benchset,
    profile: Profile,
    apps_dir: Path,
    configs: dict[int, dict[str, Path]],
    cell_cfg: Callable[[Cell], Path] | None = None,
    fam_backend: str | None = None,
) -> tuple[list[Cell], list[Skipped]]:
    """Expand into cells.

    `cell_cfg`, when given, replaces an ARTS cell's shared configuration with
    one of its own.  Counters are the reason it exists: their output directory
    is a configuration key, so cells sharing a configuration would write over
    each other's counters.
    """
    from artsrun.render import config_for

    resolved = {a.key: a for a in benchset.resolve(catalog)}
    entries = [plane.entry(k) for k in selection.entries]

    cells: list[Cell] = []
    skipped: list[Skipped] = []

    for name, versions in selection.apps.items():
        for version in versions:
            app = resolved.get(f"{name}:{version.value}")
            if app is None:
                skipped.append(
                    Skipped("*", f"{name}:{version.value}", 0,
                            "not enabled in the benchset")
                )
                continue
            for nodes in selection.node_counts:
                runnable: list[tuple] = []
                for entry in entries:
                    why = _ineligible(entry, app, nodes, profile, fam_backend)
                    if why:
                        skipped.append(Skipped(entry.key, app.key, nodes, why))
                        continue
                    if entry.kind is RuntimeKind.HPX:
                        # The HPX apps are a standalone project beside the
                        # OCR apps in the build tree, one target per program.
                        binary = apps_dir.parent / "hpx" / app.hpx_binary
                    else:
                        binary = apps_dir / entry.binary(app.binary,
                                                         hinted=False)
                    runnable.append((entry, binary))
                for repeat in range(1, selection.repeats + 1):
                    for entry, binary in runnable:
                        cell = Cell(
                            entry=entry,
                            app=app,
                            nodes=nodes,
                            repeat=repeat,
                            binary=binary,
                            args=app.args_for(nodes),
                            timeout_s=app.timeout_for(nodes) or profile.cell_timeout_s,
                            cfg=config_for(entry.kind, configs[nodes]),
                            cpu_width=profile.threads_per_node,
                            cxl=selection.cxl,
                        )
                        if cell_cfg is not None and entry.kind is RuntimeKind.ARTS:
                            cell = replace(cell, cfg=cell_cfg(cell))
                        cells.append(cell)
    return cells, skipped
