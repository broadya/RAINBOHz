// Tests for precomputation.
//
// Two halves. The pure stages are tested directly on in-memory data. The
// whole run is then tested against the real engine-db schema, in memory,
// with small scores built here.
//
// These are the tests of the mechanism. The test cases that exercise the
// musical behaviour of whole scores are designed separately.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::AmplitudePointRecord;
using rainbohz::rendering_engine::buildSegments;
using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::frameEntries;
using rainbohz::rendering_engine::FrameEntryTarget;
using rainbohz::rendering_engine::FrameNumber;
using rainbohz::rendering_engine::FrameRange;
using rainbohz::rendering_engine::frameScope;
using rainbohz::rendering_engine::FrameSize;
using rainbohz::rendering_engine::FrequencyPoint;
using rainbohz::rendering_engine::FrequencyPointRecord;
using rainbohz::rendering_engine::gatherBoundaries;
using rainbohz::rendering_engine::integrate;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::optionalDouble;
using rainbohz::rendering_engine::PartialPrecomputationError;
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::PrecomputationError;
using rainbohz::rendering_engine::PrecomputationReport;
using rainbohz::rendering_engine::precomputeAll;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::resolveFrameSize;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::Segment;
using rainbohz::rendering_engine::validateAmplitudeTimes;
using rainbohz::rendering_engine::validateFrequencyEnvelope;
using rainbohz::rendering_engine::walkChain;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A frequency point record with every property set.
///
/// @param time       Time, samples.
/// @param frequency  Frequency, radians per sample.
/// @param curveType  frequency_curve_type; linear by default.
/// @return The record, with id 0.
FrequencyPointRecord record(double time, double frequency, std::int64_t curveType = 0) {
    return FrequencyPointRecord{
        .id = 0, .time = time, .frequency = frequency, .curveType = curveType};
}

/// The three-point envelope of docs/precomputation.md section 12.
///
/// @return 0.1 at 0, 0.2 at 48000, 0.05 at 120000, all linear.
std::vector<FrequencyPoint> workedExampleEnvelope() {
    const std::vector<FrequencyPointRecord> records{
        record(0.0, 0.1), record(48000.0, 0.2), record(120000.0, 0.05)};
    return validateFrequencyEnvelope(records);
}

/// Formats a double as a Cypher literal that reads back exactly.
///
/// @param value  A finite value.
/// @return The value with enough digits to round-trip, always with a decimal
///         point or exponent so that Cypher types it as DOUBLE.
std::string literal(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << std::showpoint << value;
    return out.str();
}

/// One point of a score built by addPartial().
struct ScorePoint {
    /// Time, samples.
    double time = 0.0;
    /// Frequency, radians per sample.
    double frequency = 0.0;
    /// frequency_curve_type.
    int curveType = 0;
};

/// A point for addPartial().
///
/// @param time       Time, samples.
/// @param frequency  Frequency, radians per sample.
/// @param curveType  frequency_curve_type; linear by default.
/// @return The point.
ScorePoint at(double time, double frequency, int curveType = 0) {
    return ScorePoint{.time = time, .frequency = frequency, .curveType = curveType};
}

/// Whether addPartial() gives the partial an AmplitudePhysicalTimeline.
enum class Amplitude : std::uint8_t {
    /// Two points, at the first and last frequency times, both at -6 dB: the
    /// simplest timeline that agrees with the envelope.
    Matching,
    /// No AmplitudePhysicalTimeline at all.
    None,
};

