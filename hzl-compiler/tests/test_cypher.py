from pathlib import Path

import pytest

from hzl_compiler.canonical.cypher import emit_cypher, literal
from hzl_compiler.cli import main
from hzl_compiler.lower.score import lower_score
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
def test_literals(value, text):
    assert literal(value) == text


def test_literal_refuses_other_types():
    with pytest.raises(TypeError):
        literal(None)


def strip_comments(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.startswith("//"))


def test_tempo_map_twin_matches_the_expected_script():
    tree, ref = SourceTree.from_file(EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml")
    script = emit_cypher(lower_score(tree, ref))
    expected = (EXPECTED / "tempo-map-score.cypher").read_text(encoding="utf-8")
    assert strip_comments(script) == strip_comments(expected)


def test_cli_compiles_and_dumps(tmp_path, capsys):
    out = tmp_path / "out" / "score.cypher"
    dump = tmp_path / "out" / "score.json"
    code = main(["compile", str(EXAMPLES / "tempo-map-score" / "scores" / "tempo_map_score.yaml"), "-o", str(out), "--dump-canonical", str(dump)])
    assert code == 0
    assert out.read_text().startswith("// ====")
    assert '"tables"' in dump.read_text()
    assert "compiled scores/tempo_map_score" in capsys.readouterr().out


def test_cli_reports_hzl_errors(tmp_path, capsys):
    bad = tmp_path / "scores" / "s.yaml"
    bad.parent.mkdir()
    bad.write_text('score:\n  hzl_version: "0.1.0"\n  hzl_type: score\n  name: s\n  namespace: scores\n  layer: x\n  tempo_map: {namespace: t, name: m}\n  sound_entities: []\n')
    assert main(["compile", str(bad), "-o", str(tmp_path / "o.cypher"), "-q"]) == 1
    assert "no file for reference t/m" in capsys.readouterr().err
