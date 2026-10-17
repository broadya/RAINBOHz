# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

import pytest

from hzl_compiler.canonical import cypher
from hzl_compiler.canonical.cypher import column_type, emit_cypher, literal, value_type
from hzl_compiler.canonical.model import (
    Model,
    PartialRow,
    PartialSetRow,
    PartialStateRow,
    PartialStateSetRow,
    ScoreRow,
    TempoMapPointRow,
    TempoMapRow,
)
from hzl_compiler.cli import main
from hzl_compiler.lower.score import EngineOptions, lower_score
from hzl_compiler.source import SourceTree

EXAMPLES = Path(__file__).resolve().parent.parent / "examples"
EXPECTED = Path(__file__).resolve().parent / "expected"


@pytest.mark.parametrize(
    "value, text",
    [
        (0, "0"),
        (3840, "3840"),
        (440.0, "440.0"),
        (0.0078125, "0.0078125"),
        (1e-05, "0.00001"),
        (1e21, "1000000000000000000000.0"),
        (True, "true"),
        (False, "false"),
        ('a "b" \\ c', '"a \\"b\\" \\\\ c"'),
    ],
)
def test_literals(value: object, text: str) -> None:
    assert literal(value) == text


def test_literal_refuses_other_types() -> None:
    with pytest.raises(TypeError):
        literal(None)


def statements(script: str) -> list[str]:
    """The script's statements, comments dropped, each joined onto one line."""
    text = " ".join(line.strip() for line in script.splitlines() if not line.startswith("//"))
    return [part.strip() + ";" for part in text.split(";") if part.strip()]


def test_a_null_is_written_with_its_column_type() -> None:
    # A bare null in the first row would make Ladybug type the column STRING.
    model = Model()
    model.add(PartialStateRow(frequency=None, amplitude=-6.0))
    model.add(PartialStateRow(frequency=440.0, amplitude=None, phase=90.0, phase_free_run=False))
    [statement] = statements(emit_cypher(model))
    assert statement == (
        "UNWIND [ {frequency: CAST(NULL AS DOUBLE), amplitude: -6.0, "
        "phase: CAST(NULL AS DOUBLE), phase_free_run: CAST(NULL AS BOOL)}, "
        "{frequency: 440.0, amplitude: CAST(NULL AS DOUBLE), phase: 90.0, "
        "phase_free_run: false} ] AS r CREATE (n:PartialState {frequency: r.frequency, "
        "amplitude: r.amplitude, phase: r.phase, phase_free_run: r.phase_free_run});"
    )


def test_a_table_without_columns_is_created_by_count() -> None:
    model = Model()
    for _ in range(3):
        model.add(PartialStateSetRow())
    assert statements(emit_cypher(model)) == ["UNWIND range(1, 3) AS r CREATE (n:PartialStateSet);"]


