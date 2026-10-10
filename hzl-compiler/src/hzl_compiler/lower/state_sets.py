# SPDX-License-Identifier: Apache-2.0

"""Lowers anything state-set-valued to a concrete `PartialStateSet`.

Levels L3 and L2 of the language live here: generators (`scale`,
`harmonic_series`) make a set from a rule, and the combinator
(`virtual_partial_state_set`) makes one from other sets. A written
`partial_state_set` is already at the bottom. The resolver dispatches on the
file's type, caches results, and refuses a cycle of references.
"""

from __future__ import annotations

from dataclasses import replace

import sympy

from ..errors import Where
from ..hzl.common import (
    FREE_RUN,
    MIN_AMPLITUDE_DB,
    Phase,
    check_amplitude,
    check_frequency,
    check_phase,
)
from ..hzl.harmonic_series import HarmonicSeries, parse_harmonic_series
from ..hzl.partial_state_set import PartialStateSet, PartialStateSpec, parse_partial_state_set
from ..hzl.scale import Scale, parse_scale
from ..hzl.transformation import Transformation, TransformationType, load_transformation
from ..hzl.virtual_partial_state_set import VirtualPartialStateSet, parse_virtual_partial_state_set
from ..ref import Ref
from ..source import SourceTree

# The HzL types a state-set reference may lead to.
STATE_SET_TYPES = ("partial_state_set", "scale", "harmonic_series", "virtual_partial_state_set")

# What an expression may use besides `harmonic_index`. Kept small and
# explicit: an expression is arithmetic over the harmonic number, not a
# program.
_HARMONIC_INDEX = sympy.Symbol("harmonic_index", positive=True, integer=True)
_EXPRESSION_NAMES = {
    "harmonic_index": _HARMONIC_INDEX,
    "pi": sympy.pi,
    "e": sympy.E,
    "log": sympy.log,
    "log10": lambda x: sympy.log(x, 10),
    "log2": lambda x: sympy.log(x, 2),
    "exp": sympy.exp,
    "sqrt": sympy.sqrt,
    "sin": sympy.sin,
    "cos": sympy.cos,
    "abs": sympy.Abs,
    "floor": sympy.floor,
    "ceiling": sympy.ceiling,
    "Max": sympy.Max,
    "Min": sympy.Min,
}


def evaluate_expression(text: str, harmonic_index: int, where: Where) -> float:
    """Evaluates an expression in `harmonic_index` for one harmonic.

    Args:
        text: The expression, in SymPy's infix syntax; may use the names in
            `_EXPRESSION_NAMES`.
        harmonic_index: The harmonic number, from 1.
        where: Location of the expression, for errors.

    Returns:
        The value as a float.

    Raises:
        HzlError: if the expression does not parse, uses an unknown name, or
            does not evaluate to a real number.
    """
    try:
        expression = sympy.sympify(text, locals=_EXPRESSION_NAMES)
    except (sympy.SympifyError, SyntaxError, TypeError) as error:
        raise where.error(f"expression {text!r} does not parse: {error}") from error
    unknown = {str(s) for s in getattr(expression, "free_symbols", set())} - {"harmonic_index"}
    if unknown:
        raise where.error(f"expression {text!r} uses unknown names: {', '.join(sorted(unknown))}")
    try:
        value = (
            sympy.N(expression.subs(_HARMONIC_INDEX, harmonic_index))
            if hasattr(expression, "subs")
            else expression
        )
        result = float(value)
    except (TypeError, ValueError) as error:
        raise where.error(
            f"expression {text!r} does not give a real number at harmonic_index={harmonic_index}"
        ) from error
    return result


def lower_scale(scale: Scale) -> PartialStateSet:
    """Makes one partial per note. Identifier is the note's index; label its name."""
    states = tuple(
        PartialStateSpec(
            identifier=str(index),
            labels=(note.note,),
            frequency=note.frequency,
            amplitude=note.amplitude,
            phase=note.phase,
        )
        for index, note in enumerate(scale.notes)
    )
    return PartialStateSet(scale.header, states)


def lower_harmonic_series(series: HarmonicSeries, where: Where) -> PartialStateSet:
    """Makes one partial per harmonic, identifier and label by harmonic number from 1.

    Frequency from an expression is multiplied by `base_frequency`; a
    constant frequency is used as written. Values are range-checked as any
    written value would be.

    Raises:
        HzlError: if an expression fails to evaluate or a value is out of range.
    """
    states = []
    for h in range(1, series.number_of_harmonics + 1):
        frequency = series.frequency
        if series.frequency_expression is not None:
            value = evaluate_expression(
                series.frequency_expression, h, where.at("frequency_expression")
            )
            frequency = check_frequency(
                series.base_frequency * value, where.at("frequency_expression")
            )
        amplitude = series.amplitude
        if series.amplitude_expression is not None:
            value = evaluate_expression(
                series.amplitude_expression, h, where.at("amplitude_expression")
            )
            amplitude = check_amplitude(value, where.at("amplitude_expression"))
        phase = series.phase
        if series.phase_expression is not None:
            value = evaluate_expression(series.phase_expression, h, where.at("phase_expression"))
            phase = check_phase(value, where.at("phase_expression"))
        states.append(
            PartialStateSpec(
                identifier=str(h),
                labels=(f"harmonic {h}",),
                frequency=frequency,
                amplitude=amplitude,
                phase=phase,
            )
        )
    return PartialStateSet(series.header, tuple(states))


