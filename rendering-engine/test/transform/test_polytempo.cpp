// SPDX-License-Identifier: Apache-2.0

// Tests for polytempo: the resolution of synchronised tempo maps to physical
// time, case by case as docs/polytempo.md section 5 lays them out, with that
// document's worked numbers; the validity rules of section 6; and the
// ordering of the graph of maps (section 7).

#include "rainbohz/rendering_engine/transform/polytempo.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using rainbohz::rendering_engine::SecondsTime;
using rainbohz::rendering_engine::transform::MusicalTime;
using rainbohz::rendering_engine::transform::orderTempoMaps;
using rainbohz::rendering_engine::transform::ResolvedSegment;
using rainbohz::rendering_engine::transform::ResolvedTempoMap;
using rainbohz::rendering_engine::transform::resolveSynchronised;
using rainbohz::rendering_engine::transform::resolveTempoMapGraph;
using rainbohz::rendering_engine::transform::SyncAnchor;
using rainbohz::rendering_engine::transform::SyncEdgeRecord;
using rainbohz::rendering_engine::transform::Tempo;
using rainbohz::rendering_engine::transform::TempoMap;
using rainbohz::rendering_engine::transform::TempoMapError;
using rainbohz::rendering_engine::transform::TempoMapOrder;
using rainbohz::rendering_engine::transform::TempoMapPoint;
using rainbohz::rendering_engine::transform::TempoRamp;
using rainbohz::rendering_engine::transform::TicksPerBeat;

constexpr TicksPerBeat kResolution{960};
constexpr TempoRamp kTime = TempoRamp::PhysicalTime;
constexpr TempoRamp kPosition = TempoRamp::MusicalPosition;

/// Ticks of beat `b` at 960 ticks per beat.
///
/// @param b  A beat count; may be fractional.
/// @return Ticks.
constexpr std::int64_t beat(double b) {
    return static_cast<std::int64_t>(b * 960.0);
}

/// A point for a map built in a test.
///
/// @param b      Beats.
/// @param tempo  BPM.
/// @param hold   Whether the tempo holds to the next point.
/// @return The point.
TempoMapPoint at(double b, double tempo, bool hold) {
    return TempoMapPoint{.position = MusicalTime{beat(b)}, .tempo = Tempo{tempo}, .hold = hold};
}

/// A map at 960 ticks per beat.
///
/// @param points  Its points.
/// @param ramp    The ramp shape; physical time by default.
/// @return The validated map.
TempoMap mapOf(std::vector<TempoMapPoint> points, TempoRamp ramp = kTime) {
    return TempoMap{kResolution, std::move(points), ramp};
}

/// A sync point.
///
/// @param b        The child's beat.
/// @param seconds  Its anchor.
/// @return The anchor.
SyncAnchor sync(double b, double seconds) {
    return SyncAnchor{.childTick = MusicalTime{beat(b)}, .anchor = SecondsTime{seconds}};
}

/// Seconds at a beat of a resolved map.
///
/// @param map  The map.
/// @param b    Beats.
/// @return Seconds.
double secondsAt(const ResolvedTempoMap& map, double b) {
    return map.toSeconds(MusicalTime{beat(b)}).value();
}

/// The segment of a resolved map that contains a beat.
///
/// @param map  The map.
/// @param b    Beats.
/// @return The segment.
const ResolvedSegment& segmentAt(const ResolvedTempoMap& map, double b) {
    const auto ticks = static_cast<double>(beat(b));
    const auto& segments = map.segments();
    std::size_t index = 0;
    while (index + 1 < segments.size() && segments[index + 1].startTicks <= ticks) {
        ++index;
    }
    return segments[index];
}

/// The message of the TempoMapError that resolving throws.
///
/// @param map      The map.
/// @param anchors  Its sync points.
/// @return The message; fails the test and returns "" if nothing is thrown.
std::string rejection(const TempoMap& map, std::vector<SyncAnchor> anchors) {
    try {
        static_cast<void>(resolveSynchronised(map, anchors));
    } catch (const TempoMapError& e) {
        return e.what();
    }
    ADD_FAILURE() << "the map was accepted";
    return "";
}

