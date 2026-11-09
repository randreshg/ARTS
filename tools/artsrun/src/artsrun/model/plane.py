"""The coherence-configuration plane and the entries selectable on it."""

from __future__ import annotations

from enum import StrEnum
from functools import lru_cache

from pydantic import BaseModel, Field

from artsrun.data import load_data


class Validity(StrEnum):
    EXCL = "EXCL"
    INV = "INV"
    VAL = "VAL"


class Retention(StrEnum):
    PURGE = "PURGE"
    RETAIN = "RETAIN"


class ReadSource(StrEnum):
    HOME = "HOME"
    OWNER = "OWNER"


class RuntimeKind(StrEnum):
    ARTS = "arts"
    XSOCR = "xsocr"
    OCRVX = "ocrvx"
    HPX = "hpx"


def _arts_key(variant: str) -> str:
    """The name an ARTS configuration is selected and reported by.

    an OCR-model variant drops its `ocr_` token, which the grid already
    fixes; a variant of another model keeps the model token, which is
    exactly what distinguishes it
    """
    return "arts_" + variant.removeprefix("ocr_")


class SelectionEntry(BaseModel):
    """One selectable runtime configuration.

    `key` is the stable identifier used on the command line, in saved
    selections and in result rows: an ARTS entry is named by the
    configuration it selects, a reference entry by the runtime.  An entry
    whose `cell` is None sits on no plane position: a cross-model reference,
    selectable like any entry but tied to no coherence design point.  `model`
    is the memory model the entry's protocol requires of the program.
    """

    key: str
    label: str
    kind: RuntimeKind
    cell: str | None = None
    variant: str | None = None
    note: str | None = None
    model: str = "OCR"

    @property
    def is_reference(self) -> bool:
        return self.kind is not RuntimeKind.ARTS

    @property
    def is_external(self) -> bool:
        return self.cell is None

    @property
    def is_cxl(self) -> bool:
        """Whether this entry's store is CXL memory.

        Read off the variant name, which the naming contract gives an
        internal `_cxl_` token, rather than a field: nothing at the model
        level needs more than the distinction.
        """
        return self.variant is not None and "_cxl_" in self.variant

    def binary(self, app_binary: str, *, hinted: bool) -> str:
        """Executable name this entry runs a given application under.

        A cross-model reference does not run the OCR program in any version:
        its HPX-origin program is one target of its own, named from the
        catalog's base binary — the caller resolves it, so this mapping never
        applies.
        """
        if self.kind is RuntimeKind.HPX:
            raise ValueError("an HPX program is named by the catalog, not "
                             "derived from a version stem")
        stem = f"{app_binary}_hinted" if hinted else app_binary
        if self.kind is RuntimeKind.ARTS:
            return f"{stem}_arts_{self.variant}"
        return f"{stem}_{self.kind.value}"


def summarize_entries(entries: list[SelectionEntry]) -> str:
    """Entries in a few words: the ARTS entries counted, every other one
    named, in the order given."""
    arts = sum(1 for e in entries if e.kind is RuntimeKind.ARTS)
    words = [f"{arts} ARTS entr{'y' if arts == 1 else 'ies'}"] if arts else []
    words += [e.key for e in entries if e.kind is not RuntimeKind.ARTS]
    return ", ".join(words) or "none"


class PlaneCell(BaseModel):
    validity: Validity
    retention: Retention
    read_source: ReadSource
    variant: str | None = None
    reference: str | None = None
    reason: str | None = None
    entries: list[PlaneCellEntry] = Field(default_factory=list)

    @property
    def key(self) -> str:
        return f"{self.validity}/{self.retention}/{self.read_source}"

    @property
    def buildable(self) -> bool:
        return self.variant is not None


class PlaneCellEntry(BaseModel):
    """A further ARTS configuration a cell offers beside its base one.

    Most cells offer one ARTS configuration and, on two of them, the
    external runtime that implements the same design point; a cell whose
    comparison has more than one ARTS configuration of its own names each
    here instead of growing a fixed set of fields.
    """

    label: str
    variant: str


class ModelCell(BaseModel):
    """One arm of a memory model that is not the OCR grid's."""

    model: str
    arm: str
    label: str
    variant: str
    note: str | None = None

    @property
    def key(self) -> str:
        return f"{self.model}/{self.arm}"


class ModelSection(BaseModel):
    model: str
    label: str
    note: str
    cells: list[ModelCell]


