// SPDX-License-Identifier: Apache-2.0

#pragma once

// The frequency and phase of one integrable segment of a partial.
//
// This is the maths shared by precomputation and rendering. The schema
// requires that "exact same algorithms, maths conventions etc. must be used
// for precomputation and for rendering" (PartialSegmentNominalFrequency);
// this header is how that requirement is met: both call these functions, on
// the same stored parameters, and so agree to the last bit.
//
// Phase is computed by exact, continuous integration of the segment's nominal
// frequency, through its antiderivative -- not by summing per-sample values,
// and not by a formula specific to linear segments. Each frequency component
// contributes its own antiderivative term, so curves and modulators, when they
// arrive, add terms here without changing any caller. See
// docs/precomputation.md section 6.5.
//
// Every position within a segment is local time τ, a SampleDuration measured
// from the segment's start. Evaluating in absolute time would lose precision
// catastrophically in the τ² term; taking a SampleDuration makes that mistake
// a compile error.
//
// Header-only and free of allocation, locking and exceptions, so that it is
// usable from the render path.

#include "rainbohz/rendering_engine/units.hpp"

#include <optional>

namespace rainbohz::rendering_engine {

/// The nominal frequency path of one segment: the in-memory form of
/// PartialSegmentNominalFrequency.
///
/// f(τ) = startFrequency + linearGradient · τ
///
/// This version has a linear component only. Curve components, and the
/// curve_* columns, arrive with curve support.
struct NominalFrequency {
    /// Frequency at the start of the segment, τ = 0.
    /// Column start_nominal_frequency.
    AngularFrequency startFrequency;

    /// Gradient of the linear component; nullopt when the segment has none.
    /// Columns linear_component (whether it is set) and linear_gradient.
    /// A flat linear segment holds a zero gradient, not nullopt.
    std::optional<FrequencyGradient> linearGradient;
};

/// The segment's nominal frequency at local time `tau`.
///
/// @param nominal  The segment's frequency path.
/// @param tau      Position within the segment, from its start. Not limited
///                 to the segment's length: callers evaluating at the end, or
///                 just beyond it, get the path's continuation.
/// @return f(τ), in radians per sample.
constexpr AngularFrequency frequencyAt(
    const NominalFrequency& nominal, SampleDuration tau) noexcept {
    AngularFrequency frequency = nominal.startFrequency;
    if (nominal.linearGradient.has_value()) {
        frequency = frequency + *nominal.linearGradient * tau;
    }
    return frequency;
}

/// The antiderivative Φ of the segment's nominal frequency, at local time `tau`.
///
/// The sum of one term per frequency component:
///
///     Φ(τ) = f₀ · τ  +  ½ · g · τ²
///
/// The phase gained between two points of a segment is the difference of Φ
/// at those points. Φ(0) is zero for the components that exist now; callers
/// nonetheless subtract it (see phaseIncrement()), because a curve component
/// need not share that property.
///
/// @param nominal  The segment's frequency path.
/// @param tau      Position within the segment, from its start.
/// @return Φ(τ), in radians.
constexpr Phase antiderivative(const NominalFrequency& nominal, SampleDuration tau) noexcept {
    Phase phi = nominal.startFrequency * tau;
    if (nominal.linearGradient.has_value()) {
        phi = phi + 0.5 * ((*nominal.linearGradient * tau) * tau);
    }
    return phi;
}

/// The phase the segment accumulates between its start and local time `tau`.
///
/// The definite integral of the nominal frequency over [0, τ]: Φ(τ) − Φ(0).
/// Precomputation calls this with τ equal to the segment's length to find the
/// next segment's start phase.
///
/// @param nominal  The segment's frequency path.
/// @param tau      Position within the segment, from its start.
/// @return The phase gained over [0, τ], in radians. Negative where the
///         frequency is negative.
constexpr Phase phaseIncrement(const NominalFrequency& nominal, SampleDuration tau) noexcept {
    return antiderivative(nominal, tau) - antiderivative(nominal, SampleDuration{0.0});
}

/// The unwrapped phase of the partial at local time `tau` within a segment.
///
/// The renderer's entry point: given a segment's stored start phase and
/// parameters, the phase at any continuous point inside it.
///
/// @param startPhase  The segment's start_phase: unwrapped phase at τ = 0.
/// @param nominal     The segment's frequency path.
/// @param tau         Position within the segment, from its start.
/// @return `startPhase + phaseIncrement(nominal, tau)`, unwrapped, in radians.
constexpr Phase phaseAt(
    Phase startPhase, const NominalFrequency& nominal, SampleDuration tau) noexcept {
    return startPhase + phaseIncrement(nominal, tau);
}

/// The phase compensation in force over a span: a linear ramp, re-based to
/// the span's start (docs/precomputation.md section 6.8).
///
/// D(τ) = startDelta + slope · τ
///
/// It is added to the phase that the frequency envelope produces. Its slope
/// is a small constant frequency offset; it is not part of the nominal
/// frequency, so the roll-off does not see it.
struct NominalCompensation {
    /// The compensation at the span's start, τ = 0. Unwrapped.
    Phase startDelta;

    /// Its rate of change, radians per sample. Zero on a free-running span.
    AngularFrequency slope;
};

/// The phase compensation at local time `tau`.
///
/// @param compensation  The span's ramp.
/// @param tau           Position within the span, from its start.
/// @return D(τ), unwrapped, in radians.
constexpr Phase compensationAt(
    const NominalCompensation& compensation, SampleDuration tau) noexcept {
    return compensation.startDelta + compensation.slope * tau;
}

} // namespace rainbohz::rendering_engine
