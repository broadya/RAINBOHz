#include "rainbohz/rendering_engine/render_kernel.hpp"

#include <cmath>
#include <cstddef>
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

constexpr double kTwoPi = 2.0 * std::numbers::pi;

/// Reduces a phase to [-π, π] after one increment.
///
/// One conditional subtraction or addition per sample in the normal case,
/// where the increment is well under 2π; the loops make it correct for any
/// frequency, including one far above Nyquist, which the model does not
/// forbid and the roll-off silences.
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
      phaseStepOffset(partials, 0.0), ampGradient(partials, 0.0) {}

void anchorPartial(BlockState& block, std::size_t lane, const Anchor& anchor) noexcept {
    const double g =
        anchor.nominal.linearGradient.has_value() ? anchor.nominal.linearGradient->value() : 0.0;
    block.phase[lane] = wrap(phaseAt(anchor.startPhase, anchor.nominal, anchor.tau) +
                             compensationAt(anchor.compensation, anchor.tau))
                            .value();
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
            phase[i] = wrapRaw(phase[i] + freq[i] + phaseStepOffset[i]);
            freq[i] += gradient[i];
            amp[i] += ampGradient[i];
        }
        out[static_cast<std::size_t>(n)] += static_cast<SampleValue>(acc);
    }
}

} // namespace rainbohz::rendering_engine
