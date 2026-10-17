// SPDX-License-Identifier: Apache-2.0

// Tests for the tempo map: the segment formulas in both ramp shapes, the
// conversion of ticks to seconds, and every validity rule of
// docs/transform.md section 6.1.

#include "rainbohz/rendering_engine/transform/tempo_map.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rainbohz::rendering_engine::SecondsTime;
using rainbohz::rendering_engine::transform::MusicalDuration;
using rainbohz::rendering_engine::transform::MusicalTime;
using rainbohz::rendering_engine::transform::secondsThrough;
using rainbohz::rendering_engine::transform::Tempo;
using rainbohz::rendering_engine::transform::TempoMap;
using rainbohz::rendering_engine::transform::TempoMapError;
using rainbohz::rendering_engine::transform::TempoMapPoint;
using rainbohz::rendering_engine::transform::TempoMapPointRecord;
using rainbohz::rendering_engine::transform::TempoRamp;
using rainbohz::rendering_engine::transform::tempoRampFromScore;
using rainbohz::rendering_engine::transform::TicksPerBeat;

constexpr TicksPerBeat kResolution{960};

/// The shape of most tests here, whose expected values are the closed forms
/// of a ramp linear in musical position.
constexpr TempoRamp kPosition = TempoRamp::MusicalPosition;

/// The default shape, linear in physical time.
constexpr TempoRamp kTime = TempoRamp::PhysicalTime;

/// A point for a map built in a test.
///
/// @param position  Ticks.
/// @param tempo     BPM.
/// @param hold      Whether the tempo holds to the next point.
/// @return The point.
TempoMapPoint at(std::int64_t position, double tempo, bool hold) {
    return TempoMapPoint{.position = MusicalTime{position}, .tempo = Tempo{tempo}, .hold = hold};
}

/// A record for fromRecords(), with every property set.
///
/// @param position  Ticks.
/// @param tempo     BPM.
/// @param hold      Whether the tempo holds to the next point.
/// @return The record, with id 0.
TempoMapPointRecord record(std::int64_t position, double tempo, bool hold) {
    return TempoMapPointRecord{.id = 0, .position = position, .startTempo = tempo, .hold = hold};
}

/// The message of the TempoMapError that building a map throws.
///
/// @param resolution  Ticks per beat.
/// @param points      The points.
/// @return The message; fails the test and returns "" if nothing is thrown.
std::string rejection(TicksPerBeat resolution, std::vector<TempoMapPoint> points) {
    try {
        const TempoMap map{resolution, std::move(points), TempoRamp::PhysicalTime};
    } catch (const TempoMapError& e) {
        return e.what();
    }
    ADD_FAILURE() << "the map was accepted";
    return "";
}

// ---------------------------------------------------------------------------
// secondsThrough
// ---------------------------------------------------------------------------

TEST(SecondsThrough, AHeldTempoIsBeatsOverTempo) {
    // Two beats at 120 BPM: one second.
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{120.0}, MusicalDuration{0},
                         MusicalDuration{1920}, kResolution, kPosition)
                         .value(),
        1.0);
}

TEST(SecondsThrough, ARampIsLinearInMusicalPosition) {
    // Four beats ramping 120 -> 60: 60 * 4 * ln(0.5) / (60 - 120) = 4 ln 2.
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kPosition).value(),
        4.0 * std::numbers::ln2);
    // The whole ramp takes longer than holding 120 (2 s) and less than
    // holding 60 (4 s).
    EXPECT_GT(
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kPosition).value(),
        2.0);
    EXPECT_LT(
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kPosition).value(),
        4.0);
}

TEST(SecondsThrough, PartWayThroughARampUsesTheTempoReached) {
    // Half way through 120 -> 60 over four beats the tempo is 90:
    // 60 * 4 * ln(90 / 120) / (60 - 120) = -4 ln 0.75.
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{1920}, kResolution, kPosition)
                         .value(),
        -4.0 * std::log(0.75));
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{0}, kResolution, kPosition)
                         .value(),
        0.0);
}

