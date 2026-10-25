#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Checks that the native engine-db's tags and the engine-db schema agree.

rendering-engine's native backend holds engine-db in hand-written C++ types
(rendering-engine/include/rainbohz/rendering_engine/native_model.hpp, ADR
0028). Their comments say which part of the schema each one holds:

    @enginedb{node Table}                      a node table; a SERIAL id is the row index
    @enginedb{Table.property}                  a node or relationship property
    @enginedb{rel REL From->To: how}           one relationship pair, and how it is held
    @enginedb_unmapped{Table: why}             a table not held, with its pairs
    @enginedb_unmapped{Table.property: why}    a property never set
    @enginedb_unmapped{rel REL From->To: why}  a pair not held

Tags are read from Doxygen comments (`///`, `///<`) only, and may run across
consecutive comment lines. Anything after a colon is a note for the reader;
it is required where it says "how" or "why". This script fails unless every
node table, property and relationship pair of the schema is accounted for
exactly once, and every tag names something the schema has. It reads the
schema and never edits it.

The check is of names and coverage only. Whether a field holds the right
unit or type is for the engine's cross-backend tests to find.

Usage:
    check_engine_db_mapping.py --schema engine-db/schema/engine-schema.cypher \\
        rendering-engine/include/rainbohz/rendering_engine/native_model.hpp
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ladybug_schema_to_mermaid import NodeTable, RelTable, parse_schema

TAG_RE = re.compile(r"@enginedb(_unmapped)?\{([^}]*)\}")
"""One tag: whether it is an unmapped one, and its text."""

PAIR_RE = re.compile(r"^rel\s+(\w+)\s+(\w+)\s*->\s*(\w+)$")
"""A relationship pair: `rel REL From->To`."""

NODE_RE = re.compile(r"^node\s+(\w+)$")
"""A node table: `node Table`."""

PROPERTY_RE = re.compile(r"^(\w+)\.(\w+)$")
"""A property: `Table.property`."""

TABLE_RE = re.compile(r"^(\w+)$")
"""A bare table name, as an unmapped table is written."""


@dataclass
class Tag:
    """One tag as found in a source file."""

    file: Path
    line: int
    unmapped: bool
    subject: str
    note: str

    def where(self) -> str:
        """The tag's place, for a message."""
        return f"{self.file}:{self.line}"


@dataclass
class Coverage:
    """What the tags account for, keyed by schema name."""

    nodes: dict[str, Tag] = field(default_factory=dict)
    unmapped_nodes: dict[str, Tag] = field(default_factory=dict)
    properties: dict[tuple[str, str], Tag] = field(default_factory=dict)
    unmapped_properties: dict[tuple[str, str], Tag] = field(default_factory=dict)
    pairs: dict[tuple[str, str, str], Tag] = field(default_factory=dict)
    unmapped_pairs: dict[tuple[str, str, str], Tag] = field(default_factory=dict)


def comment_runs(text: str) -> list[tuple[int, str]]:
    """Joins each run of consecutive Doxygen comment lines into one text.

    clang-format reflows comments to the column limit, so a tag can be split
    across two `///` lines. Joining a run's lines with spaces lets a tag be
    read whole wherever it was broken.

    Args:
        text: A source file.

    Returns:
        Each run's first line number and its joined comment text.
    """
    runs: list[tuple[int, str]] = []
    start = 0
    parts: list[str] = []
    for number, line in enumerate(text.splitlines(), start=1):
        at = line.find("///")
        if at < 0:
            if parts:
                runs.append((start, " ".join(parts)))
                parts = []
            continue
        if not parts:
            start = number
        parts.append(line[at + 3 :].removeprefix("<").strip())
    if parts:
        runs.append((start, " ".join(parts)))
    return runs


def find_tags(files: list[Path]) -> list[Tag]:
    """Finds every tag in the Doxygen comments of `files`.

    Only a Doxygen comment carries a tag: an ordinary comment may quote the
    forms, as native_model.hpp's introduction does.

    Args:
        files: Source files to read.

    Returns:
        The tags, in file and line order, each with the first line of its
        comment.

    Raises:
        OSError: if a file cannot be read.
    """
    tags: list[Tag] = []
    for path in files:
        for line, comment in comment_runs(path.read_text(encoding="utf-8")):
            for match in TAG_RE.finditer(comment):
                subject, _, note = match.group(2).partition(":")
                tags.append(
                    Tag(
                        file=path,
                        line=line,
                        unmapped=match.group(1) is not None,
                        subject=" ".join(subject.split()),
                        note=" ".join(note.split()),
                    )
                )
    return tags


def collect(tags: list[Tag], problems: list[str]) -> Coverage:
    """Sorts the tags by what they name, reporting malformed and repeated ones.

    Args:
        tags: Every tag found.
        problems: Modified: gains one message per malformed or repeated tag.

    Returns:
        What the well-formed tags account for.
    """
    coverage = Coverage()

    def claim(table: dict[Any, Tag], key: object, tag: Tag) -> None:
        if key in table:
            problems.append(
                f"{tag.where()}: {tag.subject} is already tagged at {table[key].where()}"
            )
        else:
            table[key] = tag

    for tag in tags:
        if "," in tag.subject or "," in tag.note:
            problems.append(
                f"{tag.where()}: a tag holds a comma, which Doxygen's alias would split"
            )
        pair = PAIR_RE.match(tag.subject)
        node = NODE_RE.match(tag.subject)
        prop = PROPERTY_RE.match(tag.subject)
        if pair is not None:
            if not tag.note:
                problems.append(
                    f"{tag.where()}: {tag.subject} does not say how it is held, or why not"
                )
            key = (pair.group(1), pair.group(2), pair.group(3))
            claim(coverage.unmapped_pairs if tag.unmapped else coverage.pairs, key, tag)
        elif prop is not None:
            if tag.unmapped and not tag.note:
                problems.append(f"{tag.where()}: {tag.subject} does not say why it is never set")
            key2 = (prop.group(1), prop.group(2))
            properties = coverage.unmapped_properties if tag.unmapped else coverage.properties
            claim(properties, key2, tag)
        elif node is not None and not tag.unmapped:
            claim(coverage.nodes, node.group(1), tag)
        elif tag.unmapped and TABLE_RE.match(tag.subject):
            if not tag.note:
                problems.append(f"{tag.where()}: {tag.subject} does not say why it is not held")
            claim(coverage.unmapped_nodes, tag.subject, tag)
        else:
            problems.append(f"{tag.where()}: cannot read the tag {tag.subject!r}")
    return coverage


