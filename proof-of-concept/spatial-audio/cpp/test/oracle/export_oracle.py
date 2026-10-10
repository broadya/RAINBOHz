# SPDX-License-Identifier: Apache-2.0

"""Write the golden gains the C++ PoC is checked against, from the Python oracle.

Run with the spatial-audio venv, where the `spatial_poc` package is installed:

    .venv/bin/python cpp/test/oracle/export_oracle.py

Each row is one channel mode at one position: mode, azimuth and elevation in
radians, distance in metres, the channel count, then one gain per channel
(output_gains, distance included), with empty fields up to four channels.
Positions are a grid (azimuth every 15°, elevation every 15° in [−90, 90])
plus seeded random positions off the grid, with distances cycled through a
fixed list so that every mode sees every distance.

Floats are written with repr(), which round-trips exactly, so the C++ side
reads the same doubles the oracle used.
"""

from __future__ import annotations

import csv
import math
from pathlib import Path

import numpy as np

from spatial_poc.geometry import SourcePosition
from spatial_poc.mics import MicArray
from spatial_poc.presets import (
    blumlein,
    blumlein_rotated_raw,
    foa_a_tetra,
    foa_b_direct,
    mono,
    ms_theoretical,
    ms_theoretical_raw,
    xy,
)

# EngineConfig.channelMode → the preset that is its oracle.
MODES: dict[int, MicArray] = {
    0: mono(),
    1: ms_theoretical(),
    2: xy(),
    3: ms_theoretical_raw(),
    4: blumlein(),
    5: blumlein_rotated_raw(),
    6: foa_a_tetra(),
    7: foa_b_direct(),
}

MAX_CHANNELS = 4
DISTANCES_M = (1.0, 2.0, 7.5, 16384.0)
GRID_STEP_DEG = 15
RANDOM_POSITIONS = 64
RANDOM_SEED = 20261004

OUTPUT = Path(__file__).with_name("oracle_gains.csv")


def positions() -> list[tuple[float, float]]:
    """Returns the (azimuth, elevation) pairs to export, in degrees.

    Returns:
        The grid, azimuth in (−180, 180] and elevation in [−90, 90], then the
        seeded random positions in the same ranges.
    """
    grid = [
        (float(az), float(el))
        for el in range(-90, 91, GRID_STEP_DEG)
        for az in range(-180 + GRID_STEP_DEG, 181, GRID_STEP_DEG)
    ]
    rng = np.random.default_rng(RANDOM_SEED)
    random = [
        (float(rng.uniform(-180.0, 180.0)), float(rng.uniform(-90.0, 90.0)))
        for _ in range(RANDOM_POSITIONS)
    ]
    return grid + random


def main() -> None:
    """Writes oracle_gains.csv beside this script.

    Side effects:
        Overwrites oracle_gains.csv, and prints its path and row count.
    """
    rows = 0
    with OUTPUT.open("w", newline="") as f:
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(
            ["mode", "azimuth_rad", "elevation_rad", "distance_m", "channels"]
            + [f"g{c}" for c in range(MAX_CHANNELS)]
        )
        for index, (az_deg, el_deg) in enumerate(positions()):
            distance = DISTANCES_M[index % len(DISTANCES_M)]
            source = SourcePosition(az_deg, el_deg, distance)
            for mode, array in MODES.items():
                gains = [repr(float(g)) for g in array.output_gains(source)]
                writer.writerow(
                    [
                        mode,
                        repr(math.radians(az_deg)),
                        repr(math.radians(el_deg)),
                        repr(distance),
                        len(gains),
                    ]
                    + gains
                    + [""] * (MAX_CHANNELS - len(gains))
                )
                rows += 1
    print(f"wrote {rows} rows to {OUTPUT}")


if __name__ == "__main__":
    main()
