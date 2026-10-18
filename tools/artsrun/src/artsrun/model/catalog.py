"""The application catalog: what each benchmark structurally is.

Structural facts (binary name, completion marker, which versions exist, which
node counts it can run) ship with the tool.  Everything a campaign changes —
which apps run, with which arguments — lives in an experiment instead.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from functools import lru_cache

from pydantic import BaseModel, Field, model_validator

from artsrun.data import load_data
from artsrun.model.plane import RuntimeKind
from artsrun.paths import repo_root


class AppClass(StrEnum):
    TASK = "task"
    SPMD = "spmd"
    MW = "mw"


class Kind(StrEnum):
    """What the program is, as opposed to whether a campaign runs it.

    APP: cited by name — graph500, HPCG, CoMD, XSBench, STREAM — so a result
    about it means something to a reader who never saw this catalog.  A
    variant of a cited application stays an app even when it duplicates a
    sibling — that is a reason to leave it off a campaign, which
    `default_enabled` says, not a reason to reclassify it.

    TOY: a fixture, library driver, or idiom study — named after the
    operation it performs (a global sum, a reduction driver, a data-block
    create loop) or after nothing at all; it exercises one runtime mechanism
    and belongs in a regression suite rather than in any measurement.

    ATTACK: an adversarial characterization probe, written FOR measurement —
    its knobs move a designed workload across the coherence design space
    (read/write mix, sharers, handoff shapes) to force each arm's best and
    worst case by construction.  Toys check that a mechanism works; attacks
    measure what a mechanism costs when the workload is built against it.
    """

    APP = "app"
    TOY = "toy"
    ATTACK = "attack"


class Section(StrEnum):
    """Which catalog file a row comes from, and so which program each runtime
    kind runs for it.  A runtime kind the section names no program for is
    never paired with its rows: such a pair is not a cell, and a campaign
    records it once per section rather than once per cell."""

    OCR = "ocr"
    HPX = "hpx"
    EXAMPLES = "examples"

    @property
    def spec(self) -> "SectionSpec":
        return SECTIONS[self]

    @property
    def label(self) -> str:
        return self.spec.label

    def runs(self, kind: RuntimeKind) -> bool:
        return kind in self.spec.programs

    def binary_dir(self, kind: RuntimeKind) -> str:
        """Where, under the build root, this runtime kind's binary of a row
        is built."""
        return self.spec.programs[kind].directory

    @property
    def reach(self) -> str:
        """Which runtime kinds run the section and what each runs, as one
        clause; kinds running the same program share a phrase."""
        groups: dict[str, list[RuntimeKind]] = {}
        for kind, program in self.spec.programs.items():
            groups.setdefault(program.what, []).append(kind)
        phrases = []
        for what, kinds in groups.items():
            names = ["the ARTS entries" if k is RuntimeKind.ARTS else k.value
                     for k in kinds]
            who = (names[0] if len(names) == 1
                   else f"{', '.join(names[:-1])} and {names[-1]}")
            plural = len(kinds) > 1 or kinds[0] is RuntimeKind.ARTS
            phrases.append(f"{who} {'run' if plural else 'runs'} {what}")
        return "; ".join(phrases)

    def stem(self, name: str) -> str | None:
        """The name without the section's row suffix, or None when the
        section has none or the name does not carry it."""
        suffix = self.spec.suffix
        if suffix and name.endswith(suffix) and len(name) > len(suffix):
            return name[: -len(suffix)]
        return None


@dataclass(frozen=True)
class Program:
    """What one runtime kind runs of a section's row, and the directory under
    the build root its binary is built in."""

    what: str
    directory: str


@dataclass(frozen=True)
class SectionSpec:
    label: str
    file: str
    # The build option whose ON makes a benchmark tree build the section.
    option: str
    programs: dict[RuntimeKind, Program]
    # Every row of a suffixed section is named `<stem><suffix>`, its binary
    # is its name, and it has one tier; no row of another section carries
    # the suffix.
    suffix: str | None = None
    # The section's structural documents, relative to the checkout, as
    # `<docs>/<stem>.md`; None for the documents packaged with the tool.
    docs: str | None = None


_OCR_PROGRAM = Program("the OCR program", "benchmarks/apps")
_OCR_MIRROR = Program("the OCR mirror", "benchmarks/apps")

SECTIONS: dict[Section, SectionSpec] = {
    Section.OCR: SectionSpec(
        label="OCR applications",
        file="ocr_apps.yaml",
        option="ARTS_BUILD_OCR_APPS",
        programs={RuntimeKind.ARTS: _OCR_PROGRAM,
                  RuntimeKind.XSOCR: _OCR_PROGRAM,
                  RuntimeKind.OCRVX: _OCR_PROGRAM},
    ),
    Section.HPX: SectionSpec(
        label="HPX applications",
        file="hpx_apps.yaml",
        option="ARTS_BUILD_HPX_APPS",
        programs={RuntimeKind.ARTS: _OCR_MIRROR,
                  RuntimeKind.XSOCR: _OCR_MIRROR,
                  RuntimeKind.OCRVX: _OCR_MIRROR,
                  RuntimeKind.HPX: Program("the HPX original",
                                           "benchmarks/hpx")},
        suffix="_hpx",
    ),
    Section.EXAMPLES: SectionSpec(
        label="ARTS examples",
        file="examples.yaml",
        option="ARTS_BUILD_EXAMPLES",
        programs={RuntimeKind.ARTS: Program("the native program", "examples")},
        suffix="_example",
        docs="docs/examples",
    ),
}


class Version(StrEnum):
    """Which build of an application a cell runs.

    BASE is the application as published, plus the disclosed
    structure-preserving conformance adaptations every version and runtime
    shares — including whatever hints its authors shipped.  HINTED changes
    no code structure: it only adds or changes placement hints on EDTs and
    DBs, as far as hints alone can carry the program.  RESTRUCTURED
    redesigns the task/data decomposition and is registered as its own
    target.
    """

    BASE = "base"
    HINTED = "hinted"
    RESTRUCTURED = "restructured"


class ScalarKind(StrEnum):
    FLOAT = "float"
    INT = "int"
    BOOL = "bool"


class AppEntry(BaseModel):
    name: str
    binary: str
    cls: AppClass = Field(alias="class")
    kind: Kind = Kind.APP
    # Where the program came from, in one line: the suite that cites it,
    # or what it was written to exercise.
    provenance: str | None = None

    # `marker` proves the run reached its result; `scalar_re` reads that
    # result.  They are usually the same line, and default to each other.
    marker: str
    scalar_re: str | None = None
    scalar_kind: ScalarKind = ScalarKind.FLOAT
    # A pinned answer is only an answer for the workload it was derived from,
    # so it is carried with those arguments and applied only when the campaign
    # runs exactly them.
    expect: str | None = None
    expect_args: list[str] = Field(default_factory=list)
    tolerance: float = 0.0
    extra_scalars: dict[str, str] = Field(default_factory=dict)

    args: list[str] = Field(default_factory=list)
    args_by_nodes: dict[int, list[str]] = Field(default_factory=dict)

    # How many tasks this row's calibrated arguments make ready AT ONCE at
    # the campaign's widest geometry.  It is the property a strong-scaling
    # sweep depends on and the arguments alone do not show: a run narrower
    # than the machine measures the program's shape rather than the
    # runtime's.  Optional — a row that has not been sized yet declares
    # nothing and is not checked.  The check itself lives with the
    # selection, which is where the machine is known.
    # It describes THESE arguments and nothing else: it is a hand-declared
    # number tied to the catalog's own `args`/`args_by_nodes`, so a roster
    # that overrides either has invalidated it and the selection says so
    # rather than reporting a width the cell will not reach.
    width_max: int | None = Field(default=None, ge=1)

    hinted: bool = False
    restructured_as: str | None = None
    restructured_from: str | None = None

    section: Section = Section.OCR

    # Optional post-run verifier: a shell command run in the cell's working
    # directory after the binary exits 0 (chained with &&, so its exit status
    # fails the cell through the ordinary rc plumbing).  {repo} expands like
    # every other catalog path.  Declarative on purpose: each application
    # registers a command, not code.
    post_verify: str | None = None

    default_enabled: bool = True
    # An application whose correctness depends on semantics the runtime
    # deliberately does not implement.  It stays a visible row — the doc and
    # the reason are the point — but it cannot be selected or run.
    unsupported: str | None = None
    # The program leaves two write-mode acquisitions of one DataBlock
    # unordered somewhere, so it is outside the DB-WRF memory model: an entry
    # whose protocol requires DB-WRF cannot run it, and says why.  One value per
    # row — base and hinted are one program; a rewrite carries its own.
    unordered_writes: str | None = None
    multinode_skip: str | None = None
    ocrvx_skip: bool = False
    # A probe built for the ARTS variants alone: it exists to separate the
    # coherence plane's arms, so no reference runtime has a target for it.
    arts_only: bool = False
    fixtures: list[str] = Field(default_factory=list)
    timeout: int = 0
    multinode_timeout: int = 0

    model_config = {"populate_by_name": True}

    @property
    def result_re(self) -> str:
        return self.scalar_re or self.marker

    def timeout_for(self, nodes: int) -> int:
        """Per-cell wall budget, or 0 to use the profile's."""
        if nodes > 1 and self.multinode_timeout:
            return self.multinode_timeout
        return self.timeout

    def args_for(self, nodes: int) -> list[str]:
        return self.args_by_nodes.get(nodes, self.args)

    @property
    def own_versions(self) -> list[Version]:
        """Versions this entry offers as a row of its own.

        A restructured version is a separate application target, but it
        belongs in the row of the application it re-implements: the three
        versions are one program's answers to the same problem, in increasing
        order of how much of it was rewritten.
        """
        v = [Version.BASE]
        if self.hinted:
            v.append(Version.HINTED)
        if self.restructured_as:
            v.append(Version.RESTRUCTURED)
        return v

    @property
    def hpx_versions(self) -> list[Version]:
        return [Version.BASE] if self.section.runs(RuntimeKind.HPX) else []

    def hpx_target(self, version: Version) -> str | None:
        """The HPX program's build target, base tier only, on a row of a
        section the HPX entry runs: the runtime tag is the last token, as for
        every reference binary (`<stem>_xsocr`, `<stem>_ocrvx`).  A row of
        any other section has none."""
        if self.section.runs(RuntimeKind.HPX) and version is Version.BASE:
            return f"{self.binary}_hpx"
        return None

    @model_validator(mode="after")
    def _check_section(self) -> "AppEntry":
        spec = self.section.spec
        if spec.suffix:
            if self.section.stem(self.name) is None or self.binary != self.name:
                raise ValueError(
                    f"{self.name}: a row of the {spec.label} is named"
                    f" `<stem>{spec.suffix}` and its binary is that name"
                    f" ({self.section.reach})")
            if self.hinted or self.restructured_as or self.restructured_from:
                raise ValueError(
                    f"{self.name}: a row of the {spec.label} has one tier —"
                    " no hint layer or rewrite exists for it")
        for other in Section:
            if other is not self.section and other.stem(self.name) is not None:
                raise ValueError(
                    f"{self.name}: the suffix `{other.spec.suffix}` names a row"
                    f" of the {other.spec.label}; this row is in the"
                    f" {spec.label}")
        if self.arts_only and not any(
                self.section.runs(kind) for kind in RuntimeKind
                if kind is not RuntimeKind.ARTS):
            raise ValueError(
                f"{self.name}: the {spec.label} run on the ARTS entries alone,"
                " so `arts_only` restates the section")
        return self


