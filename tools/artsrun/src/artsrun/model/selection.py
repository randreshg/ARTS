"""A campaign selection: node profile x experiment x counters x node counts,
resolved and serializable.

A saved selection replays a campaign exactly, so it is written next to every
run's results.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

from pydantic import BaseModel, ConfigDict, Field

from artsrun.model.experiment import Experiment
from artsrun.model.catalog import AppClass, Catalog, Section, Version
from artsrun.model.plane import Plane
from artsrun.model.profile import Launcher, Profile


SYSFS_CPU_ROOT = Path("/sys/devices/system/cpu")


def first_sibling_cpus() -> list[int] | None:
    """The per-core first SMT threads this host exposes, ascending.

    A CPU is a first thread when it heads its own sibling list.  None when
    the topology is unreadable; the envelope wrapper re-checks the property
    per cell either way.
    """
    root = SYSFS_CPU_ROOT
    try:
        cpus = [p for p in root.iterdir() if re.fullmatch(r"cpu\d+", p.name)]
    except OSError:
        return None
    firsts: list[int] = []
    for p in cpus:
        f = p / "topology" / "thread_siblings_list"
        if not f.is_file():
            f = p / "topology" / "core_cpus_list"
        try:
            first = re.split(r"[,-]", f.read_text().strip(), maxsplit=1)[0]
        except OSError:
            continue
        if first == p.name[3:]:
            firsts.append(int(first))
    return sorted(firsts) or None


def siblings_interleaved() -> bool:
    """Whether a core's second SMT thread is numbered before the last core's
    first one, i.e. the first threads are not the prefix 0..cores-1.

    A runtime that pins its threads to absolute CPU ids 0..width-1 lands one
    thread per core only when they are.  False when the topology is
    unreadable.
    """
    firsts = first_sibling_cpus()
    return firsts is not None and firsts != list(range(len(firsts)))


def _first_sibling_cpu_count() -> int | None:
    """How many per-core first SMT threads this host exposes.

    The envelope confines ranks to per-core first threads, so this — not the
    logical CPU count — is the budget colocated local blocks must fit in.
    """
    firsts = first_sibling_cpus()
    return len(firsts) if firsts else None


@dataclass(frozen=True)
class SectionShare:
    """One section's part of a campaign: the selected entries it has a
    program for, times its selected application versions, is its cells per
    node count and repeat.  `section` is None for names the catalog no
    longer has, counted on every selected entry."""

    section: Section | None
    entries: int
    app_versions: int

    @property
    def pairs(self) -> int:
        return self.entries * self.app_versions


class Selection(BaseModel):
    # A key no field takes is refused: loading past it would replay the
    # selection as something it never was.
    model_config = ConfigDict(extra="forbid")

    profile: str
    experiment: str
    # The experiment's own entry list, recorded beside the entries this
    # campaign actually runs so a reader can tell a default run from one
    # whose entries were chosen for it.
    default_entries: list[str]
    entries: list[str] = Field(min_length=1)
    apps: dict[str, list[Version]] = Field(min_length=1)
    node_counts: list[int] = Field(min_length=1)
    repeats: int = 1
    build_dir: str | None = None

    def validate_against(
        self,
        plane: Plane,
        catalog: Catalog,
        profile: Profile,
        experiment: Experiment | None = None,
    ) -> None:
        unknown = [k for k in self.entries if k not in plane.entry_keys]
        if unknown:
            raise ValueError(f"unknown plane entries: {', '.join(unknown)}")
        # A repeat would schedule its cells twice, so it is refused like an
        # unknown key rather than folded.
        for what, items in (
            ("plane entries", self.entries),
            ("node counts", self.node_counts),
            *((f"{name} versions", vs) for name, vs in self.apps.items()),
        ):
            repeated = list(dict.fromkeys(
                str(i) for i in items if items.count(i) > 1))
            if repeated:
                raise ValueError(
                    f"{what} named more than once: {', '.join(repeated)}")
        profile.check_cxl(self.cxl_entries(plane))
        for name, versions in self.apps.items():
            if name not in catalog.apps:
                raise ValueError(f"unknown application: {name}")
            available = catalog.apps[name].own_versions
            bad = [v for v in versions if v not in available]
            if bad:
                raise ValueError(
                    f"{name}: no {', '.join(v.value for v in bad)} version"
                )
        off_sweep = [n for n in self.node_counts if n not in profile.nodes]
        if off_sweep:
            raise ValueError(
                f"node counts {off_sweep} are not in profile "
                f"'{profile.name}' sweep {profile.nodes}"
            )
        self._check_width(catalog, profile, experiment)
        # Only a reference that runs some selected application is a reference
        # this campaign places.
        sections = self.sections(catalog, experiment)
        if any(plane.entry(k).is_reference
               and any(s.runs(plane.entry(k).kind) for s in sections)
               for k in self.entries):
            self._check_reference_geometry(profile)

    def _check_width(
        self, catalog: Catalog, profile: Profile,
        experiment: Experiment | None = None,
    ) -> None:
        """Refuse a row whose declared width cannot fill the machine.

        `width_max` is a row's instantaneous task width at the widest
        geometry of the profile, so the whole machine is the floor: below it
        the widest cell of a strong-scaling sweep has idle workers for the
        whole run and its number says nothing about the runtime.  An SPMD or
        manager-worker decomposition additionally has to divide evenly —
        its width is a fixed team, and a partial round leaves part of the
        machine idle through every round rather than only at the tail.

        The declaration describes the CATALOG's arguments, so a roster that
        overrides them has invalidated it: such a cell is said out loud and
        then left unchecked rather than judged against a number that belongs
        to a workload it is not running.  Refusing it instead would be the
        wrong trade — shrinking a row is exactly what a smoke roster is for.

        Checked before anything is built, and only for rows that declare a
        width: sizing a row is the campaign's own work, and a row waiting for
        it must not be turned into a build-time error.
        """
        total = profile.max_nodes * profile.workers
        overridden = self._overridden_args(catalog, experiment)
        for name, versions in self.apps.items():
            for version in versions:
                source, _ = catalog.resolve(name, version)
                width = source.width_max
                if width is None:
                    continue
                if (name, version) in overridden:
                    print(
                        f"artsrun: {source.name}: experiment "
                        f"'{experiment.name}' overrides the arguments "
                        f"width_max={width} was declared for — width not "
                        f"checked for this campaign",
                        file=sys.stderr,
                    )
                    continue
                if width < total:
                    raise ValueError(
                        f"{source.name}: width_max={width} is below the "
                        f"{total} workers of profile '{profile.name}' at its "
                        f"widest ({profile.max_nodes} nodes x "
                        f"{profile.workers}); the widest cell would run "
                        f"narrower than the machine"
                    )
                if source.cls in (AppClass.SPMD, AppClass.MW) and width % total:
                    raise ValueError(
                        f"{source.name}: width_max={width} is not a whole "
                        f"multiple of the {total} workers of profile "
                        f"'{profile.name}' at its widest "
                        f"({profile.max_nodes} nodes x {profile.workers}); a "
                        f"{source.cls.value} decomposition divides its work "
                        f"into equal teams, so a partial round idles part of "
                        f"the machine for every round"
                    )

    @staticmethod
    def _overridden_args(
        catalog: Catalog, experiment: Experiment | None
    ) -> set[tuple[str, Version]]:
        """The (row, version) pairs whose arguments the roster replaced."""
        if experiment is None:
            return set()
        return {
            (r.name, r.version) for r in experiment.resolve(catalog)
            if r.args_overridden
        }

    def _check_reference_geometry(self, profile: Profile) -> None:
        """What a reference cell needs from the profile.

        These are selection-time errors rather than render-time ones on
        purpose: the ocr configuration is rendered for every campaign, even
        one that selects no reference, and an arts-only campaign must not be
        refused over a geometry only the references cannot express.
        """
        width = profile.threads_per_node
        if not profile.pin:
            raise ValueError(
                "reference entries require pin: true — an unpinned arts run "
                "floats over the whole machine while the references stay "
                "confined to their envelope, so the two would be measured on "
                "different hardware"
            )
        if width < 2:
            raise ValueError(
                f"reference entries require workers+progress >= 2: at width "
                f"{width} the xsocr configuration degenerates to a "
                f"communication worker with no compute workers"
            )
        if width > 255:
            raise ValueError(
                f"reference entries require workers+progress <= 255: the "
                f"runtime parses its per-core binding policy fields through "
                f"a u8 (width {width} would wrap)"
            )
        if profile.launcher is Launcher.LOCAL:
            nodes = max(self.node_counts)
            if nodes <= 1:
                return
            if nodes > 255:
                raise ValueError(
                    f"colocated reference ranks are capped at 255 ({nodes} "
                    f"requested): the runtime's block policy parses the rank "
                    f"count through a u8"
                )
            if nodes * width > 1024:
                raise ValueError(
                    f"colocated reference blocks reach cpu {nodes * width - 1}, "
                    f"past CPU_SETSIZE (1024): binding there is a silent no-op "
                    f"in glibc"
                )
            cores = _first_sibling_cpu_count()
            if cores is not None and nodes * width > cores:
                raise ValueError(
                    f"colocated reference blocks need {nodes * width} per-core "
                    f"first threads but this host has {cores}"
                )

    @classmethod
    def everything(
        cls,
        plane: Plane,
        catalog: Catalog,
        experiment: Experiment,
        profile: Profile,
        *,
        build_dir: str | None = None,
    ) -> "Selection":
        """The default state of the selection screens: every application the
        experiment enables, on the experiment's own entries."""
        apps = {
            app.name: experiment.versions_for(app)
            for app in catalog.rows
            if experiment.is_enabled(app)
        }
        return cls(
            profile=profile.name,
            experiment=experiment.name,
            default_entries=list(experiment.entries),
            entries=list(experiment.entries),
            apps=apps,
            node_counts=list(profile.nodes),
            repeats=profile.repeats,
            build_dir=build_dir,
        )

    def cxl_entries(self, plane: Plane) -> list[str]:
        return [k for k in self.entries if plane.entry(k).is_cxl]

    def _runs(self, catalog: Catalog,
              experiment: Experiment | None) -> dict[str, list[Version]]:
        """The selected applications that can run a cell.  A name the
        experiment does not enable runs none, so it is left out once the
        experiment is known."""
        if experiment is None:
            return self.apps
        enabled = {a.name for a in experiment.resolve(catalog)}
        return {n: vs for n, vs in self.apps.items() if n in enabled}

    def sections(self, catalog: Catalog,
                 experiment: Experiment | None = None) -> list[Section]:
        """The sections of the selected applications that run, in the
        catalog's order."""
        present = {catalog.apps[n].section
                   for n in self._runs(catalog, experiment)
                   if n in catalog.apps}
        return [s for s in Section if s in present]

    def shares(self, catalog: Catalog, plane: Plane,
               experiment: Experiment | None = None) -> list[SectionShare]:
        """Each section's part of the campaign, in the catalog's order.

        A saved selection can outlive what it names: an application the
        catalog no longer has, or an entry the plane no longer has, cannot
        say what it lacks, so it is counted everywhere it could be."""
        known = {e.key: e.kind for e in plane.entries}
        versions: dict[Section | None, int] = {}
        for name, vs in self._runs(catalog, experiment).items():
            app = catalog.apps.get(name)
            key = app.section if app is not None else None
            versions[key] = versions.get(key, 0) + len(vs)
        out = []
        for section in [*Section, None]:
            if section not in versions:
                continue
            entries = sum(1 for k in self.entries
                          if section is None or k not in known
                          or section.runs(known[k]))
            out.append(SectionShare(section, entries, versions[section]))
        return out

    def cell_count(self, catalog: Catalog, plane: Plane,
                   experiment: Experiment | None = None) -> int:
        """The cells this selection expands to before any row's own
        exception: every selected application version on every selected
        entry its section has a program for, at every node count and
        repeat."""
        pairs = sum(s.pairs for s in self.shares(catalog, plane, experiment))
        return pairs * len(self.node_counts) * self.repeats

    def cell_formula(self, catalog: Catalog, plane: Plane,
                     experiment: Experiment | None = None) -> str:
        """`cell_count` written out: a sum over the sections, each its
        entries times its application versions, then the node counts and
        repeats."""
        terms = [f"{s.section.label if s.section else 'not in the catalog'}: "
                 f"{s.entries} entries x {s.app_versions} app-versions"
                 for s in self.shares(catalog, plane, experiment)]
        total = " + ".join(terms)
        if len(terms) > 1:
            total = f"({total})"
        return (f"{total} x {len(self.node_counts)} node counts "
                f"x {self.repeats} repeats")
