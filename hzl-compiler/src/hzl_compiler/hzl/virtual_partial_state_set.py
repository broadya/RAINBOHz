"""`virtual_partial_state_set`: a combinator over other state sets."""

from __future__ import annotations

from dataclasses import dataclass

from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header


@dataclass(frozen=True)
class VirtualPartialStateSet:
    """A parsed `virtual_partial_state_set` file.

    Attributes:
        header: The file's header.
        transformation: Reference to the transformation to apply.
        sources: References to the state sets it combines, in order. Each may
            be any state-set-valued type: a written set, a scale, a harmonic
            series or another virtual set.
    """

    header: Header
    transformation: Ref
    sources: tuple[Ref, ...]


def parse_virtual_partial_state_set(node: Node) -> VirtualPartialStateSet:
    """Reads a `virtual_partial_state_set` body.

    Raises:
        HzlError: if the transformation or the source list is missing or
            malformed, or a key is unknown.
    """
    node.only(*HEADER_KEYS, "transformation", "partial_state_sets")
    header = parse_header(node)
    transformation = node.ref("transformation")
    sources = []
    for item in node.sequence("partial_state_sets", "partial_state_set"):
        item.only("namespace", "name")
        sources.append(Ref.parse(item.optional("namespace", "scalar"), item.optional("name", "scalar"), item.where))
    return VirtualPartialStateSet(header, transformation, tuple(sources))


def load_virtual_partial_state_set(tree: SourceTree, ref: Ref) -> VirtualPartialStateSet:
    """Loads and parses the `virtual_partial_state_set` that `ref` names."""
    return parse_virtual_partial_state_set(tree.load(ref, "virtual_partial_state_set"))
