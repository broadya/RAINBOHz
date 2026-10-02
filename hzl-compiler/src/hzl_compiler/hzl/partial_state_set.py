"""`partial_state_set`: the lowest HzL level for states, a list of partials with values.

This is also what every generator and combinator lowers to, so the dataclass
here serves as both a parsed file and a lowering result.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import (
    HEADER_KEYS,
    Header,
    check_amplitude,
    check_frequency,
    parse_header,
    parse_identifier,
    reject_not_in_this_version,
)


@dataclass(frozen=True)
class PartialStateSpec:
    """One partial's state as a state set gives it.

    Attributes:
        identifier: Unique within the set; names the partial together with the
            set's namespace.
        labels: The partial's own labels.
        frequency: Hz, or None when the set says nothing about frequency.
        amplitude: dB in [-144, 0], or None when the set says nothing about it.
    """

    identifier: str
    labels: tuple[str, ...]
    frequency: float | None
    amplitude: float | None


@dataclass(frozen=True)
class PartialStateSet:
    """A set of partial states, written or lowered.

    Attributes:
        header: The file's header, or for a lowered set the generating file's
            header with the labels the lowering produced.
        states: In file order; identifiers are unique.
    """

    header: Header
    states: tuple[PartialStateSpec, ...]

    def by_identifier(self) -> dict[str, PartialStateSpec]:
        """The states keyed by identifier."""
        return {state.identifier: state for state in self.states}


def parse_partial_state(node: Node) -> PartialStateSpec:
    """Reads one `partial_state` mapping.

    Raises:
        HzlError: on a missing or bad identifier, a value out of range, an
            unknown key, or a property this version does not carry.
    """
    reject_not_in_this_version(node)
    node.only("identifier", "labels", "frequency", "amplitude")
    frequency = node.optional("frequency", "float")
    amplitude = node.optional("amplitude", "float")
    return PartialStateSpec(
        identifier=parse_identifier(node.require("identifier", "scalar"), node.where.at("identifier")),
        labels=node.labels(),
        frequency=None if frequency is None else check_frequency(frequency, node.where.at("frequency")),
        amplitude=None if amplitude is None else check_amplitude(amplitude, node.where.at("amplitude")),
    )


def parse_partial_state_set(node: Node) -> PartialStateSet:
    """Reads a `partial_state_set` body.

    Raises:
        HzlError: as `parse_partial_state`, or if two states share an identifier.
    """
    node.only(*HEADER_KEYS, "partial_states")
    header = parse_header(node)
    states: list[PartialStateSpec] = []
    seen: dict[str, int] = {}
    for index, item in enumerate(node.sequence("partial_states", "partial_state")):
        state = parse_partial_state(item)
        if state.identifier in seen:
            raise item.error(
                f"identifier {state.identifier!r} is already used by partial_states[{seen[state.identifier]}]",
                "identifier",
            )
        seen[state.identifier] = index
        states.append(state)
    return PartialStateSet(header, tuple(states))


def load_partial_state_set(tree: SourceTree, ref: Ref) -> PartialStateSet:
    """Loads and parses the `partial_state_set` that `ref` names."""
    return parse_partial_state_set(tree.load(ref, "partial_state_set"))
