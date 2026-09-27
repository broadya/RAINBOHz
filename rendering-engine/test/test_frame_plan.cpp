// Tests for the preparation layer: render segments, chunk cuts and the
// per-set driver, over in-memory data with no database.
//
// The partials are the ones of docs/rendering.md section 12, built through
// precomputation's own pure stages so that the segments and start phases are
// exactly what engine-db would hold.

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/frame_plan.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using rainbohz::rendering_engine::AmplitudePoint;
using rainbohz::rendering_engine::buildRenderSegments;
using rainbohz::rendering_engine::buildSegments;
using rainbohz::rendering_engine::chunkCuts;
using rainbohz::rendering_engine::Decibels;
using rainbohz::rendering_engine::FrameNumber;
using rainbohz::rendering_engine::FrameSize;
using rainbohz::rendering_engine::FrequencyPoint;
using rainbohz::rendering_engine::FrequencyPointRecord;
using rainbohz::rendering_engine::gatherBoundaries;
using rainbohz::rendering_engine::integrate;
using rainbohz::rendering_engine::makeRollOff;
using rainbohz::rendering_engine::PartialData;
using rainbohz::rendering_engine::PartialPlan;
using rainbohz::rendering_engine::PartialSetPlan;
using rainbohz::rendering_engine::planPartialSet;
using rainbohz::rendering_engine::renderPartialSet;
using rainbohz::rendering_engine::RenderSegment;
using rainbohz::rendering_engine::SampleIndex;
using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::SampleValue;
using rainbohz::rendering_engine::sourceTimeAt;
using rainbohz::rendering_engine::validateFrequencyEnvelope;

const FrameSize kFrameSize{48000};
const FrameNumber kFrame3{3};

/// Builds a partial's data from its two point lists, through precomputation's
/// pure stages.
///
/// @param frequency  (time, radians per sample) pairs, at least two.
/// @param amplitude  (time, dB) pairs, at least two, spanning the same times.
/// @param id         Partial.id.
/// @return The data, with segments integrated from phase 0.
PartialData partial(const std::vector<std::pair<double, double>>& frequency,
    const std::vector<std::pair<double, double>>& amplitude, std::int64_t id = 0) {
    std::vector<FrequencyPointRecord> records;
    records.reserve(frequency.size());
    for (const auto& [time, value] : frequency) {
        records.push_back(
            FrequencyPointRecord{.id = 0, .time = time, .frequency = value, .curveType = 0});
    }
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);
    PartialData data{.partialId = id,
        .partialName = std::nullopt,
        .partialSetId = 0,
        .segments = buildSegments(gatherBoundaries(envelope), envelope),
        .amplitude = {}};
    integrate(data.segments);
    for (const auto& [time, value] : amplitude) {
        data.amplitude.push_back(
            AmplitudePoint{.time = SampleTime{time}, .amplitude = Decibels{value}});
    }
    return data;
}

/// The constant partial of the worked example.
PartialData constantPartial() {
    return partial({{0.0, 0.1}, {480000.0, 0.1}}, {{0.0, -6.0}, {480000.0, -6.0}});
}

/// The second partial of the worked example: starts half a sample into
/// frame 3, glides, fades in.
PartialData midFramePartial() {
    return partial({{144100.5, 0.05}, {146100.5, 0.06}, {300000.5, 0.06}},
        {{144100.5, -60.0}, {145100.5, -6.0}, {290000.5, -6.0}, {300000.5, -90.0}}, 1);
}

// ---------------------------------------------------------------------------
// buildRenderSegments
// ---------------------------------------------------------------------------

TEST(BuildRenderSegments, APartialCoveringTheFrameGivesOnePresentSegment) {
    const std::vector<RenderSegment> segments =
        buildRenderSegments(constantPartial(), kFrame3, kFrameSize);
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_EQ(segments[0].start.value(), 144000.0);
    EXPECT_EQ(segments[0].end.value(), 192000.0);
    EXPECT_TRUE(segments[0].present);
    EXPECT_NEAR(segments[0].startPhase.value(), 14400.0, 1e-9);
    EXPECT_EQ(segments[0].nominal.startFrequency.value(), 0.1);
    EXPECT_EQ(segments[0].nominal.linearGradient->value(), 0.0);
    EXPECT_EQ(segments[0].amplitude.startDecibels.value(), -6.0);
    EXPECT_EQ(segments[0].amplitude.gradient.value(), 0.0);
}

