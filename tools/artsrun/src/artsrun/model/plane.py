"""The coherence-configuration plane and the entries selectable on it."""

from __future__ import annotations

from enum import StrEnum
from functools import lru_cache

from pydantic import BaseModel, Field

from artsrun.data import load_data


class Family(StrEnum):
    EXCL = "EXCL"
    INV = "INV"
    VAL = "VAL"


# Selections recorded before the combining promotion name the comb-suffixed
# entries; those keys now denote what is the VAL family's plain configuration.
# Plain val keys recorded before the promotion meant the non-combining build
# and are NOT remapped — silently rewriting them would claim a measurement
# that was never made.  (The nocomb ablation twins are build variants, not
# plane entries.)
LEGACY_ENTRY_ALIASES = {
    "arts_val_wt_comb": "arts_val_wt",
    "arts_val_wb_comb": "arts_val_wb",
    "arts_val_wt_purge_comb": "arts_val_wt_purge",
}


def modern_entry_key(key: str) -> str:
    return LEGACY_ENTRY_ALIASES.get(key, key)


class Release(StrEnum):
    PURGE = "PURGE"
    RETAIN = "RETAIN"


class Write(StrEnum):
    WT = "WT"
    WB = "WB"


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


class PlaneCell(BaseModel):
    family: Family
    release: Release
    write: Write
    variant: str | None = None
    reference: str | None = None
    reason: str | None = None

    @property
    def key(self) -> str:
        return f"{self.family}/{self.release}/{self.write}"

    @property
    def buildable(self) -> bool:
        return self.variant is not None


class ModelCell(BaseModel):
    """One arm of a memory model that is not the OCR grid's.

    A cell with a variant is selectable; one with a reason is a retired arm,
    drawn so the section reads as a statement about the model rather than a
    list of what happens to be built."""

    model: str
    arm: str
    label: str
    variant: str | None = None
    note: str | None = None
    reason: str | None = None

    @property
    def key(self) -> str:
        return f"{self.model}/{self.arm}"

    @property
    def buildable(self) -> bool:
        return self.variant is not None


class ModelSection(BaseModel):
    model: str
    label: str
    note: str
    cells: list[ModelCell]


class Plane(BaseModel):
    families: list[Family]
    releases: list[Release]
    writes: list[Write]
    family_labels: dict[Family, str] = Field(default_factory=dict)
    write_labels: dict[Write, str] = Field(default_factory=dict)
    release_labels: dict[Release, str] = Field(default_factory=dict)
    cells: list[PlaneCell]
    entries: list[SelectionEntry]
    models: list[ModelSection] = Field(default_factory=list)

    # -- lookup ----------------------------------------------------------
    def cell(self, family: Family, release: Release, write: Write) -> PlaneCell:
        for c in self.cells:
            if c.family is family and c.release is release and c.write is write:
                return c
        raise KeyError(f"{family}/{release}/{write}")

    def entry(self, key: str) -> SelectionEntry:
        key = modern_entry_key(key)
        for e in self.entries:
            if e.key == key:
                return e
        raise KeyError(key)

    def entries_of(self, cell: PlaneCell | ModelCell) -> list[SelectionEntry]:
        return [e for e in self.entries if e.cell == cell.key]

    @property
    def entry_keys(self) -> list[str]:
        return [e.key for e in self.entries]

    def default_entries(self, named: list[str] | None) -> list[str]:
        """The entries a campaign runs when it names none: the ones its
        profile lists, in the plane's order, or all of them.  A name the plane
        does not have is refused rather than dropped — a list that silently
        shrank would leave a study running less than it states."""
        if named is None:
            return self.entry_keys
        wanted = [modern_entry_key(k) for k in named]
        unknown = [k for k in wanted if k not in self.entry_keys]
        if unknown:
            raise ValueError(
                f"profile lists unknown plane entries: {', '.join(unknown)}")
        return [k for k in self.entry_keys if k in wanted]

    def columns(self) -> list[tuple[Release, Write]]:
        """Column order: the write policy groups, the release policy divides.

        Writing is the coarser decision — it says where the bytes live — so it
        forms the outer pair and the release policy splits each half.
        """
        return [(r, w) for w in self.writes for r in self.releases]

    def write_label(self, write: Write) -> str:
        return self.write_labels.get(write, write.value)

    def release_label(self, release: Release) -> str:
        return self.release_labels.get(release, release.value)


def _reason(family: Family, release: Release, write: Write, texts: dict) -> str:
    """Why this grid position carries no configuration.

    The rules mirror the build's own refusals, so a disabled position always
    reports the same ground the build would.
    """
    if family is Family.EXCL and write is Write.WT:
        return texts["excl_needs_wb"]
    return texts["needs_retain"]


@lru_cache(maxsize=1)
def load_plane() -> Plane:
    raw = load_data("protocols.yaml")
    families = [Family(f) for f in raw["families"]]
    releases = [Release(r) for r in raw["releases"]]
    writes = [Write(w) for w in raw["writes"]]

    defined = {
        (Family(c["family"]), Release(c["release"]), Write(c["write"])): c
        for c in raw["cells"]
    }
    refs = raw.get("references", {})

    cells: list[PlaneCell] = []
    entries: list[SelectionEntry] = []
    for family in families:
        for release, write in [(r, w) for r in releases for w in writes]:
            spec = defined.get((family, release, write))
            if spec is None:
                cells.append(
                    PlaneCell(
                        family=family,
                        release=release,
                        write=write,
                        reason=_reason(family, release, write, raw["unbuildable"]),
                    )
                )
                continue
            cell = PlaneCell(
                family=family,
                release=release,
                write=write,
                variant=spec["variant"],
                reference=spec.get("reference"),
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
            if cell.buildable:
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
        families=families,
        releases=releases,
        writes=writes,
        family_labels={Family(k): v for k, v in raw.get("family_labels", {}).items()},
        write_labels={Write(k): v for k, v in raw.get("write_labels", {}).items()},
        release_labels={Release(k): v
                        for k, v in raw.get("release_labels", {}).items()},
        cells=cells,
        entries=entries,
        models=models,
    )
