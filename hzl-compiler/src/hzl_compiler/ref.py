# SPDX-License-Identifier: Apache-2.0

"""A reference from one HzL file to another, by namespace and name."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .errors import Where

# A namespace is one or more directory names separated by `/`; a name is one
# file name without its `.yaml`. Neither may step outside the source tree or
# contain the `#` that separates a namespace from a partial identifier.
_SEGMENT = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]*$")


@dataclass(frozen=True, order=True)
class Ref:
    """Names an HzL file: `<root>/<namespace>/<name>.yaml`.

    This is the PoC's file convention, kept as it was. A namespace may be
    nested (`a/b`), which is a directory path. Layers are not part of a
    reference; they are carried on the file's header and not enforced.

    Attributes:
        namespace: Slash-delimited directory path under the source root.
        name: The file's name without extension.
    """

    namespace: str
    name: str

    @classmethod
    def parse(cls, namespace: object, name: object, where: Where) -> "Ref":
        """Validates the two parts of a reference as written in YAML.

        Args:
            namespace: The `namespace` value; must be a non-empty string of
                segments separated by `/`, each a plain identifier-like name.
            name: The `name` value; one such segment.
            where: Location of the reference, for errors.

        Returns:
            The reference.

        Raises:
            HzlError: if either part is missing, not a string, or malformed.
        """
        if not isinstance(namespace, str) or not namespace:
            raise where.at("namespace").error("a reference needs a non-empty namespace string")
        if not isinstance(name, str) or not name:
            raise where.at("name").error("a reference needs a non-empty name string")
        for segment in namespace.split("/"):
            if not _SEGMENT.match(segment) or segment in (".", ".."):
                raise where.at("namespace").error(
                    f"namespace {namespace!r} is not a path of plain names separated by '/'"
                )
        if not _SEGMENT.match(name) or name in (".", ".."):
            raise where.at("name").error(f"name {name!r} is not a plain file name")
        return cls(namespace, name)

    def path(self, root: Path) -> Path:
        """The file this reference names, under `root`."""
        return root.joinpath(*PurePosixPath(self.namespace).parts, f"{self.name}.yaml")

    def __str__(self) -> str:
        """Formats as `namespace/name`, the fully qualified name used in output."""
        return f"{self.namespace}/{self.name}"