/// Adds a partial with a FrequencyPhysicalTimeline, and normally an AmplitudePhysicalTimeline,
/// to a database.
///
/// @param[in,out] db  Database with the engine schema applied. Gains one
///                    Partial, its FrequencyPhysicalTimeline with one point per entry
///                    of `points`, linked in order, and, unless `amplitude` is
///                    None and `points` is not empty, an AmplitudePhysicalTimeline.
/// @param name        Partial.name. Must be unique in the test.
/// @param points      The frequency timeline's points. May be empty.
/// @param amplitude   Whether to add the amplitude timeline, and what shape.
/// @param amplitudeTimes  When not empty, the amplitude points' times instead
///                    of the matching pair, all at -6 dB.
void addPartial(EngineDb& db, const std::string& name, const std::vector<ScorePoint>& points,
    Amplitude amplitude = Amplitude::Matching, const std::vector<double>& amplitudeTimes = {}) {
    db.execute("CREATE (:Partial {name: '" + name +
               "'})-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline);");
    if (points.empty()) {
        return;
    }
    std::string cypher = "MATCH (:Partial {name: '" + name +
                         "'})-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) CREATE (t)";
    for (const ScorePoint& point : points) {
        cypher += "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: " + literal(point.time) +
                  ", frequency: " + literal(point.frequency) +
                  ", frequency_curve_type: " + std::to_string(point.curveType) + "})";
    }
    db.execute(cypher + ";");

    if (amplitude == Amplitude::None) {
        return;
    }
    std::vector<double> times = amplitudeTimes;
    if (times.empty()) {
        times = {points.front().time, points.back().time};
    }
    cypher =
        "MATCH (p:Partial {name: '" + name +
        "'}) CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) WITH t CREATE (t)";
    for (const double time : times) {
        cypher += "-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: " + literal(time) +
                  ", amplitude: -6.0, amplitude_curve_type: 0})";
    }
    db.execute(cypher + ";");
}

/// Counts the rows of a query that returns one count.
///
/// @param db      Database to query.
/// @param cypher  A query returning a single integer.
/// @return That integer.
std::int64_t count(EngineDb& db, const std::string& cypher) {
    return requireInt64(db.query(cypher).at(0).at(0));
}

/// The start phases a partial's segments were written with, in chain order.
///
/// @param db    Database to query.
/// @param name  Partial.name.
/// @return One start_phase per segment, ordered by start_time.
std::vector<double> startPhases(EngineDb& db, const std::string& name) {
    std::vector<double> phases;
    const std::vector<DbRow> rows = db.query(
        "MATCH (:Partial {name: $name})-[:HAS_PHYSICAL_TIMELINE]->(:PartialSegmentPhysicalTimeline)"
        "-[:NEXT_POINT*1..30]->(s:PartialIntegrableSegment) "
        "RETURN s.start_phase ORDER BY s.start_time;",
        {{"name", name}});
    phases.reserve(rows.size());
    for (const DbRow& row : rows) {
        phases.push_back(optionalDouble(row.at(0)).value());
    }
    return phases;
}

/// An in-memory engine-db with the real schema and one EngineConfig.
class PrecomputationDb : public ::testing::Test {
protected:
    /// Applies the schema and a 48 kHz EngineConfig with no frame size.
    void SetUp() override {
        const auto report = loadCypherFile(db, RE_TEST_SCHEMA_FILE);
        ASSERT_TRUE(report.ok()) << "the engine schema did not load";
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    }

    EngineDb db;
};

// ---------------------------------------------------------------------------
// walkChain
// ---------------------------------------------------------------------------

TEST(WalkChain, FollowsLinksFromTheHead) {
    const std::unordered_map<std::int64_t, std::int64_t> next{{5, 2}, {2, 9}};
    EXPECT_EQ(walkChain(5, next, "list"), (std::vector<std::int64_t>{5, 2, 9}));
}

TEST(WalkChain, EmptyWithoutAHead) {
    EXPECT_TRUE(walkChain(std::nullopt, {}, "list").empty());
}

TEST(WalkChain, RefusesALoop) {
    const std::unordered_map<std::int64_t, std::int64_t> next{{1, 2}, {2, 3}, {3, 1}};
    EXPECT_THROW(walkChain(1, next, "list"), PartialPrecomputationError);
}

// ---------------------------------------------------------------------------
// validateFrequencyEnvelope
// ---------------------------------------------------------------------------

TEST(ValidateFrequencyEnvelope, AcceptsAValidEnvelope) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    ASSERT_EQ(envelope.size(), 3U);
    EXPECT_EQ(envelope[1].time.value(), 48000.0);
    EXPECT_EQ(envelope[2].frequency.value(), 0.05);
}

TEST(ValidateFrequencyEnvelope, NeedsTwoPoints) {
    const std::vector<FrequencyPointRecord> one{record(0.0, 0.1)};
    EXPECT_THROW(validateFrequencyEnvelope(one), PartialPrecomputationError);
    EXPECT_THROW(validateFrequencyEnvelope({}), PartialPrecomputationError);
}

TEST(ValidateFrequencyEnvelope, RefusesNullAndNonFiniteValues) {
    std::vector<FrequencyPointRecord> points{record(0.0, 0.1), record(10.0, 0.1)};
    points[1].time = std::nullopt;
    EXPECT_THROW(validateFrequencyEnvelope(points), PartialPrecomputationError);

    points = {record(0.0, 0.1), record(10.0, std::numeric_limits<double>::quiet_NaN())};
    EXPECT_THROW(validateFrequencyEnvelope(points), PartialPrecomputationError);
}

