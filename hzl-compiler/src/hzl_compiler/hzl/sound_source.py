# SPDX-License-Identifier: Apache-2.0

"""`sound_source`: the level with one spatial position, holding envelopes.

A sound source's position is its trajectory, a file of its own that the
source names; `SpatialPositionMusicalTimeline` hangs off `SoundSource` in
`engine-db`. Every partial in every envelope under the source shares that
position (ADR 0020). A source that names no trajectory is straight ahead at
1 m (`trajectory.DEFAULT_POSITION`).
"""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header
from .placement import Placement, parse_placements


@dataclass(frozen=True)
class SoundSource:
    """A parsed `sound_source` file.

    Attributes:
        header: The file's header.
        trajectory: The source's trajectory, or None for the default position.
        envelopes: The envelopes under this source, each with its offset from
            the source's start.
        where: Location of the body.
    """

    header: Header
    trajectory: Ref | None
    envelopes: tuple[Placement, ...]
    where: Where


def parse_sound_source(node: Node) -> SoundSource:
    """Reads a `sound_source` body.

    Raises:
        HzlError: if `position` is present (a position is written as a
            trajectory), the trajectory reference or the envelope list is
            malformed, or a key is unknown.
    """
    node.reject("position", "a source's position is its trajectory: write it in a trajectory file and name it with 'trajectory'")
    node.only(*HEADER_KEYS, "trajectory", "envelopes")
    header = parse_header(node)
    return SoundSource(
        header,
        node.optional_ref("trajectory"),
        parse_placements(node, "envelopes", "envelope", "offset"),
        node.where,
    )


def load_sound_source(tree: SourceTree, ref: Ref) -> SoundSource:
    """Loads and parses the `sound_source` that `ref` names."""
    return parse_sound_source(tree.load(ref, "sound_source"))
