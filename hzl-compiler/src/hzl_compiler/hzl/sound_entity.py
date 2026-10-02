"""`sound_entity`: a sound present over a period, holding sound sources."""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header
from .placement import Placement, parse_placements


@dataclass(frozen=True)
class SoundEntity:
    """A parsed `sound_entity` file.

    Attributes:
        header: The file's header.
        sound_sources: The sources under this entity, each with its offset from
            the entity's start.
        where: Location of the body.
    """

    header: Header
    sound_sources: tuple[Placement, ...]
    where: Where


def parse_sound_entity(node: Node) -> SoundEntity:
    """Reads a `sound_entity` body.

    Raises:
        HzlError: if the source list is missing or malformed, or a key is unknown.
    """
    node.only(*HEADER_KEYS, "sound_sources")
    header = parse_header(node)
    return SoundEntity(header, parse_placements(node, "sound_sources", "sound_source", "offset"), node.where)


def load_sound_entity(tree: SourceTree, ref: Ref) -> SoundEntity:
    """Loads and parses the `sound_entity` that `ref` names."""
    return parse_sound_entity(tree.load(ref, "sound_entity"))
