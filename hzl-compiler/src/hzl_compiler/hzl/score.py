# SPDX-License-Identifier: Apache-2.0

"""`score`: the compilation root, placing sound entities on a tempo map."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from ..errors import Where
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header
from .placement import Placement, parse_placements


class TempoRamp(Enum):
    """The shape of every tempo ramp in the score, by its HzL spelling (ADR 0013)."""

    PHYSICAL_TIME = "physical_time"
    """The tempo changes at a constant rate per second: the default."""

    MUSICAL_POSITION = "musical_position"
    """The tempo changes at a constant rate per beat."""


@dataclass(frozen=True)
class Score:
    """A parsed `score` file.

    Attributes:
        header: The file's header.
        tempo_map: Reference to the one tempo map of this version.
        tempo_ramp: The shape of every ramp in the score's tempo maps.
        sound_entities: The entities in the score, each at its position.
        where: Location of the body.
    """

    header: Header
    tempo_map: Ref
    tempo_ramp: TempoRamp
    sound_entities: tuple[Placement, ...]
    where: Where


def parse_score(node: Node) -> Score:
    """Reads a `score` body.

    Raises:
        HzlError: if the tempo map reference or the entity list is missing or
            malformed, `tempo_ramp` is not a known shape, or a key is unknown.
    """
    node.only(*HEADER_KEYS, "tempo_map", "tempo_ramp", "sound_entities")
    header = parse_header(node)
    text = node.optional("tempo_ramp", "str", TempoRamp.PHYSICAL_TIME.value)
    try:
        tempo_ramp = TempoRamp(text)
    except ValueError:
        names = ", ".join(repr(t.value) for t in TempoRamp)
        raise node.error(
            f"unknown tempo_ramp {text!r}; the shapes are: {names}", "tempo_ramp"
        ) from None
    return Score(
        header,
        node.ref("tempo_map"),
        tempo_ramp,
        parse_placements(node, "sound_entities", "sound_entity", "position"),
        node.where,
    )


def load_score(tree: SourceTree, ref: Ref) -> Score:
    """Loads and parses the `score` that `ref` names."""
    return parse_score(tree.load(ref, "score"))