TEST(SecondsThrough, ARisingRampIsTheMirrorOfAFallingOne) {
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(
        secondsThrough(Tempo{60.0}, Tempo{120.0}, length, length, kResolution, kPosition).value(),
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kPosition).value());
}

TEST(SecondsThrough, AFlatRampIsAHold) {
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{100.0}, Tempo{100.0}, MusicalDuration{960},
                         MusicalDuration{960}, kResolution, kPosition)
                         .value(),
        0.6);
}

// ---------------------------------------------------------------------------
// secondsThrough, linear in physical time
// ---------------------------------------------------------------------------

TEST(SecondsThroughPhysicalTime, TheReaperCaseTakesTwoSeconds) {
    // 100 -> 140 BPM over four beats: four beats at the mean tempo of 120,
    // 2 s exactly. REAPER measures 2.000 s; linear in position gives 2.019 s.
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(
        secondsThrough(Tempo{100.0}, Tempo{140.0}, length, length, kResolution, kTime).value(),
        2.0);
    EXPECT_NEAR(
        secondsThrough(Tempo{100.0}, Tempo{140.0}, length, length, kResolution, kPosition).value(),
        2.019, 0.0005);
}

TEST(SecondsThroughPhysicalTime, AWholeRampIsTheBeatsAtTheMeanTempo) {
    // 120 -> 60 over four beats: 120 * 4 / 180 = 8/3 s.
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kTime).value(),
        8.0 / 3.0);
}

TEST(SecondsThroughPhysicalTime, PartWayThroughUsesTheTempoReached) {
    // One beat into 120 -> 60 over four: T^2 = 120^2 - (120^2 - 60^2) / 4 =
    // 11700, and the seconds are 120 / (120 + sqrt(11700)).
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{960}, kResolution, kTime)
                         .value(),
        120.0 / (120.0 + std::sqrt(11700.0)));
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{0}, kResolution, kTime)
                         .value(),
        0.0);
}

TEST(SecondsThroughPhysicalTime, TheTempoIsLinearInTime) {
    // Half way through the ramp's duration (4/3 s of 8/3) the tempo is 90.
    // Over that half it averages (120 + 90) / 2 = 105 BPM, so it covers
    // 4/3 * 105 / 60 = 7/3 beats, 2240 ticks; those must take 4/3 s.
    const double half = 4.0 / 3.0;
    const double beatsAtHalf = half * (120.0 + 90.0) / 120.0;
    const auto ticks = static_cast<std::int64_t>(std::llround(beatsAtHalf * 960.0));
    ASSERT_DOUBLE_EQ(static_cast<double>(ticks), beatsAtHalf * 960.0);
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{ticks}, kResolution, kTime)
                         .value(),
        half);
}

TEST(SecondsThroughPhysicalTime, ARisingRampIsTheMirrorOfAFallingOne) {
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(
        secondsThrough(Tempo{60.0}, Tempo{120.0}, length, length, kResolution, kTime).value(),
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution, kTime).value());
}

TEST(SecondsThroughPhysicalTime, AFlatRampIsAHoldInEitherShape) {
    EXPECT_EQ(secondsThrough(Tempo{100.0}, Tempo{100.0}, MusicalDuration{960}, MusicalDuration{960},
                  kResolution, kTime),
        secondsThrough(Tempo{100.0}, Tempo{100.0}, MusicalDuration{960}, MusicalDuration{960},
            kResolution, kPosition));
}

TEST(SecondsThroughPhysicalTime, ANearlyFlatRampApproachesTheHold) {
    const MusicalDuration length{3840};
    EXPECT_NEAR(
        secondsThrough(Tempo{100.0}, Tempo{100.0 + 1e-9}, length, length, kResolution, kTime)
            .value(),
        2.4, 1e-9);
}

// ---------------------------------------------------------------------------
// The ramp shape setting
// ---------------------------------------------------------------------------