/// Three points at beats 0, 4 and 8, flat at 120 BPM, with ramps or holds.
///
/// @param hold  The hold flag of the first two points.
/// @return The points; the last holds.
std::vector<TempoMapPoint> threePoints(bool hold) {
    return {at(0, 120.0, hold), at(4, 120.0, hold), at(8, 120.0, true)};
}

/// Five points at beats 0, 2, 4, 6 and 8, flat at 120 BPM with ramps: the
/// example of docs/polytempo.md section 5.6.
std::vector<TempoMapPoint> fivePoints() {
    return {at(0, 120.0, false), at(2, 120.0, false), at(4, 120.0, false), at(6, 120.0, false),
        at(8, 120.0, true)};
}

// ---------------------------------------------------------------------------
// ResolvedTempoMap::nominal (§5.1)
// ---------------------------------------------------------------------------

TEST(ResolvedTempoMap, NominalAgreesWithTempoMapBitForBit) {
    for (const TempoRamp ramp : {kTime, kPosition}) {
        const TempoMap map = mapOf(
            {at(0, 120.0, false), at(4, 60.0, true), at(6, 90.0, false), at(10, 45.0, true)}, ramp);
        const ResolvedTempoMap resolved = ResolvedTempoMap::nominal(map);
        ASSERT_EQ(resolved.segments().size(), 4U);
        EXPECT_TRUE(std::isinf(resolved.segments().back().lengthTicks));
        for (std::int64_t tick = 0; tick <= beat(12); tick += 7) {
            EXPECT_EQ(resolved.toSeconds(MusicalTime{tick}).value(),
                map.toSeconds(MusicalTime{tick}).value())
                << "tick " << tick;
        }
    }
}

TEST(ResolvedTempoMap, RefusesANegativePosition) {
    const ResolvedTempoMap map = ResolvedTempoMap::nominal(mapOf(threePoints(true)));
    EXPECT_THROW(static_cast<void>(map.toSeconds(MusicalTime{-1})), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// One sync point (§5.2)
// ---------------------------------------------------------------------------

TEST(Polytempo, OneSyncPointShiftsTheMap) {
    // Held at 90, beat 0 tied to 4 s: beat 3 is at 6 s.
    const ResolvedTempoMap map = resolveSynchronised(mapOf({at(0, 90.0, true)}), {{sync(0, 4.0)}});
    EXPECT_DOUBLE_EQ(secondsAt(map, 0), 4.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 3), 6.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).startTempo.value(), 90.0);
}

TEST(Polytempo, AShiftKeepsRampsAndMayStartBeforeTimeZero) {
    // 120 -> 60 over four beats, then a hold; beat 4 (8/3 s nominally) tied
    // to 1 s puts beat 0 at -5/3 s (§10: allowed, the squash rule handles it).
    const TempoMap nominal = mapOf({at(0, 120.0, false), at(4, 60.0, true)});
    const ResolvedTempoMap map = resolveSynchronised(nominal, {{sync(4, 1.0)}});
    EXPECT_DOUBLE_EQ(secondsAt(map, 4), 1.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 0), 1.0 - (8.0 / 3.0));
    EXPECT_NEAR(secondsAt(map, 2) - secondsAt(map, 0),
        nominal.toSeconds(MusicalTime{beat(2)}).value(), 1e-12);
}

// ---------------------------------------------------------------------------
// Two sync points: one region (§5.3)
// ---------------------------------------------------------------------------

TEST(Polytempo, TwoSyncPointsStretchTheRegionUniformly) {
    // Held at 120, beats 0 and 8 anchored 3.2 s apart: k = 1.25, 150 BPM.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, true), at(8, 120.0, true)}), {{sync(0, 0.0), sync(8, 3.2)}});
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).startTempo.value(), 150.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 4), 1.6);
    EXPECT_EQ(secondsAt(map, 8), 3.2);
    // After the last sync point the map is nominal: a hold at 120.
    EXPECT_DOUBLE_EQ(segmentAt(map, 9).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 10), 4.2);
}