TEST(BuildRenderSegments, APartialStartingInsideTheFrame) {
    const std::vector<RenderSegment> segments =
        buildRenderSegments(midFramePartial(), kFrame3, kFrameSize);
    ASSERT_EQ(segments.size(), 4U);

    EXPECT_EQ(segments[0].start.value(), 144000.0);
    EXPECT_EQ(segments[0].end.value(), 144100.5);
    EXPECT_FALSE(segments[0].present);

    EXPECT_EQ(segments[1].start.value(), 144100.5);
    EXPECT_EQ(segments[1].end.value(), 145100.5);
    EXPECT_TRUE(segments[1].present);
    EXPECT_EQ(segments[1].startPhase.value(), 0.0);
    EXPECT_EQ(segments[1].nominal.startFrequency.value(), 0.05);
    EXPECT_DOUBLE_EQ(segments[1].nominal.linearGradient->value(), 5e-6);
    EXPECT_EQ(segments[1].amplitude.startDecibels.value(), -60.0);
    EXPECT_DOUBLE_EQ(segments[1].amplitude.gradient.value(), 0.054);

    // The amplitude boundary at 145100.5 splits the glide: the frequency
    // segment continues with the same gradient from its interpolated value.
    EXPECT_EQ(segments[2].start.value(), 145100.5);
    EXPECT_EQ(segments[2].end.value(), 146100.5);
    EXPECT_TRUE(segments[2].present);
    EXPECT_DOUBLE_EQ(segments[2].nominal.startFrequency.value(), 0.055);
    EXPECT_DOUBLE_EQ(segments[2].nominal.linearGradient->value(), 5e-6);
    EXPECT_NEAR(segments[2].startPhase.value(), (0.05 * 1000.0) + (0.5 * 5e-6 * 1e6), 1e-9);
    EXPECT_DOUBLE_EQ(segments[2].amplitude.startDecibels.value(), -6.0);
    EXPECT_EQ(segments[2].amplitude.gradient.value(), 0.0);

    EXPECT_EQ(segments[3].start.value(), 146100.5);
    EXPECT_EQ(segments[3].end.value(), 192000.0);
    EXPECT_TRUE(segments[3].present);
    EXPECT_DOUBLE_EQ(segments[3].nominal.startFrequency.value(), 0.06);
}

TEST(BuildRenderSegments, APartialEndingInsideTheFrame) {
    const PartialData data =
        partial({{0.0, 0.1}, {150000.0, 0.1}}, {{0.0, -6.0}, {150000.0, -6.0}});
    const std::vector<RenderSegment> segments = buildRenderSegments(data, kFrame3, kFrameSize);
    ASSERT_EQ(segments.size(), 2U);
    EXPECT_TRUE(segments[0].present);
    EXPECT_EQ(segments[0].end.value(), 150000.0);
    EXPECT_FALSE(segments[1].present);
    EXPECT_EQ(segments[1].start.value(), 150000.0);
    EXPECT_EQ(segments[1].end.value(), 192000.0);
}

TEST(BuildRenderSegments, APartialInsideTheFrameAtBothEnds) {
    const PartialData data =
        partial({{144500.0, 0.1}, {150000.0, 0.1}}, {{144500.0, -6.0}, {150000.0, -6.0}});
    const std::vector<RenderSegment> segments = buildRenderSegments(data, kFrame3, kFrameSize);
    ASSERT_EQ(segments.size(), 3U);
    EXPECT_FALSE(segments[0].present);
    EXPECT_TRUE(segments[1].present);
    EXPECT_FALSE(segments[2].present);
}

TEST(BuildRenderSegments, ABoundaryExactlyOnTheFrameEdgeIsNotACut) {
    // An amplitude point exactly at the frame's start: the piece from it is
    // what the frame uses, and no extra render segment is made.
    const PartialData data =
        partial({{0.0, 0.1}, {480000.0, 0.1}}, {{0.0, -6.0}, {144000.0, -12.0}, {480000.0, -12.0}});
    const std::vector<RenderSegment> segments = buildRenderSegments(data, kFrame3, kFrameSize);
    ASSERT_EQ(segments.size(), 1U);
    EXPECT_EQ(segments[0].amplitude.startDecibels.value(), -12.0);

    // A partial whose lifeline is exactly the frame.
    const PartialData exact =
        partial({{144000.0, 0.1}, {192000.0, 0.1}}, {{144000.0, -6.0}, {192000.0, -6.0}});
    const std::vector<RenderSegment> one = buildRenderSegments(exact, kFrame3, kFrameSize);
    ASSERT_EQ(one.size(), 1U);
    EXPECT_TRUE(one[0].present);
}