class Catalog(BaseModel):
    apps: dict[str, AppEntry]

    @model_validator(mode="after")
    def _check_twins(self) -> "Catalog":
        for name, app in self.apps.items():
            if app.restructured_as and app.restructured_as not in self.apps:
                raise ValueError(
                    f"{name}: restructured_as '{app.restructured_as}' not in catalog")
            if app.restructured_from and app.restructured_from not in self.apps:
                raise ValueError(
                    f"{name}: restructured_from '{app.restructured_from}' not in catalog")
        return self

    @property
    def rows(self) -> list[AppEntry]:
        """Apps that head a row: every app that is not another app's rewrite."""
        return [a for a in self.apps.values() if a.restructured_from is None]

    def rows_in(self, section: Section) -> list[AppEntry]:
        """One section's rows, in name order."""
        return sorted((a for a in self.rows if a.section is section),
                      key=lambda a: a.name.lower())

    def rows_of(self, kind: Kind) -> list[AppEntry]:
        """The OCR applications section's rows of one kind, in name order:
        that section is the one whose kinds are drawn as groups of their
        own."""
        return [a for a in self.rows_in(Section.OCR) if a.kind is kind]

    def resolve(self, name: str, version: Version) -> tuple[AppEntry, str]:
        """The catalog entry that supplies the arguments, and the binary stem.

        The `dist` version runs a different target than the row it appears in,
        and that target carries its own calibration.
        """
        app = self.apps[name]
        if version is Version.RESTRUCTURED:
            if not app.restructured_as:
                raise KeyError(f"{name} has no restructured version")
            other = self.apps[app.restructured_as]
            return other, other.binary
        if version is Version.HINTED:
            if not app.hinted:
                raise KeyError(f"{name} has no hinted version: its source "
                               "carries no hint layer")
            return app, f"{app.binary}_hinted"
        return app, app.binary