TEST(ValidateFrequencyEnvelope, RefusesAStartBeforeTheScore) {
    const std::vector<FrequencyPointRecord> points{record(-1.0, 0.1), record(10.0, 0.1)};
    EXPECT_THROW(validateFrequencyEnvelope(points), PartialPrecomputationError);
}

TEST(ValidateFrequencyEnvelope, TimesMustStrictlyIncrease) {
    const std::vector<FrequencyPointRecord> equal{record(0.0, 0.1), record(0.0, 0.2)};
    EXPECT_THROW(validateFrequencyEnvelope(equal), PartialPrecomputationError);

    const std::vector<FrequencyPointRecord> backwards{record(10.0, 0.1), record(5.0, 0.2)};
    EXPECT_THROW(validateFrequencyEnvelope(backwards), PartialPrecomputationError);
}

TEST(ValidateFrequencyEnvelope, AcceptsSegmentsShorterThanOneSample) {
    const std::vector<FrequencyPointRecord> points{record(0.0, 0.1), record(0.25, 0.2)};
    EXPECT_NO_THROW(validateFrequencyEnvelope(points));
}

TEST(ValidateFrequencyEnvelope, OnlyLinearCurvesAreSupported) {
    const std::vector<FrequencyPointRecord> curved{record(0.0, 0.1, 4), record(10.0, 0.2)};
    EXPECT_THROW(validateFrequencyEnvelope(curved), PartialPrecomputationError);

    std::vector<FrequencyPointRecord> unset{record(0.0, 0.1), record(10.0, 0.2)};
    unset[0].curveType = std::nullopt;
    EXPECT_THROW(validateFrequencyEnvelope(unset), PartialPrecomputationError);
}

TEST(ValidateFrequencyEnvelope, TheLastPointsCurveIsIgnored) {
    std::vector<FrequencyPointRecord> points{record(0.0, 0.1), record(10.0, 0.2, 3)};
    EXPECT_NO_THROW(validateFrequencyEnvelope(points));
    points[1].curveType = std::nullopt;
    EXPECT_NO_THROW(validateFrequencyEnvelope(points));
}

// ---------------------------------------------------------------------------
// gatherBoundaries, buildSegments, integrate
// ---------------------------------------------------------------------------

TEST(GatherBoundaries, AreTheEnvelopeTimes) {
    const std::vector<SampleTime> boundaries = gatherBoundaries(workedExampleEnvelope());
    ASSERT_EQ(boundaries.size(), 3U);
    EXPECT_EQ(boundaries[0].value(), 0.0);
    EXPECT_EQ(boundaries[1].value(), 48000.0);
    EXPECT_EQ(boundaries[2].value(), 120000.0);
}

TEST(BuildSegments, OneSegmentPerEnvelopePiece) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    const std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    ASSERT_EQ(segments.size(), 2U);

    EXPECT_EQ(segments[0].startTime.value(), 0.0);
    EXPECT_EQ(segments[0].endTime.value(), 48000.0);
    EXPECT_EQ(segments[1].startTime.value(), 48000.0);
    EXPECT_EQ(segments[1].endTime.value(), 120000.0);

    // At an envelope point the start frequency is the point's own value,
    // exactly, not a computed one.
    EXPECT_EQ(segments[0].nominal.startFrequency.value(), 0.1);
    EXPECT_EQ(segments[1].nominal.startFrequency.value(), 0.2);

    ASSERT_TRUE(segments[0].nominal.linearGradient.has_value());
    EXPECT_DOUBLE_EQ(segments[0].nominal.linearGradient->value(), 0.1 / 48000.0);
    EXPECT_DOUBLE_EQ(segments[1].nominal.linearGradient->value(), -0.15 / 72000.0);
}

TEST(BuildSegments, AFlatSegmentStillHasALinearComponent) {
    const std::vector<FrequencyPointRecord> records{record(0.0, 0.1), record(100.0, 0.1)};
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);
    const std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    ASSERT_TRUE(segments.at(0).nominal.linearGradient.has_value());
    EXPECT_EQ(segments.at(0).nominal.linearGradient->value(), 0.0);
}

