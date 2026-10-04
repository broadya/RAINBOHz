"""Rendering a mono source signal through a microphone array."""

from __future__ import annotations

import numpy as np

from .geometry import SourcePosition
from .mics import MicArray


def render(signal: np.ndarray, source: SourcePosition, array: MicArray) -> np.ndarray:
    """Renders a static source into the array's output channels.

    Each output channel is the signal times that channel's gain: distance gain
    times pattern gain, decoded if the array has a decode matrix. There is no
    propagation delay and no Doppler in this version.

    Args:
        signal: Mono source signal as it would be heard at 1 m, linear
            amplitude, one value per sample.
        source: Where the source is, fixed for the whole signal.
        array: The microphones and decode to render through.

    Returns:
        Array of shape (samples, output channels).
    """
    return np.outer(np.asarray(signal, dtype=float), array.output_gains(source))
