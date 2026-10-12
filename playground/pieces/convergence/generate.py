# SPDX-License-Identifier: Apache-2.0

"""Generates the HzL of the piece `convergence`, from a fixed seed.

800 partials of equal amplitude in 10 sound sources of 80, spread evenly
from -60 to +60 degrees azimuth at 1 m. Over 10 beats (10 s at 60 BPM) every
source glides to its share of a sawtooth: those on the left (positive azimuth) of one at
100 Hz, those on the right of one at 99 Hz. The pair of sources nearest the
centre takes harmonics 1-40, the next pair 41-80, and so on out to 161-200
at +-60 degrees. Each harmonic is a pair of partials that start 10% above
and 10% below it (START_DETUNE), so a pair's frequencies close in at a rate
proportional to the harmonic, and every partial reaches phase 0 at beat 10 so that the pairs
add rather than cancel and the harmonics make a true sawtooth (the engine's
oscillator is sin(phase)). The sawtooth then holds for 10 beats.

Run with the repository's venv, from anywhere:

    .venv/bin/python playground/pieces/convergence/generate.py

It rewrites every HzL file of the piece and prints the expected peak levels.
Nothing in the HzL is random; the seed only feeds the start-level estimate.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SEED = 20261012  # only for the random phases of the start-level estimate
SOURCES = 10
PARTIALS_PER_SOURCE = 80
HARMONICS_PER_SOURCE = PARTIALS_PER_SOURCE // 2
START_DETUNE = 0.10  # a pair starts this fraction above and below its harmonic
START_DB = -40.0  # each start partial
LEFT_HZ, RIGHT_HZ = 100.0, 99.0
TARGET_PEAK_DB = -3.0  # the sawtooth's peak on the louder channel
SAMPLE_RATE = 48000
HEADER = (
    '  hzl_version: "0.1.0"\n  hzl_type: {t}\n  name: {n}\n  namespace: {ns}\n'
    '  layer: "0: base"\n'
)


def azimuth(source: int) -> float:
    """Gives a source's azimuth, evenly from -60 to +60 degrees.

    Args:
        source: Source index, 0 to SOURCES - 1.

    Returns:
        Degrees, anticlockwise from the front: positive is the listener's left.
    """
    return -60.0 + 120.0 * source / (SOURCES - 1)


def first_harmonic(source: int) -> int:
    """Gives the lowest harmonic number a source converges to.

    Sources are ranked by distance from the centre; rank r takes harmonics
    r * HARMONICS_PER_SOURCE + 1 upwards, on its own side's fundamental.

    Args:
        source: Source index.

    Returns:
        The first of its HARMONICS_PER_SOURCE consecutive harmonics.
    """
    rank = abs(source - (SOURCES - 1) / 2) - 0.5
    return int(rank) * HARMONICS_PER_SOURCE + 1


def fundamental(source: int) -> float:
    """Gives the sawtooth fundamental a source converges to, in Hz.

    Args:
        source: Source index.

    Returns:
        LEFT_HZ for a source on the left, RIGHT_HZ for one on the right.
    """
    return LEFT_HZ if azimuth(source) > 0 else RIGHT_HZ


def gains(source: int) -> tuple[float, float]:
    """Gives the (L, R) gains of a source in channel mode 1 at 1 m.

    Stereo from M/S: omni M and a figure-8 S facing left, L = (M + S) / 2,
    R = (M - S) / 2 (rendering-engine/docs/rendering.md section 8.5).

    Args:
        source: Source index.

    Returns:
        Linear gains for the left and right channels.
    """
    s = math.sin(math.radians(azimuth(source)))
    return (1 + s) / 2, (1 - s) / 2


def sawtooth_peak(scale: float) -> float:
    """Simulates the held sawtooth and returns its peak over both channels.

    The two fundamentals realign once a second, so one second holds every
    shape the sum takes.

    Args:
        scale: Linear amplitude of harmonic 1 (pair summed); harmonic n is
            scale / n.

    Returns:
        The largest absolute sample, linear, of L and R.
    """
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    out = np.zeros((2, SAMPLE_RATE))
    for source in range(SOURCES):
        first = first_harmonic(source)
        n = np.arange(first, first + HARMONICS_PER_SOURCE)[:, None]
        wave = (scale / n * np.sin(2 * np.pi * fundamental(source) * n * t)).sum(axis=0)
        for channel, gain in enumerate(gains(source)):
            out[channel] += gain * wave
    return float(np.abs(out).max())


def start_peak(freqs: np.ndarray, rng: np.random.Generator) -> float:
    """Estimates the start's peak over both channels, from random phases.

    The engine chooses the start phases itself (each partial runs free to its
    target), so this is an estimate, not the rendered value.

    Args:
        freqs: Start frequencies, Hz, shape (SOURCES, PARTIALS_PER_SOURCE).
        rng: For the phases. Advanced.

    Returns:
        The largest absolute sample, linear, of L and R over one second.
    """
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    amp = 10 ** (START_DB / 20)
    out = np.zeros((2, SAMPLE_RATE))
    for source in range(SOURCES):
        f = freqs[source][:, None]
        phase = rng.uniform(0, 2 * np.pi, f.shape)
        wave = (amp * np.sin(2 * np.pi * f * t + phase)).sum(axis=0)
        for channel, gain in enumerate(gains(source)):
            out[channel] += gain * wave
    return float(np.abs(out).max())


def write(rel: str, text: str) -> None:
    """Writes one HzL file under the piece, creating its directory.

    Args:
        rel: Path relative to the piece.
        text: The file's content. Replaces any existing file.
    """
    path = HERE / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def state_set(name: str, comment: str, states: list[tuple[str, float, float, bool]]) -> str:
    """Formats a partial_state_set file in namespace state_sets.

    Args:
        name: The set's name, also its file name.
        comment: Opening comment line, without '#'.
        states: (identifier, frequency Hz, amplitude dB, phase target 0?) each.

    Returns:
        The file's text.
    """
    lines = [
        f"# {comment}",
        "partial_state_set:",
        HEADER.format(t="partial_state_set", n=name, ns="state_sets").rstrip("\n"),
        "  partial_states:",
    ]
    for ident, freq, amp, phase in states:
        lines.append(
            f"    - partial_state: {{identifier: {ident}, frequency: {freq:.6f}, "
            f"amplitude: {amp:.4f}{', phase: 0' if phase else ''}}}"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    """Generates the piece's HzL files and prints the expected levels.

    Side effects: rewrites the HzL files under this directory; prints to stdout.
    """
    rng = np.random.default_rng(SEED)
    # Partial p of a source belongs to its harmonic first + p // 2, starting
    # above it when p is even and below it when p is odd.
    freqs = np.array([
        [
            fundamental(s) * (first_harmonic(s) + p // 2)
            * (1 + START_DETUNE if p % 2 == 0 else 1 - START_DETUNE)
            for p in range(PARTIALS_PER_SOURCE)
        ]
        for s in range(SOURCES)
    ])

    scale = 10 ** (TARGET_PEAK_DB / 20) / sawtooth_peak(1.0)
    print(
        f"sawtooth: harmonic 1 at {20 * math.log10(scale):.2f} dB (pair summed), "
        f"peak {TARGET_PEAK_DB} dBFS"
    )
    print(f"start: estimated peak {20 * math.log10(start_peak(freqs, rng)):.2f} dBFS")

    write(
        "tempo_maps/main.yaml",
        "# 60 BPM throughout: a beat is a second.\ntempo_map:\n"
        + HEADER.format(t="tempo_map", n="main", ns="tempo_maps")
        + '  ticks_per_beat: 960\n  points:\n    - tempo_map_point:\n        position: "0:0"\n'
        "        tempo: 60.0\n        hold: true\n",
    )
    write(
        "transformations/update.yaml",
        "partial_state_set_transformation:\n"
        + HEADER.format(t="partial_state_set_transformation", n="update", ns="transformations")
        + '  transformation:\n    transformation_type: "Update"\n',
    )
    write(
        "scores/convergence.yaml",
        "# Generated by generate.py. Pairs 10% either side of each harmonic converge\n"
        "# to two sawteeth, 100 Hz left and 99 Hz right, over 10 s; then 10 s held.\n"
        "# Stereo from M/S (channel mode 1).\n"
        "score:\n"
        + HEADER.format(t="score", n="convergence", ns="scores")
        + "  labels: []\n  tempo_map: {namespace: tempo_maps, name: main}\n"
        "  sound_entities:\n    - sound_entity: {namespace: entities, name: field}\n"
        '      position: "0:0"\n',
    )
    write(
        "entities/field.yaml",
        "sound_entity:\n"
        + HEADER.format(t="sound_entity", n="field", ns="entities")
        + "  sound_sources:\n"
        + "".join(
            f"    - sound_source: {{namespace: sources, name: s{s}}}\n" for s in range(SOURCES)
        ),
    )

    stage = (
        '    - envelope_stage:\n        duration: "10:0"\n        envelope_transforms:\n'
        "          - envelope_transform:\n"
        "              transformation: {{namespace: transformations, name: update}}\n"
        "              source_partial_state_set: {{namespace: state_sets, name: {n}}}\n"
    )
    for s in range(SOURCES):
        az, f0, first = azimuth(s), fundamental(s), first_harmonic(s)
        last = first + HARMONICS_PER_SOURCE - 1
        side = "left" if az > 0 else "right"
        write(
            f"trajectories/t{s}.yaml",
            f"# Source s{s}: {az:+.2f} degrees ({side}), 1 m, still.\ntrajectory:\n"
            + HEADER.format(t="trajectory", n=f"t{s}", ns="trajectories")
            + f"  initial_position: {{azimuth: {az:.4f}, elevation: 0.0, distance: 1.0}}\n",
        )
        write(
            f"sources/s{s}.yaml",
            "sound_source:\n"
            + HEADER.format(t="sound_source", n=f"s{s}", ns="sources")
            + f"  trajectory: {{namespace: trajectories, name: t{s}}}\n"
            f"  envelopes:\n    - envelope: {{namespace: envelopes, name: e{s}}}\n",
        )
        ids = [f"s{s}p{p:02d}" for p in range(PARTIALS_PER_SOURCE)]
        start = [(ids[p], float(freqs[s, p]), START_DB, False) for p in range(PARTIALS_PER_SOURCE)]
        saw = []
        for p in range(PARTIALS_PER_SOURCE):
            n = first + p // 2
            amp = 20 * math.log10(scale / n / 2)  # half each: the pair arrives in phase
            saw.append((ids[p], f0 * n, amp))
        write(
            f"state_sets/s{s}_start.yaml",
            state_set(
                f"s{s}_start",
                f"Generated. Source s{s}'s start: each harmonic's pair at +-"
                f"{START_DETUNE:.0%}, all at {START_DB} dB.",
                start,
            ),
        )
        write(
            f"state_sets/s{s}_saw.yaml",
            state_set(
                f"s{s}_saw",
                f"Generated. Source s{s} arrives: harmonics {first}-{last} of {f0:g} Hz, "
                "two partials each, phase 0.",
                [(i, f, a, True) for i, f, a in saw],
            ),
        )
        write(
            f"state_sets/s{s}_hold.yaml",
            state_set(
                f"s{s}_hold",
                f"Generated. Source s{s}'s end: as s{s}_saw, without the phase target.",
                [(i, f, a, False) for i, f, a in saw],
            ),
        )
        write(
            f"envelopes/e{s}.yaml",
            f"# Source s{s}: detuned pairs to sawtooth over 10 beats, then held 10.\nenvelope:\n"
            + HEADER.format(t="envelope", n=f"e{s}", ns="envelopes")
            + f"  initial_partial_state_set: {{namespace: state_sets, name: s{s}_start}}\n"
            "  envelope_stages:\n"
            + stage.format(n=f"s{s}_saw")
            + stage.format(n=f"s{s}_hold"),
        )
        print(f"s{s}: {az:+7.2f} deg {side:5}  harmonics {first:3}-{last:3} of {f0:g} Hz")


if __name__ == "__main__":
    main()