TEST(BuildSegments, InterpolatesABoundaryInsideAPiece) {
    // Not reachable from the envelope alone in this version, but it is how
    // boundaries from other inputs will split a piece (§6.4).
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    const std::vector<SampleTime> boundaries{
        SampleTime{0.0}, SampleTime{24000.0}, SampleTime{48000.0}, SampleTime{120000.0}};
    const std::vector<Segment> segments = buildSegments(boundaries, envelope);
    ASSERT_EQ(segments.size(), 3U);
    EXPECT_DOUBLE_EQ(segments[1].nominal.startFrequency.value(), 0.15);
    EXPECT_EQ(segments[1].nominal.linearGradient, segments[0].nominal.linearGradient);
}

TEST(BuildSegments, RefusesBoundariesThatDoNotSpanTheEnvelope) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    const std::vector<SampleTime> truncated{SampleTime{0.0}, SampleTime{48000.0}};
    EXPECT_THROW(buildSegments(truncated, envelope), std::invalid_argument);
}

TEST(Integrate, WorkedExample) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);

    const Phase end = integrate(segments);

    EXPECT_EQ(segments[0].startPhase.value(), 0.0);
    EXPECT_NEAR(segments[1].startPhase.value(), 7200.0, 1e-9);
    EXPECT_NEAR(end.value(), 16200.0, 1e-9);
}

TEST(Integrate, RefusesAPhaseThatIsNotFinite) {
    const double huge = std::numeric_limits<double>::max();
    const std::vector<FrequencyPointRecord> records{record(0.0, huge), record(1e10, huge)};
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    EXPECT_THROW(integrate(segments), PartialPrecomputationError);
}

// ---------------------------------------------------------------------------
// validateAmplitudeTimes, frameEntries
// ---------------------------------------------------------------------------

/// An amplitude point record with its time set.
///
/// @param time  Time, samples.
/// @return The record, with id 0.
AmplitudePointRecord amplitudeRecord(double time) {
    return AmplitudePointRecord{.id = 0, .time = time};
}

TEST(ValidateAmplitudeTimes, AcceptsAPhysicalTimelineSpanningTheEnvelope) {
    const std::vector<AmplitudePointRecord> points{
        amplitudeRecord(0.0), amplitudeRecord(60000.0), amplitudeRecord(120000.0)};
    const std::vector<SampleTime> times = validateAmplitudeTimes(points, workedExampleEnvelope());
    ASSERT_EQ(times.size(), 3U);
    EXPECT_EQ(times[1].value(), 60000.0);
}

TEST(ValidateAmplitudeTimes, InteriorPointsNeedNotMatchFrequencyPoints) {
    const std::vector<AmplitudePointRecord> points{
        amplitudeRecord(0.0), amplitudeRecord(47999.5), amplitudeRecord(120000.0)};
    EXPECT_NO_THROW(validateAmplitudeTimes(points, workedExampleEnvelope()));
}

TEST(ValidateAmplitudeTimes, NeedsTwoPoints) {
    const std::vector<AmplitudePointRecord> one{amplitudeRecord(0.0)};
    EXPECT_THROW(validateAmplitudeTimes(one, workedExampleEnvelope()), PartialPrecomputationError);
}

TEST(ValidateAmplitudeTimes, MustStartAndEndWithTheEnvelope) {
    const std::vector<AmplitudePointRecord> late{amplitudeRecord(1.0), amplitudeRecord(120000.0)};
    EXPECT_THROW(validateAmplitudeTimes(late, workedExampleEnvelope()), PartialPrecomputationError);

    const std::vector<AmplitudePointRecord> shortened{
        amplitudeRecord(0.0), amplitudeRecord(119999.0)};
    EXPECT_THROW(
        validateAmplitudeTimes(shortened, workedExampleEnvelope()), PartialPrecomputationError);
}

TEST(ValidateAmplitudeTimes, TimesMustStrictlyIncreaseAndBeFinite) {
    const std::vector<AmplitudePointRecord> equal{amplitudeRecord(0.0), amplitudeRecord(50.0),
        amplitudeRecord(50.0), amplitudeRecord(120000.0)};
    EXPECT_THROW(
        validateAmplitudeTimes(equal, workedExampleEnvelope()), PartialPrecomputationError);

    std::vector<AmplitudePointRecord> unset{amplitudeRecord(0.0), amplitudeRecord(120000.0)};
    unset[0].time = std::nullopt;
    EXPECT_THROW(
        validateAmplitudeTimes(unset, workedExampleEnvelope()), PartialPrecomputationError);
}

