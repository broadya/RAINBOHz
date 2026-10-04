"""Directivity patterns of perfect virtual microphones.

"Perfect" means the gain depends on direction only, never on frequency. Gains
are signed: a negative gain is a polarity reversal (the rear lobe of a
figure-8, for example).

A pattern is anything with `gain(direction) -> float`. Only first-order
patterns exist so far. Higher orders (up to third-order Ambisonics) would be
further implementations of the same interface, such as a real spherical
harmonic Y(n, m), without changing microphones or arrays.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Protocol

import numpy as np

from .geometry import unit_vector

# First-order shape parameter `a` for the named patterns, in a + (1 − a)·cos θ.
OMNI = 1.0
SUBCARDIOID = 0.7
CARDIOID = 0.5
SUPERCARDIOID = (math.sqrt(3.0) - 1.0) / 2.0  # ≈ 0.366, maximum front-to-back ratio
HYPERCARDIOID = 0.25  # maximum directivity
FIGURE_8 = 0.0


class Pattern(Protocol):
    """A directivity pattern: signed linear gain for a source direction."""

    def gain(self, direction: np.ndarray) -> float:
        """Returns the signed linear gain for a source in `direction` (a unit vector)."""
        ...


@dataclass(frozen=True)
class FirstOrder:
    """First-order pattern a + (1 − a)·cos θ, θ the angle between axis and source.

    a = 1 is omni (pressure), a = 0 is figure-8 (pressure gradient); values in
    between mix the two. The axis is a unit vector; it is irrelevant for omni.
    """

    a: float
    axis: np.ndarray

    def gain(self, direction: np.ndarray) -> float:
        """Returns the signed linear gain for a source in `direction`.

        Args:
            direction: Unit vector from the receiver towards the source.

        Returns:
            a + (1 − a)·cos θ. 1 on axis; negative means polarity reversed.
        """
        return self.a + (1.0 - self.a) * float(np.dot(self.axis, direction))

    def angle_deg(self, direction: np.ndarray) -> float:
        """Returns the angle between the axis and `direction`, in degrees [0, 180]."""
        return math.degrees(math.acos(max(-1.0, min(1.0, float(np.dot(self.axis, direction))))))

    @property
    def is_omni(self) -> bool:
        """True when the pattern ignores direction (a = 1)."""
        return self.a == OMNI


def first_order(a: float, azimuth_deg: float = 0.0, elevation_deg: float = 0.0) -> FirstOrder:
    """Builds a first-order pattern pointing at an azimuth and elevation.

    Args:
        a: Shape parameter in [0, 1]; see the named constants such as CARDIOID.
        azimuth_deg: Axis azimuth in degrees, anticlockwise from the front.
        elevation_deg: Axis elevation in degrees, up from the horizontal.

    Returns:
        The pattern.

    Raises:
        ValueError: if `a` is outside [0, 1].
    """
    if not 0.0 <= a <= 1.0:
        raise ValueError(f"first-order shape parameter must be in [0, 1], got {a}")
    return FirstOrder(a, unit_vector(azimuth_deg, elevation_deg))


def omni() -> FirstOrder:
    """Builds a perfect omnidirectional pattern (pressure): gain 1 everywhere."""
    return first_order(OMNI)


def cardioid(azimuth_deg: float = 0.0, elevation_deg: float = 0.0) -> FirstOrder:
    """Builds a cardioid pointing at the given azimuth and elevation (degrees)."""
    return first_order(CARDIOID, azimuth_deg, elevation_deg)


def figure_8(azimuth_deg: float = 0.0, elevation_deg: float = 0.0) -> FirstOrder:
    """Builds a figure-8 (pressure gradient) whose positive lobe points at the given azimuth and elevation (degrees)."""
    return first_order(FIGURE_8, azimuth_deg, elevation_deg)