class Plane(BaseModel):
    validities: list[Validity]
    retentions: list[Retention]
    read_sources: list[ReadSource]
    validity_labels: dict[Validity, str] = Field(default_factory=dict)
    retention_labels: dict[Retention, str] = Field(default_factory=dict)
    read_source_labels: dict[ReadSource, str] = Field(default_factory=dict)
    cells: list[PlaneCell]
    entries: list[SelectionEntry]
    models: list[ModelSection] = Field(default_factory=list)

    # -- lookup ----------------------------------------------------------
    def cell(self, validity: Validity, retention: Retention,
             read_source: ReadSource) -> PlaneCell:
        for c in self.cells:
            if (c.validity is validity and c.retention is retention
                    and c.read_source is read_source):
                return c
        raise KeyError(f"{validity}/{retention}/{read_source}")

    def entry(self, key: str) -> SelectionEntry:
        for e in self.entries:
            if e.key == key:
                return e
        raise KeyError(key)

    def entries_of(self, cell: PlaneCell | ModelCell) -> list[SelectionEntry]:
        return [e for e in self.entries if e.cell == cell.key]

    @property
    def entry_keys(self) -> list[str]:
        return [e.key for e in self.entries]

    def ordered(self, named: list[str], owner: str) -> list[str]:
        """These entries in the plane's order, deduplicated.  A name the plane
        does not have is refused rather than dropped — a list that silently
        shrank would leave a study running less than it states.  `owner` is
        what named them, for the message."""
        unknown = [k for k in named if k not in self.entry_keys]
        if unknown:
            raise ValueError(
                f"{owner} names unknown plane entries: {', '.join(unknown)}")
        return [k for k in self.entry_keys if k in named]

    @property
    def standard_entries(self) -> list[str]:
        """The entries any program can run on any node profile: the OCR-model
        ones whose store is not CXL memory.  Another memory model obliges the
        program, and a CXL store's entries are chosen per run."""
        return [e.key for e in self.entries
                if e.model == "OCR" and not e.is_cxl]

    def columns(self) -> list[tuple[Retention, ReadSource]]:
        """Column order: the retention groups, the read source divides.

        The axes keep their order, validity -> retention -> read source: the
        retention forms the outer pair and the read source splits each half.
        """
        return [(r, s) for r in self.retentions for s in self.read_sources]

    def retention_label(self, retention: Retention) -> str:
        return self.retention_labels.get(retention, retention.value)

    def read_source_label(self, read_source: ReadSource) -> str:
        return self.read_source_labels.get(read_source, read_source.value)


def _reason(validity: Validity, retention: Retention, read_source: ReadSource,
            texts: dict) -> str:
    """Why this grid position carries no configuration.

    The rules mirror the build's own refusals, so a disabled position always
    reports the same ground the build would.
    """
    if validity is Validity.EXCL:
        return texts["excl_retention_sets_read_source"]
    return texts["owner_needs_retain"]


@lru_cache(maxsize=1)
def load_plane() -> Plane:
    raw = load_data("protocols.yaml")
    validities = [Validity(v) for v in raw["validities"]]
    retentions = [Retention(r) for r in raw["retentions"]]
    read_sources = [ReadSource(s) for s in raw["read_sources"]]

    defined = {
        (Validity(c["validity"]), Retention(c["retention"]),
         ReadSource(c["read_source"])): c
        for c in raw["cells"]
    }
    refs = raw.get("references", {})

    cells: list[PlaneCell] = []
    entries: list[SelectionEntry] = []
    for validity in validities:
        for retention, read_source in [(r, s) for r in retentions
                                       for s in read_sources]:
            spec = defined.get((validity, retention, read_source))
            if spec is None:
                cells.append(
                    PlaneCell(
                        validity=validity,
                        retention=retention,
                        read_source=read_source,
                        reason=_reason(validity, retention, read_source,
                                       raw["unbuildable"]),
                    )
                )
                continue
            cell = PlaneCell(
                validity=validity,
                retention=retention,
                read_source=read_source,
                variant=spec["variant"],
                reference=spec.get("reference"),
                entries=[PlaneCellEntry(**e) for e in spec.get("entries", [])],
            )
            cells.append(cell)
            entries.append(
                SelectionEntry(
                    # The build suffix leads with the memory model, which this
                    # plane fixes, so as a name it says nothing and reads as
                    # the wrong runtime beside the two references.  The entry
                    # names the runtime it selects; the suffix stays the
                    # build's own.
                    key=_arts_key(cell.variant),
                    label=_arts_key(cell.variant),
                    kind=RuntimeKind.ARTS,
                    cell=cell.key,
                    variant=cell.variant,
                )
            )
            for extra in cell.entries:
                entries.append(
                    SelectionEntry(
                        key=_arts_key(extra.variant),
                        label=extra.label,
                        kind=RuntimeKind.ARTS,
                        cell=cell.key,
                        variant=extra.variant,
                    )
                )
            if cell.reference:
                ref = refs[cell.reference]
                entries.append(
                    SelectionEntry(
                        key=cell.reference,
                        label=ref["label"],
                        kind=RuntimeKind(ref["kind"]),
                        cell=cell.key,
                    )
                )

    models: list[ModelSection] = []
    for model, spec in raw.get("models", {}).items():
        cells_m = [ModelCell(model=model, **c) for c in spec["cells"]]
        models.append(ModelSection(model=model, label=spec["label"],
                                   note=spec["note"], cells=cells_m))
        for cell in cells_m:
            entries.append(SelectionEntry(
                key=_arts_key(cell.variant), label=_arts_key(cell.variant),
                kind=RuntimeKind.ARTS, cell=cell.key, variant=cell.variant,
                note=cell.note, model=model))

    for key, ref in raw.get("external_references", {}).items():
        entries.append(
            SelectionEntry(
                key=key,
                label=ref["label"],
                kind=RuntimeKind(ref["kind"]),
                note=ref.get("note"),
            )
        )

    return Plane(
        validities=validities,
        retentions=retentions,
        read_sources=read_sources,
        validity_labels={Validity(k): v
                         for k, v in raw.get("validity_labels", {}).items()},
        retention_labels={Retention(k): v
                          for k, v in raw.get("retention_labels", {}).items()},
        read_source_labels={ReadSource(k): v
                            for k, v in raw.get("read_source_labels", {}).items()},
        cells=cells,
        entries=entries,
        models=models,
    )
