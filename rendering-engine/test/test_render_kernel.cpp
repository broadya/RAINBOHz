// SPDX-License-Identifier: Apache-2.0

// Tests for the render kernel.
//
// The kernel is an accumulator; every expected value here comes from the
// closed form instead -- phaseAt(), amplitudeAt() and toLinear() evaluated
// directly at each sample -- so that the raw arithmetic is checked against
// the typed reference (docs/rendering.md section 11, "the kernel is the
// boundary"). The tolerance allows for the narrowing to float at the add.

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/render_kernel.hpp"
#include "rainbohz/rendering_engine/segment_maths.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace {

using rainbohz::rendering_engine::amplitudeAt;
using rainbohz::rendering_engine::Anchor;
using rainbohz::rendering_engine::anchorPartial;
using rainbohz::rendering_engine::AngularFrequency;
using rainbohz::rendering_engine::BlockState;
using rainbohz::rendering_engine::DecibelGradient;
using rainbohz::rendering_engine::Decibels;
using rainbohz::rendering_engine::FrequencyGradient;
using rainbohz::rendering_engine::kMaxPhaseStep;
using rainbohz::rendering_engine::makeRollOff;
using rainbohz::rendering_engine::NominalAmplitude;
using rainbohz::rendering_engine::NominalFrequency;
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::phaseAt;
using rainbohz::rendering_engine::renderChunk;
using rainbohz::rendering_engine::RollOff;
using rainbohz::rendering_engine::RollOffRaw;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleIndex;
using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::SampleValue;
using rainbohz::rendering_engine::toLinear;
using rainbohz::rendering_engine::wrap;

const RollOff kRollOff = makeRollOff(SampleRate{48000});
const RollOffRaw kEdges = kRollOff.raw();

/// An anchor at the start of a linear segment.
///
/// @param startPhase  φ_s, radians.
/// @param frequency   f_s, radians per sample.
/// @param gradient    g, radians per sample².
/// @param decibels    a_s, dB.
/// @param ampGradient g_a, dB per sample.
/// @param tau         τ_a, samples.
/// @return The anchor.
Anchor anchor(double startPhase, double frequency, double gradient, double decibels,
    double ampGradient, double tau = 0.0) {
    return Anchor{
        .startPhase = Phase{startPhase},
        .nominal = NominalFrequency{.startFrequency = AngularFrequency{frequency},
            .linearGradient = FrequencyGradient{gradient}},
        .amplitude = NominalAmplitude{.startDecibels = Decibels{decibels},
            .gradient = DecibelGradient{ampGradient}},
        .compensation = {},
        .tau = SampleDuration{tau},
    };
}

/// The closed-form sample of a partial at local time `tau` from an anchor
/// whose own tau is 0.
///
/// @param a    The anchor.
/// @param tau  Position within the segment.
/// @return sin(phaseAt(τ)) · toLinear(amplitudeAt(τ) + rollOff(f(τ))).
double closedForm(const Anchor& a, double tau) {
    const SampleDuration t{tau};
    const AngularFrequency f = rainbohz::rendering_engine::frequencyAt(a.nominal, t);
    const Decibels dB =
        amplitudeAt(a.amplitude, t) + rainbohz::rendering_engine::rollOffAt(kRollOff, f);
    return std::sin(phaseAt(a.startPhase, a.nominal, t).value()) * toLinear(dB).value();
}

/// Renders one partial from `a` over `samples` samples in one chunk.
///
/// @param a        The anchor, at τ = 0.
/// @param samples  Chunk length.
/// @return The samples.
std::vector<SampleValue> renderOne(const Anchor& a, std::int32_t samples) {
    BlockState block{1};
    anchorPartial(block, 0, a);
    std::vector<SampleValue> out(static_cast<std::size_t>(samples), 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{samples}, kEdges, out);
    return out;
}

// The worked example of docs/rendering.md section 12: frame 3 of the
// constant partial. φ_s = 14400, f = 0.1, −6 dB.
TEST(RenderKernel, ConstantSineMatchesSinSampleBySample) {
    const Anchor a = anchor(14400.0, 0.1, 0.0, -6.0, 0.0);
    const std::vector<SampleValue> out = renderOne(a, 48000);

    EXPECT_NEAR(out[0], -0.43739, 1e-5);
    EXPECT_NEAR(out[1], -0.41078, 1e-5);
    EXPECT_NEAR(out[2], -0.38006, 1e-5);
    for (std::size_t n = 0; n < out.size(); n += 997) {
        EXPECT_NEAR(out[n], 0.501187 * std::sin(14400.0 + (0.1 * static_cast<double>(n))), 2e-6)
            << "sample " << n;
    }
}

