"""Virtual microphones and arrays of them.

All microphones are coincident at the receiver (the origin), so the direction
of a source is the same at every microphone and its distance only attenuates.
An array's raw channels are its microphones' outputs; an optional decode matrix
turns them into output channels (M/S to L/R, A-format to B-format).
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .geometry import SourcePosition
from .patterns import Pattern


@dataclass(frozen=True)
class Mic:
    """A named virtual microphone. Its orientation is the axis of its pattern."""

    name: str
    pattern: Pattern


@dataclass(frozen=True)
class MicArray:
    """A set of coincident microphones plus an optional linear decode.

    `decode` has one row per output channel and one column per microphone. When
    it is None the outputs are the raw microphone channels and `output_labels`
    must equal the microphone names.
    """

    name: str
    mics: tuple[Mic, ...]
    output_labels: tuple[str, ...]
    decode: np.ndarray | None = field(default=None)

    def __post_init__(self) -> None:
        """Checks that the decode matrix and labels fit the microphones.

        Raises:
            ValueError: if the decode matrix shape does not match the number of
                microphones and output labels, or, without a decode, the labels
                are not the microphone names.
        """
        if self.decode is None:
            if self.output_labels != tuple(m.name for m in self.mics):
                raise ValueError(f"{self.name}: without a decode, output labels must be the mic names")
        elif self.decode.shape != (len(self.output_labels), len(self.mics)):
            raise ValueError(
                f"{self.name}: decode is {self.decode.shape}, expected {(len(self.output_labels), len(self.mics))}"
            )

    @property
    def raw_labels(self) -> tuple[str, ...]:
        """Names of the raw (microphone) channels, in order."""
        return tuple(m.name for m in self.mics)

    def raw_gains(self, source: SourcePosition, include_distance: bool = True) -> np.ndarray:
        """Returns each microphone's signed linear gain for a source.

        Args:
            source: Where the source is.
            include_distance: Whether to apply the 1/r distance gain. False
                gives the pattern gains alone.

        Returns:
            One gain per microphone, in microphone order.
        """
        direction = source.direction
        gains = np.array([m.pattern.gain(direction) for m in self.mics])
        return gains * source.distance_gain if include_distance else gains

    def output_gains(self, source: SourcePosition, include_distance: bool = True) -> np.ndarray:
        """Returns each output channel's signed linear gain for a source.

        Args:
            source: Where the source is.
            include_distance: Whether to apply the 1/r distance gain.

        Returns:
            One gain per output channel: the raw gains, decoded if the array
            has a decode matrix.
        """
        raw = self.raw_gains(source, include_distance)
        return raw if self.decode is None else self.decode @ raw