def check(nodes: list[NodeTable], rels: list[RelTable], coverage: Coverage) -> list[str]:
    """Compares the schema with what the tags account for.

    Args:
        nodes: The schema's node tables.
        rels: The schema's relationship tables.
        coverage: What the tags account for.

    Returns:
        One message per disagreement; empty when they agree.
    """
    problems: list[str] = []
    node_names = {table.name: table for table in nodes}
    rel_names = {table.name: table for table in rels}

    for name, tag in {**coverage.nodes, **coverage.unmapped_nodes}.items():
        if name not in node_names:
            problems.append(f"{tag.where()}: the schema has no node table {name}")
    for name in coverage.nodes.keys() & coverage.unmapped_nodes.keys():
        problems.append(
            f"{coverage.unmapped_nodes[name].where()}: {name} is tagged both held and not held"
        )

    held = set(coverage.nodes)
    for table in nodes:
        if table.name not in held and table.name not in coverage.unmapped_nodes:
            problems.append(f"node table {table.name} is neither held nor listed as not held")
        if table.name not in held:
            continue
        for prop in table.properties:
            # A SERIAL primary key is the row index, which the node tag covers.
            if prop.is_pk and prop.prop_type.display() == "SERIAL":
                continue
            key = (table.name, prop.name)
            if key not in coverage.properties and key not in coverage.unmapped_properties:
                problems.append(
                    f"property {table.name}.{prop.name} is neither held nor listed as never set"
                )

    for (table_name, prop_name), tag in {
        **coverage.properties,
        **coverage.unmapped_properties,
    }.items():
        owner: NodeTable | RelTable | None = node_names.get(table_name) or rel_names.get(table_name)
        if owner is None:
            problems.append(f"{tag.where()}: the schema has no table {table_name}")
        elif prop_name not in {prop.name for prop in owner.properties}:
            problems.append(f"{tag.where()}: {table_name} has no property {prop_name}")
        elif table_name in coverage.unmapped_nodes:
            problems.append(f"{tag.where()}: {table_name} is listed as not held")
    for both in coverage.properties.keys() & coverage.unmapped_properties.keys():
        problems.append(
            f"{coverage.unmapped_properties[both].where()}: {both[0]}.{both[1]} "
            "is tagged both held and never set"
        )

    for (rel_name, src, dst), tag in {**coverage.pairs, **coverage.unmapped_pairs}.items():
        rel = rel_names.get(rel_name)
        if rel is None:
            problems.append(f"{tag.where()}: the schema has no relationship table {rel_name}")
        elif (src, dst) not in {(pair.from_table, pair.to_table) for pair in rel.pairs}:
            problems.append(f"{tag.where()}: {rel_name} has no pair {src}->{dst}")
    for twice in coverage.pairs.keys() & coverage.unmapped_pairs.keys():
        problems.append(
            f"{coverage.unmapped_pairs[twice].where()}: {twice[0]} {twice[1]}->{twice[2]} "
            "is tagged both held and not held"
        )

    for rel in rels:
        any_held = False
        for pair in rel.pairs:
            key3 = (rel.name, pair.from_table, pair.to_table)
            ends_held = pair.from_table in held and pair.to_table in held
            if key3 in coverage.pairs:
                any_held = True
                if not ends_held:
                    problems.append(
                        f"{coverage.pairs[key3].where()}: {rel.name} "
                        f"{pair.from_table}->{pair.to_table} "
                        "is tagged held, but one of its tables is not"
                    )
            elif key3 not in coverage.unmapped_pairs and ends_held:
                problems.append(
                    f"relationship {rel.name} {pair.from_table}->{pair.to_table} "
                    "is neither held nor listed as not held"
                )
        if any_held:
            for prop in rel.properties:
                key = (rel.name, prop.name)
                if key not in coverage.properties and key not in coverage.unmapped_properties:
                    problems.append(
                        f"property {rel.name}.{prop.name} is neither held nor listed as never set"
                    )
    return problems


def run(schema: Path, sources: list[Path]) -> list[str]:
    """Checks `sources` against `schema`.

    Args:
        schema: The engine-db schema's DDL.
        sources: The files holding the tags.

    Returns:
        Every problem found; empty when the tags and the schema agree.

    Raises:
        OSError: if a file cannot be read.
    """
    nodes, rels = parse_schema(schema.read_text(encoding="utf-8"))
    problems: list[str] = []
    coverage = collect(find_tags(sources), problems)
    problems.extend(check(nodes, rels, coverage))
    return problems


def main() -> int:
    """Runs the check from the command line.

    Returns:
        0 when the tags and the schema agree, 1 otherwise.
    """
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--schema", required=True, type=Path)
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()
    problems = run(args.schema, args.sources)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print(
            f"\n{len(problems)} disagreement(s) between the native model's tags and the schema.",
            file=sys.stderr,
        )
        return 1
    print("native model tags agree with the engine-db schema")
    return 0


if __name__ == "__main__":
    sys.exit(main())