TEST(Polytempo, AUniformStretchScalesARampsTempi) {
    // 120 -> 60 ramp over 4 beats takes 8/3 s; anchored at 4/3 s, k = 2.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, false), at(4, 60.0, true)}), {{sync(0, 0.0), sync(4, 4.0 / 3.0)}});
    const ResolvedSegment& ramp = segmentAt(map, 1);
    EXPECT_NEAR(ramp.startTempo.value(), 240.0, 1e-9);
    EXPECT_NEAR(ramp.endTempo.value(), 120.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Several regions (§5.5 to §5.8)
// ---------------------------------------------------------------------------

TEST(Polytempo, HoldsStepAtAnInteriorSyncPoint) {
    // §5.5: holds at 0, 4, 8 anchored 0, 2, 3 s: 120 then 240, a step.
    const ResolvedTempoMap map =
        resolveSynchronised(mapOf(threePoints(true)), {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    EXPECT_DOUBLE_EQ(segmentAt(map, 2).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 6).startTempo.value(), 240.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 6), 2.5);
    EXPECT_EQ(secondsAt(map, 4), 2.0);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
}

TEST(Polytempo, RampsAreCompensatedWithTheGeometricMeanByPhysicalTime) {
    // §5.6's table, physical time: flat at 106.19 to beat 2, ramp to 169.71
    // at beat 4, ramp to 266.66 at beat 6, flat to beat 8.
    const ResolvedTempoMap map =
        resolveSynchronised(mapOf(fivePoints()), {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    const double shared = 120.0 * std::numbers::sqrt2;
    EXPECT_NEAR(segmentAt(map, 1).startTempo.value(), 106.18594701533962, 1e-9);
    EXPECT_NEAR(segmentAt(map, 1).endTempo.value(), 106.18594701533962, 1e-9);
    EXPECT_NEAR(segmentAt(map, 3).endTempo.value(), shared, 1e-9);
    EXPECT_NEAR(segmentAt(map, 5).startTempo.value(), shared, 1e-9);
    EXPECT_NEAR(segmentAt(map, 5).endTempo.value(), 266.66296498048774, 1e-9);
    EXPECT_NEAR(segmentAt(map, 7).startTempo.value(), 266.66296498048774, 1e-9);
    EXPECT_NEAR(secondsAt(map, 2), 1.1300930431280591, 1e-12);
    EXPECT_NEAR(secondsAt(map, 6), 2.549993753317861, 1e-12);
    // The anchors are hit exactly, and the map is nominal afterwards.
    EXPECT_EQ(secondsAt(map, 0), 0.0);
    EXPECT_EQ(secondsAt(map, 4), 2.0);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 10), 4.0);
}

TEST(Polytempo, RampsAreCompensatedWithTheGeometricMeanByMusicalPosition) {
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf(fivePoints(), kPosition), {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    EXPECT_NEAR(segmentAt(map, 1).startTempo.value(), 107.29221065346967, 1e-9);
    EXPECT_NEAR(segmentAt(map, 3).endTempo.value(), 120.0 * std::numbers::sqrt2, 1e-9);
    EXPECT_NEAR(segmentAt(map, 5).endTempo.value(), 270.01339201337044, 1e-9);
    EXPECT_NEAR(secondsAt(map, 2), 1.1184409312580361, 1e-12);
    EXPECT_EQ(secondsAt(map, 4), 2.0);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
}

TEST(Polytempo, AHoldOnOneSideSetsTheSharedTempo) {
    // §5.7: region 1 is a single hold (60·4/2 = 120); region 2 has an
    // interior point and starts with a ramp, which must start at 120.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, true), at(4, 120.0, false), at(6, 120.0, false), at(8, 120.0, true)}),
        {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    EXPECT_DOUBLE_EQ(segmentAt(map, 2).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 5).startTempo.value(), 120.0);
    const double k = segmentAt(map, 7).startTempo.value() / 120.0;
    EXPECT_NEAR(segmentAt(map, 5).endTempo.value(), k * 120.0, 1e-9);
    // 2 beats ramping 120 -> k·120, then 2 beats at k·120, in exactly 1 s.
    EXPECT_NEAR((240.0 / (120.0 + (k * 120.0))) + (120.0 / (k * 120.0)), 1.0, 1e-12);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
}

TEST(Polytempo, AHoldDependencyIsResolvedInEitherDirection) {
    // The mirror of the previous case: the hold is the later region.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, false), at(2, 120.0, false), at(4, 120.0, true), at(8, 120.0, true)}),
        {{sync(0, 0.0), sync(4, 1.0), sync(8, 3.0)}});
    EXPECT_DOUBLE_EQ(segmentAt(map, 5).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 3).endTempo.value(), 120.0);
    EXPECT_EQ(secondsAt(map, 4), 1.0);
}

// ---------------------------------------------------------------------------
// Single-ramp regions (§5.9 to §5.11)
// ---------------------------------------------------------------------------

TEST(Polytempo, AnEdgeSingleRampHasAFreeOuterEnd) {
    // §5.9: 70.29 -> 169.71 -> 310.29.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf(threePoints(false)), {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    EXPECT_NEAR(segmentAt(map, 1).startTempo.value(), 70.29437251522859, 1e-9);
    EXPECT_NEAR(segmentAt(map, 1).endTempo.value(), 169.7056274847714, 1e-9);
    EXPECT_NEAR(segmentAt(map, 5).startTempo.value(), 169.7056274847714, 1e-9);
    EXPECT_NEAR(segmentAt(map, 5).endTempo.value(), 310.2943725152286, 1e-9);
    EXPECT_EQ(secondsAt(map, 4), 2.0);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
}

TEST(Polytempo, AnEdgeSingleRampIsSolvedByMusicalPosition) {
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf(threePoints(false), kPosition), {{sync(0, 0.0), sync(4, 2.0), sync(8, 3.0)}});
    const ResolvedSegment& first = segmentAt(map, 1);
    // 60·B·ln(U_i/U_o)/(U_i − U_o) = 2 s with U_i = 169.71.
    const double outer = first.startTempo.value();
    const double inner = first.endTempo.value();
    EXPECT_NEAR(inner, 120.0 * std::numbers::sqrt2, 1e-9);
    EXPECT_NEAR(60.0 * 4.0 * std::log(inner / outer) / (inner - outer), 2.0, 1e-12);
    EXPECT_EQ(secondsAt(map, 8), 3.0);
}

