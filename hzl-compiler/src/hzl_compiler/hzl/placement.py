# SPDX-License-Identifier: Apache-2.0

"""A reference placed in musical time, shared by score, entity and source."""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..musical_time import MusicalTime
from ..nodes import Node
from ..ref import Ref


@dataclass(frozen=True)
class Placement:
    """A reference to a child, with where it sits in time relative to its parent.

    Attributes:
        ref: The child.
        offset: Musical time after the parent's start. The score's children
            are placed from the score's time zero, so there it is a position.
        where: Location, for later errors.
    """

    ref: Ref
    offset: MusicalTime
    where: Where


def parse_placements(node: Node, list_key: str, item_key: str, time_key: str) -> tuple[Placement, ...]:
    """Reads a list of `{item_key: {namespace, name}, time_key: "b:t"}` items.

    Args:
        node: The mapping holding the list.
        list_key: Key of the list, e.g. `sound_entities`.
        item_key: Key of the reference within each item, e.g. `sound_entity`.
        time_key: Key of the musical time within each item; optional in the
            file, defaulting to 0:0.

    Raises:
        HzlError: if the list is missing, an item lacks its reference, or the
            time is malformed.
    """
    items = node.require(list_key, "list")
    placements = []
    for index, item in enumerate(items):
        where = node.where.at(list_key).at(index)
        if not isinstance(item, dict):
            raise where.error(f"each item here is a mapping with {item_key!r} and optionally {time_key!r}")
        item_node = Node(item, where)
        item_node.only(item_key, time_key)
        ref = item_node.ref(item_key)
        raw = item_node.optional(time_key, "scalar", 0)
        placements.append(Placement(ref, MusicalTime.parse(raw, where.at(time_key)), where))
    return tuple(placements)
