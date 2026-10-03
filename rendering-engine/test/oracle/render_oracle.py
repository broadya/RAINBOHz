#!/usr/bin/env python3
"""Independent check of one rendered frame against the closed-form model.

An oracle for rendering-engine (rendering-engine/docs/rendering.md section
11.1): given a JSON description of a score, a frame number and the WAV file
the engine wrote for that frame, compute every sample of the frame directly
and report the largest difference.

"Directly" is the point. The engine keeps a phase accumulator, re-anchors it
at chunk boundaries, and sums partials in float; this script does none of
that. The phase at each sample is the closed-form integral of the piecewise
linear frequency up to that instant, the amplitude is interpolated in dB at
that instant, and the roll-off is evaluated from the frequency at that
instant. Agreement means two independent routes reach the same samples;
disagreement localises to a sample.

The score description uses the rendering model's units exactly as engine-db
holds them: times in samples, frequencies in radians per sample, amplitudes
in dB, phases in radians. Curves are linear; the format has no field for
anything the engine does not render. A partial's "phase" is its phase
timeline, [[time, radians or null], ...]; when it is absent the partial
starts at phase 0 and runs free, which is the one-point timeline
[[start, 0.0]].

Phase control is evaluated from its definition (rendering-engine/docs/
precomputation.md section 6.8), not from anything the engine stores: the
initial phase, then a running adjustment at each target, linear between
points.

    render_oracle.py --score score.json --frame 3 --wav frame-000003.wav
    render_oracle.py --score score.json --frame 3 --wav frame-000003.set-000001.wav --set 1

Exit status 0 when the maximum absolute difference is at most the tolerance
(1e-6 by default: 16 least significant bits at 24-bit, which allows for the
engine's narrowing to float and its float summation and is far above anything
the double arithmetic on either side contributes); 1 otherwise, or on a
malformed score or file.

Descends from proof-of-concept/maths/phase_integration_render_demo.py, whose
closed-form phase is the same idea. Only NumPy is required.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from dataclasses import dataclass

import numpy as np

SILENCE_FLOOR_DB = -144.0
MIN_RENDERING_RANGE_HZ = 10.0
MIN_AUDIO_RANGE_HZ = 20.0
MAX_AUDIO_RANGE_HZ = 20000.0
MAX_RENDERING_RANGE_CAP_HZ = 30000.0
TWENTY_FOUR_BIT_LSB = 2.0**-23


class OracleError(Exception):
    """A malformed score, a malformed WAV file, or a mismatch between them."""


@dataclass
class Partial:
    """One partial: two piecewise-linear timelines in the rendering model's units."""

    partial_id: int
    frequency: np.ndarray  # shape (n, 2): time in samples, radians per sample
    amplitude: np.ndarray  # shape (m, 2): time in samples, dB
    phase: list[tuple[float, float | None]]  # (time in samples, radians or None)


@dataclass
class Score:
    """A score description as read from JSON."""

    sample_rate: int
    frame_size: int
    partial_sets: dict[int, list[Partial]]


# ---------------------------------------------------------------------------
# The score
# ---------------------------------------------------------------------------


def _timeline(points: list, what: str, partial_id: int) -> np.ndarray:
    """Convert and check one timeline's points.

    Args:
        points: A list of [time, value] pairs from the JSON.
        what: "frequency" or "amplitude", for messages.
        partial_id: The partial's id, for messages.

    Returns:
        The points as a float64 array of shape (n, 2).

    Raises:
        OracleError: if there are fewer than two points, a value is not
            finite, the first time is negative, or times do not strictly
            increase.
    """
    array = np.asarray(points, dtype=np.float64)
    if array.ndim != 2 or array.shape[1] != 2 or array.shape[0] < 2:
        raise OracleError(f"partial {partial_id}: {what} needs at least two [time, value] points")
    if not np.all(np.isfinite(array)):
        raise OracleError(f"partial {partial_id}: {what} has a non-finite value")
    if array[0, 0] < 0.0:
        raise OracleError(f"partial {partial_id}: {what} starts before the score")
    if not np.all(np.diff(array[:, 0]) > 0.0):
        raise OracleError(f"partial {partial_id}: {what} times do not strictly increase")
    return array


