"""Source positions in the AmbiX spherical convention, and distance attenuation.

Axes are right-handed: x to the front, y to the left, z up. Azimuth is measured
anticlockwise from the front (+90° is left), elevation up from the horizontal
plane. The receiver is at the origin and never moves.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

MIN_DISTANCE_M = 1.0


def wrap_azimuth_deg(azimuth_deg: float) -> float:
    """Wraps an azimuth into (−180, 180].

    Args:
        azimuth_deg: Azimuth in degrees, any value. Out-of-range values are
            legitimate (a modulator can rotate a source continuously).

    Returns:
        The same direction as an azimuth in degrees in (−180, 180].
    """
    wrapped = math.fmod(azimuth_deg + 180.0, 360.0)
    if wrapped <= 0.0:
        wrapped += 360.0
    return wrapped - 180.0


def unit_vector(azimuth_deg: float, elevation_deg: float) -> np.ndarray:
    """Returns the unit vector pointing from the origin towards a direction.

    Args:
        azimuth_deg: Azimuth in degrees, anticlockwise from the front.
        elevation_deg: Elevation in degrees, up from the horizontal plane.

    Returns:
        A length-3 array (x, y, z) of unit length.
    """
    az = math.radians(azimuth_deg)
    el = math.radians(elevation_deg)
    return np.array([math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)])


def distance_gain(distance_m: float) -> float:
    """Returns the linear amplitude gain of a point source at a distance.

    Free-field inverse-distance law, referenced to 1 m: 0 dB at 1 m, −6.02 dB
    per doubling of distance.

    Args:
        distance_m: Distance from the receiver in metres, at least 1.

    Returns:
        Linear amplitude gain, 1 / distance_m.

    Raises:
        ValueError: if distance_m is below 1 m.
    """
    if distance_m < MIN_DISTANCE_M:
        raise ValueError(f"distance must be at least {MIN_DISTANCE_M} m, got {distance_m}")
    return 1.0 / distance_m


@dataclass(frozen=True)
class SourcePosition:
    """A static source position: azimuth and elevation in degrees, distance in metres.

    Azimuth is wrapped into (−180, 180] on construction. Elevation must be in
    [−90, 90] and distance at least 1 m.
    """

    azimuth_deg: float
    elevation_deg: float = 0.0
    distance_m: float = 1.0

    def __post_init__(self) -> None:
        """Validates the position and wraps the azimuth.

        Raises:
            ValueError: if the elevation is outside [−90, 90] degrees or the
                distance is below 1 m.
        """
        if not -90.0 <= self.elevation_deg <= 90.0:
            raise ValueError(f"elevation must be in [-90, 90] degrees, got {self.elevation_deg}")
        if self.distance_m < MIN_DISTANCE_M:
            raise ValueError(f"distance must be at least {MIN_DISTANCE_M} m, got {self.distance_m}")
        object.__setattr__(self, "azimuth_deg", wrap_azimuth_deg(self.azimuth_deg))

    @property
    def direction(self) -> np.ndarray:
        """Unit vector (x, y, z) from the receiver towards the source."""
        return unit_vector(self.azimuth_deg, self.elevation_deg)

    @property
    def distance_gain(self) -> float:
        """Linear amplitude gain due to distance alone (1 / distance_m)."""
        return distance_gain(self.distance_m)