// The worked example of docs/precomputation.md section 5: frames 0, 1 and 2
// enter segments 0, 1 and 1, and every frame's amplitude entry is the point
// at 0 when the only other point is at the end.
TEST(FrameEntries, WorkedExample) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    integrate(segments);
    const std::vector<SampleTime> amplitude{SampleTime{0.0}, SampleTime{120000.0}};

    const std::vector<FrameEntryTarget> entries = frameEntries(segments, amplitude,
        FrameRange{.first = FrameNumber{0}, .last = FrameNumber{2}}, FrameSize{48000});

    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].frame.value(), 0);
    EXPECT_EQ(entries[0].segment, 0U);
    EXPECT_EQ(entries[1].frame.value(), 1);
    EXPECT_EQ(entries[1].segment, 1U) << "frame 1 starts at 48000, inside segment 1";
    EXPECT_EQ(entries[2].segment, 1U);
    EXPECT_EQ(entries[0].point, 0U);
    EXPECT_EQ(entries[1].point, 0U);
    EXPECT_EQ(entries[2].point, 0U);
}

// A partial starting mid-frame enters at its own start, not the frame's; a
// later frame enters the amplitude piece covering its start.
TEST(FrameEntries, EntryTimeIsTheLaterOfLifelineAndFrameStart) {
    const std::vector<FrequencyPointRecord> records{
        record(500.0, 0.1), record(1500.0, 0.2), record(2500.0, 0.1)};
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    const std::vector<SampleTime> amplitude{
        SampleTime{500.0}, SampleTime{1000.0}, SampleTime{2000.0}, SampleTime{2500.0}};

    const std::vector<FrameEntryTarget> entries = frameEntries(segments, amplitude,
        FrameRange{.first = FrameNumber{0}, .last = FrameNumber{2}}, FrameSize{1000});

    ASSERT_EQ(entries.size(), 3U);
    // Frame 0: t* = max(500, 0) = 500 -> segment 0, point 0.
    EXPECT_EQ(entries[0].segment, 0U);
    EXPECT_EQ(entries[0].point, 0U);
    // Frame 1: t* = 1000 -> segment 0 (ends at 1500), point 1 (at 1000).
    EXPECT_EQ(entries[1].segment, 0U);
    EXPECT_EQ(entries[1].point, 1U);
    // Frame 2: t* = 2000 -> segment 1, point 2 (at 2000).
    EXPECT_EQ(entries[2].segment, 1U);
    EXPECT_EQ(entries[2].point, 2U);
}

// ---------------------------------------------------------------------------
// frameScope, resolveFrameSize
// ---------------------------------------------------------------------------

TEST(FrameScope, WorkedExample) {
    const FrameRange frames = frameScope(SampleTime{0.0}, SampleTime{120000.0}, FrameSize{48000});
    EXPECT_EQ(frames.first.value(), 0);
    EXPECT_EQ(frames.last.value(), 2);
}

TEST(FrameScope, EndingOnAFrameBoundaryExcludesThatFrame) {
    const FrameRange frames =
        frameScope(SampleTime{50000.0}, SampleTime{96000.0}, FrameSize{48000});
    EXPECT_EQ(frames.first.value(), 1);
    EXPECT_EQ(frames.last.value(), 1);
}

TEST(FrameScope, RefusesFrameNumbersBeyondInt32) {
    EXPECT_THROW(
        frameScope(SampleTime{0.0}, SampleTime{1e12}, FrameSize{1}), PartialPrecomputationError);
}

TEST(ResolveFrameSize, UsesTheConfiguredFrameSize) {
    EXPECT_EQ(resolveFrameSize(1024, 48000).value(), 1024);
}

TEST(ResolveFrameSize, DefaultsToOneSecond) {
    EXPECT_EQ(resolveFrameSize(std::nullopt, 44100).value(), 44100);
}

TEST(ResolveFrameSize, RefusesUnusableValues) {
    EXPECT_THROW(resolveFrameSize(std::nullopt, std::nullopt), PrecomputationError);
    EXPECT_THROW(resolveFrameSize(0, 48000), PrecomputationError);
    EXPECT_THROW(resolveFrameSize(std::nullopt, -1), PrecomputationError);
}

// ---------------------------------------------------------------------------
// The whole run, against engine-db
// ---------------------------------------------------------------------------