TEST(Polytempo, AnEdgeSingleRampOutOfRangeIsRejected) {
    // §5.9: anchored at 0, 2 and 10 s, region 2 would end at 0 BPM.
    const std::string message =
        rejection(mapOf(threePoints(false)), {sync(0, 0.0), sync(4, 2.0), sync(8, 10.0)});
    EXPECT_NE(message.find("[1, 1000]"), std::string::npos) << message;
}

TEST(Polytempo, AnInteriorSingleRampExtendsAFixedTempoAtTheSlowEnd) {
    // §5.10's example: 100 -> 140 over 2 s needing 3.5 beats. The neighbours
    // are holds (100 at 60·2/1.2 BPM, 140 at 60·2/(6/7) BPM), so only a
    // fixed-tempo period is available: h = 1.5 s at the slow end, 2.5 beats.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf(
            {at(0, 100.0, true), at(2, 100.0, false), at(5.5, 140.0, true), at(7.5, 140.0, true)}),
        {{sync(0, 0.0), sync(2, 1.2), sync(5.5, 3.2), sync(7.5, 3.2 + (6.0 / 7.0))}});
    const ResolvedSegment& held = segmentAt(map, 3);
    EXPECT_DOUBLE_EQ(held.startTempo.value(), 100.0);
    EXPECT_DOUBLE_EQ(held.endTempo.value(), 100.0);
    EXPECT_NEAR(held.lengthTicks, 2.5 * 960.0, 1e-9);
    const ResolvedSegment& ramp = segmentAt(map, 5);
    EXPECT_NEAR(ramp.startTicks, 4.5 * 960.0, 1e-9);
    EXPECT_DOUBLE_EQ(ramp.startTempo.value(), 100.0);
    EXPECT_NEAR(ramp.endTempo.value(), 140.0, 1e-9);
    EXPECT_NEAR(secondsAt(map, 4.5), 2.7, 1e-9);
    EXPECT_EQ(secondsAt(map, 5.5), 3.2);
}

