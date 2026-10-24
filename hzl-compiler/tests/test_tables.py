# SPDX-License-Identifier: Apache-2.0

import json
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from hzl_compiler.canonical import tables
from hzl_compiler.canonical.model import (
    TABLE_ORDER,
    EngineConfigRow,
    Model,
    PartialRow,
    PartialSetRow,
    PartialStateRow,
    PartialStateSetRow,
    ScoreRow,
    TempoMapRow,
)
from hzl_compiler.canonical.tables import MANIFEST, TablesError, emit_tables
from hzl_compiler.cli import main
from hzl_compiler.lower.score import lower_score
from hzl_compiler.source import SourceTree

EXAMPLES = Path(__file__).resolve().parent.parent / "examples"
TEMPO_MAP_SCORE = EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml"


def manifest_of(out_dir: Path) -> dict[str, Any]:
    manifest: dict[str, Any] = json.loads((out_dir / MANIFEST).read_text(encoding="utf-8"))
    return manifest


def test_the_manifest_lists_every_table_in_model_order(tmp_path: Path) -> None:
    tree, ref = SourceTree.from_file(TEMPO_MAP_SCORE)
    model = lower_score(tree, ref)
    emit_tables(model, tmp_path, ["a comment"])
    manifest = manifest_of(tmp_path)

    assert manifest["format"] == "rainbohz-score-tables"
    assert manifest["version"] == 1
    assert manifest["comments"][-1] == "a comment"
    assert manifest["engine_config"] == {"sampleRate": 48000, "channelMode": 0}
    expected = [t for t in TABLE_ORDER if t != EngineConfigRow.TABLE and model.count(t)]
    assert [entry["table"] for entry in manifest["nodes"]] == expected
    for entry in manifest["nodes"]:
        assert entry["rows"] == model.count(entry["table"])
        if "file" in entry:
            assert pq.read_table(tmp_path / entry["file"]).num_rows == entry["rows"]
    assert sum(entry["rows"] for entry in manifest["relationships"]) == len(model.edges)
    listed = {MANIFEST} | {
        e["file"] for e in manifest["nodes"] + manifest["relationships"] if "file" in e
    }
    assert {p.name for p in tmp_path.iterdir()} == listed


def test_node_values_round_trip_exactly(tmp_path: Path) -> None:
    model = Model()
    model.add(PartialStateRow(frequency=1103.0314960629921, amplitude=None, phase=None))
    model.add(PartialStateRow(frequency=None, amplitude=-144.0, phase=90.0, phase_free_run=False))
    model.add(PartialStateRow(frequency=1e-05, amplitude=0.1 + 0.2, phase_free_run=True))
    model.add(ScoreRow(name='quote " and comma, and ünïcode', tempo_ramp_physical_time=True))
    emit_tables(model, tmp_path)

    states = pq.read_table(tmp_path / "PartialState.parquet").to_pylist()
    assert states == [
        {"frequency": 1103.0314960629921, "amplitude": None, "phase": None, "phase_free_run": None},
        {"frequency": None, "amplitude": -144.0, "phase": 90.0, "phase_free_run": False},
        {"frequency": 1e-05, "amplitude": 0.1 + 0.2, "phase": None, "phase_free_run": True},
    ]
    schema = pq.read_schema(tmp_path / "PartialState.parquet")
    assert [schema.field(n).type for n in schema.names] == [
        pa.float64(),
        pa.float64(),
        pa.float64(),
        pa.bool_(),
    ]
    score = pq.read_table(tmp_path / "Score.parquet").to_pylist()
    assert score == [{"name": 'quote " and comma, and ünïcode', "tempo_ramp_physical_time": True}]


def test_a_table_without_columns_has_a_count_and_no_file(tmp_path: Path) -> None:
    model = Model()
    for _ in range(3):
        model.add(PartialStateSetRow())
    emit_tables(model, tmp_path)
    assert manifest_of(tmp_path)["nodes"] == [{"table": "PartialStateSet", "rows": 3}]
    assert not (tmp_path / "PartialStateSet.parquet").exists()


