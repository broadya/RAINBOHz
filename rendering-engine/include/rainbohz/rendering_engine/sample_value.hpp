#pragma once

// The element type of every audio buffer, and of the WAV writer.
//
// Defined once, here, and named nowhere else: this is the representation that
// the render kernel's raw arrays and the file format share, and the one place
// to change if output moves to 64-bit or, with more work, to integers. Nothing
// else in the engine spells `float`.
//
// It is deliberately a plain alias, not a unit type (docs/rendering.md section
// 3.2). Everything computed -- phase, frequency, amplitude in both domains --
// is double; only the final product of sine and multiplier is narrowed to
// SampleValue, once, when it is added to the buffer.

namespace rainbohz::rendering_engine {

/// One audio sample: linear, unitless, nominally in [-1, 1].
using SampleValue = float;

} // namespace rainbohz::rendering_engine
