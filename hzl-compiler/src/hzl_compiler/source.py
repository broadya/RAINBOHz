# SPDX-License-Identifier: Apache-2.0

"""The HzL source tree: finding, reading and checking files by reference.

Files are found by the PoC convention `<root>/<namespace>/<name>.yaml`. Every
file is a one-key mapping whose key is its HzL type, and its header must say
the same name and namespace as its location: a file that lies about where it
is would resolve references differently from how it was written.
"""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path

import yaml

from . import HZL_VERSION
from .errors import HzlError, Where
from .nodes import Node
from .ref import Ref

# The HzL types this version reads. The value of each file's single top-level
# key is the type's body; a file whose type is not here is refused.
HZL_TYPES = (
    "score",
    "tempo_map",
    "sound_entity",
    "sound_source",
    "envelope",
    "trajectory",
    "partial_state_set_transformation",
    "partial_state_set",
    "virtual_partial_state_set",
    "scale",
    "harmonic_series",
)


class SourceTree:
    """The HzL files under one root directory, read on demand and cached.

    Attributes:
        root: The directory namespaces are resolved under.
    """

    def __init__(self, root: Path) -> None:
        """Creates a tree over `root`. Nothing is read until a load."""
        self.root = root
        self._cache: dict[Ref, tuple[str, Node]] = {}

    @classmethod
    def from_file(cls, path: Path) -> tuple["SourceTree", Ref]:
        """Opens the tree a file belongs to, working the root out from its header.

        The header's `namespace` names the directories between the root and
        the file, so the root is the file's directory with those stripped.

        Args:
            path: An HzL file, typically the score to compile.

        Returns:
            The tree, and the reference naming `path` within it.

        Raises:
            HzlError: if the file cannot be read as HzL, or its directory does
                not end in its namespace.
        """
        path = path.resolve()
        hzl_type, body = _read(path)
        where = Where(path, hzl_type)
        name = body.optional("name", "scalar")
        namespace = body.optional("namespace", "scalar")
        ref = Ref.parse(namespace, name, where)
        if path.stem != ref.name:
            raise where.at("name").error(f"name {ref.name!r} does not match the file name {path.stem!r}")
        parts = ref.namespace.split("/")
        parent = path.parent
        if [p.name for p in [parent, *parent.parents][: len(parts)]][::-1] != parts:
            raise where.at("namespace").error(
                f"namespace {ref.namespace!r} does not match the directories the file is in"
            )
        root = parent
        for _ in parts:
            root = root.parent
        return cls(root), ref

    def load_any(self, ref: Ref) -> tuple[str, Node]:
        """Reads the file `ref` names, whatever its HzL type.

        Args:
            ref: The file to read.

        Returns:
            The file's HzL type (its top-level key) and its body. The body's
            header has been checked against `ref` and the language version.

        Raises:
            HzlError: if there is no such file, it is not a one-key mapping of
                a known type, or its header disagrees with `ref`.
        """
        if ref in self._cache:
            return self._cache[ref]
        path = ref.path(self.root)
        if not path.is_file():
            raise HzlError(f"no file for reference {ref}: expected {path}")
        hzl_type, body = _read(path)
        _check_header(body, hzl_type, ref)
        self._cache[ref] = (hzl_type, body)
        return hzl_type, body

    def load(self, ref: Ref, hzl_type: str) -> Node:
        """Reads the file `ref` names, which must be of `hzl_type`.

        Raises:
            HzlError: as `load_any`, or if the file is of another type.
        """
        actual, body = self.load_any(ref)
        if actual != hzl_type:
            raise HzlError(f"{ref} is a {actual}, but a {hzl_type} is needed here", where=Where(ref.path(self.root)))
        return body


def _read(path: Path) -> tuple[str, Node]:
    """Parses one HzL file into its type and body.

    Args:
        path: The file.

    Returns:
        The top-level key and the mapping under it, located at `path`.

    Raises:
        HzlError: if the file is not valid YAML, or not a mapping with exactly
            one key naming a known HzL type whose value is a mapping.
    """
    where = Where(path)
    try:
        document = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        raise where.error(f"not valid YAML: {error}") from error
    except OSError as error:
        raise where.error(f"cannot read: {error.strerror}") from error
    if not isinstance(document, Mapping) or len(document) != 1:
        raise where.error("an HzL file is a mapping with exactly one top-level key, its type")
    (hzl_type, body), = document.items()
    if hzl_type not in HZL_TYPES:
        raise where.error(f"unknown HzL type {hzl_type!r}; the types are: {', '.join(HZL_TYPES)}")
    if not isinstance(body, Mapping):
        raise where.at(hzl_type).error("expected a mapping")
    return hzl_type, Node(body, where.at(hzl_type))


def _check_header(body: Node, hzl_type: str, ref: Ref) -> None:
    """Checks a file's header agrees with how it was reached.

    Args:
        body: The file's body.
        hzl_type: Its top-level key.
        ref: The reference it was loaded by.

    Raises:
        HzlError: if `hzl_version` is not this compiler's, `hzl_type` is not
            the top-level key, or `name`/`namespace` are not `ref`'s.
    """
    version = body.require("hzl_version", "scalar")
    if str(version) != HZL_VERSION:
        raise body.error(f"hzl_version {version!r} is not {HZL_VERSION!r}, the version this compiler reads", "hzl_version")
    declared = body.require("hzl_type", "str")
    if declared != hzl_type:
        raise body.error(f"hzl_type {declared!r} does not match the top-level key {hzl_type!r}", "hzl_type")
    name = body.require("name", "scalar")
    if str(name) != ref.name:
        raise body.error(f"name {name!r} does not match the file name {ref.name!r}", "name")
    namespace = body.require("namespace", "str")
    if namespace != ref.namespace:
        raise body.error(f"namespace {namespace!r} does not match the directories {ref.namespace!r}", "namespace")
