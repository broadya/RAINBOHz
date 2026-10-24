# SPDX-License-Identifier: Apache-2.0

"""Emits a canonical model as the Cypher script `rendering-engine --state` loads.

The `EngineConfig` is a statement of its own. The score follows as one
statement per table, in chunks of `CHUNK_ROWS` rows: nodes by
`UNWIND [rows] AS r CREATE`, then relationships by
`COPY REL FROM (UNWIND [rows] AS r RETURN ...)`, which finds both ends by
primary-key index. Nothing is looked up by a scan.

A relationship names its ends by id. The ids are the model's row indices,
because the engine loads every score into a fresh database, where a `SERIAL`
column counts from 0 in the order rows are created, and rows are created in
model order. A script loaded into a database that already holds rows would
connect the wrong nodes.

Why batches rather than one `CREATE` clause of comma-separated patterns,
which is how the score was written until 2026-10-13: Ladybug's binder merges
every pattern of a clause into one query graph, at a cost that grows faster
than the square of the pattern count. Profiled on `convergence` (macos-max,
2026-10-12), the clause took 0.6, 1.8, 7.0 and 71.7 s to load at 200, 400,
800 and 1,600 partials, almost all of it binding; parsing was under 0.4 s.
Batched, the load costs about 55 us per row whatever the chunk size, and
`convergence` (800 partials) loads in under 1 s. One `CREATE` clause per
pattern is no way out either: Ladybug crashed with SIGBUS on a statement of
493 such clauses (2026-10-02).

Rows are struct literals and columns are read by name (`r.from`), since a
list literal is indexed from 1. A NULL is written with its type
(`CAST(NULL AS DOUBLE)`): Ladybug types a list of structs from its first
row, and a bare `null` there would make the column a STRING.
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import fields
from decimal import Decimal
from typing import Any

from .model import TABLE_ORDER, Edge, EngineConfigRow, Model, Row

CHUNK_ROWS = 1000
"""Rows per statement. Load cost per row does not depend on it (measured
from 100 to 20,000); it only keeps each statement a readable size."""

_CYPHER_TYPES = {"str": "STRING", "int": "INT64", "float": "DOUBLE", "bool": "BOOL"}


def literal(value: Any) -> str:
    """Formats one property value as a Cypher literal.

    Floats are written in fixed notation from their shortest round-trip
    representation, with a decimal point always present, so that a FLOAT
    column never receives an integer literal and no exponent notation is
    relied on. Integers and booleans are written as such; strings are quoted
    with backslash and double quote escaped.

    Raises:
        TypeError: for a value of any other type.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        text = format(Decimal(repr(value)), "f")
        return text if "." in text else text + ".0"
    if isinstance(value, str):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    raise TypeError(f"no Cypher literal for {type(value).__name__}")


def typed_literal(value: Any, cypher_type: str) -> str:
    """Formats `value` as `literal` does, and None as a NULL of `cypher_type`."""
    return f"CAST(NULL AS {cypher_type})" if value is None else literal(value)


def properties(values: dict[str, Any]) -> str:
    """Formats a property map as ` {k: v, ...}`, or nothing when empty."""
    if not values:
        return ""
    return " {" + ", ".join(f"{key}: {literal(value)}" for key, value in values.items()) + "}"


def column_type(annotation: Any) -> str:
    """The Cypher type of a row field, from its annotation.

    Annotations are strings here (`from __future__ import annotations`):
    `float`, or `float | None` for a column that may be NULL.

    Raises:
        TypeError: for an annotation that is not one Python scalar type,
            optionally with None.
    """
    text = annotation if isinstance(annotation, str) else getattr(annotation, "__name__", "")
    names = [part.strip() for part in text.split("|") if part.strip() != "None"]
    if len(names) != 1 or names[0] not in _CYPHER_TYPES:
        raise TypeError(f"no Cypher type for the annotation {annotation!r}")
    return _CYPHER_TYPES[names[0]]


def value_type(values: Iterable[Any]) -> str:
    """The Cypher type of a column of Python values, None values aside.

    Raises:
        TypeError: when the values mix types, or are all None, so that no
            type can be told.
    """
    kinds = {type(value).__name__ for value in values if value is not None}
    if len(kinds) != 1 or (kind := kinds.pop()) not in _CYPHER_TYPES:
        raise TypeError(f"no single Cypher type for a column of {sorted(kinds) or 'NULLs'}")
    return _CYPHER_TYPES[kind]


