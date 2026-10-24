# SPDX-License-Identifier: Apache-2.0

"""Emits a canonical model as Parquet tables and the manifest that lists them.

This is the compiler's normal output. `rendering-engine --state` (or
`PUT /v1/score`) takes the manifest, and the engine loads each file with
Ladybug's `COPY FROM`, which reads the values as data: nothing is parsed as
Cypher. The Cypher script (cypher.py) is the same model as statements, kept
for debugging, and loads the same database.

The directory holds `manifest.json` and one Parquet file per node table and
per relationship (FROM, TO) pair of tables, named `<Table>.parquet` and
`<REL>.<From>.<To>.parquet`. A node table with no columns has no file: the
manifest gives its row count and the engine creates that many nodes.

**Ids.** As in the Cypher script, a relationship names its ends by id, and
the ids are the model's row indices. That holds because the engine loads
every score into a fresh database, where a `SERIAL` id counts from 0 in the
order rows are copied in. Ladybug reads a Parquet file one row group per
task, in parallel, so a node file of several row groups would be numbered
out of order and connect the wrong nodes (seen on 2026-10-21: echoes'
frames differed by more than their own peak). Every node file is therefore
written as one row group, whatever its size. pyarrow's default would split
a table of more than 1,048,576 rows. Relationship files may be split: edge
order does not reach the audio.

Measured on echoes (5,120 partials, 82,005 rows, macos-max, 2026-10-21):
the score loads in about 0.11 s from these files against 9.2 s from the
Cypher script, and renders byte-identical frames.
"""

from __future__ import annotations

import json
from collections.abc import Iterable, Sequence
from dataclasses import fields
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from .cypher import column_type, value_type
from .model import TABLE_ORDER, Edge, EngineConfigRow, Model, Row

MANIFEST = "manifest.json"
"""The manifest's file name in the output directory."""

FORMAT = "rainbohz-score-tables"
"""The manifest's `format`, which marks a directory as this emitter's."""

VERSION = 1
"""The manifest's `version`. The engine refuses any other."""

_ARROW_TYPES = {
    "STRING": pa.string(),
    "INT64": pa.int64(),
    "DOUBLE": pa.float64(),
    "BOOL": pa.bool_(),
}


class TablesError(Exception):
    """The output directory cannot be written: it is not this emitter's."""


def node_file(table: str) -> str:
    """The file name of node table `table`."""
    return f"{table}.parquet"


def edge_file(rel: str, src: str, dst: str) -> str:
    """The file name of relationship `rel` from table `src` to table `dst`."""
    return f"{rel}.{src}.{dst}.parquet"


def _arrow_column(values: Sequence[Any], cypher_type: str) -> pa.Array:
    """One column as an Arrow array of the type `cypher_type` maps to; None is NULL."""
    return pa.array(values, type=_ARROW_TYPES[cypher_type])


def _write_node_file(path: Path, columns: dict[str, pa.Array], rows: int) -> None:
    """Writes a node table as one row group, so Ladybug numbers its rows in order."""
    pq.write_table(pa.table(columns), path, row_group_size=max(rows, 1))


def _node_entry(out_dir: Path, table: str, rows: Sequence[Row]) -> dict[str, Any]:
    """Writes one node table's file, if it has columns, and returns its manifest entry."""
    columns = [(f.name, column_type(f.type)) for f in fields(rows[0])]
    entry: dict[str, Any] = {"table": table, "rows": len(rows)}
    if not columns:
        return entry
    arrays = {
        name: _arrow_column([getattr(row, name) for row in rows], kind) for name, kind in columns
    }
    _write_node_file(out_dir / node_file(table), arrays, len(rows))
    entry["file"] = node_file(table)
    entry["columns"] = [name for name, _ in columns]
    return entry


def _edge_entry(out_dir: Path, rel: str, edges: Sequence[Edge]) -> dict[str, Any]:
    """Writes one relationship pair's file and returns its manifest entry.

    Raises:
        ValueError: if the edges do not all carry the same property names.
    """
    names = list(edges[0].properties)
    if any(list(edge.properties) != names for edge in edges):
        raise ValueError(f"{rel} edges carry different properties")
    src, dst = edges[0].src.table, edges[0].dst.table
    arrays = {
        "from": _arrow_column([edge.src.index for edge in edges], "INT64"),
        "to": _arrow_column([edge.dst.index for edge in edges], "INT64"),
    }
    for name in names:
        values = [edge.properties[name] for edge in edges]
        arrays[name] = _arrow_column(values, value_type(values))
    name = edge_file(rel, src, dst)
    pq.write_table(pa.table(arrays), out_dir / name)
    return {"table": rel, "from": src, "to": dst, "rows": len(edges), "file": name}


def _prepare(out_dir: Path) -> None:
    """Makes `out_dir` an empty directory, removing an earlier output of this emitter.

    Only a directory that is empty, or that holds nothing but an earlier
    manifest and the files it lists, is touched: anything else in it means
    the path was not meant for this output.

    Raises:
        TablesError: if `out_dir` is a file, or holds anything else.
    """
    if out_dir.exists() and not out_dir.is_dir():
        raise TablesError(f"{out_dir} is not a directory")
    if not out_dir.exists():
        out_dir.mkdir(parents=True)
        return
    present = {entry.name for entry in out_dir.iterdir()}
    if not present:
        return
    owned = _earlier_output(out_dir)
    strangers = sorted(present - owned)
    if strangers:
        raise TablesError(
            f"{out_dir} holds files that are not an earlier hzlc output "
            f"({', '.join(strangers[:3])}{', ...' if len(strangers) > 3 else ''}); "
            "name an empty or new directory"
        )
    for name in owned & present:
        (out_dir / name).unlink()


def _earlier_output(out_dir: Path) -> set[str]:
    """The names an earlier manifest in `out_dir` accounts for, itself included.

    Empty when there is no manifest, or it is not one of this emitter's.
    """
    try:
        manifest = json.loads((out_dir / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return set()
    if not isinstance(manifest, dict) or manifest.get("format") != FORMAT:
        return set()
    names = {MANIFEST}
    for key in ("nodes", "relationships"):
        for entry in manifest.get(key, []):
            if isinstance(entry, dict) and isinstance(entry.get("file"), str):
                names.add(entry["file"])
    return names


def emit_tables(model: Model, out_dir: Path, comment_lines: Iterable[str] = ()) -> Path:
    """Writes `model` to `out_dir` as Parquet tables and a manifest.

    The manifest is written last, so a directory with a manifest is complete.

    Args:
        model: The compiled score.
        out_dir: The output directory, created if absent. An earlier output
            of this emitter there is replaced.
        comment_lines: Lines describing the output, for a person reading the
            manifest.

    Returns:
        The manifest's path.

    Raises:
        TablesError: if `out_dir` is not this emitter's to write.
    """
    _prepare(out_dir)
    manifest: dict[str, Any] = {
        "format": FORMAT,
        "version": VERSION,
        "comments": [
            "Generated by hzlc. Do not edit: edit the HzL source and recompile.",
            *comment_lines,
        ],
        "engine_config": {},
        "nodes": [],
        "relationships": [],
    }
    for row in model.rows[EngineConfigRow.TABLE]:
        manifest["engine_config"] = row.properties()

    for table in TABLE_ORDER:
        rows = model.rows[table]
        if table == EngineConfigRow.TABLE or not rows:
            continue
        manifest["nodes"].append(_node_entry(out_dir, table, rows))

    for (rel, _, _), edges in model.edge_groups().items():
        manifest["relationships"].append(_edge_entry(out_dir, rel, edges))

    path = out_dir / MANIFEST
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return path
