# SPDX-License-Identifier: Apache-2.0

"""Lowers a trajectory to a source's spatial position timeline: level L1.

The initial position gives the first point, complete; each stage gives the
next point, carrying only the properties it states. Points are placed
relative to the source's start here; the score places them.

The transform's rules (rendering-engine/docs/transform.md §6.8) are met by
construction: the first point sets all three properties, since the parser
requires it; later absent properties are "no envelope node here"; and the
timeline starts with its source, so the position is defined before any
partial under the source starts, since envelope offsets are not negative.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..hzl.envelope import LINEAR, CurveSpec
from ..hzl.trajectory import DEFAULT_POSITION, SpatialPosition, Trajectory


@dataclass(frozen=True)
class PositionPoint:
    """One point of a source's position timeline, relative to its start.

    Attributes:
        offset_ticks: Ticks after the source starts.
        offset_seconds: Physical offset, or None.
        azimuth_curve: Path of azimuth from this point to the next.
        elevation_curve: Path of elevation from this point to the next.
        distance_curve: Path of distance from this point to the next.
        position: The values stated here; absent ones continue.
    """

    offset_ticks: int
    offset_seconds: float | None
    azimuth_curve: CurveSpec
    elevation_curve: CurveSpec
    distance_curve: CurveSpec
    position: SpatialPosition


def lower_trajectory(trajectory: Trajectory, ticks_per_beat: int) -> tuple[PositionPoint, ...]:
    """Lowers `trajectory` on a map with `ticks_per_beat`.

    A stage's curves describe the path through the stage, which the model
    stores on the point the stage starts from (design.md §5.3); the last
    point's curve fields are linear and mean nothing, since the value holds
    after it.

    Args:
        trajectory: The parsed trajectory.
        ticks_per_beat: The score's tempo map resolution.

    Returns:
        The initial point, then one per stage, at the cumulative durations.

    Raises:
        HzlError: if a duration is malformed for the map.
    """
    curves = [(s.azimuth_curve, s.elevation_curve, s.distance_curve) for s in trajectory.stages] + [(LINEAR,) * 3]
    points = [PositionPoint(0, None, *curves[0], trajectory.initial_position)]
    elapsed = 0
    for index, stage in enumerate(trajectory.stages):
        elapsed += stage.duration.to_ticks(ticks_per_beat, stage.where.at("duration"))
        points.append(PositionPoint(elapsed, stage.offset_seconds, *curves[index + 1], stage.position))
    return tuple(points)


def default_position() -> tuple[PositionPoint, ...]:
    """The timeline of a source that names no trajectory: one point, at its start.

    The transform requires a position for every source with partials, and
    supplying one is the compiler's job (transform.md §6.8).
    """
    return (PositionPoint(0, None, LINEAR, LINEAR, LINEAR, DEFAULT_POSITION),)
