"""`envelope`: a partial set's history in musical time, as stages of change.

An envelope starts from a state set, which gives the partials and their first
state, and then runs through stages. Each stage lasts a musical duration and
applies transformations to the partials its filters select; the result is a
new point on the set's timeline. This is the PoC's envelope with seconds
replaced by `beats:ticks` and an optional physical offset per stage.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass
from enum import Enum

from ..errors import Where
from ..musical_time import MusicalTime
from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header


class Curve(Enum):
    """Curve types, numbered as `engine-db` stores them."""

    LINEAR = 0
    STEP = 1
    ACCELERATE = 2
    DECELERATE = 3
    S_CURVE = 4


# How each curve is spelt in HzL.
CURVE_NAMES = {
    "linear": Curve.LINEAR,
    "step": Curve.STEP,
    "accelerate": Curve.ACCELERATE,
    "decelerate": Curve.DECELERATE,
    "s_curve": Curve.S_CURVE,
}


@dataclass(frozen=True)
class CurveSpec:
    """A curve type with its amount.

    Attributes:
        type: The curve.
        amount: In [0, 1]: 0 is fully linear, 1 fully the curve. No effect
            when the type is linear.
    """

    type: Curve = Curve.LINEAR
    amount: float = 0.0


LINEAR = CurveSpec()


@dataclass(frozen=True)
class EnvelopeTransform:
    """One transformation applied in a stage.

    Attributes:
        filter: Regular expression selecting partials by label: a partial is
            selected when the expression matches any of its labels. None
            selects every partial.
        transformation: Reference to the transformation; must be `Update` in
            this version.
        source_partial_state_set: Reference to the state set whose states
            are applied to the selected partials, matched by identifier.
        where: Location, for later errors.
    """

    filter: re.Pattern[str] | None
    transformation: Ref
    source_partial_state_set: Ref
    where: Where


@dataclass(frozen=True)
class EnvelopeStage:
    """One stage of an envelope.

    Attributes:
        duration: How long the stage lasts, in musical time.
        offset_seconds: Physical offset of the point the stage ends at, or
            None for no offset. Lets a percussive attack keep its length at
            any tempo.
        frequency_curve: Shape of the frequency path through this stage.
        amplitude_curve: Shape of the amplitude path through this stage.
        transforms: Applied in order; a later one overrides an earlier one
            where both set the same property of the same partial.
        where: Location, for later errors.
    """

    duration: MusicalTime
    offset_seconds: float | None
    frequency_curve: CurveSpec
    amplitude_curve: CurveSpec
    transforms: tuple[EnvelopeTransform, ...]
    where: Where


@dataclass(frozen=True)
class Envelope:
    """A parsed `envelope` file.

    Attributes:
        header: The file's header.
        initial_partial_state_set: Reference to the set the envelope starts from.
        stages: At least one.
        where: Location of the body, for later errors.
    """

    header: Header
    initial_partial_state_set: Ref
    stages: tuple[EnvelopeStage, ...]
    where: Where


def parse_curve(node: Node, type_key: str, amount_key: str, default: CurveSpec) -> CurveSpec:
    """Reads a curve type and amount from two optional keys.

    Args:
        node: The mapping holding them.
        type_key: Key of the curve name.
        amount_key: Key of the amount.
        default: Used for whichever of the two is absent.

    Raises:
        HzlError: on an unknown curve name or an amount outside [0, 1].
    """
    kind = default.type
    name = node.optional(type_key, "str")
    if name is not None:
        if name not in CURVE_NAMES:
            raise node.error(f"unknown curve {name!r}; the curves are: {', '.join(CURVE_NAMES)}", type_key)
        kind = CURVE_NAMES[name]
    amount = node.optional(amount_key, "float", default.amount)
    if not 0.0 <= amount <= 1.0:
        raise node.error(f"{amount_key} must lie in [0, 1], got {amount!r}", amount_key)
    return CurveSpec(kind, amount)


def parse_envelope_transform(node: Node) -> EnvelopeTransform:
    """Reads one `envelope_transform` mapping.

    Raises:
        HzlError: if the filter is not a valid regular expression, a reference
            is missing or malformed, or a key is unknown.
    """
    node.only("filter", "transformation", "source_partial_state_set")
    pattern = None
    text = node.optional("filter", "str")
    if text is not None:
        try:
            pattern = re.compile(text)
        except re.error as error:
            raise node.error(f"filter is not a valid regular expression: {error}", "filter") from error
    return EnvelopeTransform(
        filter=pattern,
        transformation=node.ref("transformation"),
        source_partial_state_set=node.ref("source_partial_state_set"),
        where=node.where,
    )


def parse_envelope_stage(node: Node) -> EnvelopeStage:
    """Reads one `envelope_stage` mapping.

    The stage-level `curve` and `curve_amount` apply to every property; the
    per-property keys override them.

    Raises:
        HzlError: on a missing or bad duration, a non-finite offset, a bad
            curve, or an unknown key.
    """
    node.only(
        "duration",
        "offset_seconds",
        "curve",
        "curve_amount",
        "frequency_curve",
        "frequency_curve_amount",
        "amplitude_curve",
        "amplitude_curve_amount",
        "envelope_transforms",
    )
    duration = MusicalTime.parse(node.require("duration", "scalar"), node.where.at("duration"))
    offset = node.optional("offset_seconds", "float")
    if offset is not None and not math.isfinite(offset):
        raise node.error("offset_seconds must be a finite number of seconds", "offset_seconds")
    stage_curve = parse_curve(node, "curve", "curve_amount", LINEAR)
    return EnvelopeStage(
        duration=duration,
        offset_seconds=offset,
        frequency_curve=parse_curve(node, "frequency_curve", "frequency_curve_amount", stage_curve),
        amplitude_curve=parse_curve(node, "amplitude_curve", "amplitude_curve_amount", stage_curve),
        transforms=tuple(
            parse_envelope_transform(item)
            for item in node.sequence("envelope_transforms", "envelope_transform", required=False)
        ),
        where=node.where,
    )


def parse_envelope(node: Node) -> Envelope:
    """Reads an `envelope` body.

    Raises:
        HzlError: if the initial set reference is missing, there are no
            stages, or any stage is malformed.
    """
    node.only(*HEADER_KEYS, "initial_partial_state_set", "envelope_stages")
    header = parse_header(node)
    stages = tuple(parse_envelope_stage(item) for item in node.sequence("envelope_stages", "envelope_stage"))
    if not stages:
        raise node.error("an envelope needs at least one stage: a partial needs an end as well as a start", "envelope_stages")
    return Envelope(header, node.ref("initial_partial_state_set"), stages, node.where)


def load_envelope(tree: SourceTree, ref: Ref) -> Envelope:
    """Loads and parses the `envelope` that `ref` names."""
    return parse_envelope(tree.load(ref, "envelope"))
