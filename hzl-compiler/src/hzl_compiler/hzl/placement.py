# SPDX-License-Identifier: Apache-2.0

"""A reference placed in musical time, shared by score, entity and source.

A score places each sound entity on a tempo map, the root by default, and
everything under the entity counts in that map's ticks. An envelope placement
may name another map (polytempo); it then gives an absolute `position` on that
map instead of an `offset`, since where the entity's start falls in another
map's ticks is only known once the maps are resolved.
"""

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
            are placed from the score's time zero, so there it is a position;
            so it is too on an envelope that names its own `tempo_map`.
        where: Location, for later errors.
        tempo_map: The map the child keeps time by, when the placement names
            one; None for the parent's.
    """

    ref: Ref
    offset: MusicalTime
    where: Where
    tempo_map: Ref | None = None


def parse_placements(
    node: Node,
    list_key: str,
    item_key: str,
    time_key: str,
    *,
    tempo_map_time_key: str | None = None,
) -> tuple[Placement, ...]:
    """Reads a list of `{item_key: {namespace, name}, time_key: "b:t"}` items.

    Args:
        node: The mapping holding the list.
        list_key: Key of the list, e.g. `sound_entities`.
        item_key: Key of the reference within each item, e.g. `sound_entity`.
        time_key: Key of the musical time within each item; optional in the
            file, defaulting to 0:0.
        tempo_map_time_key: When given, an item may name a `tempo_map`, and
            this is the key of its time then. If it is `time_key`, the time
            reads the same with or without the map. Otherwise the map comes
            with an absolute time under this key, required, in place of
            `time_key`, which is refused, as this key is without a map.

    Raises:
        HzlError: if the list is missing, an item lacks its reference, the
            time is malformed, or the time keys and `tempo_map` are mixed
            as described above.
    """
    items = node.require(list_key, "list")
    placements = []
    for index, item in enumerate(items):
        where = node.where.at(list_key).at(index)
        if not isinstance(item, dict):
            raise where.error(
                f"each item here is a mapping with {item_key!r} and optionally {time_key!r}"
            )
        item_node = Node(item, where)
        if tempo_map_time_key is None:
            item_node.only(item_key, time_key)
        else:
            item_node.only(*dict.fromkeys((item_key, "tempo_map", time_key, tempo_map_time_key)))
        ref = item_node.ref(item_key)
        tempo_map = item_node.optional_ref("tempo_map") if tempo_map_time_key else None
        key = time_key
        if tempo_map_time_key not in (None, time_key):
            if tempo_map is not None:
                item_node.reject(
                    time_key,
                    f"an {item_key} on its own tempo_map starts at an absolute "
                    f"{tempo_map_time_key!r} on that map; {time_key!r} counts on the "
                    f"sound entity's map",
                )
                key = tempo_map_time_key
                raw = item_node.require(key, "scalar")
            else:
                item_node.reject(
                    tempo_map_time_key,
                    f"{tempo_map_time_key!r} is the start on the tempo_map an {item_key} names; "
                    f"without 'tempo_map', write {time_key!r}, counted on the sound entity's map",
                )
                raw = item_node.optional(key, "scalar", 0)
        else:
            raw = item_node.optional(key, "scalar", 0)
        placements.append(Placement(ref, MusicalTime.parse(raw, where.at(key)), where, tempo_map))
    return tuple(placements)
