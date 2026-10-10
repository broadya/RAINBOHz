# SPDX-License-Identifier: Apache-2.0

"""`tempo_map`: tempo against musical position, the clock every placement reads.

The validity rules are the transform's (rendering-engine/docs/transform.md
§6.1, ADR 0013), checked here so that they are reported in HzL terms.
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
        hold: True holds `tempo` until the next point; False ramps linearly in
            musical position to the next point's tempo.
    """

    position: MusicalTime
    position_ticks: int
    tempo: float
    hold: bool


@dataclass(frozen=True)
class TempoMap:
    """A parsed and validated `tempo_map` file.

    Attributes:
        header: The file's header.
        ticks_per_beat: Resolution of every musical time on this map.
        points: At least one, the first at position 0, strictly increasing,
            the last holding.
        where: Location of the body.
    """

    header: Header
    ticks_per_beat: int
    points: tuple[TempoMapPoint, ...]
    where: Where


def parse_tempo_map(node: Node) -> TempoMap:
    """Reads and validates a `tempo_map` body.

    Raises:
        HzlError: if `ticks_per_beat` is not positive; there are no points;
            the first is not at 0; positions do not strictly increase; a tempo
            is outside [1, 1000] or not finite; or the last point does not hold.
    """
    node.only(*HEADER_KEYS, "ticks_per_beat", "points")
    header = parse_header(node)
    ticks_per_beat = node.require("ticks_per_beat", "int")
    if ticks_per_beat < 1:
        raise node.error(f"ticks_per_beat must be 1 or more, got {ticks_per_beat}", "ticks_per_beat")
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
            raise item.error(f"tempo is BPM in [{MIN_TEMPO_BPM:g}, {MAX_TEMPO_BPM:g}], got {tempo!r}", "tempo")
        hold = item.require("hold", "bool")
        if index == 0 and ticks != 0:
            raise item.error(f"the first point must be at position 0:0, time zero of the score; got {position}", "position")
        if points and ticks <= points[-1].position_ticks:
            raise item.error(
                f"positions must strictly increase; {position} is not after {points[-1].position}", "position"
            )
        points.append(TempoMapPoint(position, ticks, tempo, hold))
    if not points[-1].hold:
        raise items[-1].error("the last point must hold: there is no next tempo to ramp to", "hold")
    return TempoMap(header, ticks_per_beat, tuple(points), node.where)


def load_tempo_map(tree: SourceTree, ref: Ref) -> TempoMap:
    """Loads and parses the `tempo_map` that `ref` names."""
    return parse_tempo_map(tree.load(ref, "tempo_map"))
