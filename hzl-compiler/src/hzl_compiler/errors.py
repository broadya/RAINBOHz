# SPDX-License-Identifier: Apache-2.0

"""The one error type every stage reports faults with, and where a fault is."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Where:
    """A place in HzL source: a file and a dotted path into its YAML.

    Attributes:
        file: The source file, or None for a fault that is not in one file.
        path: The place in that file, such as
            `envelope.envelope_stages[1].duration`. Empty when not known.
    """

    file: Path | None = None
    path: str = ""

    def at(self, step: str | int) -> Where:
        """Returns the location one step deeper.

        Args:
            step: A mapping key, appended as `.key`, or a sequence index,
                appended as `[index]`.

        Returns:
            A new location; this one is unchanged.
        """
        if isinstance(step, int):
            return Where(self.file, f"{self.path}[{step}]")
        return Where(self.file, f"{self.path}.{step}" if self.path else step)

    def error(self, message: str) -> HzlError:
        """Builds an error at this location, for the caller to raise."""
        return HzlError(message, where=self)

    def __str__(self) -> str:
        """Formats as `file: path`, leaving out whichever part is not known."""
        parts = [str(part) for part in (self.file, self.path) if part]
        return ": ".join(parts)


class HzlError(Exception):
    """A fault in HzL source, or in what it asks for, reported in HzL terms.

    One exception type serves every stage, because the practitioner does not
    care which stage found the fault: they need the file, the place in it and
    what is wrong. A fault between two files (the same envelope placed twice, say)
    is reported at one of them and names the other in the message.

    Attributes:
        message: What is wrong, in the practitioner's terms.
        where: The file and path the fault is at, as far as known.
    """

    def __init__(self, message: str, *, where: Where | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.where = where if where is not None else Where()

    def __str__(self) -> str:
        """Formats as `file: path: message`, dropping the parts not known."""
        location = str(self.where)
        return f"{location}: {self.message}" if location else self.message
