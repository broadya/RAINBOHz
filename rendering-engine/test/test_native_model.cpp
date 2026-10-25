// SPDX-License-Identifier: Apache-2.0

// Tests for the native engine-db model: the schema's rules it keeps on
// write, how it numbers and removes rows, and its census.

#include "rainbohz/rendering_engine/native_model.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using rainbohz::rendering_engine::Census;
using rainbohz::rendering_engine::RowCount;
using rainbohz::rendering_engine::native::censusOf;
using rainbohz::rendering_engine::native::CompensationRow;
using rainbohz::rendering_engine::native::CurvePoint;
using rainbohz::rendering_engine::native::EdgeList;
using rainbohz::rendering_engine::native::FrameEntryRow;
using rainbohz::rendering_engine::native::FrameTables;
using rainbohz::rendering_engine::native::labelNamesOf;
using rainbohz::rendering_engine::native::LabelRow;
using rainbohz::rendering_engine::native::Multiplicity;
using rainbohz::rendering_engine::native::NativeModel;
using rainbohz::rendering_engine::native::NativeModelError;
using rainbohz::rendering_engine::native::PhasePoint;
using rainbohz::rendering_engine::native::PhysicalTimelines;
using rainbohz::rendering_engine::native::reserveFor;
using rainbohz::rendering_engine::native::SegmentTimelines;

/// The count a census gives one table, 0 when it is not listed.
std::int64_t countOf(const std::vector<RowCount>& lines, const std::string& table) {
    for (const auto& [name, count] : lines) {
        if (name == table) {
            return count;
        }
    }
    return 0;
}

/// Three points of a frequency timeline.
std::vector<CurvePoint<double>> threePoints(double start) {
    return {{.time = start, .value = 1.0, .curveType = 0, .curveAmount = 0.0F},
        {.time = start + 10.0, .value = 2.0, .curveType = 0, .curveAmount = 0.0F},
        {.time = start + 20.0, .value = 3.0, .curveType = 0, .curveAmount = 0.0F}};
}

TEST(NativeReserveTest, GrowsGeometricallySoAppendsStayLinear) {
    std::vector<int> rows;
    std::vector<bool> flags;
    int reallocations = 0;
    for (int i = 0; i < 100000; ++i) {
        const std::size_t before = rows.capacity();
        reserveFor(rows, 1);
        reserveFor(flags, 1);
        reallocations += rows.capacity() != before ? 1 : 0;
        rows.push_back(i);
        flags.push_back(true);
    }
    EXPECT_LE(reallocations, 20);
    EXPECT_GE(flags.capacity(), flags.size());
}

TEST(NativeEdgeListTest, OneManyAllowsOneEdgeToEachTarget) {
    EdgeList<> edges{Multiplicity::OneMany};
    edges.add(0, 1);
    edges.add(0, 2);
    EXPECT_THROW(edges.add(3, 1), NativeModelError);
    EXPECT_EQ(edges.size(), 2U);
}

TEST(NativeEdgeListTest, ManyOneAllowsOneEdgeFromEachSource) {
    EdgeList<> edges{Multiplicity::ManyOne};
    edges.add(1, 0);
    edges.add(2, 0);
    EXPECT_THROW(edges.add(1, 5), NativeModelError);
    EXPECT_EQ(edges.size(), 2U);
}

TEST(NativeEdgeListTest, OneOneRefusesBranchesAndMerges) {
    EdgeList<> edges{Multiplicity::OneOne};
    edges.add(0, 1);
    EXPECT_THROW(edges.add(0, 2), NativeModelError);
    EXPECT_THROW(edges.add(3, 1), NativeModelError);
    edges.add(1, 2);
    ASSERT_EQ(edges.size(), 2U);
    EXPECT_EQ(edges.edges()[1].from, 1);
    EXPECT_EQ(edges.edges()[1].to, 2);
}