def _phase_timeline(points: list | None, frequency: np.ndarray, partial_id: int):
    """Convert and check a phase timeline.

    Args:
        points: A list of [time, radians or null] pairs from the JSON, or
            None when the partial has no "phase" entry.
        frequency: The partial's frequency timeline, for its lifeline.
        partial_id: The partial's id, for messages.

    Returns:
        The points as (time, phase or None) tuples; [(start, 0.0)] when
        `points` is None.

    Raises:
        OracleError: if there are no points, the first is not at the
            lifeline's start, times do not strictly increase or pass its end,
            or a phase is not finite or outside [-pi, pi].
    """
    start, end = float(frequency[0, 0]), float(frequency[-1, 0])
    if points is None:
        return [(start, 0.0)]
    timeline = [(float(time), None if value is None else float(value)) for time, value in points]
    if not timeline or timeline[0][0] != start:
        raise OracleError(f"partial {partial_id}: phase must start with a point at {start}")
    for (before, _), (time, _) in zip(timeline, timeline[1:]):
        if not time > before:
            raise OracleError(f"partial {partial_id}: phase times do not strictly increase")
    for time, value in timeline:
        if time > end:
            raise OracleError(f"partial {partial_id}: phase point at {time} is after the lifeline")
        if value is not None and not (math.isfinite(value) and -math.pi <= value <= math.pi):
            raise OracleError(f"partial {partial_id}: phase {value} is not in [-pi, pi]")
    return timeline


def load_score(path: str) -> Score:
    """Read and validate a score description.

    Args:
        path: The JSON file.

    Returns:
        The score, with each partial's timelines checked for the invariants
        the engine also validates: at least two points each, strictly
        increasing times, and the same first and last times on both.

    Raises:
        OracleError: if the file is not a score description, or a partial
            violates one of those invariants.
        OSError: if the file cannot be read.
    """
    with open(path, encoding="utf-8") as handle:
        raw = json.load(handle)
    try:
        sample_rate = int(raw["sample_rate"])
        frame_size = int(raw.get("frame_size", sample_rate))
        sets_raw = raw["partial_sets"]
    except (KeyError, TypeError, ValueError) as error:
        raise OracleError(f"{path}: not a score description ({error})") from error
    if sample_rate <= 0 or frame_size <= 0:
        raise OracleError(f"{path}: sample_rate and frame_size must be positive")

    partial_sets: dict[int, list[Partial]] = {}
    for set_raw in sets_raw:
        set_id = int(set_raw["id"])
        partials = []
        for partial_raw in set_raw["partials"]:
            partial_id = int(partial_raw["id"])
            frequency = _timeline(partial_raw["frequency"], "frequency", partial_id)
            amplitude = _timeline(partial_raw["amplitude"], "amplitude", partial_id)
            if frequency[0, 0] != amplitude[0, 0] or frequency[-1, 0] != amplitude[-1, 0]:
                raise OracleError(
                    f"partial {partial_id}: frequency and amplitude timelines must start and "
                    "end together"
                )
            phase = _phase_timeline(partial_raw.get("phase"), frequency, partial_id)
            partials.append(Partial(partial_id, frequency, amplitude, phase))
        if set_id in partial_sets:
            raise OracleError(f"{path}: partial set {set_id} appears twice")
        partial_sets[set_id] = partials
    return Score(sample_rate, frame_size, partial_sets)


# ---------------------------------------------------------------------------
# The WAV file
# ---------------------------------------------------------------------------