TEST_F(PrecomputationDb, WritesTheWorkedExample) {
    addPartial(db, "P", {at(0.0, 0.1), at(48000.0, 0.2), at(120000.0, 0.05)});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    ASSERT_TRUE(report.ok()) << *report.partials[0].error;
    EXPECT_EQ(report.frameSize.value(), 48000);
    EXPECT_EQ(report.partials[0].segmentCount, 2U);
    EXPECT_EQ(report.partials[0].frames->first.value(), 0);
    EXPECT_EQ(report.partials[0].frames->last.value(), 2);

    const std::vector<double> phases = startPhases(db, "P");
    ASSERT_EQ(phases.size(), 2U);
    EXPECT_EQ(phases[0], 0.0);
    EXPECT_NEAR(phases[1], 7200.0, 1e-9);

    EXPECT_EQ(count(db, "MATCH (:Partial)-[r:PARTIAL_IN_FRAME]->(:Frame) RETURN count(r);"), 3);
    EXPECT_EQ(report.partials[0].entryCount, 3U);
    EXPECT_EQ(count(db, "MATCH (e:FrameEntry) RETURN count(e);"), 3);
    // Each entry is joined to its frame, its partial, one segment and one
    // amplitude point; frame 1's entry leads to the segment starting at 48000.
    EXPECT_EQ(count(db, "MATCH (:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)-[:FRAME_ENTRY_PARTIAL]->"
                        "(:Partial) RETURN count(e);"),
        3);
    EXPECT_EQ(count(db, "MATCH (:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->"
                        "(:PartialIntegrableSegment) RETURN count(*);"),
        3);
    EXPECT_EQ(count(db, "MATCH (:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->"
                        "(:AmplitudePhysicalTimelinePoint) RETURN count(*);"),
        3);
    EXPECT_EQ(count(db, "MATCH (f:Frame {frame_number: 1})-[:FRAME_ENTRY]->(:FrameEntry)"
                        "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(s:PartialIntegrableSegment) "
                        "WHERE s.start_time = 48000.0 RETURN count(s);"),
        1);
    EXPECT_EQ(
        count(db,
            "MATCH "
            "(:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:AmplitudePhysicalTimelinePoint) "
            "WHERE a.time = 0.0 RETURN count(*);"),
        3);
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->"
                        "(n:PartialSegmentNominalFrequency) WHERE n.linear_component "
                        "AND NOT n.curve_component AND n.curve_frequency IS NULL "
                        "AND n.curve_start_phase IS NULL AND n.curve_scaling_factor IS NULL "
                        "RETURN count(n);"),
        2);
}

TEST_F(PrecomputationDb, UsesTheConfiguredFrameSize) {
    db.execute("MATCH (c:EngineConfig) SET c.frameSize = 1000;");
    addPartial(db, "P", {at(500.0, 0.1), at(2500.0, 0.1)});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.partials[0].frames->first.value(), 0);
    EXPECT_EQ(report.partials[0].frames->last.value(), 2);
    EXPECT_EQ(count(db, "MATCH (f:Frame) RETURN count(f);"), 3);
}

// Not the fixture: the example score brings its own EngineConfig.
TEST(Precomputation, PrecomputesTheExampleScore) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_EXAMPLE_STATE).ok());

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    ASSERT_TRUE(report.ok()) << *report.partials[0].error;
    EXPECT_EQ(report.partials[0].segmentCount, 1U);
    EXPECT_EQ(report.partials[0].frames->last.value(), 9);
}

// Ladybug refuses variable-length patterns over 30 hops, so a long timeline
// proves that the list is walked rather than matched in one pattern.
TEST_F(PrecomputationDb, HandlesPhysicalTimelinesLongerThanThirtyPoints) {
    std::vector<ScorePoint> points;
    for (int i = 0; i <= 100; ++i) {
        points.push_back(at(100.0 * i, 0.1 + (0.001 * (i % 7))));
    }
    addPartial(db, "Long", points);

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_TRUE(report.ok()) << *report.partials[0].error;
    EXPECT_EQ(report.partials[0].segmentCount, 100U);
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 100);
}