TEST(NativeEdgeListTest, ManyManyHasNoLimitAndKeepsOrderAndProperties) {
    EdgeList<double> edges{Multiplicity::ManyMany};
    edges.add(0, 1, 0.5);
    edges.add(0, 1, 1.5);
    edges.add(2, 1, 2.5);
    ASSERT_EQ(edges.size(), 3U);
    EXPECT_DOUBLE_EQ(edges.edges()[0].properties, 0.5);
    EXPECT_DOUBLE_EQ(edges.edges()[2].properties, 2.5);
}

TEST(NativePhysicalTimelinesTest, IdsAreRowIndicesAndListsAreRanges) {
    PhysicalTimelines<CurvePoint<double>> timelines;
    const std::vector<CurvePoint<double>> first = threePoints(0.0);
    const std::vector<CurvePoint<double>> second = threePoints(100.0);
    EXPECT_EQ(timelines.append(7, first), 0);
    EXPECT_EQ(timelines.append(9, second), 1);
    ASSERT_EQ(timelines.points().size(), 6U);
    EXPECT_EQ(timelines.pointsOf(1).front().time, 100.0);
    EXPECT_EQ(timelines.timelineOfPoint(4), 1);
    EXPECT_EQ(timelines.timelineOf(9), 1);
    EXPECT_FALSE(timelines.timelineOf(8).has_value());
    EXPECT_EQ(timelines.liveTimelineCount(), 2U);
    EXPECT_EQ(timelines.livePointCount(), 6U);
}

TEST(NativePhysicalTimelinesTest, AnOwnerHasOneLiveTimelineOfAKind) {
    PhysicalTimelines<CurvePoint<double>> timelines;
    const std::vector<CurvePoint<double>> points = threePoints(0.0);
    timelines.append(7, points);
    EXPECT_THROW(timelines.append(7, points), NativeModelError);
    EXPECT_EQ(timelines.points().size(), 3U);
}

TEST(NativePhysicalTimelinesTest, RemovedRowsStayAndTheirIdsAreNotReused) {
    PhysicalTimelines<PhasePoint> timelines;
    const std::vector<PhasePoint> points{{.time = 0.0, .phase = 0.5}, {.time = 4.0, .phase = {}}};
    timelines.append(3, points);
    timelines.remove(0);
    EXPECT_FALSE(timelines.timelineOf(3).has_value());
    EXPECT_FALSE(timelines.isLivePoint(0));
    EXPECT_EQ(timelines.livePointCount(), 0U);
    EXPECT_EQ(timelines.append(3, points), 1);
    EXPECT_TRUE(timelines.isLivePoint(2));
    EXPECT_FALSE(timelines.isLivePoint(1));
    EXPECT_FALSE(timelines.isLivePoint(4));
    EXPECT_FALSE(timelines.isLivePoint(-1));
    EXPECT_EQ(timelines.livePointCount(), 2U);
}

TEST(NativePhysicalTimelinesTest, RemovingAnUnknownOrDeadTimelineIsIgnored) {
    PhysicalTimelines<PhasePoint> timelines;
    timelines.remove(0);
    timelines.remove(-1);
    const std::vector<PhasePoint> points{{.time = 0.0, .phase = 0.0}};
    timelines.append(1, points);
    timelines.remove(0);
    timelines.remove(0);
    EXPECT_EQ(timelines.liveTimelineCount(), 0U);
    EXPECT_EQ(timelines.livePointCount(), 0U);
}

