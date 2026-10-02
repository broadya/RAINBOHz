// Tests for the tempo map: the segment formulas, the conversion of ticks to
// seconds, and every validity rule of docs/transform.md section 6.1.

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
using rainbohz::rendering_engine::transform::TicksPerBeat;

constexpr TicksPerBeat kResolution{960};

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
        const TempoMap map{resolution, std::move(points)};
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
                         MusicalDuration{1920}, kResolution)
                         .value(),
        1.0);
}

TEST(SecondsThrough, ARampIsLinearInMusicalPosition) {
    // Four beats ramping 120 -> 60: 60 * 4 * ln(0.5) / (60 - 120) = 4 ln 2.
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution).value(),
        4.0 * std::numbers::ln2);
    // The whole ramp takes longer than holding 120 (2 s) and less than
    // holding 60 (4 s).
    EXPECT_GT(secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution).value(), 2.0);
    EXPECT_LT(secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution).value(), 4.0);
}

TEST(SecondsThrough, PartWayThroughARampUsesTheTempoReached) {
    // Half way through 120 -> 60 over four beats the tempo is 90:
    // 60 * 4 * ln(90 / 120) / (60 - 120) = -4 ln 0.75.
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{1920}, kResolution)
                         .value(),
        -4.0 * std::log(0.75));
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{120.0}, Tempo{60.0}, MusicalDuration{3840},
                         MusicalDuration{0}, kResolution)
                         .value(),
        0.0);
}

TEST(SecondsThrough, ARisingRampIsTheMirrorOfAFallingOne) {
    const MusicalDuration length{3840};
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{60.0}, Tempo{120.0}, length, length, kResolution).value(),
        secondsThrough(Tempo{120.0}, Tempo{60.0}, length, length, kResolution).value());
}

TEST(SecondsThrough, AFlatRampIsAHold) {
    EXPECT_DOUBLE_EQ(secondsThrough(Tempo{100.0}, Tempo{100.0}, MusicalDuration{960},
                         MusicalDuration{960}, kResolution)
                         .value(),
        0.6);
}

// ---------------------------------------------------------------------------
// TempoMap
// ---------------------------------------------------------------------------

TEST(TempoMap, OnePointHoldsForever) {
    const TempoMap map{kResolution, {at(0, 120.0, true)}};
    EXPECT_EQ(map.toSeconds(MusicalTime{0}), SecondsTime{0.0});
    EXPECT_EQ(map.toSeconds(MusicalTime{960}), SecondsTime{0.5});
    EXPECT_EQ(map.toSeconds(MusicalTime{std::int64_t{960} * 120 * 60}), SecondsTime{3600.0});
}

TEST(TempoMap, AccumulatesSecondsAcrossSegments) {
    // Ramp 120 -> 60 over four beats, then hold 60.
    const TempoMap map{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}};
    const double ramp = 4.0 * std::numbers::ln2;
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840}).value(), ramp);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1920}).value(), -4.0 * std::log(0.75));
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840 + 960}).value(), ramp + 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840 + 9600}).value(), ramp + 10.0);
}

TEST(TempoMap, AHeldSegmentIgnoresTheNextTempo) {
    const TempoMap map{kResolution, {at(0, 120.0, true), at(1920, 60.0, true)}};
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{960}).value(), 0.5);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1920}).value(), 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{2880}).value(), 2.0);
}

TEST(TempoMap, SixtyBpmMakesTicksSubdivisionsOfASecond) {
    const TempoMap map{TicksPerBeat{48000}, {at(0, 60.0, true)}};
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{48000}).value(), 1.0);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{1}).value(), 1.0 / 48000.0);
}

TEST(TempoMap, RefusesANegativePosition) {
    const TempoMap map{kResolution, {at(0, 120.0, true)}};
    EXPECT_THROW(static_cast<void>(map.toSeconds(MusicalTime{-1})), std::invalid_argument);
}

TEST(TempoMap, ExposesItsPointsAndResolution) {
    const TempoMap map{kResolution, {at(0, 120.0, false), at(3840, 60.0, true)}};
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
    const TempoMap edges{kResolution, {at(0, 1.0, false), at(1, 1000.0, true)}};
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
    const TempoMap map = TempoMap::fromRecords(
        960, std::vector<TempoMapPointRecord>{record(0, 120.0, false), record(3840, 60.0, true)});
    EXPECT_EQ(map.ticksPerBeat(), kResolution);
    EXPECT_DOUBLE_EQ(map.toSeconds(MusicalTime{3840}).value(), 4.0 * std::numbers::ln2);
}

TEST(TempoMapFromRecords, RefusesNulls) {
    const std::vector<TempoMapPointRecord> sound{record(0, 120.0, true)};
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(std::nullopt, sound)), TempoMapError);
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(0, sound)), TempoMapError);

    std::vector<TempoMapPointRecord> records{record(0, 120.0, true)};
    records[0].position = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records)), TempoMapError);
    records = {record(0, 120.0, true)};
    records[0].startTempo = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records)), TempoMapError);
    records = {record(0, 120.0, true)};
    records[0].hold = std::nullopt;
    EXPECT_THROW(static_cast<void>(TempoMap::fromRecords(960, records)), TempoMapError);
}

} // namespace