TEST(Polytempo, AnInteriorSingleRampContinuesTheNeighboursGradient) {
    // Region 1 (beats 0–4) is an edge ramp falling into the shared tempo
    // 120·√0.8 = 107.33 from 132.67, so g_L = −12.67 BPM/s. Region 2 (beats
    // 4–8, 2.5 s) must end at the hold's 160 BPM: a fixed tempo at the slow
    // end would need h = 3.58 s > D, so the gradient is continued instead,
    // for h = 2.2329 s, to a virtual point at 79.04 BPM and tick 7169.31.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, false), at(4, 120.0, false), at(8, 120.0, true), at(12, 120.0, true)}),
        {{sync(0, 0.0), sync(4, 2.0), sync(8, 4.5), sync(12, 6.0)}});
    const ResolvedSegment& before = segmentAt(map, 2);
    const ResolvedSegment& extension = segmentAt(map, 5);
    const ResolvedSegment& ramp = segmentAt(map, 7.9);
    EXPECT_NEAR(before.endTempo.value(), 107.3312629199899, 1e-9);
    EXPECT_NEAR(extension.startTempo.value(), 107.3312629199899, 1e-9);
    EXPECT_NEAR(extension.endTempo.value(), 79.04262753452461, 1e-9);
    EXPECT_NEAR(ramp.startTicks, 7169.306148918331, 1e-6);
    EXPECT_NEAR(ramp.startSeconds.value(), 4.232948336271159, 1e-12);
    EXPECT_DOUBLE_EQ(ramp.endTempo.value(), 160.0);
    // The gradient is continuous across the sync point at beat 4.
    const double gradientBefore = (before.endTempo.value() - before.startTempo.value()) / 2.0;
    const double gradientAfter = (extension.endTempo.value() - extension.startTempo.value()) /
                                 (extension.startSeconds.value() - ramp.startSeconds.value()) *
                                 -1.0;
    EXPECT_NEAR(gradientBefore, gradientAfter, 1e-9);
    EXPECT_EQ(secondsAt(map, 8), 4.5);
    EXPECT_EQ(secondsAt(map, 12), 6.0);
}

TEST(Polytempo, AdjacentSingleRampsAreSolvedInOrder) {
    // §5.11: two single-ramp regions share beat 4, at 120·√2 = 169.71 BPM.
    // The holds either side are forced to 80 and 300 BPM. Region 1 (80 →
    // 169.71 in 1 s) runs too fast and extends a fixed 80 at its start, the
    // only candidate it may use; region 2 (169.71 → 300 in 0.5 s) runs too
    // slow and extends the hold's 300 at its end. Every anchor is hit.
    const ResolvedTempoMap map =
        resolveSynchronised(mapOf({at(0, 120.0, true), at(2, 120.0, false), at(4, 120.0, false),
                                at(6, 120.0, true), at(8, 120.0, true)}),
            {{sync(0, 0.0), sync(2, 1.5), sync(4, 2.5), sync(6, 3.0), sync(8, 3.4)}});
    EXPECT_EQ(secondsAt(map, 2), 1.5);
    EXPECT_EQ(secondsAt(map, 4), 2.5);
    EXPECT_EQ(secondsAt(map, 6), 3.0);
    EXPECT_EQ(secondsAt(map, 8), 3.4);
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).startTempo.value(), 80.0);
    EXPECT_NEAR(segmentAt(map, 3.9).endTempo.value(), 120.0 * std::numbers::sqrt2, 1e-9);
    EXPECT_NEAR(segmentAt(map, 7).startTempo.value(), 300.0, 1e-9);
    // Seven segments: hold, extension, ramp, ramp, extension, hold, and the
    // nominal hold after the span.
    EXPECT_EQ(map.segments().size(), 7U);
    // Every segment's tempi are continuous through the synced span.
    const auto& segments = map.segments();
    for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
        EXPECT_NEAR(segments[i - 1].endTempo.value(), segments[i].startTempo.value(), 1e-9)
            << "at tick " << segments[i].startTicks;
    }
}