TEST(TempoRampFromScore, NullOrTrueIsPhysicalTime) {
    EXPECT_EQ(tempoRampFromScore(std::nullopt), TempoRamp::PhysicalTime);
    EXPECT_EQ(tempoRampFromScore(true), TempoRamp::PhysicalTime);
    EXPECT_EQ(tempoRampFromScore(false), TempoRamp::MusicalPosition);
}

// ---------------------------------------------------------------------------
// TempoMap
// ---------------------------------------------------------------------------

TEST(TempoMap, OnePointHoldsForever) {
    const TempoMap map{kResolution, {at(0, 120.0, true)}, kPosition};
    EXPECT_EQ(map.toSeconds(MusicalTime{0}), SecondsTime{0.0});
    EXPECT_EQ(map.toSeconds(MusicalTime{960}), SecondsTime{0.5});
    EXPECT_EQ(map.toSeconds(MusicalTime{std::int64_t{960} * 120 * 60}), SecondsTime{3600.0});
}

TEST(TempoMap, AccumulatesSecondsAcrossSegments) {
    // Ramp 120 -> 60 over four beats, then hold 60.
    const TempoMap map{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}, kPosition};
    const double ramp = 4.0 * std::numbers::ln2;
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840}).value(), ramp);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1920}).value(), -4.0 * std::log(0.75));
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840 + 960}).value(), ramp + 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840 + 9600}).value(), ramp + 10.0);
}

TEST(TempoMap, AHeldSegmentIgnoresTheNextTempo) {
    const TempoMap map{kResolution, {at(0, 120.0, true), at(1920, 60.0, true)}, kPosition};
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{960}).value(), 0.5);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1920}).value(), 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{2880}).value(), 2.0);
}

TEST(TempoMap, SixtyBpmMakesTicksSubdivisionsOfASecond) {
    const TempoMap map{TicksPerBeat{48000}, {at(0, 60.0, true)}, kPosition};
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{48000}).value(), 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1}).value(), 1.0 / 48000.0);
}

TEST(TempoMap, ConvertsInItsRampShape) {
    // The examples' map, 120 -> 60 over four beats then held, in both shapes.
    const TempoMap time{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}, kTime};
    const TempoMap position{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}, kPosition};
    EXPECT_EQ(time.ramp(), kTime);
    EXPECT_EQ(position.ramp(), kPosition);
    EXPECT_DOUBLE_EQ(time.toSeconds(MusicalTime{3840}).value(), 8.0 / 3.0);
    EXPECT_DOUBLE_EQ(position.toSeconds(MusicalTime{3840}).value(), 4.0 * std::numbers::ln2);
    EXPECT_NEAR(time.toSeconds(MusicalTime{960}).value(), 0.525932, 5e-7);
    EXPECT_NEAR(time.toSeconds(MusicalTime{1920}).value(), 1.116963, 5e-7);
    EXPECT_NEAR(time.toSeconds(MusicalTime{2880}).value(), 1.805665, 5e-7);
    EXPECT_DOUBLE_EQ(time.toSeconds(MusicalTime{3840 + 960}).value(), (8.0 / 3.0) + 1.0);
}

TEST(TempoMap, HeldTemposAreTheSameInEitherShape) {
    const std::vector<TempoMapPoint> points{at(0, 120.0, true), at(1920, 60.0, true)};
    const TempoMap time{kResolution, points, kTime};
    const TempoMap position{kResolution, points, kPosition};
    for (const std::int64_t tick : {0, 960, 1920, 2880, 100000}) {
        EXPECT_EQ(time.toSeconds(MusicalTime{tick}), position.toSeconds(MusicalTime{tick}));
    }
}

TEST(TempoMap, RefusesANegativePosition) {
    const TempoMap map{kResolution, {at(0, 120.0, true)}, kPosition};
    EXPECT_THROW(static_cast<void>(map.toSeconds(MusicalTime{-1})), std::invalid_argument);
}

TEST(TempoMap, ExposesItsPointsAndResolution) {
    const TempoMap map{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}, kPosition};
    EXPECT_EQ(map.ticksPerBeat(), kResolution);
    ASSERT_EQ(map.points().size(), 2U);
    EXPECT_FALSE(map.points()[0].hold);
    EXPECT_TRUE(map.points()[1].hold);
}