def test_rows_are_chunked_and_ids_run_on(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(cypher, "CHUNK_ROWS", 2)
    model = Model()
    sets = model.add(PartialSetRow("s"))
    for index in range(5):
        model.link("HAS_PARTIAL", sets, model.add(PartialRow(f"p{index}")))
    script = statements(emit_cypher(model))
    partials = [s for s in script if "CREATE (n:Partial " in s]
    copies = [s for s in script if s.startswith("COPY HAS_PARTIAL")]
    assert [s.count('{name: "') for s in partials] == [2, 2, 1]
    assert '{name: "p4"}' in partials[2]
    assert [s.count("{from:") for s in copies] == [2, 2, 1]
    assert "{from: 0, to: 4}" in copies[2]
    # Nodes come before every relationship that names them.
    assert script.index(partials[-1]) < script.index(copies[0])


def test_each_table_pair_of_a_relationship_is_its_own_copy() -> None:
    model = Model()
    score = model.add(ScoreRow("s", True))
    tempo_map = model.add(TempoMapRow("m", 960))
    model.link("NEXT_POINT", score, tempo_map)
    for position in (0, 960):
        point = model.add(TempoMapPointRow(position, 120.0, False))
        model.link("NEXT_POINT", tempo_map, point)
    copies = [s for s in statements(emit_cypher(model)) if s.startswith("COPY")]
    assert copies == [
        "COPY NEXT_POINT FROM (UNWIND [ {from: 0, to: 0} ] AS r RETURN r.from, r.to) "
        "(from='Score', to='TempoMap');",
        "COPY NEXT_POINT FROM (UNWIND [ {from: 0, to: 0}, {from: 0, to: 1} ] AS r "
        "RETURN r.from, r.to) (from='TempoMap', to='TempoMapPoint');",
    ]


def test_relationship_properties_are_columns_of_the_copy() -> None:
    model = Model()
    tempo_map = model.add(TempoMapRow("m", 960))
    point = model.add(PartialSetRow("s"))
    model.link("POINT_AT", point, tempo_map, sync_point=3840, offset_seconds=0.25)
    [copy] = [s for s in statements(emit_cypher(model)) if s.startswith("COPY")]
    assert "{from: 0, to: 0, sync_point: 3840, offset_seconds: 0.25}" in copy
    assert "RETURN r.from, r.to, r.sync_point, r.offset_seconds)" in copy


def test_edges_of_one_relationship_must_carry_the_same_properties() -> None:
    model = Model()
    tempo_map = model.add(TempoMapRow("m", 960))
    point = model.add(PartialSetRow("s"))
    model.link("POINT_AT", point, tempo_map, sync_point=0)
    model.link("POINT_AT", point, tempo_map)
    with pytest.raises(ValueError, match="POINT_AT"):
        emit_cypher(model)


@pytest.mark.parametrize(
    "annotation, kind",
    [("str", "STRING"), ("int", "INT64"), ("float | None", "DOUBLE"), ("bool | None", "BOOL")],
)
def test_column_types(annotation: str, kind: str) -> None:
    assert column_type(annotation) == kind


@pytest.mark.parametrize("annotation", ["list[int]", "int | str", "None"])
def test_column_type_refuses_other_annotations(annotation: str) -> None:
    with pytest.raises(TypeError):
        column_type(annotation)


def test_value_types() -> None:
    assert value_type([None, True]) == "BOOL"
    assert value_type([1, 2]) == "INT64"
    with pytest.raises(TypeError):
        value_type([None, None])
    with pytest.raises(TypeError):
        value_type([1, 2.0])


def strip_comments(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.startswith("//"))


def test_tempo_map_twin_matches_the_expected_script() -> None:
    tree, ref = SourceTree.from_file(
        EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml"
    )
    script = emit_cypher(lower_score(tree, ref))
    expected = (EXPECTED / "tempo-map-score.cypher").read_text(encoding="utf-8")
    assert strip_comments(script) == strip_comments(expected)


def test_spatial_twin_matches_the_expected_script() -> None:
    # Rendered with channel mode 1, this script gives the frames of
    # engine-db/examples/spatial-musical-score.cypher to within 2e-12
    # (checked by hand on 2026-10-05); this test keeps the script fixed.
    tree, ref = SourceTree.from_file(EXAMPLES / "spatial-score" / "scores" / "spatial_score.yaml")
    script = emit_cypher(lower_score(tree, ref, EngineOptions(channel_mode=1)))
    expected = (EXPECTED / "spatial-score.cypher").read_text(encoding="utf-8")
    assert strip_comments(script) == strip_comments(expected)


def test_cli_compiles_and_dumps(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = tmp_path / "out" / "score.cypher"
    dump = tmp_path / "out" / "score.json"
    code = main(
        [
            "compile",
            str(EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml"),
            "-o",
            str(out),
            "--dump-canonical",
            str(dump),
        ]
    )
    assert code == 0
    assert out.read_text().startswith("// ====")
    assert '"tables"' in dump.read_text()
    assert "compiled scores/tempo_map_score" in capsys.readouterr().out


def test_cli_header_names_the_tree_without_its_path(tmp_path: Path) -> None:
    # The script must be the same whichever machine or directory compiled it.
    score = EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml"
    out = tmp_path / "o.cypher"
    assert main(["compile", str(score), "-o", str(out), "-q"]) == 0
    header = [line for line in out.read_text().splitlines() if line.startswith("//")]
    assert "// Score scores/tempo_map_score, from source tree tempo-map-score" in header
    assert not any(str(EXAMPLES) in line for line in header)


def test_cli_reports_hzl_errors(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    bad = tmp_path / "scores" / "s.yaml"
    bad.parent.mkdir()
    bad.write_text(
        'score:\n  hzl_version: "0.1.0"\n  hzl_type: score\n  name: s\n  namespace: scores\n'
        "  layer: x\n  tempo_map: {namespace: t, name: m}\n  sound_entities: []\n"
    )
    assert main(["compile", str(bad), "-o", str(tmp_path / "o.cypher"), "-q"]) == 1
    assert "no file for reference t/m" in capsys.readouterr().err