TEST(NativeSegmentTimelinesTest, NominalsAreNumberedInSegmentOrder) {
    SegmentTimelines timelines;
    const std::vector<SegmentTimelines::NewSegment> segments{
        {.startTime = 0.0,
            .endTime = 10.0,
            .startPhase = 0.0,
            .nominal = {.startNominalFrequency = 0.1,
                .linearComponent = false,
                .linearGradient = std::nullopt,
                .curveComponent = false}},
        {.startTime = 10.0,
            .endTime = 20.0,
            .startPhase = 1.0,
            .nominal = {.startNominalFrequency = 0.2,
                .linearComponent = true,
                .linearGradient = 0.01,
                .curveComponent = false}}};
    const std::vector<CompensationRow> compensation{
        {.startTime = 0.0, .endTime = 20.0, .startPhaseDelta = 0.0, .endPhaseDelta = 0.0}};
    EXPECT_EQ(timelines.append(4, segments, compensation), 0);
    EXPECT_EQ(timelines.append(5, segments, compensation), 1);
    ASSERT_EQ(timelines.segments().size(), 4U);
    EXPECT_EQ(timelines.segments()[3].nominal, 3);
    EXPECT_TRUE(timelines.nominals()[3].linearGradient.has_value());
    EXPECT_EQ(timelines.timelineOfSegment(2), 1);
    EXPECT_EQ(timelines.timelineOfCompensation(1), 1);
    EXPECT_THROW(timelines.append(4, segments, compensation), NativeModelError);

    timelines.remove(0);
    EXPECT_FALSE(timelines.isLiveSegment(0));
    EXPECT_TRUE(timelines.isLiveSegment(2));
    EXPECT_FALSE(timelines.isLiveCompensation(0));
    EXPECT_EQ(timelines.liveSegmentCount(), 2U);
    EXPECT_EQ(timelines.liveCompensationCount(), 1U);
    EXPECT_EQ(timelines.append(4, segments, compensation), 2);
}

TEST(NativeFrameTablesTest, AnEntryNeedsItsFrameAndAScopeNeedsEveryFrame) {
    FrameTables frames;
    EXPECT_THROW(frames.appendEntry(FrameEntryRow{.frame = 2}), NativeModelError);
    frames.mergeFrame(2);
    frames.mergeFrame(2);
    frames.mergeFrame(4);
    EXPECT_EQ(frames.frames().size(), 2U);
    EXPECT_THROW(frames.setScope(1, 2, 4), NativeModelError);
    EXPECT_THROW(frames.setScope(1, 4, 2), NativeModelError);
    frames.mergeFrame(3);
    frames.setScope(1, 2, 4);
    frames.setScope(5, 3, 3);
    EXPECT_EQ(frames.scopeEdgeCount(), 4);
    frames.setScope(1, 2, 2);
    EXPECT_EQ(frames.scopeEdgeCount(), 2);
    frames.clearScope(1);
    EXPECT_EQ(frames.scopeEdgeCount(), 1);
}

TEST(NativeFrameTablesTest, EntriesAreFoundByFrameAndPartialAndRemoved) {
    FrameTables frames;
    frames.mergeFrame(0);
    frames.mergeFrame(1);
    EXPECT_EQ(frames.appendEntry(FrameEntryRow{.frame = 0, .partial = 7}), 0);
    EXPECT_EQ(frames.appendEntry(FrameEntryRow{.frame = 1, .partial = 7}), 1);
    EXPECT_EQ(frames.appendEntry(FrameEntryRow{.frame = 0, .partial = 8}), 2);
    EXPECT_EQ(frames.liveEntriesOf(0), (std::vector<std::int64_t>{0, 2}));
    EXPECT_EQ(frames.liveEntriesOfPartial(7), (std::vector<std::int64_t>{0, 1}));
    frames.removeEntry(0);
    frames.removeEntry(0);
    frames.removeEntry(42);
    EXPECT_EQ(frames.liveEntriesOf(0), (std::vector<std::int64_t>{2}));
    EXPECT_TRUE(frames.liveEntriesOf(9).empty());
    EXPECT_EQ(frames.liveEntryCount(), 2U);
}