def test_relationships_name_their_ends_by_row_index(tmp_path: Path) -> None:
    model = Model()
    s0, s1 = model.add(PartialSetRow("s0")), model.add(PartialSetRow("s1"))
    p0, p1 = model.add(PartialRow("p0")), model.add(PartialRow("p1"))
    t0 = model.add(TempoMapRow("t", 960))
    model.link("HAS_PARTIAL", s1, p0)
    model.link("HAS_PARTIAL", s0, p1)
    model.link("POINT_AT", s0, t0, sync_point=False)
    emit_tables(model, tmp_path)

    rels = manifest_of(tmp_path)["relationships"]
    assert rels == [
        {
            "table": "HAS_PARTIAL",
            "from": "PartialSet",
            "to": "Partial",
            "rows": 2,
            "file": "HAS_PARTIAL.PartialSet.Partial.parquet",
        },
        {
            "table": "POINT_AT",
            "from": "PartialSet",
            "to": "TempoMap",
            "rows": 1,
            "file": "POINT_AT.PartialSet.TempoMap.parquet",
        },
    ]
    assert pq.read_table(tmp_path / rels[0]["file"]).to_pylist() == [
        {"from": 1, "to": 0},
        {"from": 0, "to": 1},
    ]
    assert pq.read_table(tmp_path / rels[1]["file"]).to_pylist() == [
        {"from": 0, "to": 0, "sync_point": False}
    ]


def test_edges_of_one_pair_must_carry_the_same_properties(tmp_path: Path) -> None:
    model = Model()
    s, t = model.add(PartialSetRow("s")), model.add(TempoMapRow("t", 960))
    model.link("POINT_AT", s, t, sync_point=True)
    model.link("POINT_AT", s, t)
    with pytest.raises(ValueError, match="different properties"):
        emit_tables(model, tmp_path)


def test_a_node_file_is_one_row_group_past_pyarrows_default(tmp_path: Path) -> None:
    # Ladybug reads row groups in parallel, which would number the rows out
    # of order and connect relationships to the wrong nodes. pyarrow's
    # default splits a table at 1,048,576 rows.
    rows = 1_048_576 + 1
    path = tmp_path / "big.parquet"
    tables._write_node_file(path, {"x": pa.array(range(rows), pa.int64())}, rows)
    assert pq.ParquetFile(path).metadata.num_row_groups == 1


def test_an_earlier_output_is_replaced(tmp_path: Path) -> None:
    model = Model()
    model.add(PartialRow("p"))
    model.add(PartialSetRow("s"))
    emit_tables(model, tmp_path)
    assert (tmp_path / "PartialSet.parquet").exists()

    smaller = Model()
    smaller.add(PartialRow("p"))
    emit_tables(smaller, tmp_path)
    assert {p.name for p in tmp_path.iterdir()} == {MANIFEST, "Partial.parquet"}


def test_a_directory_holding_other_files_is_refused(tmp_path: Path) -> None:
    (tmp_path / "notes.txt").write_text("mine")
    with pytest.raises(TablesError, match="notes.txt"):
        emit_tables(Model(), tmp_path)
    assert (tmp_path / "notes.txt").read_text() == "mine"


def test_a_file_in_the_way_is_refused(tmp_path: Path) -> None:
    (tmp_path / "out").write_text("mine")
    with pytest.raises(TablesError, match="not a directory"):
        emit_tables(Model(), tmp_path / "out")


def test_cli_writes_tables_by_default(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = tmp_path / "build" / "score"
    assert main(["compile", str(TEMPO_MAP_SCORE), "-o", str(out)]) == 0
    assert manifest_of(out)["format"] == "rainbohz-score-tables"
    assert str(out / MANIFEST) in capsys.readouterr().out


def test_cli_refuses_a_cypher_name_without_debug_cypher(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    out = tmp_path / "score.cypher"
    assert main(["compile", str(TEMPO_MAP_SCORE), "-o", str(out), "-q"]) == 2
    assert "--debug-cypher" in capsys.readouterr().err
    assert not out.exists()


def test_cli_reports_a_directory_it_may_not_write(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    (tmp_path / "notes.txt").write_text("mine")
    assert main(["compile", str(TEMPO_MAP_SCORE), "-o", str(tmp_path), "-q"]) == 1
    assert "notes.txt" in capsys.readouterr().err
