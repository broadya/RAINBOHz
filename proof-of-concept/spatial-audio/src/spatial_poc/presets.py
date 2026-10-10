# SPDX-License-Identifier: Apache-2.0

"""The preset microphone arrays offered by the proof of concept.

Every preset is built from first-order patterns plus, where needed, a decode
matrix. B-format follows AmbiX: ACN channel order (W, Y, Z, X) and SN3D
normalisation, under which first order is W = 1, Y = y, Z = z, X = x for a
unit-amplitude source in direction (x, y, z).
"""

from __future__ import annotations

import math
from collections.abc import Callable

import numpy as np

from .mics import Mic, MicArray
from .patterns import CARDIOID, cardioid, figure_8, first_order, omni

# Elevation of the tetrahedron's vertices: asin(1/√3) ≈ 35.26°.
TETRA_ELEVATION_DEG = math.degrees(math.asin(1.0 / math.sqrt(3.0)))

# Capsule order for A-format: front-left-up, front-right-down, back-left-down, back-right-up.
TETRA_CAPSULES = (
    ("FLU", 45.0, TETRA_ELEVATION_DEG),
    ("FRD", -45.0, -TETRA_ELEVATION_DEG),
    ("BLD", 135.0, -TETRA_ELEVATION_DEG),
    ("BRU", -135.0, TETRA_ELEVATION_DEG),
)

_INV_SQRT2 = 1.0 / math.sqrt(2.0)
_HALF_SQRT3 = math.sqrt(3.0) / 2.0

# A-format (FLU, FRD, BLD, BRU) of perfect cardioids to AmbiX SN3D (W, Y, Z, X).
# The four capsule axes sum to zero, so the sum of the capsules is exactly 2·omni;
# each sum-and-difference of capsules is (2/√3)·(a figure-8 along an axis).
A_TO_B = np.array(
    [
        [0.5, 0.5, 0.5, 0.5],  # W
        [_HALF_SQRT3, -_HALF_SQRT3, _HALF_SQRT3, -_HALF_SQRT3],  # Y
        [_HALF_SQRT3, -_HALF_SQRT3, -_HALF_SQRT3, _HALF_SQRT3],  # Z
        [_HALF_SQRT3, _HALF_SQRT3, -_HALF_SQRT3, -_HALF_SQRT3],  # X
    ]
)

# Omni M and figure-8 S to L/R: L = (M + S)/2, R = (M − S)/2. Hard left is 0 dB
# in L only; centre is −6 dB in each. A constant-amplitude law: L + R = M.
MS_OMNI_TO_LR = np.array([[0.5, 0.5], [0.5, -0.5]])

# Figure-8 M and figure-8 S to L/R: L = (M + S)/√2, R = (M − S)/√2. This equals
# a Blumlein pair (figure-8s at ±45°).
MS_FIGURE_8_TO_LR = np.array([[_INV_SQRT2, _INV_SQRT2], [_INV_SQRT2, -_INV_SQRT2]])


def mono() -> MicArray:
    """One perfect omni: mono output."""
    return MicArray("mono", (Mic("M", omni()),), ("M",))


def ms_theoretical_raw() -> MicArray:
    """The theoretical M/S pair, undecoded: omni M plus a figure-8 S whose positive lobe faces left.

    S = cos(el)·sin(az). It is zero at the front, back, top and bottom, and its
    equal-gain surfaces are cones about the y axis.
    """
    return MicArray("ms_theoretical_raw", (Mic("M", omni()), Mic("S", figure_8(90.0))), ("M", "S"))


def ms_theoretical() -> MicArray:
    """The theoretical M/S pair (omni M, figure-8 S), decoded to L/R stereo."""
    raw = ms_theoretical_raw()
    return MicArray("ms_theoretical", raw.mics, ("L", "R"), MS_OMNI_TO_LR)


def xy(half_angle_deg: float = 45.0, a: float = CARDIOID) -> MicArray:
    """An X/Y pair: two coincident first-order mics angled left and right, output directly as L/R.

    The default is the classic pair of cardioids at ±45° (90° between them),
    modelled as perfect first-order cardioids: 0.5 + 0.5·cos θ. Real cardioids
    narrow at high frequencies; a perfect one does not, which is what keeps the
    maths easy and the gain continuous everywhere.

    Args:
        half_angle_deg: Angle of each mic off the front, in degrees; L points
            at +half_angle_deg, R at −half_angle_deg.
        a: First-order shape parameter of both mics, in [0, 1]; the default
            is cardioid. Smaller values (super- or hypercardioid) narrow the
            pattern and add a reversed-polarity rear lobe.

    Returns:
        The array, named "xy".

    Raises:
        ValueError: if `a` is outside [0, 1].
    """
    mics = (Mic("L", first_order(a, half_angle_deg)), Mic("R", first_order(a, -half_angle_deg)))
    return MicArray("xy", mics, ("L", "R"))


def blumlein() -> MicArray:
    """A Blumlein pair: figure-8s at +45° (L) and −45° (R), output directly as L/R."""
    return MicArray("blumlein", (Mic("L", figure_8(45.0)), Mic("R", figure_8(-45.0))), ("L", "R"))


def blumlein_rotated_raw() -> MicArray:
    """Blumlein rotated by 45°, undecoded: figure-8s at 0° (M) and 90° (S), i.e. figure-8 M/S."""
    return MicArray("blumlein_rotated_raw", (Mic("M", figure_8(0.0)), Mic("S", figure_8(90.0))), ("M", "S"))


def blumlein_rotated() -> MicArray:
    """Blumlein rotated by 45° (figure-8 M/S), decoded to L/R. Identical to `blumlein` by construction."""
    raw = blumlein_rotated_raw()
    return MicArray("blumlein_rotated", raw.mics, ("L", "R"), MS_FIGURE_8_TO_LR)


def foa_a_tetra() -> MicArray:
    """A perfect tetrahedral array of four coincident cardioids: FOA A-format."""
    mics = tuple(Mic(name, cardioid(az, el)) for name, az, el in TETRA_CAPSULES)
    return MicArray("foa_a_tetra", mics, tuple(m.name for m in mics))


def foa_b_from_a() -> MicArray:
    """The tetrahedral A-format array decoded to AmbiX first-order B-format (W, Y, Z, X; SN3D)."""
    a = foa_a_tetra()
    return MicArray("foa_b_from_a", a.mics, ("W", "Y", "Z", "X"), A_TO_B)


def foa_b_direct() -> MicArray:
    """AmbiX first-order B-format encoded directly: an omni and three figure-8s along y, z and x."""
    mics = (
        Mic("W", omni()),
        Mic("Y", figure_8(90.0)),
        Mic("Z", figure_8(0.0, 90.0)),
        Mic("X", figure_8(0.0)),
    )
    return MicArray("foa_b_direct", mics, ("W", "Y", "Z", "X"))


PRESETS: dict[str, Callable[[], MicArray]] = {
    "mono": mono,
    "ms_theoretical_raw": ms_theoretical_raw,
    "ms_theoretical": ms_theoretical,
    "xy": xy,
    "blumlein": blumlein,
    "blumlein_rotated_raw": blumlein_rotated_raw,
    "blumlein_rotated": blumlein_rotated,
    "foa_a_tetra": foa_a_tetra,
    "foa_b_from_a": foa_b_from_a,
    "foa_b_direct": foa_b_direct,
}