def expand_repo(value, root: str):
    """Resolve `{repo}` in argument lists against this checkout.

    An application that reads an input file is given an absolute path, since
    a cell runs from the scratch directory rather than the checkout.  The
    catalog is committed, so it cannot carry one machine's absolute path.
    """
    if isinstance(value, str):
        return value.replace("{repo}", root)
    if isinstance(value, list):
        return [expand_repo(v, root) for v in value]
    if isinstance(value, dict):
        return {k: expand_repo(v, root) for k, v in value.items()}
    return value


def merge_sections(sections, root: str) -> dict[str, AppEntry]:
    """Fold the catalog's sections into one namespace.

    A row is addressed by name alone everywhere downstream — on the command
    line, in a roster, in a result — so one name may head only one row, and a
    name that appears in two sections is rejected rather than resolved by
    section order.
    """
    apps: dict[str, AppEntry] = {}
    for entries, section in sections:
        for name, spec in entries.items():
            if name in apps:
                raise ValueError(
                    f"{name}: named in two catalog sections "
                    f"({apps[name].section.value} and {section.value})")
            spec = dict(spec)
            spec.setdefault("name", name)
            spec.setdefault("binary", name)
            spec["section"] = section.value
            apps[name] = AppEntry.model_validate(expand_repo(spec, root))
    return apps


@lru_cache(maxsize=1)
def load_catalog() -> Catalog:
    root = str(repo_root())
    return Catalog(apps=merge_sections(
        ((load_data(section.spec.file)["apps"], section)
         for section in Section),
        root))
