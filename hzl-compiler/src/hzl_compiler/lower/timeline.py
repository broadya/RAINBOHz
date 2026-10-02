"""Lowers an envelope to a partial set's timeline: level L1.

An envelope is a partial set and its musical timeline in one. The initial
state set gives the partials and the first point; each stage gives the next
point, carrying a state for every partial a transformation touched. Points
are placed relative to the envelope's start here; the score places them.

Two rules from the transform (rendering-engine/docs/transform.md §6.2) are
checked here, so that they are reported against the envelope: every partial
has both frequency and amplitude at the first point and at the last, since
those two knots bound its lifeline.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import Where
from ..hzl.envelope import LINEAR, CurveSpec, Envelope
from ..hzl.transformation import TransformationType, load_transformation
from ..source import SourceTree
from .state_sets import StateSetResolver


@dataclass(frozen=True)
class PartialSpec:
    """A partial of the set, as the canonical model will name it.

    Attributes:
        identifier: Unique in the set; what Update matches on.
        name: `<initial set namespace>#<identifier>`, the `Partial.name`.
        labels: The partial's labels, from its initial state.
    """

    identifier: str
    name: str
    labels: tuple[str, ...]


@dataclass(frozen=True)
class StateValues:
    """A partial's values at one point; None means no knot for that property."""

    frequency: float | None
    amplitude: float | None


@dataclass(frozen=True)
class TimelinePoint:
    """One point of the set's timeline, relative to the envelope's start.

    Attributes:
        offset_ticks: Ticks after the envelope starts.
        offset_seconds: Physical offset, or None.
        frequency_curve: Path of frequency from this point to the next.
        amplitude_curve: Path of amplitude from this point to the next.
        states: The partials with a state here, by identifier, in set order.
    """

    offset_ticks: int
    offset_seconds: float | None
    frequency_curve: CurveSpec
    amplitude_curve: CurveSpec
    states: dict[str, StateValues]


@dataclass(frozen=True)
class PartialSetTimeline:
    """A lowered envelope.

    Attributes:
        name: The envelope's qualified name, the `PartialSet.name`.
        labels: The envelope's labels.
        partials: In initial-set order.
        points: At least two.
    """

    name: str
    labels: tuple[str, ...]
    partials: tuple[PartialSpec, ...]
    points: tuple[TimelinePoint, ...]


def lower_envelope(envelope: Envelope, tree: SourceTree, resolver: StateSetResolver, ticks_per_beat: int) -> PartialSetTimeline:
    """Lowers `envelope` on a map with `ticks_per_beat`.

    Each stage's `Update` transformations are applied in order: a partial the
    filter selects, whose identifier the source set has a state for, takes
    that state's present values at the stage's point. A later transformation
    overrides an earlier one property by property. Partials the stage leaves
    alone have no state at its point, and so continue to their next knot.

    A stage's curve describes the path through the stage, which the model
    stores on the point the stage starts from; the last point's curve fields
    are linear and meaningless.

    Args:
        envelope: The parsed envelope.
        tree: Source files, for the transformations.
        resolver: Lowers the state sets referenced.
        ticks_per_beat: The score's tempo map resolution.

    Raises:
        HzlError: if the initial set is empty or a partial in it lacks
            frequency or amplitude; a stage's transformation is not `Update`;
            a duration is malformed for the map; or a partial lacks frequency
            or amplitude at the last point.
    """
    initial = resolver.resolve(envelope.initial_partial_state_set, envelope.where.at("initial_partial_state_set"))
    if not initial.states:
        raise envelope.where.at("initial_partial_state_set").error(
            f"{envelope.initial_partial_state_set} has no partials; an envelope needs at least one"
        )
    partials = []
    first: dict[str, StateValues] = {}
    for spec in initial.states:
        missing = [p for p, v in (("frequency", spec.frequency), ("amplitude", spec.amplitude)) if v is None]
        if missing:
            raise envelope.where.at("initial_partial_state_set").error(
                f"partial {spec.identifier!r} of {envelope.initial_partial_state_set} has no {' or '.join(missing)}; "
                f"every partial needs both at the start of an envelope"
            )
        partials.append(PartialSpec(spec.identifier, f"{initial.header.namespace}#{spec.identifier}", spec.labels))
        first[spec.identifier] = StateValues(spec.frequency, spec.amplitude)

    # The curve of stage k is stored on point k, where the stage starts.
    curves = [(stage.frequency_curve, stage.amplitude_curve) for stage in envelope.stages] + [(LINEAR, LINEAR)]
    points = [TimelinePoint(0, None, curves[0][0], curves[0][1], first)]
    elapsed = 0
    for index, stage in enumerate(envelope.stages):
        elapsed += stage.duration.to_ticks(ticks_per_beat, stage.where.at("duration"))
        states: dict[str, StateValues] = {}
        for transform in stage.transforms:
            transformation = load_transformation(tree, transform.transformation)
            if transformation.transformation_type is not TransformationType.UPDATE:
                raise transform.where.at("transformation").error(
                    f"{transform.transformation} is {transformation.transformation_type.value!r}; in an envelope "
                    f"stage only '{TransformationType.UPDATE.value}' is in this version"
                )
            source = resolver.resolve(transform.source_partial_state_set, transform.where.at("source_partial_state_set"))
            by_identifier = source.by_identifier()
            for partial in partials:
                if transform.filter is not None and not any(transform.filter.search(label) for label in partial.labels):
                    continue
                spec = by_identifier.get(partial.identifier)
                if spec is None:
                    continue
                current = states.get(partial.identifier, StateValues(None, None))
                states[partial.identifier] = StateValues(
                    spec.frequency if spec.frequency is not None else current.frequency,
                    spec.amplitude if spec.amplitude is not None else current.amplitude,
                )
        ordered = {p.identifier: states[p.identifier] for p in partials if p.identifier in states}
        points.append(TimelinePoint(elapsed, stage.offset_seconds, curves[index + 1][0], curves[index + 1][1], ordered))

    last = points[-1].states
    for partial in partials:
        values = last.get(partial.identifier)
        missing = [p for p in ("frequency", "amplitude") if values is None or getattr(values, p) is None]
        if missing:
            raise envelope.stages[-1].where.error(
                f"partial {partial.identifier!r} has no {' or '.join(missing)} at the last stage of {envelope.header.qualified_name}; "
                f"every partial needs both at the end of an envelope, since the last point ends its lifeline"
            )
    return PartialSetTimeline(envelope.header.qualified_name, envelope.header.labels, tuple(partials), tuple(points))
