// Tests for the segment maths shared by precomputation and rendering.
//
// Expected values are derived by hand from the closed form
// Φ(τ) = f₀·τ + ½·g·τ² (docs/precomputation.md section 6.5).

#include "rainbohz/rendering_engine/segment_maths.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

using rainbohz::rendering_engine::AngularFrequency;
using rainbohz::rendering_engine::antiderivative;
using rainbohz::rendering_engine::frequencyAt;
using rainbohz::rendering_engine::FrequencyGradient;
using rainbohz::rendering_engine::NominalFrequency;
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::phaseAt;
using rainbohz::rendering_engine::phaseIncrement;
using rainbohz::rendering_engine::SampleDuration;

/// A segment path with a linear component.
///
/// @param startFrequency  f₀, radians per sample.
/// @param gradient        g, radians per sample².
/// @return The path f(τ) = f₀ + g·τ.
NominalFrequency linear(double startFrequency, double gradient) {
    return NominalFrequency{
        .startFrequency = AngularFrequency{startFrequency},
        .linearGradient = FrequencyGradient{gradient},
    };
}

TEST(SegmentMaths, ConstantFrequency) {
    const NominalFrequency nominal = linear(0.1, 0.0);
    EXPECT_DOUBLE_EQ(phaseIncrement(nominal, SampleDuration{1000.0}).value(), 100.0);
    EXPECT_DOUBLE_EQ(frequencyAt(nominal, SampleDuration{1000.0}).value(), 0.1);
}

TEST(SegmentMaths, NoLinearComponentIsConstant) {
    const NominalFrequency nominal{
        .startFrequency = AngularFrequency{0.1}, .linearGradient = std::nullopt};
    EXPECT_DOUBLE_EQ(phaseIncrement(nominal, SampleDuration{1000.0}).value(), 100.0);
    EXPECT_DOUBLE_EQ(frequencyAt(nominal, SampleDuration{1000.0}).value(), 0.1);
}

TEST(SegmentMaths, RisingGlide) {
    // 0.1 → 0.2 over 1000 samples: 0.1·1000 + ½·1e-4·1000² = 100 + 50.
    const NominalFrequency nominal = linear(0.1, 1e-4);
    EXPECT_DOUBLE_EQ(phaseIncrement(nominal, SampleDuration{1000.0}).value(), 150.0);
    EXPECT_DOUBLE_EQ(frequencyAt(nominal, SampleDuration{1000.0}).value(), 0.2);
}

TEST(SegmentMaths, FallingGlideThroughZero) {
    // 0.1 → −0.1 over 1000 samples: the positive and negative halves cancel.
    const NominalFrequency nominal = linear(0.1, -2e-4);
    EXPECT_NEAR(phaseIncrement(nominal, SampleDuration{1000.0}).value(), 0.0, 1e-12);
    EXPECT_DOUBLE_EQ(frequencyAt(nominal, SampleDuration{1000.0}).value(), -0.1);
    // The peak phase is at the zero crossing, τ = 500: 50 − 25.
    EXPECT_DOUBLE_EQ(phaseIncrement(nominal, SampleDuration{500.0}).value(), 25.0);
}

TEST(SegmentMaths, NegativeFrequencyAccumulatesNegativePhase) {
    EXPECT_DOUBLE_EQ(phaseIncrement(linear(-0.2, 0.0), SampleDuration{10.0}).value(), -2.0);
}

TEST(SegmentMaths, FractionalLength) {
    // Time is continuous: 0.5·2.5 + ½·0.1·2.5² = 1.25 + 0.3125.
    EXPECT_DOUBLE_EQ(phaseIncrement(linear(0.5, 0.1), SampleDuration{2.5}).value(), 1.5625);
}

TEST(SegmentMaths, AntiderivativeIsZeroAtTheStart) {
    EXPECT_EQ(antiderivative(linear(0.3, 1e-3), SampleDuration{0.0}).value(), 0.0);
}

TEST(SegmentMaths, PhaseAtAddsToTheStartPhase) {
    const NominalFrequency nominal = linear(0.1, 1e-4);
    const Phase start{1234.5};
    EXPECT_EQ(phaseAt(start, nominal, SampleDuration{0.0}).value(), start.value());
    EXPECT_DOUBLE_EQ(phaseAt(start, nominal, SampleDuration{1000.0}).value(), 1384.5);
}

// Splitting a segment in two, and continuing the path from the split, must
// not change the phase at the end. This is what lets boundaries from other
// inputs subdivide the envelope without disturbing it (§6.3).
TEST(SegmentMaths, SplittingASegmentPreservesPhase) {
    const NominalFrequency whole = linear(0.05, 3e-6);
    const SampleDuration split{12345.25};
    const SampleDuration rest{30000.0};

    const NominalFrequency second{
        .startFrequency = frequencyAt(whole, split),
        .linearGradient = whole.linearGradient,
    };
    const Phase joined = phaseIncrement(whole, split) + phaseIncrement(second, rest);
    EXPECT_NEAR(joined.value(), phaseIncrement(whole, split + rest).value(), 1e-9);
}

// The worked example of docs/precomputation.md section 12.
TEST(SegmentMaths, WorkedExample) {
    const NominalFrequency first = linear(0.1, (0.2 - 0.1) / 48000.0);
    const NominalFrequency second = linear(0.2, (0.05 - 0.2) / 72000.0);
    const Phase atBoundary = phaseIncrement(first, SampleDuration{48000.0});
    EXPECT_NEAR(atBoundary.value(), 7200.0, 1e-9);
    EXPECT_NEAR(phaseAt(atBoundary, second, SampleDuration{72000.0}).value(), 16200.0, 1e-9);
}

} // namespace
