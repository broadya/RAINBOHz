# SPDX-License-Identifier: Apache-2.0

from pathlib import Path

import pytest
import yaml
from conftest import BASE, TreeBuilder, hzl

from hzl_compiler import source
from hzl_compiler.errors import HzlError
from hzl_compiler.hzl.partial_state_set import load_partial_state_set
from hzl_compiler.ref import Ref
from hzl_compiler.source import SourceTree

EMPTY_SET = "partial_states: []"


def test_from_file_works_out_the_root_from_a_nested_namespace(
    tmp_path: Path, tree: TreeBuilder
) -> None:
    t = tree({"a/b/thing": hzl("partial_state_set", "a/b/thing", EMPTY_SET)})
    opened, ref = SourceTree.from_file(tmp_path / "a" / "b" / "thing.yaml")
    assert opened.root == tmp_path.resolve()
    assert ref == Ref("a/b", "thing")
    assert t.load(ref, "partial_state_set").keys()[0] == "hzl_version"


def test_from_file_refuses_a_file_outside_its_namespace(tmp_path: Path, tree: TreeBuilder) -> None:
    tree({"a/thing": hzl("partial_state_set", "b/thing", EMPTY_SET)})
    with pytest.raises(HzlError, match="namespace 'b' does not match the directories"):
        SourceTree.from_file(tmp_path / "a" / "thing.yaml")


def test_header_must_agree_with_location(tree: TreeBuilder) -> None:
    t = tree({"ns/x": hzl("partial_state_set", "ns/y", EMPTY_SET)})
    with pytest.raises(HzlError, match="name 'y' does not match the file name 'x'"):
        t.load(Ref("ns", "x"), "partial_state_set")


def test_version_and_type_are_checked(tree: TreeBuilder) -> None:
    t = tree({"ns/x": hzl("partial_state_set", "ns/x", EMPTY_SET).replace('"0.1.0"', '"9.0"')})
    with pytest.raises(HzlError, match="hzl_version '9.0' is not '0.1.0'"):
        t.load(Ref("ns", "x"), "partial_state_set")
    t = tree(
        {
            "ns/z": hzl("partial_state_set", "ns/z", EMPTY_SET).replace(
                "hzl_type: partial_state_set", "hzl_type: scale"
            )
        }
    )
    with pytest.raises(HzlError, match="hzl_type 'scale' does not match the top-level key"):
        t.load(Ref("ns", "z"), "partial_state_set")


def test_missing_file_and_wrong_type_are_reported(tree: TreeBuilder) -> None:
    t = tree(BASE)
    with pytest.raises(HzlError, match="no file for reference ns/missing"):
        t.load(Ref("ns", "missing"), "scale")
    with pytest.raises(
        HzlError, match="tempo_maps/main is a tempo_map, but a scale is needed here"
    ):
        t.load(Ref("tempo_maps", "main"), "scale")


def test_unknown_keys_are_refused_with_their_path(tree: TreeBuilder) -> None:
    t = tree({"ns/x": hzl("partial_state_set", "ns/x", EMPTY_SET + "\ncolour: blue")})
    with pytest.raises(HzlError, match=r"partial_state_set.colour: unknown key 'colour'"):
        load_partial_state_set(t, Ref("ns", "x"))


def test_a_file_must_be_one_known_type(tree: TreeBuilder) -> None:
    t = tree({"ns/x": "banana:\n  a: 1\n"})
    with pytest.raises(HzlError, match="unknown HzL type 'banana'"):
        t.load_any(Ref("ns", "x"))
    t = tree({"ns/y": "a: 1\nb: 2\n"})
    with pytest.raises(HzlError, match="exactly one top-level key"):
        t.load_any(Ref("ns", "y"))
    t = tree({"ns/z": "score: [\n"})
    with pytest.raises(HzlError, match="not valid YAML"):
        t.load_any(Ref("ns", "z"))


def test_files_are_parsed_with_libyaml() -> None:
    assert source._LOADER is yaml.CSafeLoader


def test_a_yaml_error_quotes_the_line_and_marks_the_fault(tree: TreeBuilder) -> None:
    # libyaml's own message has no source line or caret; the pure-Python
    # parser's is reported instead.
    t = tree({"ns/z": "score:\n  oops: [1\n  x: 2\n"})
    with pytest.raises(HzlError) as raised:
        t.load_any(Ref("ns", "z"))
    assert "line 2, column 9:\n      oops: [1\n            ^" in raised.value.message
