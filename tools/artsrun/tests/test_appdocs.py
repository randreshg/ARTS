"""The application documents: one page per application, openable from the
roster and from the command line, falling back to catalog facts."""

from __future__ import annotations

import re

import asyncio

from artsrun.data import load_doc
from artsrun.docs import document, facts_markdown
from artsrun.model.catalog import Kind, Section, load_catalog
from artsrun.tui.app import ArtsRunApp
from artsrun.tui.docview import AppDocScreen

# Every document tells the same structural story in the same order, so a
# reader who knows one page knows them all.
REQUIRED_SECTIONS = [
    "## Overview",
    "## Parameters",
    "## Structure",
    "## Wiring",
    "## Flow",
    "## Placement",
    "## Sizing",
]


def drive(coro_factory):
    async def main():
        app = ArtsRunApp(profile="fixture-local", experiment="paper-main")
        async with app.run_test() as pilot:
            return await coro_factory(app, pilot)

    return asyncio.run(main())


def test_a_written_document_loads_and_a_missing_one_is_none():
    assert load_doc("fibonacci")
    assert load_doc("no_such_application") is None


def test_every_application_row_has_a_document():
    """A result is claimed about an application; every application row the
    roster offers must open to a real structural document, not the fallback.

    Every HPX application owes one whatever its kind: there the document is
    also where the mirror's correspondence to the origin program is stated,
    which a toy needs as much as an application.  So does every ARTS
    example: its document is the example's own page in the documentation."""
    catalog = load_catalog()
    missing = [a.name for a in [*catalog.rows_of(Kind.APP),
                                *catalog.rows_in(Section.HPX),
                                *catalog.rows_in(Section.EXAMPLES)]
               if load_doc(a.name) is None]
    assert not missing, f"rows without a document: {missing}"


def test_every_document_names_a_catalog_row():
    """The inverse: a document whose row is gone describes nothing a
    campaign can run, and is deleted with the row."""
    from importlib.resources import files

    from artsrun.paths import repo_root

    catalog = load_catalog()
    packaged = [p.name.removesuffix(".md")
                for p in files("artsrun.data").joinpath("appdocs").iterdir()
                if p.name.endswith(".md")]
    orphans = [n for n in packaged if n not in catalog.apps]
    for section in Section:
        if section.spec.docs is None:
            continue
        for page in (repo_root() / section.spec.docs).glob("*.md"):
            name = f"{page.stem}{section.spec.suffix}"
            if catalog.apps.get(name) is None or \
                    catalog.apps[name].section is not section:
                orphans.append(str(page.relative_to(repo_root())))
    assert not orphans, f"documents naming no catalog row: {orphans}"


def test_an_example_row_reads_its_page_from_the_documentation():
    from artsrun.paths import repo_root

    catalog = load_catalog()
    for row in catalog.rows_in(Section.EXAMPLES):
        stem = Section.EXAMPLES.stem(row.name)
        page = repo_root() / "docs" / "examples" / f"{stem}.md"
        assert load_doc(row.name) == page.read_text(encoding="utf-8")


def test_every_written_document_tells_the_whole_story():
    catalog = load_catalog()
    for name in catalog.apps:
        text = load_doc(name)
        if text is None:
            continue
        assert text.startswith(f"# {name}\n"), f"{name}: title is not the entry name"
        # A heading may carry a qualifier after the fixed title — a rewrite
        # row has no base/hinted pair, so its placement section says which
        # placement it describes — as long as the title itself is there.
        for section in REQUIRED_SECTIONS:
            assert re.search(rf"^{re.escape(section)}(?:\s|$)", text, re.M), (
                f"{name}: missing '{section}'")


def test_an_entry_without_a_document_still_renders_its_facts():
    catalog = load_catalog()
    probe = next(a for a in catalog.apps.values() if a.kind is Kind.ATTACK)
    text = document(probe)
    assert probe.name in text
    assert probe.binary in facts_markdown(probe)


def test_clicking_an_application_name_opens_its_document():
    async def check(app, pilot):
        app.query_one("#exp")
        await app.run_action("show_app_doc('fibonacci')")
        await pilot.pause()
        opened = isinstance(app.screen, AppDocScreen)
        name = app.screen.entry.name if opened else None
        await pilot.press("escape")
        await pilot.pause()
        closed = not isinstance(app.screen, AppDocScreen)
        return opened, name, closed

    opened, name, closed = drive(check)
    assert opened and name == "fibonacci" and closed


def test_an_unknown_name_from_a_stale_label_is_ignored():
    async def check(app, pilot):
        await app.run_action("show_app_doc('no_such_application')")
        await pilot.pause()
        return isinstance(app.screen, AppDocScreen)

    assert drive(check) is False


def test_clicking_a_roster_name_opens_that_row_s_document():
    async def check(app, pilot):
        await pilot.press("1")
        await pilot.pause()
        # The first .bench-name is the header row's empty label; the first
        # roster row follows it.  The link spans only the name's own
        # characters and the label is padded, so aim at its first column
        # rather than its centre.
        target = app.query(".bench-name").nodes[1]
        target.scroll_visible(animate=False)
        await pilot.pause()
        await pilot.click(target, offset=(1, 0))
        await pilot.pause()
        opened = isinstance(app.screen, AppDocScreen)
        return opened, app.screen.entry.name if opened else None

    opened, name = drive(check)
    assert opened
    catalog = load_catalog()
    assert name in catalog.apps
