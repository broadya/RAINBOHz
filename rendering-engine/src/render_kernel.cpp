// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/render_kernel.hpp"

#include "rainbohz/rendering_engine/spatial_gains.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

/// @file
/// @brief The serial, scalar reference render kernel.
///
/// Implements anchorPartial() and renderChunk() of render_kernel.hpp as
/// docs/rendering.md section 7 lays them out. The loop order is samples
/// outer, partials inner, so that a sample's sum is complete as soon as the
/// inner loop ends and the only per-sample storage is the output buffer.
///
/// @see render_kernel.hpp

namespace rainbohz::rendering_engine {

namespace {

/// 2π: one full cycle, in radians.
constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Reduces a phase to [-π, π] after one increment.
///
/// One conditional subtraction or addition per sample in the normal case,
/// where the increment is well under 2π; the loops make it correct for a
/// frequency far above Nyquist, which the model does not forbid and the
/// roll-off silences. The caller never passes a step beyond kMaxPhaseStep,
/// so the loops make at most about a thousand passes.
///
/// @param phase  A phase just advanced from a wrapped value.
/// @return The same angle in [-π, π].
inline double wrapRaw(double phase) noexcept {
    while (phase > std::numbers::pi) {
        phase -= kTwoPi;
    }
    while (phase < -std::numbers::pi) {
        phase += kTwoPi;
    }
    return phase;
}

} // namespace

BlockState::BlockState(std::size_t partials)
    : phase(partials, 0.0), freq(partials, 0.0), amp(partials, 0.0), gradient(partials, 0.0),
      phaseStepOffset(partials, 0.0), ampGradient(partials, 0.0), cappedPhaseSteps(partials, 0) {}

void anchorPartial(BlockState& block, std::size_t lane, const Anchor& anchor) noexcept {
    const double g =
        anchor.nominal.linearGradient.has_value() ? anchor.nominal.linearGradient->value() : 0.0;
    const double phase = wrap(phaseAt(anchor.startPhase, anchor.nominal, anchor.tau) +
                              compensationAt(anchor.compensation, anchor.tau))
                             .value();
    // A phase that overflowed is not carried into the loop, where one NaN
    // would poison the whole buffer: the lane starts at 0 and the count
    // records it, as for a step that is too large.
    if (std::isfinite(phase)) [[likely]] {
        block.phase[lane] = phase;
    } else {
        block.phase[lane] = 0.0;
        ++block.cappedPhaseSteps[lane];
    }
    block.freq[lane] = frequencyAt(anchor.nominal, anchor.tau).value();
    block.amp[lane] = amplitudeAt(anchor.amplitude, anchor.tau).value();
    block.gradient[lane] = g;
    block.phaseStepOffset[lane] = (0.5 * g) + anchor.compensation.slope.value();
    block.ampGradient[lane] = anchor.amplitude.gradient.value();
}

void renderChunk(BlockState& block, std::span<const std::size_t> active, SampleIndex first,
    SampleIndex last, const RollOffRaw& rollOff, std::span<SampleValue> out) noexcept {
    std::vector<double>& phase = block.phase;
    std::vector<double>& freq = block.freq;
    std::vector<double>& amp = block.amp;
    const std::vector<double>& gradient = block.gradient;
    const std::vector<double>& phaseStepOffset = block.phaseStepOffset;
    const std::vector<double>& ampGradient = block.ampGradient;
    std::vector<std::int64_t>& cappedPhaseSteps = block.cappedPhaseSteps;

    for (std::int32_t n = first.value(); n < last.value(); ++n) {
        double acc = 0.0;
        for (const std::size_t i : active) {
            const double raw = std::sin(phase[i]);
            const double decibels = amp[i] + raw::rollOff(rollOff, freq[i]);
            acc += raw * raw::toLinear(decibels);

            // The exact phase gained over one sample of a linear segment:
            // Φ(τ+1) − Φ(τ) = f(τ) + g/2, not the forward-Euler f(τ). ADR 0007
            // records why the g/2 is there; removing it is a regression. The
            // offset also carries the compensation's slope (ADR 0019).
            //
            // A step beyond kMaxPhaseStep, or one that is not a number, is
            // not taken: the phase holds, and the lane's count records it.
            // The check bounds wrapRaw() to about a thousand passes; the
            // sum is written out again so that the phase is bit-for-bit what
            // it was before the check existed.
            if (std::abs(freq[i] + phaseStepOffset[i]) <= kMaxPhaseStep) [[likely]] {
                phase[i] = wrapRaw(phase[i] + freq[i] + phaseStepOffset[i]);
            } else {
                ++cappedPhaseSteps[i];
            }
            freq[i] += gradient[i];
            amp[i] += ampGradient[i];
        }
        out[static_cast<std::size_t>(n)] += static_cast<SampleValue>(acc);
    }
}

void spatialiseChunk(const SpatialAnchor& anchor, ChannelMode mode,
    std::span<const SampleValue> mono, SampleIndex first, SampleIndex last,
    std::span<SampleValue> interleaved) noexcept {
    const std::size_t channels = channelCount(mode);
    const double azimuth = anchor.position.startAzimuth.value();
    const double azimuthGradient = anchor.position.azimuthGradient.value();
    const double elevation = anchor.position.startElevation.value();
    const double elevationGradient = anchor.position.elevationGradient.value();
    const double distance = anchor.position.startDistance.value();
    const double distanceGradient = anchor.position.distanceGradient.value();
    const double tau0 = anchor.tau.value() - static_cast<double>(first.value());

    for (std::int32_t n = first.value(); n < last.value(); ++n) {
        const auto index = static_cast<std::size_t>(n);
        const double tau = tau0 + static_cast<double>(n);
        const raw::Gains gains =
            raw::positionGains(mode, raw::wrapAzimuth(azimuth + (azimuthGradient * tau)),
                elevation + (elevationGradient * tau), distance + (distanceGradient * tau));
        const auto sample = static_cast<double>(mono[index]);
        for (std::size_t c = 0; c < channels; ++c) {
            interleaved[(index * channels) + c] += static_cast<SampleValue>(gains[c] * sample);
        }
    }
}

} // namespace rainbohz::rendering_engine
