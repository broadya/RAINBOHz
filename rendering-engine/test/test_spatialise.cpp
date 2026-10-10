// SPDX-License-Identifier: Apache-2.0

// Tests for the spatial stage: the spatial kernel, a source's spatial
// segments and cuts, and the driver that spreads a source's mono sum over the
// output channels (docs/rendering.md section 8.5).
//
// Expected values come from positionGains() on positionAt(), evaluated
// directly at each sample: the typed reference the raw kernel must agree
// with. The tolerance allows for the narrowing to float at the add.

#include "rainbohz/rendering_engine/channel_mode.hpp"
#include "rainbohz/rendering_engine/frame_plan.hpp"
#include "rainbohz/rendering_engine/render_kernel.hpp"
#include "rainbohz/rendering_engine/spatial_gains.hpp"
#include "rainbohz/rendering_engine/spatial_maths.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace {

using rainbohz::rendering_engine::AngleGradient;
using rainbohz::rendering_engine::Azimuth;
using rainbohz::rendering_engine::AzimuthPoint;
using rainbohz::rendering_engine::buildSpatialSegments;
using rainbohz::rendering_engine::channelCount;
using rainbohz::rendering_engine::ChannelGains;
using rainbohz::rendering_engine::ChannelMode;
using rainbohz::rendering_engine::DistanceGradient;
using rainbohz::rendering_engine::DistancePoint;
using rainbohz::rendering_engine::Elevation;
using rainbohz::rendering_engine::ElevationPoint;
using rainbohz::rendering_engine::FrameNumber;
using rainbohz::rendering_engine::FrameSize;
using rainbohz::rendering_engine::Metres;
using rainbohz::rendering_engine::NominalPosition;
using rainbohz::rendering_engine::planSoundSource;
using rainbohz::rendering_engine::positionAt;
using rainbohz::rendering_engine::positionGains;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleIndex;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::SampleValue;
using rainbohz::rendering_engine::SoundSourcePlan;
using rainbohz::rendering_engine::SourceData;
using rainbohz::rendering_engine::SpatialAnchor;
using rainbohz::rendering_engine::spatialiseChunk;
using rainbohz::rendering_engine::spatialiseSource;
using rainbohz::rendering_engine::SpatialPosition;
using rainbohz::rendering_engine::SpatialSegment;

constexpr double kPi = std::numbers::pi;

/// A mono test signal: a slow sine, so that no sample is zero by accident.
///
/// @param size  Number of samples.
/// @return The samples.
std::vector<SampleValue> monoSignal(std::size_t size) {
    std::vector<SampleValue> mono(size);
    for (std::size_t n = 0; n < size; ++n) {
        mono[n] = static_cast<SampleValue>(0.5 * std::sin((0.01 * static_cast<double>(n)) + 0.3));
    }
    return mono;
}

/// A source that moves on every axis: round the back, upward and away.
const NominalPosition kMoving{.startAzimuth = Azimuth{3.0},
    .azimuthGradient = AngleGradient{0.001},
    .startElevation = Elevation{-0.2},
    .elevationGradient = AngleGradient{0.0005},
    .startDistance = Metres{1.5},
    .distanceGradient = DistanceGradient{0.01}};

/// Checks every channel of `interleaved` against the typed reference.
///
/// @param mode         The channel mode.
/// @param interleaved  The kernel's output.
/// @param mono         Its input.
/// @param positionOf   The source's position at a sample.
template <typename PositionOf>
void expectReference(ChannelMode mode, const std::vector<SampleValue>& interleaved,
    const std::vector<SampleValue>& mono, PositionOf positionOf) {
    const std::size_t channels = channelCount(mode);
    ASSERT_EQ(interleaved.size(), mono.size() * channels);
    for (std::size_t n = 0; n < mono.size(); ++n) {
        const ChannelGains gains = positionGains(mode, positionOf(n));
        for (std::size_t c = 0; c < channels; ++c) {
            ASSERT_NEAR(interleaved[(n * channels) + c],
                gains.gains[c].value() * static_cast<double>(mono[n]), 1e-7)
                << "sample " << n << ", channel " << c;
        }
    }
}

// ---------------------------------------------------------------------------
// The kernel
// ---------------------------------------------------------------------------