TEST(Polytempo, EqualEndTempiBetweenHoldsAreRejectedWithoutTolerance) {
    // Holds at 120 on both sides of a flat ramp whose anchors want 2.5 s for
    // 4 beats: nothing can move, so it is rejected (author, 2026-10-17).
    const TempoMap map =
        mapOf({at(0, 120.0, true), at(2, 120.0, false), at(6, 120.0, true), at(8, 120.0, true)});
    const std::string message =
        rejection(map, {sync(0, 0.0), sync(2, 1.0), sync(6, 3.5), sync(8, 4.5)});
    EXPECT_NE(message.find("nothing can move"), std::string::npos) << message;
    // Off by the last few bits as well: no tolerance.
    EXPECT_NE(
        rejection(map, {sync(0, 0.0), sync(2, 1.0), sync(6, 3.0 + 1e-13), sync(8, 4.0 + 1e-13)})
            .find("cannot be fitted"),
        std::string::npos);
    // An exact fit is a plain ramp.
    const ResolvedTempoMap fits =
        resolveSynchronised(map, {{sync(0, 0.0), sync(2, 1.0), sync(6, 3.0), sync(8, 4.0)}});
    EXPECT_DOUBLE_EQ(segmentAt(fits, 4).startTempo.value(), 120.0);
}

// ---------------------------------------------------------------------------
// Outside the synced span (§5.4)
// ---------------------------------------------------------------------------

TEST(Polytempo, ARampIntoTheFirstSyncPointArrivesAtTheSyncedTempo) {
    // Beats 4 to 8 held, anchored 10 to 11 s: 240 BPM. The ramp from beat 0
    // (nominally 60 -> 120) now goes 60 -> 240 in 120·4/300 = 1.6 s, so
    // beat 0 is at 8.4 s. After beat 8 a hold gives a hard transition back
    // to the nominal 120, then the nominal ramp 120 -> 60 over beats 12–16.
    const ResolvedTempoMap map =
        resolveSynchronised(mapOf({at(0, 60.0, false), at(4, 120.0, true), at(8, 120.0, true),
                                at(12, 120.0, false), at(16, 60.0, true)}),
            {{sync(4, 10.0), sync(8, 11.0)}});
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).startTempo.value(), 60.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).endTempo.value(), 240.0);
    EXPECT_NEAR(secondsAt(map, 0), 8.4, 1e-12);
    EXPECT_EQ(secondsAt(map, 4), 10.0);
    EXPECT_EQ(secondsAt(map, 8), 11.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 9).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 12), 13.0);
    EXPECT_NEAR(secondsAt(map, 16), 13.0 + (8.0 / 3.0), 1e-12);
}

TEST(Polytempo, ARampOutOfTheLastSyncPointLeavesFromTheSyncedTempo) {
    // Beats 4 to 8 held, anchored 1 to 2 s: 240 BPM. The ramp from beat 8
    // (nominally 120 -> 60) now goes 240 -> 60 in 1.6 s. The hold before
    // beat 4 is outside the span and nominal, so beat 0 is a second before
    // time zero.
    const ResolvedTempoMap map = resolveSynchronised(
        mapOf({at(0, 120.0, true), at(4, 120.0, true), at(8, 120.0, false), at(12, 60.0, true)}),
        {{sync(4, 1.0), sync(8, 2.0)}});
    const ResolvedSegment& ramp = segmentAt(map, 9);
    EXPECT_DOUBLE_EQ(ramp.startTempo.value(), 240.0);
    EXPECT_DOUBLE_EQ(ramp.endTempo.value(), 60.0);
    EXPECT_DOUBLE_EQ(segmentAt(map, 1).startTempo.value(), 120.0);
    EXPECT_DOUBLE_EQ(secondsAt(map, 0), -1.0);
    EXPECT_EQ(secondsAt(map, 8), 2.0);
    EXPECT_NEAR(secondsAt(map, 12), 3.6, 1e-12);
    EXPECT_NEAR(secondsAt(map, 16), 7.6, 1e-12);
}

// ---------------------------------------------------------------------------
// Validity (§6)
// ---------------------------------------------------------------------------

TEST(Polytempo, RejectsAnchorsThatDoNotIncrease) {
    const std::string message =
        rejection(mapOf(threePoints(true)), {sync(0, 0.0), sync(4, 2.0), sync(8, 1.5)});
    EXPECT_NE(message.find("strictly increase"), std::string::npos) << message;
    EXPECT_NE(rejection(mapOf(threePoints(true)), {sync(0, 0.0), sync(4, 0.0)}).find("strictly"),
        std::string::npos);
}