def combine_phase(a: Phase, b: Phase) -> Phase:
    """Combines two partials' phases in a flattened cartesian product.

    The author's rule (2026-10-03): two targets give their arithmetic mean,
    which is not wrapped and so stays in [-180, 180]; a target with anything
    else gives the target; free-running with free-running or the default
    gives free-running; two defaults give the default.
    """
    if isinstance(a, float) and isinstance(b, float):
        return (a + b) / 2.0
    if isinstance(a, float):
        return a
    if isinstance(b, float):
        return b
    if a is FREE_RUN or b is FREE_RUN:
        return FREE_RUN
    return None


def flattened_cartesian_product(
    virtual: VirtualPartialStateSet,
    a: PartialStateSet,
    b: PartialStateSet,
    transformation: Transformation,
) -> PartialStateSet:
    """Combines every partial of `a` with every partial of `b`.

    The PoC's rules, in dB: frequencies multiply, amplitudes add (the dB form
    of a linear product) and are floored at -144 dB, since quieter than
    silence is silence; a property only one side has is taken from that side.
    Phase follows `combine_phase`.
    Labels are the union of the two partials', `a`'s first. The identifier is
    `<a>*<b>`. The set's labels are the union of the virtual set's, the
    transformation's, `a`'s and `b`'s.
    """
    states = []
    for sa in a.states:
        for sb in b.states:
            if sa.frequency is not None and sb.frequency is not None:
                frequency: float | None = sa.frequency * sb.frequency
            else:
                frequency = sa.frequency if sa.frequency is not None else sb.frequency
            if sa.amplitude is not None and sb.amplitude is not None:
                amplitude: float | None = max(MIN_AMPLITUDE_DB, sa.amplitude + sb.amplitude)
            else:
                amplitude = sa.amplitude if sa.amplitude is not None else sb.amplitude
            labels = sa.labels + tuple(label for label in sb.labels if label not in sa.labels)
            states.append(
                PartialStateSpec(
                    identifier=f"{sa.identifier}*{sb.identifier}",
                    labels=labels,
                    frequency=frequency,
                    amplitude=amplitude,
                    phase=combine_phase(sa.phase, sb.phase),
                )
            )
    labels: list[str] = []
    for source in (
        virtual.header.labels,
        transformation.header.labels,
        a.header.labels,
        b.header.labels,
    ):
        for label in source:
            if label not in labels:
                labels.append(label)
    return PartialStateSet(replace(virtual.header, labels=tuple(labels)), tuple(states))


class StateSetResolver:
    """Resolves a reference to a concrete state set, whatever file type it names.

    Results are cached per reference, so a set used by several envelopes is
    lowered once. A reference chain that returns to a set it is already
    resolving is a cycle and an error.
    """

    def __init__(self, tree: SourceTree) -> None:
        """Creates a resolver over `tree`."""
        self._tree = tree
        self._cache: dict[Ref, PartialStateSet] = {}
        self._stack: list[Ref] = []

    def resolve(self, ref: Ref, where: Where) -> PartialStateSet:
        """Lowers the state set `ref` names.

        Args:
            ref: Any state-set-valued file.
            where: Where the reference was written, for errors.

        Returns:
            The concrete set.

        Raises:
            HzlError: if the file is missing or not state-set-valued, the
                chain of references is cyclic, or the file fails to lower.
        """
        if ref in self._cache:
            return self._cache[ref]
        if ref in self._stack:
            chain = " -> ".join(str(r) for r in self._stack[self._stack.index(ref) :] + [ref])
            raise where.error(f"state sets refer to each other in a cycle: {chain}")
        hzl_type, node = self._tree.load_any(ref)
        if hzl_type not in STATE_SET_TYPES:
            raise where.error(
                f"{ref} is a {hzl_type}; a state set is needed here "
                f"(one of: {', '.join(STATE_SET_TYPES)})"
            )
        self._stack.append(ref)
        try:
            if hzl_type == "partial_state_set":
                result = parse_partial_state_set(node)
            elif hzl_type == "scale":
                result = lower_scale(parse_scale(node))
            elif hzl_type == "harmonic_series":
                result = lower_harmonic_series(parse_harmonic_series(node), node.where)
            else:
                result = self._virtual(parse_virtual_partial_state_set(node), node.where)
        finally:
            self._stack.pop()
        self._cache[ref] = result
        return result

    def _virtual(self, virtual: VirtualPartialStateSet, where: Where) -> PartialStateSet:
        """Lowers a virtual set by resolving its sources and applying its transformation.

        Raises:
            HzlError: if the transformation is not a product, or there are not
                exactly two sources.
        """
        transformation = load_transformation(self._tree, virtual.transformation)
        if transformation.transformation_type is not TransformationType.FLATTENED_CARTESIAN_PRODUCT:
            raise where.at("transformation").error(
                f"{virtual.transformation} is {transformation.transformation_type.value!r}; "
                "a virtual partial state set applies "
                f"'{TransformationType.FLATTENED_CARTESIAN_PRODUCT.value}'"
            )
        if len(virtual.sources) != 2:
            raise where.at("partial_state_sets").error(
                "a flattened cartesian product takes exactly two state sets, "
                f"got {len(virtual.sources)}"
            )
        a = self.resolve(virtual.sources[0], where.at("partial_state_sets").at(0))
        b = self.resolve(virtual.sources[1], where.at("partial_state_sets").at(1))
        return flattened_cartesian_product(virtual, a, b, transformation)
