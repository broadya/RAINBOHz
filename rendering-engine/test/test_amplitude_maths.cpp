// Tests for the amplitude maths: the ramp, the dB conversion with its clip
// and floor, and the frequency-range roll-off.
//
// Expected values are derived by hand from docs/rendering.md section 8, and
// the band edges from its table.

#include "rainbohz/rendering_engine/amplitude_maths.hpp"

#include <gtest/gtest.h>

namespace {

using rainbohz::rendering_engine::amplitudeAt;
using rainbohz::rendering_engine::AngularFrequency;
using rainbohz::rendering_engine::DecibelGradient;
using rainbohz::rendering_engine::Decibels;
using rainbohz::rendering_engine::kSilenceFloor;
using rainbohz::rendering_engine::makeRollOff;
using rainbohz::rendering_engine::NominalAmplitude;
using rainbohz::rendering_engine::RollOff;
using rainbohz::rendering_engine::rollOffAt;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::toLinear;

TEST(AmplitudeMaths, TheRampIsLinearInDecibels) {
    // The fade of the worked example: -60 to -6 dB over 1000 samples.
    const NominalAmplitude fade{
        .startDecibels = Decibels{-60.0}, .gradient = DecibelGradient{0.054}};
    EXPECT_DOUBLE_EQ(amplitudeAt(fade, SampleDuration{0.0}).value(), -60.0);
    EXPECT_DOUBLE_EQ(amplitudeAt(fade, SampleDuration{500.0}).value(), -33.0);
    EXPECT_DOUBLE_EQ(amplitudeAt(fade, SampleDuration{1000.0}).value(), -6.0);
    EXPECT_DOUBLE_EQ(amplitudeAt(fade, SampleDuration{0.5}).value(), -59.973);
}

TEST(AmplitudeMaths, GradientFromAChangeOverADuration) {
    const DecibelGradient gradient = (Decibels{-6.0} - Decibels{-60.0}) / SampleDuration{1000.0};
    EXPECT_DOUBLE_EQ(gradient.value(), 0.054);
}

TEST(AmplitudeMaths, ToLinearConverts) {
    EXPECT_NEAR(toLinear(Decibels{-6.0}).value(), 0.501187, 1e-6);
    EXPECT_DOUBLE_EQ(toLinear(Decibels{0.0}).value(), 1.0);
    EXPECT_NEAR(toLinear(Decibels{-20.0}).value(), 0.1, 1e-12);
}

TEST(AmplitudeMaths, ToLinearClipsAboveFullScale) {
    EXPECT_DOUBLE_EQ(toLinear(Decibels{3.0}).value(), 1.0);
    EXPECT_DOUBLE_EQ(toLinear(Decibels{1000.0}).value(), 1.0);
}

TEST(AmplitudeMaths, ToLinearFloorsAtSilence) {
    EXPECT_EQ(toLinear(kSilenceFloor).value(), 0.0);
    EXPECT_EQ(toLinear(Decibels{-200.0}).value(), 0.0);
    // Just above the floor is not silent: the step to zero is one 24-bit LSB.
    EXPECT_GT(toLinear(Decibels{-143.999}).value(), 0.0);
    EXPECT_NEAR(toLinear(Decibels{-143.999}).value(), 6.3e-8, 1e-8);
}

/// Checks one rate's edges against the table of docs/rendering.md section 8.2.
///
/// @param rate       Sample rate.
/// @param lowOuter   Expected f_low,0, radians per sample.
/// @param lowInner   Expected f_low,1.
/// @param highInner  Expected f_high,1.
/// @param highOuter  Expected f_high,0.
void expectEdges(
    std::int32_t rate, double lowOuter, double lowInner, double highInner, double highOuter) {
    const RollOff edges = makeRollOff(SampleRate{rate});
    EXPECT_NEAR(edges.lowOuter.value(), lowOuter, 1e-6) << rate;
    EXPECT_NEAR(edges.lowInner.value(), lowInner, 1e-6) << rate;
    EXPECT_NEAR(edges.highInner.value(), highInner, 1e-4) << rate;
    EXPECT_NEAR(edges.highOuter.value(), highOuter, 1e-4) << rate;
}

TEST(AmplitudeMaths, RollOffEdgesAtTheUsualRates) {
    expectEdges(44100, 0.001425, 0.002850, 2.8495, 2.9956);
    expectEdges(48000, 0.001309, 0.002618, 2.6180, 2.8798);
    // At 96 kHz the upper edge is capped at 30 kHz rather than 34 kHz.
    expectEdges(96000, 0.000654, 0.001309, 1.3090, 1.9635);
}

TEST(AmplitudeMaths, RollOffIsZeroInTheAudioRange) {
    const RollOff edges = makeRollOff(SampleRate{48000});
    EXPECT_EQ(rollOffAt(edges, edges.lowInner).value(), 0.0);
    EXPECT_EQ(rollOffAt(edges, AngularFrequency{0.1}).value(), 0.0);
    EXPECT_EQ(rollOffAt(edges, edges.highInner).value(), 0.0);
}

TEST(AmplitudeMaths, RollOffReachesTheFloorAtTheOuterEdges) {
    const RollOff edges = makeRollOff(SampleRate{48000});
    EXPECT_EQ(rollOffAt(edges, edges.lowOuter), kSilenceFloor);
    EXPECT_EQ(rollOffAt(edges, edges.highOuter), kSilenceFloor);
    EXPECT_EQ(rollOffAt(edges, AngularFrequency{0.0001}), kSilenceFloor);
    EXPECT_EQ(rollOffAt(edges, AngularFrequency{3.0}), kSilenceFloor);
}

TEST(AmplitudeMaths, RollOffIsLinearInDecibelsAcrossEachBand) {
    const RollOff edges = makeRollOff(SampleRate{48000});
    const AngularFrequency lowMid = 0.5 * (edges.lowOuter + edges.lowInner);
    const AngularFrequency highMid = 0.5 * (edges.highInner + edges.highOuter);
    EXPECT_NEAR(rollOffAt(edges, lowMid).value(), -72.0, 1e-9);
    EXPECT_NEAR(rollOffAt(edges, highMid).value(), -72.0, 1e-9);
    // A quarter of the way into the high band: a quarter of the floor.
    const AngularFrequency highQuarter =
        edges.highInner + 0.25 * (edges.highOuter - edges.highInner);
    EXPECT_NEAR(rollOffAt(edges, highQuarter).value(), -36.0, 1e-9);
}

TEST(AmplitudeMaths, NegativeFrequencyIsSilent) {
    const RollOff edges = makeRollOff(SampleRate{48000});
    EXPECT_EQ(rollOffAt(edges, AngularFrequency{-0.1}), kSilenceFloor);
    EXPECT_EQ(rollOffAt(edges, AngularFrequency{0.0}), kSilenceFloor);
}

TEST(AmplitudeMaths, RawAndTypedAgree) {
    const RollOff edges = makeRollOff(SampleRate{44100});
    for (const double f : {-1.0, 0.0, 0.002, 0.5, 2.9, 3.5}) {
        EXPECT_EQ(rollOffAt(edges, AngularFrequency{f}).value(),
            rainbohz::rendering_engine::raw::rollOff(edges.raw(), f));
    }
    for (const double dB : {-200.0, -144.0, -50.0, 0.0, 5.0}) {
        EXPECT_EQ(toLinear(Decibels{dB}).value(), rainbohz::rendering_engine::raw::toLinear(dB));
    }
}

} // namespace
