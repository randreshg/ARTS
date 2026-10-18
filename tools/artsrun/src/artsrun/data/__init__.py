"""Packaged catalog data (values that ship with the tool)."""

from __future__ import annotations

from importlib.resources import files
from typing import Any

from ruamel.yaml import YAML

_yaml = YAML(typ="safe")


def load_data(name: str) -> Any:
    """Load a packaged YAML catalog by file name."""
    with files(__package__).joinpath(name).open("r", encoding="utf-8") as fh:
        return _yaml.load(fh)


def load_doc(name: str) -> str | None:
    """The structural document for one catalog entry, or None.

    Documents are per-entry markdown files shipped with the tool, except a
    section's whose documents live in the checkout (`<docs>/<stem>.md`, the
    row name's section suffix dropped); an entry without one (attack probes,
    mostly) is an absence, not an error.
    """
    from artsrun.model.catalog import Section
    from artsrun.paths import repo_root

    doc = files(__package__).joinpath("appdocs", f"{name}.md")
    for section in Section:
        stem = section.stem(name)
        if stem is not None and section.spec.docs is not None:
            doc = repo_root() / section.spec.docs / f"{stem}.md"
            break
    if not doc.is_file():
        return None
    return doc.read_text(encoding="utf-8")