def _chunks(items: Sequence[Any]) -> Iterable[Sequence[Any]]:
    """`items` in consecutive slices of at most `CHUNK_ROWS`."""
    for start in range(0, len(items), CHUNK_ROWS):
        yield items[start : start + CHUNK_ROWS]


def _struct_list(rows: Iterable[str]) -> str:
    """A list literal of the given struct literals, one per line."""
    return "[\n" + ",\n".join(f"    {row}" for row in rows) + "\n]"


def _node_statements(table: str, rows: Sequence[Row]) -> list[str]:
    """The statements that create `rows`, in order, so ids follow the indices."""
    columns = [(f.name, column_type(f.type)) for f in fields(rows[0])]
    if not columns:
        return [
            f"UNWIND range(1, {len(chunk)}) AS r CREATE (n:{table});" for chunk in _chunks(rows)
        ]
    assignments = ", ".join(f"{name}: r.{name}" for name, _ in columns)
    statements = []
    for chunk in _chunks(rows):
        structs = (
            "{"
            + ", ".join(
                f"{name}: {typed_literal(getattr(row, name), kind)}" for name, kind in columns
            )
            + "}"
            for row in chunk
        )
        statements.append(
            f"UNWIND {_struct_list(structs)} AS r CREATE (n:{table} {{{assignments}}});"
        )
    return statements


def _edge_statements(rel: str, edges: Sequence[Edge]) -> list[str]:
    """The statements that create `edges`, all of one relationship and one
    (FROM, TO) pair of tables, in order.

    Raises:
        ValueError: if the edges do not all carry the same property names.
    """
    names = list(edges[0].properties)
    if any(list(edge.properties) != names for edge in edges):
        raise ValueError(f"{rel} edges carry different properties")
    kinds = {name: value_type(edge.properties[name] for edge in edges) for name in names}
    returned = ", ".join(["r.from", "r.to"] + [f"r.{name}" for name in names])
    ends = f"(from='{edges[0].src.table}', to='{edges[0].dst.table}')"
    statements = []
    for chunk in _chunks(edges):
        structs = (
            "{"
            + ", ".join(
                [f"from: {edge.src.index}", f"to: {edge.dst.index}"]
                + [f"{name}: {typed_literal(edge.properties[name], kinds[name])}" for name in names]
            )
            + "}"
            for edge in chunk
        )
        statements.append(
            f"COPY {rel} FROM (UNWIND {_struct_list(structs)} AS r RETURN {returned}) {ends};"
        )
    return statements


def emit_cypher(model: Model, comment_lines: Iterable[str] = ()) -> str:
    """Serialises `model` as a Cypher script.

    Args:
        model: The compiled score.
        comment_lines: Lines for the header comment, after the generator's
            own, without the `//` prefix.

    Returns:
        The script text: a header comment, the `EngineConfig` statement, then
        the score's nodes table by table and its relationships by type and
        (FROM, TO) pair of tables, each headed by a comment with its count.
    """
    out: list[str] = []
    out.append("// " + "=" * 60)
    out.append("// Generated by hzlc. Do not edit: edit the HzL source and recompile.")
    for line in comment_lines:
        out.append(f"// {line}".rstrip())
    out.append("// " + "=" * 60)
    out.append("")

    config = model.rows[EngineConfigRow.TABLE]
    if config:
        out.append("// Engine configuration, from compiler options; not part of the score.")
        for row in config:
            out.append(f"CREATE (n:{row.TABLE}{properties(row.properties())});")
        out.append("")

    for table in TABLE_ORDER:
        rows = model.rows[table]
        if table == EngineConfigRow.TABLE or not rows:
            continue
        out.append(f"// {table}: {len(rows)}")
        out.extend(_node_statements(table, rows))
        out.append("")

    # Unlike node order, which sets the ids, edge order does not reach the
    # audio: convergence rendered byte-identical frames either way
    # (2026-10-13).
    for (rel, src, dst), edges in model.edge_groups().items():
        out.append(f"// {rel}, {src} -> {dst}: {len(edges)}")
        out.extend(_edge_statements(rel, edges))
        out.append("")
    return "\n".join(out)
