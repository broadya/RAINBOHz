# SPDX-License-Identifier: Apache-2.0

"""`harmonic_series`: a generator giving one partial per harmonic, from expressions.

Each property is either a constant, the same for every harmonic, or an
expression in `harmonic_index` (1-based) evaluated per harmonic. Expressions
are evaluated by SymPy, exactly where it can, so that `1/3` is a third and not
0.333... until the final conversion to a float.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import (
    HEADER_KEYS,
    NOT_IN_THIS_VERSION,
    Header,
    Phase,
    check_amplitude,
    check_frequency,
    optional_phase,
    parse_header,
    reject_not_in_this_version,
)

# The properties a series can give, each as `<name>` (constant) or
# `<name>_expression`.
PROPERTIES = ("frequency", "amplitude", "phase")


@dataclass(frozen=True)
class HarmonicSeries:
    """A parsed `harmonic_series` file.

    At most one of `frequency`/`frequency_expression` may be given, and the
    same for amplitude and phase; any property may be absent altogether.

    Attributes:
        header: The file's header.
        base_frequency: Hz; multiplies the frequency expression's value.
            Defaults to 1, so an expression of `harmonic_index` gives the
            harmonic numbers themselves, to be scaled by a later product.
        number_of_harmonics: How many partials, 1 or more.
        frequency: Constant Hz for every harmonic, or None.
        frequency_expression: Expression in `harmonic_index`, or None.
        amplitude: Constant dB for every harmonic, or None.
        amplitude_expression: Expression in `harmonic_index` giving dB, or None.
        phase: Constant target in degrees, `FREE_RUN`, or None for the default.
        phase_expression: Expression in `harmonic_index` giving a target in
            degrees, or None.
    """

    header: Header
    base_frequency: float
    number_of_harmonics: int
    frequency: float | None
    frequency_expression: str | None
    amplitude: float | None
    amplitude_expression: str | None
    phase: Phase = None
    phase_expression: str | None = None


def parse_harmonic_series(node: Node) -> HarmonicSeries:
    """Reads a `harmonic_series` body.

    Raises:
        HzlError: if `number_of_harmonics` is missing or below 1, a property
            is given both as a constant and as an expression, a constant is out
            of range, or a key is unknown or not in this version.
    """
    reject_not_in_this_version(node)
    for key in ("azimuth_expression", "elevation_expression", "distance_expression"):
        node.reject(key, NOT_IN_THIS_VERSION[key.split("_")[0]])
    node.only(
        *HEADER_KEYS,
        "base_frequency",
        "number_of_harmonics",
        *PROPERTIES,
        *(f"{p}_expression" for p in PROPERTIES),
    )
    header = parse_header(node)
    count = node.require("number_of_harmonics", "int")
    if count < 1:
        raise node.error(f"number_of_harmonics must be 1 or more, got {count}", "number_of_harmonics")
    base = node.optional("base_frequency", "float", 1.0)
    check_frequency(base, node.where.at("base_frequency"))
    for prop in PROPERTIES:
        if node.has(prop) and node.has(f"{prop}_expression"):
            raise node.error(f"give either {prop!r} or '{prop}_expression', not both", prop)
    frequency = node.optional("frequency", "float")
    amplitude = node.optional("amplitude", "float")
    return HarmonicSeries(
        header=header,
        base_frequency=base,
        number_of_harmonics=count,
        frequency=None if frequency is None else check_frequency(frequency, node.where.at("frequency")),
        frequency_expression=node.optional("frequency_expression", "str"),
        amplitude=None if amplitude is None else check_amplitude(amplitude, node.where.at("amplitude")),
        amplitude_expression=node.optional("amplitude_expression", "str"),
        phase=optional_phase(node),
        phase_expression=node.optional("phase_expression", "str"),
    )


def load_harmonic_series(tree: SourceTree, ref: Ref) -> HarmonicSeries:
    """Loads and parses the `harmonic_series` that `ref` names."""
    return parse_harmonic_series(tree.load(ref, "harmonic_series"))