TEST(Polytempo, RejectsASyncPointOffTheMapsPoints) {
    const std::string message = rejection(mapOf(threePoints(true)),
        {SyncAnchor{.childTick = MusicalTime{1000}, .anchor = SecondsTime{}}});
    EXPECT_NE(message.find("not the position"), std::string::npos) << message;
}

TEST(Polytempo, RejectsTwoSyncPointsAtOneTick) {
    EXPECT_NE(rejection(mapOf(threePoints(true)), {sync(4, 1.0), sync(4, 2.0)})
                  .find("different TempoMapPoint"),
        std::string::npos);
}

TEST(Polytempo, RejectsNoSyncPoints) {
    EXPECT_THROW(
        static_cast<void>(resolveSynchronised(mapOf(threePoints(true)), {})), TempoMapError);
}

TEST(Polytempo, RejectsAResolvedTempoOutOfRange) {
    // Four held beats in 0.2 s is 1200 BPM.
    const std::string message =
        rejection(mapOf(threePoints(true)), {sync(0, 0.0), sync(4, 2.0), sync(8, 2.2)});
    EXPECT_NE(message.find("would be 1199.99"), std::string::npos) << message;
    EXPECT_NE(message.find("[1, 1000]"), std::string::npos) << message;
}

TEST(Polytempo, RejectsAPinnedStretchThatCannotFillItsTime) {
    // Region 2 (beats 4–8, one interior point) has both ends pinned by
    // geometric means; by physical time its duration stays finite as k → 0,
    // so 20 s cannot be filled.
    const std::string message =
        rejection(mapOf({at(0, 120.0, false), at(4, 120.0, false), at(6, 120.0, false),
                      at(8, 120.0, false), at(12, 120.0, true)}),
            {sync(0, 0.0), sync(4, 2.0), sync(8, 22.0), sync(12, 24.0)});
    EXPECT_NE(message.find("cannot be stretched"), std::string::npos) << message;
}

// ---------------------------------------------------------------------------
// The graph (§4, §7, §5.12)
// ---------------------------------------------------------------------------

/// An edge with both ticks set.
///
/// @param child       Child TempoMap.id.
/// @param parent      Parent TempoMap.id.
/// @param childTick   sync_point_child.
/// @param parentTick  sync_point_parent.
/// @return The record.
SyncEdgeRecord edge(
    std::int64_t child, std::int64_t parent, std::int64_t childTick, std::int64_t parentTick) {
    return SyncEdgeRecord{.childId = child,
        .parentId = parent,
        .syncPointChild = childTick,
        .syncPointParent = parentTick};
}

TEST(TempoMapGraph, OrdersParentsFirstFromTheRoots) {
    // 1 is the root of Score 7; 2 and 3 are its children; 4 is a child of
    // both 2 and 3 (§5.12).
    const std::vector<std::int64_t> ids{4, 3, 2, 1};
    const std::vector<SyncEdgeRecord> edges{
        edge(4, 3, 0, 0), edge(2, 1, 0, 960), edge(4, 2, 960, 0), edge(3, 1, 0, 1920)};
    const std::vector<std::pair<std::int64_t, std::int64_t>> roots{{1, 7}};
    const TempoMapOrder order = orderTempoMaps(ids, edges, roots);
    EXPECT_EQ(order.order, (std::vector<std::int64_t>{1, 2, 3, 4}));
    for (const std::int64_t id : ids) {
        EXPECT_EQ(order.scoreOfMap.at(id), 7);
    }
}