// A chirp: the accumulator must follow phaseAt, which is exact, and not the
// forward-Euler step, which drifts by g/2 per sample (ADR 0007).
TEST(RenderKernel, ChirpMatchesPhaseAt) {
    const Anchor a = anchor(0.3, 0.05, 5e-6, 0.0, 0.0);
    const std::int32_t samples = 20000;
    const std::vector<SampleValue> out = renderOne(a, samples);
    for (std::size_t n = 0; n < out.size(); n += 101) {
        EXPECT_NEAR(out[n], closedForm(a, static_cast<double>(n)), 2e-6) << "sample " << n;
    }

    // The accumulator's own phase after the chunk, against the closed form.
    // Forward Euler would be off by g/2 · N = 0.05 radians here.
    BlockState block{1};
    anchorPartial(block, 0, a);
    std::vector<SampleValue> scratch(static_cast<std::size_t>(samples), 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{samples}, kEdges, scratch);
    const double expected =
        wrap(phaseAt(a.startPhase, a.nominal, SampleDuration{static_cast<double>(samples)}))
            .value();
    EXPECT_NEAR(block.phase[0], expected, 1e-9);
    EXPECT_NEAR(block.freq[0], 0.05 + (5e-6 * samples), 1e-12);
}

TEST(RenderKernel, FadeMatchesAmplitudeAt) {
    const Anchor a = anchor(0.0, 0.2, 0.0, -60.0, 0.054);
    const std::vector<SampleValue> out = renderOne(a, 1000);
    for (std::size_t n = 0; n < out.size(); n += 37) {
        EXPECT_NEAR(out[n], closedForm(a, static_cast<double>(n)), 2e-6) << "sample " << n;
    }
}

TEST(RenderKernel, AnchoringMidSegmentMatchesTheClosedForm) {
    // The second partial of the worked example at its first sample, τ = 0.5.
    const Anchor a = anchor(0.0, 0.05, 5e-6, -60.0, 0.054, 0.5);
    BlockState block{1};
    anchorPartial(block, 0, a);
    EXPECT_NEAR(block.phase[0], 0.02500063, 1e-8);
    EXPECT_NEAR(block.freq[0], 0.0500025, 1e-12);
    EXPECT_NEAR(block.amp[0], -59.973, 1e-9);
    EXPECT_DOUBLE_EQ(block.gradient[0], 5e-6);
    EXPECT_DOUBLE_EQ(block.phaseStepOffset[0], 2.5e-6);
    EXPECT_DOUBLE_EQ(block.ampGradient[0], 0.054);
}

TEST(RenderKernel, AnchoringWrapsThePhase) {
    const Anchor a = anchor(14400.0, 0.1, 0.0, -6.0, 0.0);
    BlockState block{1};
    anchorPartial(block, 0, a);
    EXPECT_NEAR(block.phase[0], -1.06072, 1e-5);
    EXPECT_LE(std::abs(block.phase[0]), std::numbers::pi);
}

// Cutting a chunk in two and re-anchoring at the cut must give the same
// samples as the uncut render: that is what makes error bounded by the
// chunk length rather than the position in the score.
TEST(RenderKernel, AChunkCutAndReanchoredAgreesWithAnUncutRender) {
    const Anchor a = anchor(1.5, 0.07, -3e-6, -12.0, -0.01);
    const std::vector<SampleValue> whole = renderOne(a, 1000);

    BlockState block{1};
    std::vector<SampleValue> split(1000, 0);
    const std::vector<std::size_t> active{0};
    anchorPartial(block, 0, a);
    renderChunk(block, active, SampleIndex{0}, SampleIndex{400}, kEdges, split);
    Anchor second = a;
    second.tau = SampleDuration{400.0};
    anchorPartial(block, 0, second);
    renderChunk(block, active, SampleIndex{400}, SampleIndex{1000}, kEdges, split);

    for (std::size_t n = 0; n < 1000; ++n) {
        EXPECT_NEAR(split[n], whole[n], 1e-6) << "sample " << n;
    }
}

TEST(RenderKernel, RollOffSilencesAPartialOutsideTheRenderingRange) {
    const std::vector<SampleValue> above = renderOne(anchor(0.0, 3.0, 0.0, 0.0, 0.0), 100);
    const std::vector<SampleValue> negative = renderOne(anchor(0.0, -0.1, 0.0, 0.0, 0.0), 100);
    for (std::size_t n = 0; n < 100; ++n) {
        EXPECT_EQ(above[n], 0.0F);
        EXPECT_EQ(negative[n], 0.0F);
    }
    // Inside the high band the partial is attenuated, not silent.
    const double inBand = 0.5 * (kRollOff.highInner.value() + kRollOff.highOuter.value());
    const std::vector<SampleValue> band = renderOne(anchor(1.0, inBand, 0.0, 0.0, 0.0), 100);
    EXPECT_NEAR(band[0], std::sin(1.0) * toLinear(Decibels{-72.0}).value(), 1e-6);
}