TEST(NativeCensusTest, CountsAsLadybugWould) {
    NativeModel model;
    model.canonical.partials.resize(2);
    model.canonical.partialStateSets = 3;
    model.canonical.hasPartial.add(0, 0);
    model.canonical.hasPartial.add(0, 1);
    model.canonical.tempoMapNext.add(0, 1);

    const std::vector<CurvePoint<double>> points = threePoints(0.0);
    model.physical.frequency.append(0, points);
    model.physical.frequency.append(1, points);
    model.physical.frequency.remove(1);
    const std::vector<CurvePoint<float>> empty;
    model.physical.distance.append(0, empty);

    const std::vector<SegmentTimelines::NewSegment> segments(2);
    const std::vector<CompensationRow> compensation(1);
    model.segments.append(0, segments, compensation);
    model.frames.mergeFrame(0);
    model.frames.mergeFrame(1);
    model.frames.setScope(0, 0, 1);
    model.frames.appendEntry(FrameEntryRow{.frame = 0, .partial = 0});
    model.frames.appendEntry(FrameEntryRow{.frame = 1, .partial = 0});

    const Census census = censusOf(model);
    EXPECT_EQ(countOf(census.nodes, "Partial"), 2);
    EXPECT_EQ(countOf(census.nodes, "PartialStateSet"), 3);
    EXPECT_EQ(countOf(census.nodes, "FrequencyPhysicalTimeline"), 1);
    EXPECT_EQ(countOf(census.nodes, "FrequencyPhysicalTimelinePoint"), 3);
    EXPECT_EQ(countOf(census.nodes, "DistancePhysicalTimeline"), 1);
    EXPECT_EQ(countOf(census.nodes, "DistancePhysicalTimelinePoint"), 0);
    EXPECT_EQ(countOf(census.nodes, "PartialIntegrableSegment"), 2);
    EXPECT_EQ(countOf(census.nodes, "PartialSegmentNominalFrequency"), 2);
    EXPECT_EQ(countOf(census.nodes, "PartialPhaseCompensationSegment"), 1);
    EXPECT_EQ(countOf(census.nodes, "Frame"), 2);
    EXPECT_EQ(countOf(census.nodes, "FrameEntry"), 2);

    // NEXT_POINT: 1 canonical, 3 frequency points, 2 segments, 1 compensation.
    EXPECT_EQ(countOf(census.relationships, "NEXT_POINT"), 7);
    EXPECT_EQ(countOf(census.relationships, "HAS_PARTIAL"), 2);
    EXPECT_EQ(countOf(census.relationships, "HAS_PHYSICAL_TIMELINE"), 3);
    EXPECT_EQ(countOf(census.relationships, "SEGMENT_TO_FREQUENCY_NOMINAL"), 2);
    EXPECT_EQ(countOf(census.relationships, "PARTIAL_IN_FRAME"), 2);
    EXPECT_EQ(countOf(census.relationships, "FRAME_ENTRY"), 2);
    EXPECT_EQ(countOf(census.relationships, "FRAME_ENTRY_PARTIAL"), 2);
    EXPECT_EQ(countOf(census.relationships, "FRAME_ENTRY_PHYSICAL_TIMELINE"), 12);

    std::int64_t nodes = 0;
    for (const auto& [name, count] : census.nodes) {
        EXPECT_NE(count, 0) << name << " is listed with no rows";
        nodes += count;
    }
    EXPECT_EQ(census.totalNodes, nodes);
    EXPECT_TRUE(std::ranges::is_sorted(census.nodes));
    EXPECT_TRUE(std::ranges::is_sorted(census.relationships));
}

TEST(NativeCensusTest, AnEmptyModelIsEmpty) {
    EXPECT_TRUE(censusOf(NativeModel{}).empty());
}

TEST(NativeModelTest, LabelNamesLeaveOutUnnamedLabels) {
    NativeModel model;
    model.canonical.labels = {LabelRow{.name = "b"}, LabelRow{}, LabelRow{.name = "a"}};
    EXPECT_EQ(labelNamesOf(model), (std::set<std::string>{"a", "b"}));
}

} // namespace