TEST(SpatialiseChunk, InMonoAtOneMetreTheOutputIsTheInput) {
    const std::vector<SampleValue> mono = monoSignal(64);
    std::vector<SampleValue> out(64, 0);
    const SpatialAnchor still{.position = NominalPosition{.startAzimuth = Azimuth{1.0},
                                  .azimuthGradient = AngleGradient{0.0},
                                  .startElevation = Elevation{0.4},
                                  .elevationGradient = AngleGradient{0.0},
                                  .startDistance = Metres{1.0},
                                  .distanceGradient = DistanceGradient{0.0}},
        .tau = SampleDuration{0.0}};
    spatialiseChunk(still, ChannelMode::Mono, mono, SampleIndex{0}, SampleIndex{64}, out);
    EXPECT_EQ(out, mono) << "an omni at 1 m has a gain of exactly 1 in every direction";
}

TEST(SpatialiseChunk, AMovingSourceMatchesTheReferenceInEveryMode) {
    const std::vector<SampleValue> mono = monoSignal(400);
    for (std::int64_t value = 0; value <= 7; ++value) {
        const ChannelMode mode = rainbohz::rendering_engine::channelModeFromInt(value);
        std::vector<SampleValue> out(mono.size() * channelCount(mode), 0);
        spatialiseChunk(SpatialAnchor{.position = kMoving, .tau = SampleDuration{0.0}}, mode, mono,
            SampleIndex{0}, SampleIndex{400}, out);
        expectReference(mode, out, mono, [](std::size_t n) {
            return positionAt(kMoving, SampleDuration{static_cast<double>(n)});
        });
    }
}

TEST(SpatialiseChunk, AddsIntoTheBufferAndTouchesOnlyItsChunk) {
    const std::vector<SampleValue> mono = monoSignal(10);
    std::vector<SampleValue> out(20, 1.0F);
    const SpatialAnchor anchor{.position = kMoving, .tau = SampleDuration{4.0}};
    spatialiseChunk(anchor, ChannelMode::StereoFromXY, mono, SampleIndex{4}, SampleIndex{7}, out);
    for (std::size_t n = 0; n < 10; ++n) {
        const bool inside = n >= 4 && n < 7;
        const ChannelGains gains = positionGains(
            ChannelMode::StereoFromXY, positionAt(kMoving, SampleDuration{static_cast<double>(n)}));
        for (std::size_t c = 0; c < 2; ++c) {
            const double expected =
                1.0 + (inside ? gains.gains[c].value() * static_cast<double>(mono[n]) : 0.0);
            EXPECT_NEAR(out[(n * 2) + c], expected, 1e-6) << "sample " << n << ", channel " << c;
        }
    }
}

TEST(SpatialiseChunk, AChunkSplitAndReAnchoredAgreesWithOneUncut) {
    const std::vector<SampleValue> mono = monoSignal(300);
    std::vector<SampleValue> whole(600, 0);
    std::vector<SampleValue> split(600, 0);
    spatialiseChunk(SpatialAnchor{.position = kMoving, .tau = SampleDuration{0.0}},
        ChannelMode::StereoFromMidSide, mono, SampleIndex{0}, SampleIndex{300}, whole);
    spatialiseChunk(SpatialAnchor{.position = kMoving, .tau = SampleDuration{0.0}},
        ChannelMode::StereoFromMidSide, mono, SampleIndex{0}, SampleIndex{123}, split);
    spatialiseChunk(SpatialAnchor{.position = kMoving, .tau = SampleDuration{123.0}},
        ChannelMode::StereoFromMidSide, mono, SampleIndex{123}, SampleIndex{300}, split);
    EXPECT_EQ(whole, split);
}

// ---------------------------------------------------------------------------
// Spatial segments and the driver
// ---------------------------------------------------------------------------

/// A source with an azimuth sweep across the back inside frame 1 of size
/// 1000, a still elevation that starts before the frame, and a distance that
/// changes at a fractional time and holds after its last point.
SourceData sweepingSource() {
    return SourceData{.soundSourceId = 7,
        .azimuth = {AzimuthPoint{.time = SampleTime{1000.0}, .azimuth = Azimuth{3.0}},
            AzimuthPoint{.time = SampleTime{1400.0}, .azimuth = Azimuth{-3.0}},
            AzimuthPoint{.time = SampleTime{3000.0}, .azimuth = Azimuth{-3.0}}},
        .elevation = {ElevationPoint{.time = SampleTime{0.0}, .elevation = Elevation{0.1}},
            ElevationPoint{.time = SampleTime{5000.0}, .elevation = Elevation{0.1}}},
        .distance = {DistancePoint{.time = SampleTime{500.0}, .distance = Metres{1.0}},
            DistancePoint{.time = SampleTime{1100.5}, .distance = Metres{4.0}},
            DistancePoint{.time = SampleTime{1600.0}, .distance = Metres{2.0}}}};
}

