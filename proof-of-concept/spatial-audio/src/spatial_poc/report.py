# SPDX-License-Identifier: Apache-2.0

"""The `spatial-poc-report` command: gain tables, polar plots and test WAVs.

Tables go to stdout. Plots (PNG) and WAVs go to the output directory.
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np

from .geometry import SourcePosition
from .mics import MicArray
from .patterns import FirstOrder
from .presets import PRESETS
from .render import render

SAMPLE_RATE_HZ = 48000

# Positions covering the cases that matter: the cardinal directions, the rear
# half, elevation towards the poles, and distance doublings.
DEFAULT_POSITIONS = (
    SourcePosition(0.0),
    SourcePosition(30.0),
    SourcePosition(45.0),
    SourcePosition(90.0),
    SourcePosition(135.0),
    SourcePosition(180.0),
    SourcePosition(-135.0),
    SourcePosition(-90.0),
    SourcePosition(-30.0),
    SourcePosition(90.0, 45.0),
    SourcePosition(90.0, 80.0),
    SourcePosition(0.0, 90.0),
    SourcePosition(0.0, -90.0),
    SourcePosition(30.0, 0.0, 2.0),
    SourcePosition(30.0, 0.0, 4.0),
)


def to_db(gain: float) -> str:
    """Formats a linear gain's magnitude in dB, as '-inf' when it is (numerically) zero."""
    return "-inf" if abs(gain) < 1e-12 else f"{20.0 * math.log10(abs(gain)):+.2f}"


def polarity(gain: float) -> str:
    """Returns '+', '−' or '0' for the sign of a gain."""
    if abs(gain) < 1e-12:
        return "0"
    return "+" if gain > 0 else "-"


def position_label(source: SourcePosition) -> str:
    """Formats a position as 'az +30 el +0 r 1m'."""
    return f"az {source.azimuth_deg:+.0f} el {source.elevation_deg:+.0f} r {source.distance_m:g}m"


def print_table(array: MicArray, positions: tuple[SourcePosition, ...]) -> None:
    """Prints the gain table for one array to stdout.

    For each position: per microphone, the angle off its axis, the pattern gain
    (linear, dB and polarity) and the total gain including distance; then, if
    the array decodes, the gain of each output channel.

    Args:
        array: The array to tabulate.
        positions: The source positions, one block each.
    """
    print(f"\n=== {array.name} ===")
    for source in positions:
        direction = source.direction
        print(f"-- {position_label(source)}   distance gain {source.distance_gain:.4f} ({to_db(source.distance_gain)} dB)")
        print(f"   {'mic':<5}{'angle°':>8}{'pattern':>10}{'dB':>9}{'pol':>5}{'total':>10}{'dB':>9}")
        for mic in array.mics:
            pattern = mic.pattern
            angle = "-" if isinstance(pattern, FirstOrder) and pattern.is_omni else f"{pattern.angle_deg(direction):.1f}"
            g = pattern.gain(direction)
            total = g * source.distance_gain
            print(f"   {mic.name:<5}{angle:>8}{g:>10.4f}{to_db(g):>9}{polarity(g):>5}{total:>10.4f}{to_db(total):>9}")
        if array.decode is not None:
            outputs = array.output_gains(source)
            cells = "  ".join(f"{label}={g:+.4f} ({to_db(g)} dB)" for label, g in zip(array.output_labels, outputs))
            print(f"   decoded: {cells}")


