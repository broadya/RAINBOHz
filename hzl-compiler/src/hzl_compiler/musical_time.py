# SPDX-License-Identifier: Apache-2.0

"""Musical time as HzL writes it: exact `beats:ticks`, never a float.

Every position and duration in HzL is a count of beats and a count of ticks
within the beat, both integers. The pair only becomes one tick count once the
tempo map's `ticks_per_beat` is known, so the two parts are kept apart until
then: a value written for one map must not be misread on another. There is no
floating point anywhere on this path. A position that is off by one tick is a
wrong note, not a rounding error, and a practitioner who needs finer resolution
raises `ticks_per_beat` rather than writing a fraction.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from .errors import Where

_PATTERN = re.compile(r"^\s*(\d+)\s*(?::\s*(\d+)\s*)?$")


@dataclass(frozen=True, order=True)
class MusicalTime:
    """A duration or position in beats and ticks, before the map is known.

    Attributes:
        beats: Whole beats, 0 or more.
        ticks: Ticks within the last beat, 0 or more. Must be below the tempo
            map's `ticks_per_beat`, which `to_ticks` checks: this type does
            not know the map.
    """

    beats: int
    ticks: int = 0

    @classmethod
    def parse(cls, value: object, where: Where) -> "MusicalTime":
        """Reads a `beats:ticks` value as the YAML loader delivers it.

        Accepts a string such as `"2:480"`, a string holding a bare integer
        such as `"4"`, or a bare YAML integer; a bare integer means that many
        beats and no ticks. Anything else is an error, including a float such
        as `1.5`: fractions of a beat are written as ticks.

        Args:
            value: The YAML scalar.
            where: Its location, for the error.

        Returns:
            The parsed time.

        Raises:
            HzlError: if the value is not a non-negative `beats:ticks`.
        """
        if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
            return cls(value, 0)
        if isinstance(value, str):
            match = _PATTERN.match(value)
            if match:
                return cls(int(match.group(1)), int(match.group(2) or 0))
        raise where.error(
            f'expected musical time as "beats:ticks" (for example "2:480") or a whole '
            f"number of beats, got {value!r}; fractions of a beat are written as ticks"
        )

    def to_ticks(self, ticks_per_beat: int, where: Where) -> int:
        """Converts to a tick count on a map with `ticks_per_beat`.

        Args:
            ticks_per_beat: The map's resolution, positive.
            where: Where this time was written, for the error.

        Returns:
            `beats * ticks_per_beat + ticks`, exactly.

        Raises:
            HzlError: if `ticks` is not below `ticks_per_beat`.
        """
        if self.ticks >= ticks_per_beat:
            raise where.error(
                f"{self} has {self.ticks} ticks but the tempo map has {ticks_per_beat} "
                f"ticks per beat; ticks run from 0 to {ticks_per_beat - 1}"
            )
        return self.beats * ticks_per_beat + self.ticks

    def __str__(self) -> str:
        """Formats as `beats:ticks`."""
        return f"{self.beats}:{self.ticks}"