TEST(BuildRenderSegments, RefusesDataThatDoesNotCoverAPresentSegment) {
    PartialData data = constantPartial();
    data.amplitude = {AmplitudePoint{.time = SampleTime{0.0}, .amplitude = Decibels{-6.0}},
        AmplitudePoint{.time = SampleTime{100.0}, .amplitude = Decibels{-6.0}}};
    EXPECT_THROW(buildRenderSegments(data, kFrame3, kFrameSize), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// chunkCuts
// ---------------------------------------------------------------------------

/// A partial plan with render segments starting at the given source times,
/// the last ending at `end`. Presence and parameters do not matter to the
/// cuts.
///
/// @param starts  Segment start times, increasing.
/// @param end     End of the last segment.
/// @return The plan.
PartialPlan segmentsStartingAt(const std::vector<double>& starts, double end) {
    PartialPlan plan{.partialId = 0, .segments = {}};
    for (std::size_t i = 0; i < starts.size(); ++i) {
        plan.segments.push_back(RenderSegment{.start = SampleTime{starts[i]},
            .end = SampleTime{i + 1 < starts.size() ? starts[i + 1] : end},
            .present = true,
            .startPhase = {},
            .nominal = {},
            .amplitude = {}});
    }
    return plan;
}

TEST(ChunkCuts, TwoPartialsWithInterleavedBoundaries) {
    const std::vector<PartialPlan> partials{
        segmentsStartingAt({144000.0, 144010.5, 144020.0}, 192000.0),
        segmentsStartingAt({144000.0, 144015.0}, 192000.0),
    };
    const std::vector<SampleIndex> cuts =
        chunkCuts(partials, SampleTime{144000.0}, 1.0, kFrameSize);
    ASSERT_EQ(cuts.size(), 5U);
    EXPECT_EQ(cuts[0].value(), 0);
    EXPECT_EQ(cuts[1].value(), 11) << "a boundary at 10.5 cuts at the first sample after it";
    EXPECT_EQ(cuts[2].value(), 15);
    EXPECT_EQ(cuts[3].value(), 20) << "a boundary exactly on a sample cuts at that sample";
    EXPECT_EQ(cuts[4].value(), 48000);
}

TEST(ChunkCuts, TheWorkedExample) {
    const std::vector<PartialData> data{constantPartial(), midFramePartial()};
    const PartialSetPlan plan = planPartialSet(0, data, kFrame3, kFrameSize);
    std::vector<std::int32_t> cuts;
    cuts.reserve(plan.cuts.size());
    for (const SampleIndex cut : plan.cuts) {
        cuts.push_back(cut.value());
    }
    EXPECT_EQ(cuts, (std::vector<std::int32_t>{0, 101, 1101, 2101, 48000}));
    EXPECT_EQ(sourceTimeAt(plan, SampleIndex{101}).value(), 144101.0);
}

// ---------------------------------------------------------------------------
// renderPartialSet
// ---------------------------------------------------------------------------

TEST(RenderPartialSet, TheWorkedExample) {
    const std::vector<PartialData> data{constantPartial()};
    const PartialSetPlan plan = planPartialSet(0, data, kFrame3, kFrameSize);
    std::vector<SampleValue> out(48000, 0);
    renderPartialSet(plan, makeRollOff(SampleRate{48000}), out);

    EXPECT_NEAR(out[0], -0.43739, 1e-5);
    EXPECT_NEAR(out[1], -0.41078, 1e-5);
    EXPECT_NEAR(out[2], -0.38006, 1e-5);
    EXPECT_NEAR(out[47999], 0.501187 * std::sin(14400.0 + (0.1 * 47999.0)), 2e-6);
}

TEST(RenderPartialSet, PresenceFollowsTheHalfOpenLifeline) {
    // Alive on [144100, 144200): first sounds at sample 100, last at 199.
    // Phase 0.3 at the start so that the first sample is not a zero crossing.
    PartialData data =
        partial({{144100.0, 0.1}, {144200.0, 0.1}}, {{144100.0, 0.0}, {144200.0, 0.0}});
    data.segments[0].startPhase = rainbohz::rendering_engine::Phase{0.3};
    const std::vector<PartialData> partials{data};
    const PartialSetPlan plan = planPartialSet(0, partials, kFrame3, kFrameSize);
    std::vector<SampleValue> out(48000, 0);
    renderPartialSet(plan, makeRollOff(SampleRate{48000}), out);

    EXPECT_EQ(out[99], 0.0F);
    EXPECT_NEAR(out[100], std::sin(0.3), 1e-6);
    EXPECT_NEAR(out[199], std::sin(0.3 + (0.1 * 99.0)), 1e-6);
    EXPECT_EQ(out[200], 0.0F);
}

TEST(RenderPartialSet, ReanchoringAtEveryCutAgreesWithTheClosedForm) {
    // Two partials: the constant one is re-anchored at the mid-frame
    // partial's cuts, to the value it already holds.
    const std::vector<PartialData> data{constantPartial(), midFramePartial()};
    const PartialSetPlan plan = planPartialSet(0, data, kFrame3, kFrameSize);
    std::vector<SampleValue> out(48000, 0);
    renderPartialSet(plan, makeRollOff(SampleRate{48000}), out);

    for (const std::size_t n : {0U, 100U, 101U, 1100U, 1101U, 2100U, 2101U, 47999U}) {
        const double t = 144000.0 + static_cast<double>(n);
        double expected = 0.501187 * std::sin(14400.0 + (0.1 * static_cast<double>(n)));
        if (t >= 144100.5) {
            const double tau = t - 144100.5;
            const double glide = std::min(tau, 2000.0);
            const double phase = (0.05 * glide) + (0.5 * 5e-6 * glide * glide) +
                                 (0.06 * std::max(tau - 2000.0, 0.0));
            const double decibels = tau < 1000.0 ? -60.0 + (0.054 * tau) : -6.0;
            expected += std::sin(phase) * std::pow(10.0, decibels / 20.0);
        }
        EXPECT_NEAR(out[n], expected, 2e-6) << "sample " << n;
    }
}

} // namespace