def plot_array(array: MicArray, out_dir: Path) -> Path:
    """Writes a PNG of the array's output gains around the horizontal plane and against elevation.

    Left: a polar plot of |gain| against azimuth at elevation 0, solid where
    the gain is positive and dashed where the polarity is reversed. Right: the
    signed gain against elevation at azimuth +90° (left) and +30°.

    Args:
        array: The array to plot (distance gain excluded).
        out_dir: Directory to write into; must exist.

    Returns:
        The path of the PNG written.
    """
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    azimuths = np.linspace(-180.0, 180.0, 721)
    elevations = np.linspace(-90.0, 90.0, 361)
    fig = plt.figure(figsize=(12, 5.5))
    polar = fig.add_subplot(1, 2, 1, projection="polar")
    sweep = fig.add_subplot(1, 2, 2)
    colours = plt.rcParams["axes.prop_cycle"].by_key()["color"]

    gains = np.array([array.output_gains(SourcePosition(az), include_distance=False) for az in azimuths])
    for ch, label in enumerate(array.output_labels):
        g = gains[:, ch]
        colour = colours[ch % len(colours)]
        theta = np.radians(azimuths)
        polar.plot(theta, np.where(g >= 0, g, np.nan), color=colour, label=label)
        polar.plot(theta, np.where(g < 0, -g, np.nan), color=colour, linestyle="--")
    polar.set_theta_zero_location("N")  # front at the top, left (+az) to the left
    ticks = np.arange(-135, 181, 45)
    polar.set_thetagrids(np.mod(ticks, 360), [f"{t:+d}°" if t not in (0, 180) else f"{t}°" for t in ticks])
    polar.set_title(f"{array.name}: |gain| at el 0 (dashed = reversed)")
    polar.legend(loc="lower left", bbox_to_anchor=(-0.15, -0.12))

    for az, style in ((90.0, "-"), (30.0, ":")):
        g_el = np.array([array.output_gains(SourcePosition(az, el), include_distance=False) for el in elevations])
        for ch, label in enumerate(array.output_labels):
            sweep.plot(elevations, g_el[:, ch], color=colours[ch % len(colours)], linestyle=style, label=f"{label} az {az:+.0f}")
    sweep.axhline(0.0, color="grey", linewidth=0.5)
    sweep.set_xlabel("elevation (degrees)")
    sweep.set_ylabel("signed linear gain")
    sweep.set_title("gain against elevation")
    sweep.legend(fontsize="small")

    path = out_dir / f"{array.name}.png"
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return path


def write_wav(array: MicArray, positions: tuple[SourcePosition, ...], out_dir: Path) -> Path:
    """Writes a multichannel float32 WAV stepping a test tone through the positions.

    Each position gets a 1 s, 440 Hz tone burst at 0.5 amplitude (at 1 m) with
    20 ms raised-cosine fades, then 0.25 s of silence. The channel order is the
    array's output labels; a sidecar .txt lists the channels and positions.

    Args:
        array: The array to render through.
        positions: The positions to step through, in order.
        out_dir: Directory to write into; must exist.

    Returns:
        The path of the WAV written.
    """
    from scipy.io import wavfile

    n_tone = SAMPLE_RATE_HZ
    n_fade = int(0.02 * SAMPLE_RATE_HZ)
    t = np.arange(n_tone) / SAMPLE_RATE_HZ
    tone = 0.5 * np.sin(2.0 * math.pi * 440.0 * t)
    fade = 0.5 - 0.5 * np.cos(np.linspace(0.0, math.pi, n_fade))
    tone[:n_fade] *= fade
    tone[-n_fade:] *= fade[::-1]
    gap = np.zeros((SAMPLE_RATE_HZ // 4, len(array.output_labels)))

    blocks = []
    for source in positions:
        blocks.append(render(tone, source, array))
        blocks.append(gap)
    audio = np.concatenate(blocks).astype(np.float32)

    path = out_dir / f"{array.name}.wav"
    wavfile.write(path, SAMPLE_RATE_HZ, audio)
    lines = [f"channels: {', '.join(array.output_labels)}", "positions (1 s tone, 0.25 s gap each):"]
    lines += [f"  {i + 1:2d}. {position_label(p)}" for i, p in enumerate(positions)]
    path.with_suffix(".txt").write_text("\n".join(lines) + "\n")
    return path


def main(argv: list[str] | None = None) -> int:
    """Runs `spatial-poc-report`.

    Prints the gain tables to stdout and, unless suppressed, writes plots and
    WAVs into the output directory, creating it.

    Args:
        argv: Command-line arguments; None means sys.argv.

    Returns:
        0 on success, 2 on an unknown preset name.
    """
    parser = argparse.ArgumentParser(prog="spatial-poc-report", description=__doc__.splitlines()[0])
    parser.add_argument("presets", nargs="*", help=f"presets to report (default all): {', '.join(PRESETS)}")
    parser.add_argument("-o", "--out", type=Path, default=Path("out"), help="directory for plots and WAVs (default ./out)")
    parser.add_argument("--no-plots", action="store_true", help="do not write PNG plots")
    parser.add_argument("--no-wavs", action="store_true", help="do not write WAV files")
    args = parser.parse_args(argv)

    names = args.presets or list(PRESETS)
    unknown = [n for n in names if n not in PRESETS]
    if unknown:
        print(f"spatial-poc-report: error: unknown preset(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    if not (args.no_plots and args.no_wavs):
        args.out.mkdir(parents=True, exist_ok=True)
    for name in names:
        array = PRESETS[name]()
        print_table(array, DEFAULT_POSITIONS)
        if not args.no_plots:
            plot_array(array, args.out)
        if not args.no_wavs:
            write_wav(array, DEFAULT_POSITIONS, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