TEST_F(PrecomputationDb, RebuildingIsIdempotent) {
    addPartial(db, "P", {at(0.0, 0.1), at(48000.0, 0.2), at(120000.0, 0.05)});

    ASSERT_TRUE(precomputeAll(db).ok());
    const std::vector<double> first = startPhases(db, "P");
    ASSERT_TRUE(precomputeAll(db).ok());

    EXPECT_EQ(startPhases(db, "P"), first);
    EXPECT_EQ(count(db, "MATCH (t:PartialSegmentPhysicalTimeline) RETURN count(t);"), 1);
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 2);
    EXPECT_EQ(count(db, "MATCH (n:PartialSegmentNominalFrequency) RETURN count(n);"), 2);
    EXPECT_EQ(count(db, "MATCH ()-[r:PARTIAL_IN_FRAME]->() RETURN count(r);"), 3);
    EXPECT_EQ(count(db, "MATCH (f:Frame) RETURN count(f);"), 3);
    EXPECT_EQ(count(db, "MATCH (e:FrameEntry) RETURN count(e);"), 3);
    EXPECT_EQ(count(db, "MATCH ()-[r:FRAME_ENTRY_PHYSICAL_TIMELINE]->() RETURN count(r);"), 6);
}

TEST_F(PrecomputationDb, APartialWithoutAnAmplitudePhysicalTimelineFails) {
    addPartial(db, "P", {at(0.0, 0.1), at(100.0, 0.2)}, Amplitude::None);

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    EXPECT_FALSE(report.partials[0].ok());
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 0);
}

TEST_F(PrecomputationDb, AnAmplitudePhysicalTimelineThatDoesNotSpanTheLifelineFails) {
    addPartial(db, "P", {at(0.0, 0.1), at(100.0, 0.2)}, Amplitude::Matching, {0.0, 99.0});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    ASSERT_FALSE(report.partials[0].ok());
    EXPECT_NE(report.partials[0].error->find("AmplitudePhysicalTimeline"), std::string::npos);
}

// The example score's partial lives for ten frames, so it gets ten entries.
TEST(Precomputation, WritesAnEntryPerFrameForTheExampleScore) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_EXAMPLE_STATE).ok());

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_TRUE(report.ok()) << *report.partials[0].error;
    EXPECT_EQ(report.partials[0].entryCount, 10U);
    EXPECT_EQ(count(db, "MATCH (e:FrameEntry) RETURN count(e);"), 10);
}

TEST_F(PrecomputationDb, AFailedPartialDoesNotStopTheOthers) {
    addPartial(db, "Good", {at(0.0, 0.1), at(100.0, 0.2)});
    addPartial(db, "Curved", {at(0.0, 0.1, 2), at(100.0, 0.2)});
    addPartial(db, "Empty", {});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 3U);
    EXPECT_EQ(report.failureCount(), 2U);
    EXPECT_TRUE(report.partials[0].ok());
    EXPECT_FALSE(report.partials[1].ok());
    EXPECT_FALSE(report.partials[2].ok());
    EXPECT_EQ(count(db, "MATCH (t:PartialSegmentPhysicalTimeline) RETURN count(t);"), 1);
}

TEST_F(PrecomputationDb, APartialWithoutAFrequencyPhysicalTimelineFails) {
    db.execute("CREATE (:Partial {name: 'Bare'});");

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    EXPECT_FALSE(report.partials[0].ok());
}

// A failure rolls back the whole partial, including the deletion of its
// earlier output (§6.2). Documented behaviour, pinned so that it changes only
// deliberately -- dirty flags will want it changed.
TEST_F(PrecomputationDb, AFailedRebuildKeepsTheEarlierOutput) {
    addPartial(db, "P", {at(0.0, 0.1), at(100.0, 0.2)});
    ASSERT_TRUE(precomputeAll(db).ok());

    db.execute("MATCH (f:FrequencyPhysicalTimelinePoint) WHERE f.time = 0.0 SET "
               "f.frequency_curve_type = 1;");
    EXPECT_FALSE(precomputeAll(db).ok());

    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 1);
}

TEST_F(PrecomputationDb, NoEngineConfigIsFatal) {
    db.execute("MATCH (c:EngineConfig) DELETE c;");
    addPartial(db, "P", {at(0.0, 0.1), at(100.0, 0.2)});

    EXPECT_THROW(precomputeAll(db), PrecomputationError);
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 0);
}

TEST_F(PrecomputationDb, NoPartialsIsNotAnError) {
    const PrecomputationReport report = precomputeAll(db);
    EXPECT_TRUE(report.ok());
    EXPECT_TRUE(report.partials.empty());
}

} // namespace
