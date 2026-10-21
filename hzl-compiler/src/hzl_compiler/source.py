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

# Files are parsed with libyaml, which is about eight times faster than PyYAML's
# pure-Python parser: parsing was most of hzlc's time on a large piece. It is
# required, so that a PyYAML built without it does not quietly run slow.
try:
    _LOADER = yaml.CSafeLoader
except AttributeError as missing:
    raise ImportError(
        "hzl-compiler needs PyYAML built with libyaml (yaml.CSafeLoader); "
        "reinstall the venv with `make venv`"
    ) from missing

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
        # Parsed YAML by path, so a file the tree scan read is not parsed again.
        self._documents: dict[Path, object] = {}

    @classmethod
    def from_file(cls, path: Path) -> tuple[SourceTree, Ref]:
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
            raise where.at("name").error(
                f"name {ref.name!r} does not match the file name {path.stem!r}"
            )
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
        hzl_type, body = _read(path, self._document(path))
        _check_header(body, hzl_type, ref)
        self._cache[ref] = (hzl_type, body)
        return hzl_type, body

    def refs_of_type(self, hzl_type: str) -> list[Ref]:
        """Every file in the tree whose type is `hzl_type`, in reference order.

        Walks `<root>/<namespace>/<name>.yaml`. A file directly under the root
        has no namespace and is not HzL (a piece's own configuration, say);
        nor is anything under a directory whose name starts with `.`. Other
        YAML files are read, and those whose one top-level key is not
        `hzl_type` are left alone, so a non-HzL file in the tree does no harm.
        A file that is not valid YAML is an error: it could be one of the
        files looked for. A match is loaded as `load_any` loads it, header
        checks and all.

        Args:
            hzl_type: The type to look for, such as `tempo_map`.

        Returns:
            The references, sorted.

        Raises:
            HzlError: if a file is not valid YAML, or a file of `hzl_type`
                sits where no reference could name it, or fails `load_any`.
        """
        refs: list[Ref] = []
        for path in sorted(self.root.rglob("*.yaml")):
            parts = path.relative_to(self.root).parts
            if len(parts) < 2 or any(part.startswith(".") for part in parts):
                continue
            try:
                document = self._document(path)
            except HzlError as error:
                raise Where(path).error(
                    f"{error.message}, so it cannot be told apart from a {hzl_type}"
                ) from error
            if not isinstance(document, Mapping) or list(document) != [hzl_type]:
                continue
            ref = Ref.parse("/".join(parts[:-1]), path.stem, Where(path, hzl_type))
            self.load_any(ref)
            refs.append(ref)
        return sorted(refs)

    def _document(self, path: Path) -> object:
        """The YAML document at `path`, parsed once per tree.

        Raises:
            HzlError: if the file cannot be read or is not valid YAML.
        """
        if path not in self._documents:
            self._documents[path] = _parse(path)
        return self._documents[path]

    def load(self, ref: Ref, hzl_type: str) -> Node:
        """Reads the file `ref` names, which must be of `hzl_type`.

        Raises:
            HzlError: as `load_any`, or if the file is of another type.
        """
        actual, body = self.load_any(ref)
        if actual != hzl_type:
            raise HzlError(
                f"{ref} is a {actual}, but a {hzl_type} is needed here",
                where=Where(ref.path(self.root)),
            )
        return body


def _parse(path: Path) -> object:
    """Parses the YAML file at `path` with libyaml.

    Raises:
        HzlError: if the file cannot be read or is not valid YAML.
    """
    where = Where(path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise where.error(f"cannot read: {error.strerror}") from error
    try:
        return yaml.load(text, Loader=_LOADER)
    except yaml.YAMLError as error:
        raise where.error(f"not valid YAML: {_explained(text, error)}") from error


def _explained(text: str, error: yaml.YAMLError) -> yaml.YAMLError:
    """The error to report for `text`, which libyaml failed to parse.

    libyaml's messages give the line and column but not the source line with a
    `^` under the fault, so the pure-Python parser is run again just for its
    message. Should it accept what libyaml refused, libyaml's error stands.
    """
    try:
        yaml.load(text, Loader=yaml.SafeLoader)
    except yaml.YAMLError as explained:
        return explained
    return error


def _read(path: Path, document: object | None = None) -> tuple[str, Node]:
    """Turns one HzL file into its type and body.

    Args:
        path: The file.
        document: Its parsed YAML, if already to hand; parsed here otherwise.

    Returns:
        The top-level key and the mapping under it, located at `path`.

    Raises:
        HzlError: if the file is not valid YAML, or not a mapping with exactly
            one key naming a known HzL type whose value is a mapping.
    """
    where = Where(path)
    if document is None:
        document = _parse(path)
    if not isinstance(document, Mapping) or len(document) != 1:
        raise where.error("an HzL file is a mapping with exactly one top-level key, its type")
    ((hzl_type, body),) = document.items()
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
        raise body.error(
            f"hzl_version {version!r} is not {HZL_VERSION!r}, the version this compiler reads",
            "hzl_version",
        )
    declared = body.require("hzl_type", "str")
    if declared != hzl_type:
        raise body.error(
            f"hzl_type {declared!r} does not match the top-level key {hzl_type!r}", "hzl_type"
        )
    name = body.require("name", "scalar")
    if str(name) != ref.name:
        raise body.error(f"name {name!r} does not match the file name {ref.name!r}", "name")
    namespace = body.require("namespace", "str")
    if namespace != ref.namespace:
        raise body.error(
            f"namespace {namespace!r} does not match the directories {ref.namespace!r}", "namespace"
        )