TEST(RenderKernel, InactiveLanesAreNeitherReadNorWritten) {
    BlockState block{2};
    anchorPartial(block, 0, anchor(0.5, 0.1, 0.0, 0.0, 0.0));
    anchorPartial(block, 1, anchor(1.0, 0.2, 0.0, 0.0, 0.0));
    const BlockState before = block;

    std::vector<SampleValue> out(10, 0);
    const std::vector<std::size_t> active{1};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{10}, kEdges, out);

    EXPECT_EQ(block.phase[0], before.phase[0]);
    EXPECT_NE(block.phase[1], before.phase[1]);
    EXPECT_NEAR(out[0], std::sin(1.0), 1e-6);
}

TEST(RenderKernel, AddsIntoTheBufferRatherThanOverwriting) {
    std::vector<SampleValue> out(10, 1.0F);
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.0, 0.1, 0.0, 0.0, 0.0));
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{2}, SampleIndex{5}, kEdges, out);
    EXPECT_EQ(out[0], 1.0F);
    EXPECT_EQ(out[1], 1.0F);
    EXPECT_NEAR(out[2], 1.0 + std::sin(0.0), 1e-6);
    EXPECT_NEAR(out[3], 1.0 + std::sin(0.1), 1e-6);
    EXPECT_EQ(out[5], 1.0F);
}

// The model does not bound frequency; a partial far above Nyquist is
// silenced by the roll-off but its phase must still stay wrapped.
TEST(RenderKernel, LargeIncrementsStayWrapped) {
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.0, 10.0, 0.0, 0.0, 0.0));
    std::vector<SampleValue> out(1000, 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{1000}, kEdges, out);
    EXPECT_LE(std::abs(block.phase[0]), std::numbers::pi);
    EXPECT_NEAR(block.phase[0], wrap(Phase{10000.0}).value(), 1e-6);
    EXPECT_EQ(block.cappedPhaseSteps[0], 0) << "10 rad per sample is far inside the cap";
}

// A step just inside the cap is taken, and needs about a thousand passes of
// the wrap loop; nothing is counted.
TEST(RenderKernel, AStepAtTheCapIsTaken) {
    const double step = kMaxPhaseStep - 1.0;
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.0, step, 0.0, 0.0, 0.0));
    std::vector<SampleValue> out(3, 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{3}, kEdges, out);
    EXPECT_EQ(block.cappedPhaseSteps[0], 0);
    EXPECT_LE(std::abs(block.phase[0]), std::numbers::pi);
    EXPECT_NEAR(block.phase[0], wrap(Phase{3.0 * step}).value(), 1e-8);
}

// Beyond the cap the phase holds and every sample is counted (author,
// 2026-10-11). The output stays finite and is silenced by the roll-off.
// Before the cap this frequency made the wrap loop run for ever.
TEST(RenderKernel, AStepBeyondTheCapHoldsThePhaseAndIsCounted) {
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.25, 1.0e20, 0.0, 0.0, 0.0));
    const double anchored = block.phase[0];
    std::vector<SampleValue> out(100, 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{100}, kEdges, out);
    EXPECT_EQ(block.phase[0], anchored);
    EXPECT_EQ(block.cappedPhaseSteps[0], 100);
    for (const SampleValue sample : out) {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_LT(std::abs(sample), 1e-6);
    }
}

// A step that is not a number is held in the same way, so that a NaN never
// reaches the phase.
TEST(RenderKernel, AStepThatIsNotANumberIsHeld) {
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.25, 0.1, 0.0, 0.0, 0.0));
    block.phaseStepOffset[0] = std::numeric_limits<double>::quiet_NaN();
    std::vector<SampleValue> out(10, 0);
    const std::vector<std::size_t> active{0};
    renderChunk(block, active, SampleIndex{0}, SampleIndex{10}, kEdges, out);
    EXPECT_EQ(block.phase[0], 0.25);
    EXPECT_EQ(block.cappedPhaseSteps[0], 10);
}

// An anchor whose closed-form phase overflows starts the lane at 0 and is
// counted, and the count is not reset by the next anchor.
TEST(RenderKernel, AnAnchorPhaseThatIsNotFiniteStartsAtZero) {
    BlockState block{1};
    anchorPartial(block, 0, anchor(0.0, 1.0e300, 0.0, 0.0, 0.0, 1.0e300));
    EXPECT_EQ(block.phase[0], 0.0);
    EXPECT_EQ(block.cappedPhaseSteps[0], 1);

    anchorPartial(block, 0, anchor(0.5, 0.1, 0.0, 0.0, 0.0));
    EXPECT_EQ(block.phase[0], 0.5);
    EXPECT_EQ(block.cappedPhaseSteps[0], 1);
}

} // namespace
