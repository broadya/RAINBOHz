"""`sound_source`: the level with one spatial position, holding envelopes.

A sound source is where a spatial position will be written when this
compiler carries one: `SpatialPositionMusicalTimeline` hangs off
`SoundSource` in `engine-db`. In this version a `position` key is refused.
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
        envelopes: The envelopes under this source, each with its offset from
            the source's start.
        where: Location of the body.
    """

    header: Header
    envelopes: tuple[Placement, ...]
    where: Where


def parse_sound_source(node: Node) -> SoundSource:
    """Reads a `sound_source` body.

    Raises:
        HzlError: if `position` is present (not in this version), the envelope
            list is missing or malformed, or a key is unknown.
    """
    node.reject("position", "spatial position is not in this version of the compiler; leave it out")
    node.only(*HEADER_KEYS, "envelopes")
    header = parse_header(node)
    return SoundSource(header, parse_placements(node, "envelopes", "envelope", "offset"), node.where)


def load_sound_source(tree: SourceTree, ref: Ref) -> SoundSource:
    """Loads and parses the `sound_source` that `ref` names."""
    return parse_sound_source(tree.load(ref, "sound_source"))