TEST(SpatialSegments, BoundariesAreThePointsInsideTheFrame) {
    const std::vector<SpatialSegment> segments =
        buildSpatialSegments(sweepingSource(), FrameNumber{1}, FrameSize{1000});
    ASSERT_EQ(segments.size(), 4U);
    EXPECT_EQ(segments[0].start.value(), 1000.0);
    EXPECT_EQ(segments[1].start.value(), 1100.5);
    EXPECT_EQ(segments[2].start.value(), 1400.0);
    EXPECT_EQ(segments[3].start.value(), 1600.0);
    EXPECT_EQ(segments[3].end.value(), 2000.0);

    // In the first, distance is re-based to the frame start: 1 + 3·500/600.5.
    EXPECT_NEAR(segments[0].position.startDistance.value(), 1.0 + (3.0 * 500.0 / 600.5), 1e-12);
    EXPECT_DOUBLE_EQ(segments[0].position.startAzimuth.value(), 3.0);
    // After the last distance point, the distance holds.
    EXPECT_DOUBLE_EQ(segments[3].position.startDistance.value(), 2.0);
    EXPECT_DOUBLE_EQ(segments[3].position.distanceGradient.value(), 0.0);
    // The azimuth sweep's second segment starts 100.5 samples in, along the
    // shorter arc.
    const double g = ((2.0 * kPi) - 6.0) / 400.0;
    EXPECT_NEAR(segments[1].position.azimuthGradient.value(), g, 1e-15);
    EXPECT_NEAR(segments[1].position.startAzimuth.value(), 3.0 + (g * 100.5), 1e-12);
}

TEST(SpatialSegments, ATimelineStartingLaterHoldsItsFirstValue) {
    SourceData source = sweepingSource();
    const std::vector<SpatialSegment> segments =
        buildSpatialSegments(source, FrameNumber{0}, FrameSize{1000});
    // Frame 0: the azimuth's first point is at 1000, the frame's end, so it
    // holds 3.0 throughout; distance holds 1 until 500, then ramps.
    ASSERT_EQ(segments.size(), 2U);
    EXPECT_DOUBLE_EQ(segments[0].position.startAzimuth.value(), 3.0);
    EXPECT_DOUBLE_EQ(segments[0].position.azimuthGradient.value(), 0.0);
    EXPECT_DOUBLE_EQ(segments[0].position.startDistance.value(), 1.0);
    EXPECT_DOUBLE_EQ(segments[0].position.distanceGradient.value(), 0.0);
    EXPECT_EQ(segments[1].start.value(), 500.0);
    EXPECT_NEAR(segments[1].position.distanceGradient.value(), 3.0 / 600.5, 1e-15);
}

TEST(SoundSourcePlan, CutsFallOnTheFirstSampleAtOrAfterEachBoundary) {
    const SoundSourcePlan plan = planSoundSource(sweepingSource(), FrameNumber{1}, FrameSize{1000});
    EXPECT_EQ(plan.soundSourceId, 7);
    ASSERT_EQ(plan.cuts.size(), 5U);
    EXPECT_EQ(plan.cuts[0].value(), 0);
    EXPECT_EQ(plan.cuts[1].value(), 101);
    EXPECT_EQ(plan.cuts[2].value(), 400);
    EXPECT_EQ(plan.cuts[3].value(), 600);
    EXPECT_EQ(plan.cuts[4].value(), 1000);
}

TEST(SoundSourcePlan, SpatialiseSourceMatchesTheReferenceAcrossSegments) {
    const SourceData source = sweepingSource();
    const SoundSourcePlan plan = planSoundSource(source, FrameNumber{1}, FrameSize{1000});
    const std::vector<SampleValue> mono = monoSignal(1000);
    std::vector<SampleValue> out(2000, 0);
    spatialiseSource(plan, ChannelMode::StereoFromMidSide, mono, out);

    // The reference: the covering segment's closed form at each sample.
    expectReference(ChannelMode::StereoFromMidSide, out, mono, [&](std::size_t n) {
        const SampleTime t{1000.0 + static_cast<double>(n)};
        std::size_t i = 0;
        while (i + 1 < plan.spatial.size() && plan.spatial[i].end <= t) {
            ++i;
        }
        return positionAt(plan.spatial[i].position, t - plan.spatial[i].start);
    });

    // The sweep passes behind: left (positive S) before, right after.
    const auto side = [&](std::size_t n) {
        return static_cast<double>(out[n * 2]) - static_cast<double>(out[(n * 2) + 1]);
    };
    const auto sign = [&](std::size_t n) { return mono[n] > 0 ? 1.0 : -1.0; };
    EXPECT_GT(side(50) * sign(50), 0.0) << "at 3.0 rad the source is to the left";
    EXPECT_LT(side(700) * sign(700), 0.0) << "at -3.0 rad the source is to the right";
}

} // namespace
