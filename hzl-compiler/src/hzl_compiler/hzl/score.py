# SPDX-License-Identifier: Apache-2.0

"""`score`: the compilation root, placing sound entities on a tempo map."""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header
from .placement import Placement, parse_placements


@dataclass(frozen=True)
class Score:
    """A parsed `score` file.

    Attributes:
        header: The file's header.
        tempo_map: Reference to the one tempo map of this version.
        sound_entities: The entities in the score, each at its position.
        where: Location of the body.
    """

    header: Header
    tempo_map: Ref
    sound_entities: tuple[Placement, ...]
    where: Where


def parse_score(node: Node) -> Score:
    """Reads a `score` body.

    Raises:
        HzlError: if the tempo map reference or the entity list is missing or
            malformed, or a key is unknown.
    """
    node.only(*HEADER_KEYS, "tempo_map", "sound_entities")
    header = parse_header(node)
    return Score(
        header,
        node.ref("tempo_map"),
        parse_placements(node, "sound_entities", "sound_entity", "position"),
        node.where,
    )


def load_score(tree: SourceTree, ref: Ref) -> Score:
    """Loads and parses the `score` that `ref` names."""
    return parse_score(tree.load(ref, "score"))