def read_float_wav(path: str) -> tuple[int, int, np.ndarray]:
    """Read a RIFF/WAVE file of IEEE float samples, checking its layout.

    Python's `wave` module does not read float data, so this is a small RIFF
    parser of its own, and every run of the oracle is also an independent
    check of the container the engine wrote.

    Args:
        path: The file.

    Returns:
        (sample_rate, channels, samples): the header's rate and channel count,
        and the samples as float64, interleaved as in the file.

    Raises:
        OracleError: if the file is not RIFF/WAVE, has no `fmt ` or `data`
            chunk, is not format 3 (IEEE float) at 32 bits, or has a chunk
            size that does not fit the file.
        OSError: if the file cannot be read.
    """
    with open(path, "rb") as handle:
        blob = handle.read()
    if len(blob) < 12 or blob[0:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise OracleError(f"{path}: not a RIFF/WAVE file")
    riff_size = struct.unpack_from("<I", blob, 4)[0]
    if riff_size + 8 != len(blob):
        raise OracleError(f"{path}: RIFF size {riff_size} does not match file size {len(blob)}")

    fmt = None
    data = None
    at = 12
    while at + 8 <= len(blob):
        chunk_id = blob[at : at + 4]
        chunk_size = struct.unpack_from("<I", blob, at + 4)[0]
        body = blob[at + 8 : at + 8 + chunk_size]
        if len(body) != chunk_size:
            raise OracleError(f"{path}: chunk {chunk_id!r} of {chunk_size} bytes overruns the file")
        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise OracleError(f"{path}: fmt chunk of {chunk_size} bytes is too short")
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif chunk_id == b"data":
            data = body
        at += 8 + chunk_size + (chunk_size & 1)  # chunks are word-aligned
    if fmt is None or data is None:
        raise OracleError(f"{path}: missing fmt or data chunk")

    tag, channels, rate, byte_rate, block_align, bits = fmt
    if tag != 3 or bits != 32:
        raise OracleError(f"{path}: format tag {tag} at {bits} bits is not IEEE float 32")
    if block_align != 4 * channels or byte_rate != rate * block_align:
        raise OracleError(f"{path}: fmt chunk's block align or byte rate is inconsistent")
    if len(data) % block_align != 0:
        raise OracleError(f"{path}: data chunk is not a whole number of frames")
    samples = np.frombuffer(data, dtype="<f4").astype(np.float64)
    return rate, channels, samples


# ---------------------------------------------------------------------------
# The model, evaluated directly
# ---------------------------------------------------------------------------


def roll_off_edges(sample_rate: int) -> tuple[float, float, float, float]:
    """The roll-off's band edges in radians per sample (rendering.md section 8.2).

    Args:
        sample_rate: Samples per second.

    Returns:
        (low_outer, low_inner, high_inner, high_outer).
    """
    nyquist = sample_rate / 2.0
    high_outer_hz = min(MAX_RENDERING_RANGE_CAP_HZ, (MAX_AUDIO_RANGE_HZ + nyquist) / 2.0)
    to_angular = lambda hz: 2.0 * math.pi * hz / sample_rate  # noqa: E731
    return (
        to_angular(MIN_RENDERING_RANGE_HZ),
        to_angular(MIN_AUDIO_RANGE_HZ),
        to_angular(MAX_AUDIO_RANGE_HZ),
        to_angular(high_outer_hz),
    )


def roll_off_db(frequency: np.ndarray, edges: tuple[float, float, float, float]) -> np.ndarray:
    """The roll-off term for signed instantaneous frequencies.

    Args:
        frequency: Radians per sample, any sign.
        edges: From roll_off_edges().

    Returns:
        dB per element: 0 in the audio range, the floor outside the rendering
        range, a linear ramp in dB between. Negative frequency is silent.
    """
    low_outer, low_inner, high_inner, high_outer = edges
    term = np.zeros_like(frequency)
    term[frequency < low_outer] = SILENCE_FLOOR_DB
    band = (frequency >= low_outer) & (frequency < low_inner)
    term[band] = SILENCE_FLOOR_DB * (low_inner - frequency[band]) / (low_inner - low_outer)
    band = (frequency > high_inner) & (frequency <= high_outer)
    term[band] = SILENCE_FLOOR_DB * (frequency[band] - high_inner) / (high_outer - high_inner)
    term[frequency > high_outer] = SILENCE_FLOOR_DB
    return term


def to_linear(decibels: np.ndarray) -> np.ndarray:
    """Clip at 0 dB, floor at -144 dB, convert (rendering.md section 8.1)."""
    linear = np.power(10.0, np.minimum(decibels, 0.0) / 20.0)
    linear[decibels <= SILENCE_FLOOR_DB] = 0.0
    return linear


def wrap_half_open(phase: float) -> float:
    """The same angle in [-pi, pi): an exact half cycle is -pi."""
    reduced = math.remainder(phase, 2.0 * math.pi)
    return reduced - 2.0 * math.pi if reduced >= math.pi else reduced


def natural_phase(partial: Partial, t: np.ndarray) -> np.ndarray:
    """The phase the frequency envelope alone gives, from 0 at the start.

    The integral of the piecewise-linear frequency from the partial's start:
    the sum of the exact integrals of the pieces before the one containing
    `t`, plus the integral within it. Nothing is accumulated sample by sample.

    Args:
        partial: The partial.
        t: Absolute times in samples within [start, end].

    Returns:
        The unwrapped phase at each time.
    """
    times = partial.frequency[:, 0]
    freqs = partial.frequency[:, 1]
    lengths = np.diff(times)
    gradients = np.diff(freqs) / lengths
    piece_phase = freqs[:-1] * lengths + 0.5 * gradients * lengths**2
    start_phase = np.concatenate(([0.0], np.cumsum(piece_phase)[:-1]))
    piece = np.clip(np.searchsorted(times, t, side="right") - 1, 0, len(lengths) - 1)
    tau = t - times[piece]
    return start_phase[piece] + freqs[piece] * tau + 0.5 * gradients[piece] * tau**2


def phase_control(partial: Partial) -> tuple[float, np.ndarray, np.ndarray]:
    """The initial phase and the compensation, from the phase timeline.

    The initial phase is the first point's phase; if that is null, the one
    from which running free meets the first point that has a phase. After
    that point, each point with a phase moves a running adjustment by the
    smallest change, in [-pi, pi), that meets it; a null point leaves it
    alone. The adjustment is linear between points and holds after the last.

    Args:
        partial: The partial.

    Returns:
        (initial phase, knot times, adjustment at each knot), for linear
        interpolation over the lifeline.
    """
    def natural0(time: float) -> float:
        return float(natural_phase(partial, np.array([time]))[0])

    timeline = partial.phase
    anchor = next((i for i, (_, value) in enumerate(timeline) if value is not None), None)
    if anchor is None:
        initial = 0.0
    elif anchor == 0:
        initial = timeline[0][1]
    else:
        time, value = timeline[anchor]
        initial = wrap_half_open(value - natural0(time))

    knot_times = [timeline[0][0]]
    knot_deltas = [0.0]
    delta = 0.0
    for i, (time, value) in enumerate(timeline[1:], start=1):
        if anchor is not None and i > anchor and value is not None:
            delta += wrap_half_open(value - (initial + natural0(time) + delta))
        knot_times.append(time)
        knot_deltas.append(delta)
    return initial, np.array(knot_times), np.array(knot_deltas)


def partial_samples(partial: Partial, t: np.ndarray, edges) -> np.ndarray:
    """One partial's samples at the instants `t`, from the closed form.

    The phase at `t` is the initial phase, plus the integral of the
    piecewise-linear frequency from the partial's start (natural_phase()),
    plus the phase compensation at `t` (phase_control()).

    Args:
        partial: The partial.
        t: Absolute times in samples, one per output sample.
        edges: From roll_off_edges().

    Returns:
        The samples; zero where the partial is not alive.
    """
    times = partial.frequency[:, 0]
    freqs = partial.frequency[:, 1]
    lengths = np.diff(times)
    gradients = np.diff(freqs) / lengths

    alive = (t >= times[0]) & (t < times[-1])
    out = np.zeros_like(t)
    if not np.any(alive):
        return out
    ta = t[alive]

    piece = np.searchsorted(times, ta, side="right") - 1
    piece = np.clip(piece, 0, len(lengths) - 1)
    tau = ta - times[piece]
    frequency = freqs[piece] + gradients[piece] * tau
    initial, knot_times, knot_deltas = phase_control(partial)
    phase = initial + natural_phase(partial, ta) + np.interp(ta, knot_times, knot_deltas)

    a_times = partial.amplitude[:, 0]
    a_values = partial.amplitude[:, 1]
    a_gradients = np.diff(a_values) / np.diff(a_times)
    a_piece = np.searchsorted(a_times, ta, side="right") - 1
    a_piece = np.clip(a_piece, 0, len(a_gradients) - 1)
    amplitude_db = a_values[a_piece] + a_gradients[a_piece] * (ta - a_times[a_piece])

    total_db = amplitude_db + roll_off_db(frequency, edges)
    out[alive] = np.sin(phase) * to_linear(total_db)
    return out


def expected_frame(score: Score, frame: int, set_ids: list[int]) -> np.ndarray:
    """The frame's samples from the closed form, summed over the given sets.

    Args:
        score: The score.
        frame: Frame number, from 0.
        set_ids: Which partial sets to sum.

    Returns:
        `frame_size` samples as float64.
    """
    n = np.arange(score.frame_size, dtype=np.float64)
    t = float(frame) * float(score.frame_size) + n
    edges = roll_off_edges(score.sample_rate)
    total = np.zeros_like(t)
    for set_id in set_ids:
        for partial in score.partial_sets[set_id]:
            total += partial_samples(partial, t, edges)
    return total


# ---------------------------------------------------------------------------
# The comparison
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    """Compare one frame's WAV file against the closed form and report.

    Args:
        argv: Command-line arguments; None for sys.argv.

    Returns:
        The process exit status: 0 on agreement within the tolerance, 1
        otherwise. Prints the maximum absolute difference, the sample at
        which it occurs, and the difference in 24-bit least significant bits
        to stdout; errors go to stderr.
    """
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--score", required=True, help="JSON score description")
    parser.add_argument("--frame", required=True, type=int, help="frame number, from 0")
    parser.add_argument("--wav", required=True, help="the engine's WAV file for that frame")
    parser.add_argument("--set", type=int, default=None, help="check a per-set file for this set")
    parser.add_argument("--tolerance", type=float, default=1e-6, help="maximum difference allowed")
    args = parser.parse_args(argv)

    try:
        score = load_score(args.score)
        rate, channels, engine = read_float_wav(args.wav)
        if rate != score.sample_rate:
            raise OracleError(f"{args.wav}: sample rate {rate} differs from the score's {score.sample_rate}")
        if channels != 1:
            raise OracleError(f"{args.wav}: {channels} channels; this version is mono")
        if len(engine) != score.frame_size:
            raise OracleError(f"{args.wav}: {len(engine)} samples, not one frame of {score.frame_size}")
        if args.set is not None and args.set not in score.partial_sets:
            raise OracleError(f"{args.score}: no partial set {args.set}")
    except (OracleError, OSError) as error:
        print(f"render_oracle: {error}", file=sys.stderr)
        return 1

    set_ids = [args.set] if args.set is not None else sorted(score.partial_sets)
    expected = expected_frame(score, args.frame, set_ids)
    difference = np.abs(engine - expected)
    worst = int(np.argmax(difference)) if len(difference) else 0
    maximum = float(difference[worst]) if len(difference) else 0.0

    which = f"set {args.set}" if args.set is not None else f"{len(set_ids)} set(s)"
    partials = sum(len(score.partial_sets[s]) for s in set_ids)
    print(
        f"frame {args.frame}, {which}, {partials} partial(s): max |difference| {maximum:.3e} "
        f"at sample {worst} ({maximum / TWENTY_FOUR_BIT_LSB:.2f} LSB at 24-bit); "
        f"engine peak {np.max(np.abs(engine)) if len(engine) else 0.0:.6f}"
    )
    if maximum > args.tolerance:
        print(f"render_oracle: difference exceeds the tolerance {args.tolerance:g}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