TEST(TempoMapGraph, RejectsCyclesOrphansAndSharedMaps) {
    const std::vector<std::int64_t> ids{1, 2, 3};
    const auto message =
        [&](std::vector<SyncEdgeRecord> edges,
            std::vector<std::pair<std::int64_t, std::int64_t>> theRoots = {{1, 7}}) {
            try {
                static_cast<void>(orderTempoMaps(ids, edges, theRoots));
            } catch (const TempoMapError& e) {
                return std::string{e.what()};
            }
            ADD_FAILURE() << "the graph was accepted";
            return std::string{};
        };
    // A cycle between 2 and 3, reached from nowhere.
    EXPECT_NE(message({edge(2, 3, 0, 0), edge(3, 2, 0, 0)}).find("cycle"), std::string::npos);
    // 3 has no edge and no Score.
    EXPECT_NE(message({edge(2, 1, 0, 0)}).find("not the TempoMap of any Score"), std::string::npos);
    // The root has a parent.
    EXPECT_NE(message({edge(1, 2, 0, 0), edge(2, 1, 0, 0), edge(3, 1, 0, 0)}).find("root"),
        std::string::npos);
    // An edge names a map that does not exist.
    EXPECT_NE(
        message({edge(2, 1, 0, 0), edge(3, 9, 0, 0)}).find("does not exist"), std::string::npos);
    // 3 is reached from two Scores.
    EXPECT_NE(message({edge(3, 1, 0, 0), edge(3, 2, 0, 0)}, {{1, 7}, {2, 8}}).find("both Score"),
        std::string::npos);
}

TEST(TempoMapGraph, ResolvesChildrenAgainstResolvedParents) {
    // Root 1 holds 120. Child 2 holds 120 and is stretched: beats 0 and 8 to
    // root beats 0 and 6 (3 s), so 160 BPM. Grandchild 3 holds 60 and its
    // beat 0 is tied to beat 4 of child 2, which is 1.5 s, not the nominal 2.
    std::unordered_map<std::int64_t, TempoMap> maps;
    maps.emplace(1, mapOf({at(0, 120.0, true)}));
    maps.emplace(2, mapOf({at(0, 120.0, true), at(8, 120.0, true)}));
    maps.emplace(3, mapOf({at(0, 60.0, true)}));
    const std::vector<SyncEdgeRecord> edges{
        edge(2, 1, 0, 0), edge(2, 1, beat(8), beat(6)), edge(3, 2, 0, beat(4))};
    const std::vector<std::int64_t> ids{1, 2, 3};
    const TempoMapOrder order = orderTempoMaps(ids, edges, {{{1, 7}}});
    const auto resolved = resolveTempoMapGraph(maps, edges, order);
    EXPECT_DOUBLE_EQ(segmentAt(resolved.at(2), 1).startTempo.value(), 160.0);
    EXPECT_DOUBLE_EQ(secondsAt(resolved.at(3), 0), 1.5);
    EXPECT_DOUBLE_EQ(secondsAt(resolved.at(3), 1), 2.5);
    // The root is nominal, bit for bit.
    EXPECT_EQ(secondsAt(resolved.at(1), 5), maps.at(1).toSeconds(MusicalTime{beat(5)}).value());
}

TEST(TempoMapGraph, RejectsNullOrNegativeEdgeTicksNamingTheMap) {
    std::unordered_map<std::int64_t, TempoMap> maps;
    maps.emplace(1, mapOf({at(0, 120.0, true)}));
    maps.emplace(2, mapOf({at(0, 120.0, true)}));
    const std::vector<std::int64_t> ids{1, 2};
    const auto message = [&](const SyncEdgeRecord& bad) {
        const std::vector<SyncEdgeRecord> edges{bad};
        try {
            static_cast<void>(
                resolveTempoMapGraph(maps, edges, orderTempoMaps(ids, edges, {{{1, 7}}})));
        } catch (const TempoMapError& e) {
            return std::string{e.what()};
        }
        ADD_FAILURE() << "the edge was accepted";
        return std::string{};
    };
    EXPECT_NE(
        message(
            SyncEdgeRecord{
                .childId = 2, .parentId = 1, .syncPointChild = std::nullopt, .syncPointParent = 0})
            .find("TempoMap 2: its synchronisation edge to TempoMap 1 has a NULL sync_point_child"),
        std::string::npos);
    EXPECT_NE(
        message(
            SyncEdgeRecord{
                .childId = 2, .parentId = 1, .syncPointChild = 0, .syncPointParent = std::nullopt})
            .find("NULL sync_point_parent"),
        std::string::npos);
    EXPECT_NE(message(edge(2, 1, 0, -1)).find("must be >= 0"), std::string::npos);
    // A failure inside the resolution names the map too.
    EXPECT_NE(
        message(edge(2, 1, 5, 0)).find("TempoMap 2: sync point at tick 5"), std::string::npos);
}

} // namespace