// ---------------------------------------------------------------------------
// Validity rules
// ---------------------------------------------------------------------------

TEST(TempoMapValidity, NeedsAPositiveResolution) {
    EXPECT_NE(
        rejection(TicksPerBeat{0}, {at(0, 120.0, true)}).find("ticks_per_beat"), std::string::npos);
    EXPECT_NE(
        rejection(TicksPerBeat{-960}, {at(0, 120.0, true)}).find("positive"), std::string::npos);
}

TEST(TempoMapValidity, NeedsAtLeastOnePoint) {
    EXPECT_NE(rejection(kResolution, {}).find("at least one"), std::string::npos);
}

TEST(TempoMapValidity, TheFirstPointIsAtZero) {
    EXPECT_NE(rejection(kResolution, {at(960, 120.0, true)}).find("position 0"), std::string::npos);
}

TEST(TempoMapValidity, PositionsStrictlyIncrease) {
    EXPECT_NE(
        rejection(kResolution, {at(0, 120.0, true), at(960, 100.0, true), at(960, 90.0, true)})
            .find("strictly increase"),
        std::string::npos);
    EXPECT_NE(
        rejection(kResolution, {at(0, 120.0, true), at(960, 100.0, true), at(480, 90.0, true)})
            .find("TempoMapPoint 2"),
        std::string::npos);
}

TEST(TempoMapValidity, TemposStayInTheSchemaRange) {
    EXPECT_NE(rejection(kResolution, {at(0, 0.5, true)}).find("start_tempo"), std::string::npos);
    EXPECT_NE(rejection(kResolution, {at(0, 1000.5, true)}).find("[1, 1000]"), std::string::npos);
    EXPECT_NE(
        rejection(kResolution, {at(0, std::nan(""), true)}).find("start_tempo"), std::string::npos);
    const TempoMap edges{kResolution, {at(0, 1.0, false), at(1, 1000.0, true)}, kPosition};
    EXPECT_GT(edges.toSeconds(MusicalTime{1}).value(), 0.0);
}

TEST(TempoMapValidity, TheLastPointMustHold) {
    EXPECT_NE(rejection(kResolution, {at(0, 120.0, false)}).find("must hold"), std::string::npos);
    EXPECT_NE(
        rejection(kResolution, {at(0, 120.0, false), at(960, 60.0, false)}).find("TempoMapPoint 1"),
        std::string::npos);
}

// ---------------------------------------------------------------------------
// fromRecords
// ---------------------------------------------------------------------------

TEST(TempoMapFromRecords, BuildsAValidMap) {
    const TempoMap map = TempoMap::fromRecords(960,
        std::vector<TempoMapPointRecord>{record(0, 120.0, false), record(3840, 60.0, true)},
        kPosition);
    EXPECT_EQ(map.ticksPerBeat(), kResolution);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840}).value(), 4.0 * std::numbers::ln2);
}

TEST(TempoMapFromRecords, KeepsTheRampShape) {
    const TempoMap map = TempoMap::fromRecords(960,
        std::vector<TempoMapPointRecord>{record(0, 120.0, false), record(3840, 60.0, true)},
        tempoRampFromScore(std::nullopt));
    EXPECT_EQ(map.ramp(), kTime);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840}).value(), 8.0 / 3.0);
}

TEST(TempoMapFromRecords, RefusesNulls) {
    const std::vector<TempoMapPointRecord> sound{record(0, 120.0, true)};
    EXPECT_THROW(
        static_cast<void>(TempoMap::fromRecords(std::nullopt, sound, kPosition)), TempoMapError);
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(0, sound, kPosition)), TempoMapError);

    std::vector<TempoMapPointRecord> records{record(0, 120.0, true)};
    records[0].position = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records, kPosition)), TempoMapError);
    records = {record(0, 120.0, true)};
    records[0].startTempo = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records, kPosition)), TempoMapError);
    records = {record(0, 120.0, true)};
    records[0].hold = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records, kPosition)), TempoMapError);
}

} // namespace
