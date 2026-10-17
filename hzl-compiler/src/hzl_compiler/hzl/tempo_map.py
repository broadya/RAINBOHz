# SPDX-License-Identifier: Apache-2.0

"""`tempo_map`: tempo against musical position, the clock every placement reads.

The validity rules are the transform's (rendering-engine/docs/transform.md
§6.1, ADR 0013), checked here so that they are reported in HzL terms. A map
other than the score's root ties itself to parent maps with synchronisation
points (rendering-engine/docs/polytempo.md §4); the rules that need the
parent are checked when the maps are lowered together (lower/tempo_maps.py).
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from ..errors import Where
from ..musical_time import MusicalTime
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header

MIN_TEMPO_BPM = 1.0
MAX_TEMPO_BPM = 1000.0


@dataclass(frozen=True)
class TempoMapPoint:
    """A point of a tempo map.

    Attributes:
        position: Where the point is, on this map.
        position_ticks: The same, as ticks of this map.
        tempo: BPM from this point, in [1, 1000].
        hold: True holds `tempo` until the next point; False ramps to the next
            point's tempo, in the score's `tempo_ramp` shape.
    """

    position: MusicalTime
    position_ticks: int
    tempo: float
    hold: bool


@dataclass(frozen=True)
class SyncPoint:
    """A synchronisation point: a point of this map tied to a parent map.

    Attributes:
        position: Where the tie is, on this map: one of its points.
        position_ticks: The same, as ticks of this map.
        parent: The parent map, in a strictly lower layer.
        parent_position: Where the tie is on the parent, in the parent's
            beats:ticks; converted to ticks once the parent is loaded.
        where: Location of the item, for later errors.
    """

    position: MusicalTime
    position_ticks: int
    parent: Ref
    parent_position: MusicalTime
    where: Where


@dataclass(frozen=True)
class TempoMap:
    """A parsed and validated `tempo_map` file.

    Attributes:
        header: The file's header.
        ticks_per_beat: Resolution of every musical time on this map.
        points: At least one, the first at position 0, strictly increasing,
            the last holding.
        sync_points: This map's ties to its parents, in file order, each at
            one of `points` and no two at the same tick. Empty on the root.
        where: Location of the body.
    """

    header: Header
    ticks_per_beat: int
    points: tuple[TempoMapPoint, ...]
    sync_points: tuple[SyncPoint, ...]
    where: Where


def parse_tempo_map(node: Node) -> TempoMap:
    """Reads and validates a `tempo_map` body.

    Raises:
        HzlError: if `ticks_per_beat` is not positive; there are no points;
            the first is not at 0; positions do not strictly increase; a tempo
            is outside [1, 1000] or not finite; the last point does not hold;
            or a synchronisation point is not at one of the points, or shares
            its tick with another.
    """
    node.only(*HEADER_KEYS, "ticks_per_beat", "points", "synchronization_points")
    header = parse_header(node)
    ticks_per_beat = node.require("ticks_per_beat", "int")
    if ticks_per_beat < 1:
        raise node.error(
            f"ticks_per_beat must be 1 or more, got {ticks_per_beat}", "ticks_per_beat"
        )
    items = node.sequence("points", "tempo_map_point")
    if not items:
        raise node.error("a tempo map needs at least one point", "points")
    points: list[TempoMapPoint] = []
    for index, item in enumerate(items):
        item.only("position", "tempo", "hold")
        position = MusicalTime.parse(item.require("position", "scalar"), item.where.at("position"))
        ticks = position.to_ticks(ticks_per_beat, item.where.at("position"))
        tempo = item.require("tempo", "float")
        if not (math.isfinite(tempo) and MIN_TEMPO_BPM <= tempo <= MAX_TEMPO_BPM):
            raise item.error(
                f"tempo is BPM in [{MIN_TEMPO_BPM:g}, {MAX_TEMPO_BPM:g}], got {tempo!r}", "tempo"
            )
        hold = item.require("hold", "bool")
        if index == 0 and ticks != 0:
            raise item.error(
                f"the first point must be at position 0:0, time zero of the score; got {position}",
                "position",
            )
        if points and ticks <= points[-1].position_ticks:
            raise item.error(
                f"positions must strictly increase; {position} is not after {points[-1].position}",
                "position",
            )
        points.append(TempoMapPoint(position, ticks, tempo, hold))
    if not points[-1].hold:
        raise items[-1].error("the last point must hold: there is no next tempo to ramp to", "hold")
    sync_points = _parse_sync_points(node, ticks_per_beat, points)
    return TempoMap(header, ticks_per_beat, tuple(points), sync_points, node.where)


def _parse_sync_points(
    node: Node, ticks_per_beat: int, points: list[TempoMapPoint]
) -> tuple[SyncPoint, ...]:
    """Reads the optional `synchronization_points` of a `tempo_map` body.

    Raises:
        HzlError: if an item is malformed, its `position` is not one of the
            map's points, or two items share a position.
    """
    at_point = {point.position_ticks for point in points}
    seen: dict[int, Where] = {}
    sync_points: list[SyncPoint] = []
    for item in node.sequence("synchronization_points", "synchronization_point", required=False):
        item.only("position", "parent", "parent_position")
        position = MusicalTime.parse(item.require("position", "scalar"), item.where.at("position"))
        ticks = position.to_ticks(ticks_per_beat, item.where.at("position"))
        if ticks not in at_point:
            raise item.error(
                f"a synchronization point is at one of this map's points; "
                f"there is no tempo_map_point at {position}",
                "position",
            )
        if ticks in seen:
            raise item.error(
                f"{position} already has a synchronization point, at {seen[ticks]}", "position"
            )
        seen[ticks] = item.where
        parent = item.ref("parent")
        parent_position = MusicalTime.parse(
            item.require("parent_position", "scalar"), item.where.at("parent_position")
        )
        sync_points.append(SyncPoint(position, ticks, parent, parent_position, item.where))
    return tuple(sync_points)


def load_tempo_map(tree: SourceTree, ref: Ref) -> TempoMap:
    """Loads and parses the `tempo_map` that `ref` names."""
    return parse_tempo_map(tree.load(ref, "tempo_map"))
