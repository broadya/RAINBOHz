# SPDX-License-Identifier: Apache-2.0

"""Tests for check_engine_db_mapping.py: each disagreement it must catch."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from check_engine_db_mapping import run  # noqa: E402

SCHEMA = """
CREATE NODE TABLE A ( id SERIAL, name STRING, size INT32, PRIMARY KEY(id) );
CREATE NODE TABLE B ( id SERIAL, PRIMARY KEY(id) );
CREATE NODE TABLE Later ( id SERIAL, PRIMARY KEY(id) );
CREATE NODE TABLE Frame ( frame_number INT32, PRIMARY KEY(frame_number) );
CREATE REL TABLE LINKS ( FROM A TO B, FROM A TO Later, weight DOUBLE, ONE_MANY );
CREATE REL TABLE IN_FRAME ( FROM A TO Frame, MANY_MANY );
"""

COMPLETE = """
/// @enginedb{node A}
/// @enginedb{A.name}
/// @enginedb{A.size}
/// @enginedb{node B}
/// @enginedb_unmapped{Later: not built yet}
/// @enginedb{node Frame}
/// @enginedb{Frame.frame_number: the set of frames}
/// @enginedb{rel LINKS A->B: edges}
/// @enginedb{LINKS.weight}
/// @enginedb{rel IN_FRAME A->Frame: a range}
"""


def check(tmp_path: Path, source: str) -> list[str]:
    """Runs the check on SCHEMA and one source file holding `source`."""
    schema = tmp_path / "schema.cypher"
    schema.write_text(SCHEMA, encoding="utf-8")
    header = tmp_path / "model.hpp"
    header.write_text(source, encoding="utf-8")
    return run(schema, [header])


def without(line: str) -> str:
    """COMPLETE with one line removed."""
    assert line in COMPLETE
    return COMPLETE.replace(line + "\n", "")


def test_complete_tags_pass(tmp_path: Path) -> None:
    assert check(tmp_path, COMPLETE) == []


@pytest.mark.parametrize(
    ("missing", "expected"),
    [
        ("/// @enginedb{node B}", "node table B is neither"),
        ("/// @enginedb{A.size}", "property A.size is neither"),
        ("/// @enginedb{Frame.frame_number: the set of frames}", "property Frame.frame_number"),
        ("/// @enginedb{rel LINKS A->B: edges}", "relationship LINKS A->B is neither"),
        ("/// @enginedb{LINKS.weight}", "property LINKS.weight is neither"),
        ("/// @enginedb_unmapped{Later: not built yet}", "node table Later is neither"),
    ],
)
def test_a_missing_tag_fails(tmp_path: Path, missing: str, expected: str) -> None:
    problems = check(tmp_path, without(missing))
    assert any(expected in problem for problem in problems), problems


@pytest.mark.parametrize(
    ("extra", "expected"),
    [
        ("/// @enginedb{node Gone}", "no node table Gone"),
        ("/// @enginedb{A.colour}", "A has no property colour"),
        ("/// @enginedb{rel LINKS B->A: edges}", "LINKS has no pair B->A"),
        ("/// @enginedb{rel NOPE A->B: edges}", "no relationship table NOPE"),
        ("/// @enginedb{rel LINKS A->Later: edges}", "one of its tables is not"),
        ("/// @enginedb{A.name}", "already tagged"),
        ("/// @enginedb_unmapped{A.size: never}", "both held and never set"),
        ("/// @enginedb{node Later}", "both held and not held"),
    ],
)
def test_a_stale_or_conflicting_tag_fails(tmp_path: Path, extra: str, expected: str) -> None:
    problems = check(tmp_path, COMPLETE + extra + "\n")
    assert any(expected in problem for problem in problems), problems


@pytest.mark.parametrize(
    ("tag", "expected"),
    [
        ("/// @enginedb_unmapped{Later}", "does not say why it is not held"),
        ("/// @enginedb{rel IN_FRAME A->Frame}", "does not say how it is held"),
        ("/// @enginedb{rel IN_FRAME A->Frame: a range, of frames}", "comma"),
    ],
)
def test_a_tag_without_its_note_or_with_a_comma_fails(
    tmp_path: Path, tag: str, expected: str
) -> None:
    source = COMPLETE.replace("/// @enginedb_unmapped{Later: not built yet}", "").replace(
        "/// @enginedb{rel IN_FRAME A->Frame: a range}", ""
    )
    source += "/// @enginedb_unmapped{Later: not built yet}\n" if "Later" not in tag else ""
    source += "/// @enginedb{rel IN_FRAME A->Frame: a range}\n" if "IN_FRAME" not in tag else ""
    problems = check(tmp_path, source + tag + "\n")
    assert any(expected in problem for problem in problems), problems


def test_a_tag_broken_across_comment_lines_is_read_whole(tmp_path: Path) -> None:
    source = without("/// @enginedb{rel IN_FRAME A->Frame: a range}")
    source += "/// @enginedb{rel IN_FRAME\n///     A->Frame: a range}\n"
    assert check(tmp_path, source) == []


def test_a_tag_in_an_ordinary_comment_is_ignored(tmp_path: Path) -> None:
    assert check(tmp_path, COMPLETE + "// @enginedb{node Gone}\n") == []


def test_the_real_model_agrees_with_the_real_schema() -> None:
    root = Path(__file__).resolve().parents[2]
    header = root / "rendering-engine" / "include" / "rainbohz" / "rendering_engine"
    problems = run(
        root / "engine-db" / "schema" / "engine-schema.cypher", [header / "native_model.hpp"]
    )
    assert problems == []
