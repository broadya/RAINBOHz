"""Lowers a score to the canonical model: level L0.

The score names the tempo map and places sound entities; entities place
sources; sources place envelopes and carry a position. Every placement is exact tick arithmetic
on the one tempo map, and every object becomes one row with its edges.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..canonical.model import (
    EngineConfigRow,
    Model,
    NodeKey,
    PartialRow,
    PartialSetMusicalTimelinePointRow,
    PartialSetMusicalTimelineRow,
    PartialSetRow,
    PartialStateRow,
    PartialStateSetRow,
    ScoreRow,
    SoundEntityRow,
    SoundSourceRow,
    SpatialPositionMusicalTimelinePointRow,
    SpatialPositionMusicalTimelineRow,
    SpatialPositionStateRow,
    TempoMapPointRow,
    TempoMapRow,
)
from ..errors import Where
from ..hzl.common import FREE_RUN, Phase
from ..hzl.envelope import load_envelope
from ..hzl.score import Score, load_score
from ..hzl.sound_entity import load_sound_entity
from ..hzl.sound_source import load_sound_source
from ..hzl.tempo_map import load_tempo_map
from ..hzl.trajectory import load_trajectory
from ..ref import Ref
from ..source import SourceTree
from .state_sets import StateSetResolver
from .timeline import PartialSetTimeline, lower_envelope
from .trajectory import PositionPoint, default_position, lower_trajectory

# EngineConfig.channelMode, numbered as engine-schema.cypher numbers them.
CHANNEL_MODES = {
    0: "mono",
    1: "stereo from M/S",
    2: "stereo from X/Y",
    3: "M/S",
    4: "Blumlein",
    5: "Blumlein rotated 45 degrees",
    6: "FOA A-format",
    7: "FOA B-format",
}


@dataclass(frozen=True)
class EngineOptions:
    """What the engine needs that is not part of the score.

    Attributes:
        sample_rate: Hz, positive.
        channel_mode: One of `CHANNEL_MODES`, 0 to 7.
    """

    sample_rate: int = 48000
    channel_mode: int = 0

    def __post_init__(self) -> None:
        """Checks the two values are ones `engine-db` accepts."""
        if self.sample_rate < 1:
            raise ValueError(f"sample rate must be positive, got {self.sample_rate}")
        if self.channel_mode not in CHANNEL_MODES:
            raise ValueError(f"channel mode must be one of {sorted(CHANNEL_MODES)}, got {self.channel_mode}")


def lower_score(tree: SourceTree, ref: Ref, options: EngineOptions = EngineOptions()) -> Model:
    """Compiles the score `ref` names, and everything it reaches, to a model.

    Args:
        tree: The source tree.
        ref: The score file.
        options: Engine configuration to emit alongside.

    Returns:
        The canonical model, ready to serialise.

    Raises:
        HzlError: for any fault in the files reached, including an envelope
            placed more than once in the score.
    """
    score = load_score(tree, ref)
    tempo_map = load_tempo_map(tree, score.tempo_map)
    resolver = StateSetResolver(tree)
    model = Model()
    model.add(EngineConfigRow(options.sample_rate, options.channel_mode))

    score_key = model.add(ScoreRow(score.header.qualified_name))
    for label in score.header.labels:
        model.link("HAS_LABEL", score_key, model.label(label))

    map_key = model.add(TempoMapRow(tempo_map.header.qualified_name, tempo_map.ticks_per_beat))
    model.link("NEXT_POINT", score_key, map_key)
    previous = map_key
    for point in tempo_map.points:
        point_key = model.add(TempoMapPointRow(point.position_ticks, point.tempo, point.hold))
        model.link("NEXT_POINT", previous, point_key)
        previous = point_key

    tpb = tempo_map.ticks_per_beat
    placed: dict[Ref, Where] = {}
    for entity_placement in score.sound_entities:
        entity = load_sound_entity(tree, entity_placement.ref)
        entity_start = entity_placement.offset.to_ticks(tpb, entity_placement.where.at("position"))
        entity_key = model.add(SoundEntityRow(entity.header.qualified_name))
        model.link("HAS_SOUNDENTITY", score_key, entity_key)
        for source_placement in entity.sound_sources:
            source = load_sound_source(tree, source_placement.ref)
            source_start = entity_start + source_placement.offset.to_ticks(tpb, source_placement.where.at("offset"))
            source_key = model.add(SoundSourceRow(source.header.qualified_name))
            model.link("HAS_SOUNDSOURCE", entity_key, source_key)
            if source.trajectory is None:
                position = default_position()
            else:
                position = lower_trajectory(load_trajectory(tree, source.trajectory), tpb)
            _add_position(model, source_key, map_key, position, source_start)
            for envelope_placement in source.envelopes:
                if envelope_placement.ref in placed:
                    raise envelope_placement.where.error(
                        f"envelope {envelope_placement.ref} is already placed at {placed[envelope_placement.ref]}; "
                        f"an envelope appears once in a score in this version"
                    )
                placed[envelope_placement.ref] = envelope_placement.where
                envelope = load_envelope(tree, envelope_placement.ref)
                start = source_start + envelope_placement.offset.to_ticks(tpb, envelope_placement.where.at("offset"))
                timeline = lower_envelope(envelope, tree, resolver, tpb)
                _add_timeline(model, source_key, map_key, timeline, start)
    return model


def _add_timeline(model: Model, source_key: NodeKey, map_key: NodeKey, timeline: PartialSetTimeline, start_ticks: int) -> None:
    """Adds a partial set, its partials and its musical timeline to `model`.

    Args:
        model: Modified: rows and edges are appended.
        source_key: The `SoundSource` the set hangs from.
        map_key: The `TempoMap` every point's `POINT_AT` leads to.
        timeline: The lowered envelope.
        start_ticks: Position of the envelope's first point on the map.
    """
    set_key = model.add(PartialSetRow(timeline.name))
    model.link("HAS_PARTIALSET", source_key, set_key)
    partial_keys: dict[str, NodeKey] = {}
    for partial in timeline.partials:
        key = model.add(PartialRow(partial.name))
        model.link("HAS_PARTIAL", set_key, key)
        for label in partial.labels:
            model.link("HAS_LABEL", key, model.label(label))
        partial_keys[partial.identifier] = key

    timeline_key = model.add(PartialSetMusicalTimelineRow())
    model.link("HAS_MUSICAL_TIMELINE", set_key, timeline_key)
    previous = timeline_key
    for point in timeline.points:
        point_key = model.add(
            PartialSetMusicalTimelinePointRow(
                frequency_curve_type=point.frequency_curve.type.value,
                frequency_curve_amount=point.frequency_curve.amount,
                amplitude_curve_type=point.amplitude_curve.type.value,
                amplitude_curve_amount=point.amplitude_curve.amount,
            )
        )
        model.link("NEXT_POINT", previous, point_key)
        previous = point_key
        model.link(
            "POINT_AT",
            point_key,
            map_key,
            sync_point=start_ticks + point.offset_ticks,
            offset_seconds=0.0 if point.offset_seconds is None else point.offset_seconds,
        )
        if not point.states:
            continue
        set_key_at_point = model.add(PartialStateSetRow())
        model.link("POINT_STATE", point_key, set_key_at_point)
        for identifier, values in point.states.items():
            phase, free_run = _phase_columns(values.phase)
            state_key = model.add(PartialStateRow(values.frequency, values.amplitude, phase, free_run))
            model.link("MEMBER_OF_SET_MANY_ONE", state_key, set_key_at_point)
            model.link("STATE_OF", state_key, partial_keys[identifier])


def _add_position(model: Model, source_key: NodeKey, map_key: NodeKey, points: tuple[PositionPoint, ...], start_ticks: int) -> None:
    """Adds a source's spatial position timeline to `model`.

    Args:
        model: Modified: rows and edges are appended.
        source_key: The `SoundSource` the timeline belongs to.
        map_key: The `TempoMap` every point's `POINT_AT` leads to.
        points: The lowered trajectory, or the default position.
        start_ticks: Position of the source's start on the map, where the
            first point goes.
    """
    timeline_key = model.add(SpatialPositionMusicalTimelineRow())
    model.link("HAS_MUSICAL_TIMELINE", source_key, timeline_key)
    previous = timeline_key
    for point in points:
        point_key = model.add(
            SpatialPositionMusicalTimelinePointRow(
                azimuth_curve_type=point.azimuth_curve.type.value,
                azimuth_curve_amount=point.azimuth_curve.amount,
                elevation_curve_type=point.elevation_curve.type.value,
                elevation_curve_amount=point.elevation_curve.amount,
                distance_curve_type=point.distance_curve.type.value,
                distance_curve_amount=point.distance_curve.amount,
            )
        )
        model.link("NEXT_POINT", previous, point_key)
        previous = point_key
        model.link(
            "POINT_AT",
            point_key,
            map_key,
            sync_point=start_ticks + point.offset_ticks,
            offset_seconds=0.0 if point.offset_seconds is None else point.offset_seconds,
        )
        state = point.position
        state_key = model.add(SpatialPositionStateRow(state.azimuth, state.elevation, state.distance))
        model.link("POINT_STATE", point_key, state_key)


def _phase_columns(phase: Phase) -> tuple[float | None, bool | None]:
    """Spells an HzL phase as `PartialState.phase` and `phase_free_run`.

    A target is (degrees, False), free-running is (None, True) and the
    default is (None, None): the three combinations the schema allows.
    """
    if phase is None:
        return None, None
    if phase is FREE_RUN:
        return None, True
    return phase, False
