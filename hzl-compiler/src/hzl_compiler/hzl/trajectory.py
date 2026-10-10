# SPDX-License-Identifier: Apache-2.0

"""`trajectory`: a sound source's spatial position in musical time.

A trajectory is to a source's position what an envelope is to a partial
set: a start, then stages of change, each lasting a musical duration and
ending at a new point. Each stage gives its values directly, so there is no
state set and no `Update`; a point holds one position, not a set of partials.

A trajectory is its own file so that one path can be used by several
sources (author, 2026-10-05). It makes no partials, so using it twice is
just emitting its points twice. It starts at the start of the source that
uses it and has no offset of its own. A trajectory with no stages is a
position that never changes.

Azimuth and elevation are degrees, distance metres, as in the canonical
`SpatialPositionState` (rendering-engine/docs/transform.md §6.8). Azimuth
is measured anticlockwise from straight ahead, seen from above, so 90 is to
the listener's left; elevation is up from the horizontal plane.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

from ..errors import Where
from ..musical_time import MusicalTime
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import (
    HEADER_KEYS,
    MAX_AZIMUTH_DEG,
    MAX_DISTANCE_M,
    MAX_ELEVATION_DEG,
    MIN_AZIMUTH_DEG,
    MIN_DISTANCE_M,
    MIN_ELEVATION_DEG,
    Header,
    check_in_range,
    parse_header,
)
from .envelope import LINEAR, CurveSpec, parse_curve

# The three properties of a position, in the order the canonical model lists
# them, each with its unit and range.
SPATIAL_PROPERTIES = (
    ("azimuth", "degrees", MIN_AZIMUTH_DEG, MAX_AZIMUTH_DEG),
    ("elevation", "degrees", MIN_ELEVATION_DEG, MAX_ELEVATION_DEG),
    ("distance", "metres", MIN_DISTANCE_M, MAX_DISTANCE_M),
)


@dataclass(frozen=True)
class SpatialPosition:
    """A position, any property of which may be absent.

    Attributes:
        azimuth: Degrees in [-180, 180], anticlockwise from the front, or
            None for no envelope node here.
        elevation: Degrees in [-90, 90], up from the horizontal, or None.
        distance: Metres in [1, 16384], or None.
    """

    azimuth: float | None = None
    elevation: float | None = None
    distance: float | None = None

    def is_complete(self) -> bool:
        """True when all three properties are given."""
        return None not in (self.azimuth, self.elevation, self.distance)

    def is_empty(self) -> bool:
        """True when no property is given."""
        return (self.azimuth, self.elevation, self.distance) == (None, None, None)


# Where a source is when it names no trajectory: straight ahead, level, at
# the reference distance (author, 2026-10-05).
DEFAULT_POSITION = SpatialPosition(0.0, 0.0, 1.0)


@dataclass(frozen=True)
class TrajectoryStage:
    """One stage of a trajectory.

    Attributes:
        duration: How long the stage lasts, in musical time.
        offset_seconds: Physical offset of the point the stage ends at, or
            None for no offset, as on an envelope stage.
        azimuth_curve: Shape of the azimuth path through this stage.
        elevation_curve: Shape of the elevation path through this stage.
        distance_curve: Shape of the distance path through this stage.
        position: The values at the point the stage ends at. A property
            left out does not change here: it continues from its previous
            value to its next one.
        where: Location, for later errors.
    """

    duration: MusicalTime
    offset_seconds: float | None
    azimuth_curve: CurveSpec
    elevation_curve: CurveSpec
    distance_curve: CurveSpec
    position: SpatialPosition
    where: Where


@dataclass(frozen=True)
class Trajectory:
    """A parsed `trajectory` file.

    Attributes:
        header: The file's header.
        initial_position: Where the source is at its start; complete.
        stages: May be empty: a source that does not move.
        where: Location of the body.
    """

    header: Header
    initial_position: SpatialPosition
    stages: tuple[TrajectoryStage, ...]
    where: Where


def parse_position(node: Node, *, complete: bool) -> SpatialPosition:
    """Reads a `{azimuth, elevation, distance}` mapping.

    Args:
        node: The mapping.
        complete: When True, all three properties are required. A first
            position must be complete, since there is nothing before it to
            continue from.

    Raises:
        HzlError: on an unknown key, a value out of range or not a number, a
            missing property when `complete`, or an empty mapping.
    """
    node.only(*(name for name, *_ in SPATIAL_PROPERTIES))
    values: dict[str, float | None] = {}
    for name, unit, low, high in SPATIAL_PROPERTIES:
        value = node.require(name, "float") if complete else node.optional(name, "float")
        values[name] = None if value is None else check_in_range(name, unit, low, high, value, node.where.at(name))
    position = SpatialPosition(**values)
    if position.is_empty():
        raise node.error("a position gives at least one of azimuth, elevation and distance")
    return position


def parse_trajectory_stage(node: Node) -> TrajectoryStage:
    """Reads one `trajectory_stage` mapping.

    The stage-level `curve` and `curve_amount` apply to every property; the
    per-property keys override them, as on an envelope stage.

    Raises:
        HzlError: on a missing or bad duration, a non-finite offset, a bad
            curve, a missing or bad position, or an unknown key.
    """
    curve_keys = [key for name, *_ in SPATIAL_PROPERTIES for key in (f"{name}_curve", f"{name}_curve_amount")]
    node.only("duration", "offset_seconds", "curve", "curve_amount", *curve_keys, "position")
    duration = MusicalTime.parse(node.require("duration", "scalar"), node.where.at("duration"))
    offset = node.optional("offset_seconds", "float")
    if offset is not None and not math.isfinite(offset):
        raise node.error("offset_seconds must be a finite number of seconds", "offset_seconds")
    stage_curve = parse_curve(node, "curve", "curve_amount", LINEAR)
    return TrajectoryStage(
        duration=duration,
        offset_seconds=offset,
        azimuth_curve=parse_curve(node, "azimuth_curve", "azimuth_curve_amount", stage_curve),
        elevation_curve=parse_curve(node, "elevation_curve", "elevation_curve_amount", stage_curve),
        distance_curve=parse_curve(node, "distance_curve", "distance_curve_amount", stage_curve),
        position=parse_position(node.mapping("position"), complete=False),
        where=node.where,
    )


def parse_trajectory(node: Node) -> Trajectory:
    """Reads a `trajectory` body.

    Raises:
        HzlError: if the initial position is missing or incomplete, or a
            stage is malformed.
    """
    node.only(*HEADER_KEYS, "initial_position", "trajectory_stages")
    header = parse_header(node)
    initial = parse_position(node.mapping("initial_position"), complete=True)
    stages = tuple(
        parse_trajectory_stage(item) for item in node.sequence("trajectory_stages", "trajectory_stage", required=False)
    )
    return Trajectory(header, initial, stages, node.where)


def load_trajectory(tree: SourceTree, ref: Ref) -> Trajectory:
    """Loads and parses the `trajectory` that `ref` names."""
    return parse_trajectory(tree.load(ref, "trajectory"))
