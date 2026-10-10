# SPDX-License-Identifier: Apache-2.0

"""`partial_state_set_transformation`: names how state sets combine.

The file carries only a type for now, as in the PoC. It is kept as its own
file so that a transformation can grow parameters later without a change to
the files that reference it.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from ..nodes import Node
from ..ref import Ref
from ..source import SourceTree
from .common import HEADER_KEYS, Header, parse_header


class TransformationType(Enum):
    """The transformations this version knows, by their HzL spelling."""

    UPDATE = "Update"
    FLATTENED_CARTESIAN_PRODUCT = "Flattened Cartesian Product"


@dataclass(frozen=True)
class Transformation:
    """A parsed `partial_state_set_transformation` file.

    Attributes:
        header: The file's header.
        transformation_type: Which transformation.
    """

    header: Header
    transformation_type: TransformationType


def parse_transformation(node: Node) -> Transformation:
    """Reads a `partial_state_set_transformation` body.

    Raises:
        HzlError: if the `transformation` block or its `transformation_type` is
            missing, the type is unknown, or a key is unknown.
    """
    node.only(*HEADER_KEYS, "transformation")
    header = parse_header(node)
    inner = node.mapping("transformation")
    inner.only("transformation_type")
    text = inner.require("transformation_type", "str")
    try:
        kind = TransformationType(text)
    except ValueError:
        names = ", ".join(repr(t.value) for t in TransformationType)
        raise inner.error(
            f"unknown transformation_type {text!r}; the types are: {names}", "transformation_type"
        ) from None
    return Transformation(header, kind)


def load_transformation(tree: SourceTree, ref: Ref) -> Transformation:
    """Loads and parses the `partial_state_set_transformation` that `ref` names."""
    return parse_transformation(tree.load(ref, "partial_state_set_transformation"))
