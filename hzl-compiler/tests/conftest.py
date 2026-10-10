# SPDX-License-Identifier: Apache-2.0

"""Shared fixtures: build a small HzL source tree in a temporary directory."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from textwrap import dedent, indent

import pytest

from hzl_compiler.ref import Ref
from hzl_compiler.source import SourceTree

TreeBuilder = Callable[[dict[str, str]], SourceTree]


def hzl(hzl_type: str, ref: str, body: str, labels: str = "[]", layer: str = "0: base") -> str:
    """Assembles an HzL file of `hzl_type` at `ref` (`namespace/name`) around `body`.

    `body` is the type's own keys, at any indentation; it is dedented and
    re-indented under the top-level key, after the header.
    """
    namespace, name = ref.rsplit("/", 1)
    return (
        f"{hzl_type}:\n"
        f'  hzl_version: "0.1.0"\n'
        f"  hzl_type: {hzl_type}\n"
        f"  name: {name}\n"
        f"  namespace: {namespace}\n"
        f'  layer: "{layer}"\n'
        f"  labels: {labels}\n" + indent(dedent(body).strip("\n") + "\n", "  ")
    )


def write_tree(root: Path, files: dict[str, str]) -> SourceTree:
    """Writes `files` (`namespace/name` -> YAML text) under `root` and opens a tree over it."""
    for ref, text in files.items():
        path = Ref(*ref.rsplit("/", 1)).path(root)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return SourceTree(root)


@pytest.fixture
def tree(tmp_path: Path) -> TreeBuilder:
    """Returns a builder: `tree({"ns/name": yaml, ...}) -> SourceTree`."""
    return lambda files: write_tree(tmp_path, files)


# A tempo map, two transformations and the hierarchy around one envelope,
# which most tests share. Each test adds the state sets and envelope it is
# about.
BASE = {
    "tempo_maps/main": hzl(
        "tempo_map",
        "tempo_maps/main",
        """
        ticks_per_beat: 960
        points:
          - tempo_map_point: {position: "0:0", tempo: 120.0, hold: true}
        """,
    ),
    "transformations/update": hzl(
        "partial_state_set_transformation",
        "transformations/update",
        """
        transformation:
          transformation_type: "Update"
        """,
    ),
    "transformations/product": hzl(
        "partial_state_set_transformation",
        "transformations/product",
        """
        transformation:
          transformation_type: "Flattened Cartesian Product"
        """,
        labels="[product]",
    ),
    "scores/score": hzl(
        "score",
        "scores/score",
        """
        tempo_map: {namespace: tempo_maps, name: main}
        sound_entities:
          - sound_entity: {namespace: entities, name: entity}
            position: "1:0"
        """,
        labels="[demo]",
    ),
    "entities/entity": hzl(
        "sound_entity",
        "entities/entity",
        """
        sound_sources:
          - sound_source: {namespace: sources, name: source}
            offset: "0:480"
        """,
    ),
    "sources/source": hzl(
        "sound_source",
        "sources/source",
        """
        envelopes:
          - envelope: {namespace: envelopes, name: env}
            offset: "1:0"
        """,
    ),
    "state_sets/start": hzl(
        "partial_state_set",
        "state_sets/start",
        """
        partial_states:
          - partial_state: {identifier: a, frequency: 440.0, amplitude: -6.0, labels: [lead, x]}
          - partial_state: {identifier: b, frequency: 660.0, amplitude: -6.0, labels: [pad]}
        """,
    ),
    "state_sets/end": hzl(
        "partial_state_set",
        "state_sets/end",
        """
        partial_states:
          - partial_state: {identifier: a, frequency: 220.0, amplitude: -144.0}
          - partial_state: {identifier: b, frequency: 330.0, amplitude: -144.0}
        """,
    ),
}
