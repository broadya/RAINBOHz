// SPDX-License-Identifier: Apache-2.0

#pragma once

// The amplitude of one partial at one instant: the nominal amplitude ramp,
// the frequency-range roll-off, and the one conversion from dB to a linear
// multiplier.
//
// Every amplitude effect in the model is expressed in dB and composes by
// addition; the schema's clip at 0 dB and floor at -144 dB apply to the sum,
// once, when it is converted (docs/rendering.md section 8). The roll-off is a
// stateless function of the signed instantaneous frequency (ADR 0008).
//
// Two layers share one set of formulae. The `raw` functions take and return
// plain doubles and are what the render kernel calls on its Structure-of-
// Arrays state, where unit types do not reach (AGENTS.md, C++ Conventions).
// The unit-typed functions wrap them, and are what everything outside the
// kernel uses and what the kernel's tests compare against. Sharing the body
// is what guarantees that the two agree.
//
// Header-only and free of allocation, locking and exceptions, so that it is
// usable from the render path, exactly as segment_maths.hpp is.

#include "rainbohz/rendering_engine/units.hpp"

#include <algorithm>
#include <cmath>

namespace rainbohz::rendering_engine {

/// The floor of the dB scale: at or below it a partial is silent, and the
/// exponential is not evaluated. -144 dB is the dynamic range of 24-bit audio
/// by the 6 dB-per-bit rule (schema, "Units and limits"), so the step from it
/// to zero is one least significant bit and cannot click.
inline constexpr Decibels kSilenceFloor{-144.0};

/// Full scale. A dB sum above it is rendered at full scale.
inline constexpr Decibels kFullScale{0.0};

/// The band edges of the frequency-range roll-off, in Hz (schema header,
/// "frequency, partial nominal"). The upper outer edge is the lesser of
/// kMaxRenderingRangeCap and halfway between kMaxAudioRange and Nyquist.
/// @{
inline constexpr Hertz kMinRenderingRange{10.0};
inline constexpr Hertz kMinAudioRange{20.0};
inline constexpr Hertz kMaxAudioRange{20000.0};
inline constexpr Hertz kMaxRenderingRangeCap{30000.0};
/// @}

/// The nominal amplitude path of one render segment: the amplitude piece
/// covering it, re-based to its start.
///
/// a(τ) = startDecibels + gradient · τ
struct NominalAmplitude {
    /// Amplitude at the start of the segment, τ = 0.
    Decibels startDecibels;

    /// Rate of change of the piece the segment lies in.
    DecibelGradient gradient;
};

/// The band edges of the roll-off, as the kernel uses them: radians per
/// sample, no unit types.
struct RollOffRaw {
    /// Below this the partial is silent: kMinRenderingRange.
    double lowOuter = 0.0;

    /// Above this the low ramp has reached 0 dB: kMinAudioRange.
    double lowInner = 0.0;

    /// Up to this the high ramp has not started: kMaxAudioRange.
    double highInner = 0.0;

    /// Above this the partial is silent: the upper rendering limit.
    double highOuter = 0.0;
};

namespace raw {

/// kSilenceFloor as a double, for the kernel.
inline constexpr double kSilenceFloorDecibels = kSilenceFloor.value();

/// Converts a dB sum to a linear multiplier: clip at 0 dB, floor at -144 dB
/// (docs/rendering.md section 8.1).
///
/// @param decibels  The sum of every amplitude term, dB.
/// @return 0 at or below the floor, otherwise 10^(min(decibels, 0) / 20).
inline double toLinear(double decibels) noexcept {
    if (decibels <= kSilenceFloorDecibels) {
        return 0.0;
    }
    return std::pow(10.0, std::min(decibels, kFullScale.value()) / 20.0);
}

/// The roll-off term for a signed instantaneous frequency (docs/rendering.md
/// section 8.2): 0 dB in the audio range, linear in dB down to the floor at
/// the outer edge of each band, the floor beyond. Negative frequency is below
/// the range and so silent.
///
/// @param edges      The band edges, radians per sample.
/// @param frequency  Signed instantaneous frequency, radians per sample.
/// @return The term, from kSilenceFloor to 0 dB.
constexpr double rollOff(const RollOffRaw& edges, double frequency) noexcept {
    if (frequency < edges.lowOuter) {
        return kSilenceFloorDecibels;
    }
    if (frequency < edges.lowInner) {
        return kSilenceFloorDecibels * (edges.lowInner - frequency) /
               (edges.lowInner - edges.lowOuter);
    }
    if (frequency <= edges.highInner) {
        return 0.0;
    }
    if (frequency <= edges.highOuter) {
        return kSilenceFloorDecibels * (frequency - edges.highInner) /
               (edges.highOuter - edges.highInner);
    }
    return kSilenceFloorDecibels;
}

} // namespace raw

/// The nominal amplitude at local time `tau` within a render segment.
///
/// @param nominal  The segment's amplitude path.
/// @param tau      Position within the segment, from its start.
/// @return a(τ), in dB.
constexpr Decibels amplitudeAt(const NominalAmplitude& nominal, SampleDuration tau) noexcept {
    return nominal.startDecibels + nominal.gradient * tau;
}

/// Converts a dB sum to the multiplier applied to the sine: clip at
/// kFullScale, floor at kSilenceFloor.
///
/// @param decibels  The sum of every amplitude term.
/// @return 0 at or below the floor, 1 at or above full scale, and the
///         exponential between.
inline LinearAmplitude toLinear(Decibels decibels) noexcept {
    return LinearAmplitude{raw::toLinear(decibels.value())};
}

/// The band edges of the roll-off in the unit the kernel compares in.
struct RollOff {
    /// kMinRenderingRange.
    AngularFrequency lowOuter;

    /// kMinAudioRange.
    AngularFrequency lowInner;

    /// kMaxAudioRange.
    AngularFrequency highInner;

    /// min(kMaxRenderingRangeCap, (kMaxAudioRange + Nyquist) / 2).
    AngularFrequency highOuter;

    /// The same edges as plain doubles, for the kernel.
    constexpr RollOffRaw raw() const noexcept {
        return RollOffRaw{.lowOuter = lowOuter.value(),
            .lowInner = lowInner.value(),
            .highInner = highInner.value(),
            .highOuter = highOuter.value()};
    }
};

/// The roll-off's band edges for a sample rate, converted once per run.
///
/// @param rate  Sample rate. Must be positive.
/// @return The four edges in radians per sample.
constexpr RollOff makeRollOff(SampleRate rate) noexcept {
    const double nyquist = static_cast<double>(rate.value()) / 2.0;
    const Hertz highOuter{
        std::min(kMaxRenderingRangeCap.value(), (kMaxAudioRange.value() + nyquist) / 2.0)};
    return RollOff{
        .lowOuter = toAngular(kMinRenderingRange, rate),
        .lowInner = toAngular(kMinAudioRange, rate),
        .highInner = toAngular(kMaxAudioRange, rate),
        .highOuter = toAngular(highOuter, rate),
    };
}

/// The roll-off term for a signed instantaneous frequency (ADR 0008).
///
/// @param rollOff    The band edges from makeRollOff().
/// @param frequency  Signed instantaneous frequency.
/// @return 0 dB inside the audio range, kSilenceFloor outside the rendering
///         range, a linear ramp in dB between; negative frequency is silent.
constexpr Decibels rollOffAt(const RollOff& rollOff, AngularFrequency frequency) noexcept {
    return Decibels{raw::rollOff(rollOff.raw(), frequency.value())};
}

} // namespace rainbohz::rendering_engine
