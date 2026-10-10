# SPDX-License-Identifier: Apache-2.0

"""Validated access to a YAML mapping, with every value's location to hand.

The YAML loader gives back plain dicts, lists and scalars with no idea of
where they came from. A `Node` wraps a mapping together with its `Where`, so
that every accessor can report a missing key, a wrong type or an unknown key
at the exact place in the file, in the terms a practitioner reads.

Type names are deliberately few and strict. YAML's own typing is loose (an
unquoted `4` is an int, `4.0` a float, `yes` a bool), and the accessors do not
paper over that: a field that wants a string gets a string or an error.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
from typing import Any

from .errors import HzlError, Where
from .ref import Ref

# The scalar kinds an accessor can ask for, and how each is checked. `float`
# accepts an int as well, since `440` is a reasonable way to write 440 Hz;
# `int` does not accept a float, since `4.0` beats is not an integer tick
# count. Neither accepts a bool, which Python would otherwise let through.
_KINDS: dict[str, Callable[[Any], bool]] = {
    "str": lambda v: isinstance(v, str),
    "int": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "float": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "bool": lambda v: isinstance(v, bool),
    "mapping": lambda v: isinstance(v, Mapping),
    "list": lambda v: isinstance(v, Sequence) and not isinstance(v, (str, bytes)),
    "scalar": lambda v: isinstance(v, (str, int, float)) and not isinstance(v, bool),
}

_MISSING = object()


class Node:
    """A YAML mapping and its location.

    Attributes:
        where: Where this mapping is in its file.
    """

    def __init__(self, data: Mapping[str, Any], where: Where) -> None:
        """Wraps `data`, which the caller has already checked is a mapping."""
        self._data = data
        self.where = where

    def has(self, key: str) -> bool:
        """True when `key` is present, whatever its value."""
        return key in self._data

    def keys(self) -> list[str]:
        """The mapping's keys in file order."""
        return list(self._data)

    def error(self, message: str, key: str | None = None) -> HzlError:
        """Builds an error at this mapping, or at one of its keys."""
        where = self.where.at(key) if key is not None else self.where
        return where.error(message)

    def only(self, *allowed: str) -> None:
        """Rejects any key not in `allowed`.

        Strictness here is what catches a misspelt key, which YAML would
        otherwise accept silently and the compiler would then ignore.

        Raises:
            HzlError: naming the first unknown key and the keys allowed.
        """
        for key in self._data:
            if key not in allowed:
                raise self.error(
                    f"unknown key {key!r}; the keys allowed here are: {', '.join(allowed)}", key
                )

    def reject(self, key: str, reason: str) -> None:
        """Errors if `key` is present, with `reason` for why it is not allowed."""
        if key in self._data:
            raise self.error(reason, key)

    def require(self, key: str, kind: str) -> Any:
        """Returns the value at `key`, which must be present and of `kind`.

        Args:
            key: The mapping key.
            kind: One of `str`, `int`, `float`, `bool`, `mapping`, `list`,
                `scalar`. A `float` value is returned as a Python float.

        Raises:
            HzlError: if the key is missing, null, or of another kind.
        """
        value = self._data.get(key, _MISSING)
        if value is _MISSING or value is None:
            raise self.error(f"missing required key {key!r}", key)
        return self._checked(key, value, kind)

    def optional(self, key: str, kind: str, default: Any = None) -> Any:
        """Returns the value at `key` if present and not null, else `default`.

        Args:
            key: The mapping key.
            kind: As for `require`.
            default: Returned when the key is absent or null.

        Raises:
            HzlError: if the key is present with a value of another kind.
        """
        value = self._data.get(key, _MISSING)
        if value is _MISSING or value is None:
            return default
        return self._checked(key, value, kind)

    def mapping(self, key: str) -> Node:
        """Returns the required mapping at `key` as a Node located there."""
        return Node(self.require(key, "mapping"), self.where.at(key))

    def sequence(self, key: str, item_key: str, *, required: bool = True) -> list[Node]:
        """Returns the list at `key`, whose items are each `{item_key: {...}}`.

        This is the PoC's list shape, kept as it was: every item of
        `envelope_stages` is a one-key mapping `envelope_stage`, and so on.
        Each returned Node is located at `key[i].item_key`.

        Args:
            key: The mapping key holding the list.
            item_key: The one key every item must have.
            required: When False, an absent or null list is an empty list.

        Raises:
            HzlError: if the list is missing (when required), not a list, or
                an item is not a one-key mapping of `item_key` to a mapping.
        """
        items = self.require(key, "list") if required else self.optional(key, "list", [])
        nodes = []
        for index, item in enumerate(items):
            item_where = self.where.at(key).at(index)
            if not isinstance(item, Mapping) or list(item) != [item_key]:
                raise item_where.error(
                    f"each item here is a mapping with the single key {item_key!r}"
                )
            body = item[item_key]
            if not isinstance(body, Mapping):
                raise item_where.at(item_key).error("expected a mapping")
            nodes.append(Node(body, item_where.at(item_key)))
        return nodes

    def ref(self, key: str) -> Ref:
        """Returns the required `{namespace, name}` reference at `key`."""
        node = self.mapping(key)
        node.only("namespace", "name")
        return Ref.parse(
            node.optional("namespace", "scalar"), node.optional("name", "scalar"), node.where
        )

    def optional_ref(self, key: str) -> Ref | None:
        """Returns the reference at `key`, or None when absent."""
        if self._data.get(key) is None:
            return None
        return self.ref(key)

    def labels(self, key: str = "labels") -> tuple[str, ...]:
        """Returns the optional list of label strings at `key`, in order.

        Raises:
            HzlError: if an item is not a string, or a label is repeated.
        """
        values = self.optional(key, "list", [])
        labels: list[str] = []
        for index, value in enumerate(values):
            if not isinstance(value, str) or not value:
                raise self.where.at(key).at(index).error("a label is a non-empty string")
            if value in labels:
                raise self.where.at(key).at(index).error(f"label {value!r} is repeated")
            labels.append(value)
        return tuple(labels)

    def _checked(self, key: str, value: Any, kind: str) -> Any:
        """Checks `value` is of `kind`, converting an int to float for `float`."""
        check = _KINDS[kind]
        if not check(value):
            raise self.error(f"expected {kind} for {key!r}, got {_describe(value)}", key)
        if kind == "float":
            return float(value)
        return value


def _describe(value: Any) -> str:
    """Names a YAML value's kind for an error message."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return f"boolean {value!r}"
    if isinstance(value, (int, float, str)):
        return repr(value)
    if isinstance(value, Mapping):
        return "a mapping"
    return "a list"
