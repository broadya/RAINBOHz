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

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::AmplitudePointRecord;
using rainbohz::rendering_engine::buildCompensation;
using rainbohz::rendering_engine::buildSegments;
using rainbohz::rendering_engine::checkSpatialCoverage;
using rainbohz::rendering_engine::compensationAt;
using rainbohz::rendering_engine::compensationFrom;
using rainbohz::rendering_engine::CompensationSegment;
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
using rainbohz::rendering_engine::initialPhaseOf;
using rainbohz::rendering_engine::integrate;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::naturalPhaseAt;
using rainbohz::rendering_engine::NominalCompensation;
using rainbohz::rendering_engine::optionalDouble;
using rainbohz::rendering_engine::PartialPrecomputationError;
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::PhasePointRecord;
using rainbohz::rendering_engine::PhaseTarget;
using rainbohz::rendering_engine::PrecomputationError;
using rainbohz::rendering_engine::PrecomputationReport;
using rainbohz::rendering_engine::precomputeAll;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::resolveFrameSize;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::Segment;
using rainbohz::rendering_engine::SpatialKind;
using rainbohz::rendering_engine::SpatialPointRecord;
using rainbohz::rendering_engine::validateAmplitudeTimes;
using rainbohz::rendering_engine::validateFrequencyEnvelope;
using rainbohz::rendering_engine::validatePhaseTimeline;
using rainbohz::rendering_engine::validateSpatialTimeline;
using rainbohz::rendering_engine::walkChain;
using rainbohz::rendering_engine::wrapHalfOpen;
using rainbohz::rendering_engine::test::addSoundSource;
using rainbohz::rendering_engine::test::SpatialTestPoint;
using rainbohz::rendering_engine::test::stillAzimuth;
using rainbohz::rendering_engine::test::stillDistance;
using rainbohz::rendering_engine::test::stillElevation;

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

/// A compensation segment with no compensation.
///
/// @param start  Start time, samples.
/// @param end    End time, samples.
/// @return A flat segment at delta 0 over `[start, end)`.
CompensationSegment flat(double start, double end) {
    return CompensationSegment{.startTime = SampleTime{start},
        .endTime = SampleTime{end},
        .startDelta = Phase{0.0},
        .endDelta = Phase{0.0}};
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

/// Whether addPartial() gives the partial a PhasePhysicalTimeline.
enum class PhaseTimeline : std::uint8_t {
    /// One point, at the first frequency time, phase 0: the timeline every
    /// partial must have at least.
    Default,
    /// No PhasePhysicalTimeline; addPhaseTimeline() can add one.
    None,
};

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
/// @param phase       Whether to add the default phase timeline. It is added
///                    only when `points` is not empty.
void addPartial(EngineDb& db, const std::string& name, const std::vector<ScorePoint>& points,
    Amplitude amplitude = Amplitude::Matching, const std::vector<double>& amplitudeTimes = {},
    PhaseTimeline phase = PhaseTimeline::Default) {
    // Every partial is in one set of one still source, which precomputation
    // needs for the spatial frame entries; the first partial creates them.
    if (db.query("MATCH (s:PartialSet {name: 'Set'}) RETURN s.id;").empty()) {
        addSoundSource(db, "Source");
        db.execute("MATCH (s:SoundSource {name: 'Source'}) "
                   "CREATE (s)-[:HAS_PARTIALSET]->(:PartialSet {name: 'Set'});");
    }
    db.execute("MATCH (s:PartialSet {name: 'Set'}) CREATE (s)-[:HAS_PARTIAL]->(:Partial {name: '" +
               name + "'})-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline);");
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

    if (phase == PhaseTimeline::Default) {
        db.execute("MATCH (p:Partial {name: '" + name +
                   "'}) CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(:PhasePhysicalTimeline)"
                   "-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: " +
                   literal(points.front().time) + ", phase: 0.0});");
    }

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

/// One point of a phase timeline built by addPhaseTimeline(): time in
/// samples, and phase in radians or nullopt for NULL.
using PhasePoint = std::pair<double, std::optional<double>>;

/// Adds a PhasePhysicalTimeline to a partial made with PhaseTimeline::None.
///
/// @param[in,out] db  Database holding the partial.
/// @param name        Partial.name.
/// @param points      The points, linked in order. May be empty.
void addPhaseTimeline(
    EngineDb& db, const std::string& name, const std::vector<PhasePoint>& points) {
    std::string cypher = "MATCH (p:Partial {name: '" + name +
                         "'}) CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline)";
    if (!points.empty()) {
        cypher += " WITH t CREATE (t)";
    }
    for (const auto& [time, phase] : points) {
        cypher += "-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: " + literal(time) +
                  ", phase: " + (phase.has_value() ? literal(*phase) : "NULL") + "})";
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

// ---------------------------------------------------------------------------
// validateSpatialTimeline and checkSpatialCoverage
// ---------------------------------------------------------------------------

/// A spatial point record for the pure tests.
///
/// @param time       Samples.
/// @param value      Radians or metres.
/// @param curveType  The curve to the next point.
/// @return The record.
SpatialPointRecord spatialRecord(double time, double value, std::int64_t curveType = 0) {
    return SpatialPointRecord{.id = 0, .time = time, .value = value, .curveType = curveType};
}

TEST(ValidateSpatialTimeline, AcceptsAValidTimelineOfEachKind) {
    EXPECT_EQ(validateSpatialTimeline(
                  SpatialKind::Azimuth, std::vector{spatialRecord(0.0, -std::numbers::pi),
                                            spatialRecord(10.0, std::numbers::pi)})
                  .size(),
        2U);
    EXPECT_NO_THROW(validateSpatialTimeline(
        SpatialKind::Elevation, std::vector{spatialRecord(0.0, -std::numbers::pi / 2.0),
                                    spatialRecord(10.0, std::numbers::pi / 2.0)}));
    EXPECT_NO_THROW(validateSpatialTimeline(
        SpatialKind::Distance, std::vector{spatialRecord(0.0, 1.0), spatialRecord(10.0, 16384.0)}));
    // One point is a valid timeline on its own; coverage is a separate check.
    EXPECT_NO_THROW(
        validateSpatialTimeline(SpatialKind::Distance, std::vector{spatialRecord(5.0, 2.0)}));
}

TEST(ValidateSpatialTimeline, RefusesValuesOutOfRange) {
    EXPECT_THROW(validateSpatialTimeline(SpatialKind::Azimuth,
                     std::vector{spatialRecord(0.0, 0.0), spatialRecord(10.0, 3.2)}),
        PartialPrecomputationError);
    EXPECT_THROW(
        validateSpatialTimeline(SpatialKind::Elevation, std::vector{spatialRecord(0.0, -1.6)}),
        PartialPrecomputationError);
    EXPECT_THROW(
        validateSpatialTimeline(SpatialKind::Distance, std::vector{spatialRecord(0.0, 0.5)}),
        PartialPrecomputationError);
    EXPECT_THROW(
        validateSpatialTimeline(SpatialKind::Distance, std::vector{spatialRecord(0.0, 20000.0)}),
        PartialPrecomputationError);
}

TEST(ValidateSpatialTimeline, RefusesNoPointsNullsDisorderAndCurves) {
    EXPECT_THROW(validateSpatialTimeline(SpatialKind::Azimuth, std::vector<SpatialPointRecord>{}),
        PartialPrecomputationError);
    std::vector<SpatialPointRecord> unset{spatialRecord(0.0, 0.0)};
    unset[0].value = std::nullopt;
    EXPECT_THROW(validateSpatialTimeline(SpatialKind::Azimuth, unset), PartialPrecomputationError);
    EXPECT_THROW(validateSpatialTimeline(SpatialKind::Azimuth,
                     std::vector{spatialRecord(10.0, 0.0), spatialRecord(10.0, 0.1)}),
        PartialPrecomputationError);
    EXPECT_THROW(validateSpatialTimeline(SpatialKind::Azimuth,
                     std::vector{spatialRecord(0.0, 0.0, 3), spatialRecord(10.0, 0.1)}),
        PartialPrecomputationError);
    // The last point's curve type has no effect and is not checked.
    EXPECT_NO_THROW(validateSpatialTimeline(
        SpatialKind::Azimuth, std::vector{spatialRecord(0.0, 0.0), spatialRecord(10.0, 0.1, 3)}));
}

TEST(CheckSpatialCoverage, TheTimelineMustSpanTheLifeline) {
    const std::vector<SampleTime> times{SampleTime{100.0}, SampleTime{500.0}};
    EXPECT_NO_THROW(
        checkSpatialCoverage(SpatialKind::Azimuth, times, SampleTime{100.0}, SampleTime{500.0}));
    EXPECT_NO_THROW(
        checkSpatialCoverage(SpatialKind::Azimuth, times, SampleTime{200.0}, SampleTime{300.0}));
    EXPECT_THROW(
        checkSpatialCoverage(SpatialKind::Azimuth, times, SampleTime{99.5}, SampleTime{300.0}),
        PartialPrecomputationError);
    EXPECT_THROW(
        checkSpatialCoverage(SpatialKind::Distance, times, SampleTime{200.0}, SampleTime{500.5}),
        PartialPrecomputationError);
}

// The worked example of docs/precomputation.md section 5: frames 0, 1 and 2
// enter segments 0, 1 and 1, and every frame's amplitude entry is the point
// at 0 when the only other point is at the end.
TEST(FrameEntries, WorkedExample) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    integrate(segments);
    const std::vector<SampleTime> amplitude{SampleTime{0.0}, SampleTime{120000.0}};
    const std::vector<CompensationSegment> compensation{flat(0.0, 120000.0)};

    const std::vector<SampleTime> place{SampleTime{0.0}, SampleTime{120000.0}};

    const std::vector<FrameEntryTarget> entries =
        frameEntries(segments, amplitude, compensation, {place, place, place},
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
    EXPECT_EQ(entries[0].compensation, 0U);
    EXPECT_EQ(entries[2].compensation, 0U);
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
    const std::vector<CompensationSegment> compensation{
        flat(500.0, 1200.0), flat(1200.0, 2000.0), flat(2000.0, 2500.0)};

    // The source's timelines span more than this partial: they start before
    // it. The azimuth has a point between frame 0's start and the partial's,
    // and one exactly at frame 1's start; the distance starts inside frame 0.
    const std::vector<SampleTime> azimuth{SampleTime{0.0}, SampleTime{300.0}, SampleTime{1000.0},
        SampleTime{1700.0}, SampleTime{3000.0}};
    const std::vector<SampleTime> elevation{SampleTime{-100.0}, SampleTime{9000.0}};
    const std::vector<SampleTime> distance{
        SampleTime{200.0}, SampleTime{2000.5}, SampleTime{5000.0}};

    const std::vector<FrameEntryTarget> entries =
        frameEntries(segments, amplitude, compensation, {azimuth, elevation, distance},
            FrameRange{.first = FrameNumber{0}, .last = FrameNumber{2}}, FrameSize{1000});

    ASSERT_EQ(entries.size(), 3U);
    // Frame 0: t* = max(500, 0) = 500 -> segment 0, point 0, compensation 0.
    EXPECT_EQ(entries[0].segment, 0U);
    EXPECT_EQ(entries[0].point, 0U);
    EXPECT_EQ(entries[0].compensation, 0U);
    // Frame 1: t* = 1000 -> segment 0 (ends at 1500), point 1 (at 1000),
    // compensation 0 (mid-segment: it ends at 1200).
    EXPECT_EQ(entries[1].segment, 0U);
    EXPECT_EQ(entries[1].point, 1U);
    EXPECT_EQ(entries[1].compensation, 0U);
    // The spatial entries depend on the frame, not on t*: the last point at
    // or before the frame's start, else the first point. Azimuth 0 (at 0, not
    // the point at 300 that t* = 500 would give), 2 (exactly at 1000), 3
    // (1700); elevation 0 throughout; distance 0 in frame 0 (its first point,
    // at 200, inside the frame), then 0, 0 (2000.5 is after 2000).
    EXPECT_EQ(entries[0].spatial, (std::array<std::size_t, 3>{0, 0, 0}));
    EXPECT_EQ(entries[1].spatial, (std::array<std::size_t, 3>{2, 0, 0}));
    EXPECT_EQ(entries[2].spatial, (std::array<std::size_t, 3>{3, 0, 0}));
    // Frame 2: t* = 2000 -> segment 1, point 2 (at 2000), compensation 2,
    // which starts exactly at the frame's start.
    EXPECT_EQ(entries[2].segment, 1U);
    EXPECT_EQ(entries[2].point, 2U);
    EXPECT_EQ(entries[2].compensation, 2U);
}

// ---------------------------------------------------------------------------
// Phase control (§6.8)
// ---------------------------------------------------------------------------

constexpr double kPi = std::numbers::pi;

/// A phase point record with every property but the phase set.
///
/// @param time   Time, samples.
/// @param phase  Phase, radians; nullopt for NULL.
/// @return The record, with id 0.
PhasePointRecord phaseRecord(double time, std::optional<double> phase) {
    return PhasePointRecord{.id = 0, .time = time, .phase = phase};
}

/// Validated phase targets, from (time, phase) pairs.
///
/// @param points  The points; a nullopt phase is NULL.
/// @return The targets, unvalidated.
std::vector<PhaseTarget> targetsOf(const std::vector<PhasePoint>& points) {
    std::vector<PhaseTarget> targets;
    targets.reserve(points.size());
    for (const auto& [time, phase] : points) {
        targets.push_back(PhaseTarget{.time = SampleTime{time},
            .phase = phase.has_value() ? std::optional<Phase>{Phase{*phase}} : std::nullopt});
    }
    return targets;
}

/// The segments of a constant-frequency partial over `[start, end)`,
/// integrated from the initial phase the targets give, as precomputation
/// does it.
///
/// @param start      Lifeline start, samples.
/// @param end        Lifeline end, samples.
/// @param frequency  Radians per sample.
/// @param targets    The phase timeline.
/// @return The integrated segments.
std::vector<Segment> integratedConstant(
    double start, double end, double frequency, const std::vector<PhaseTarget>& targets) {
    const std::vector<FrequencyPointRecord> records{
        record(start, frequency), record(end, frequency)};
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    integrate(segments);
    integrate(segments, initialPhaseOf(segments, targets));
    return segments;
}

/// The phase actually rendered at `t`: the envelope's plus the compensation.
///
/// @param segments      Integrated segments.
/// @param compensation  Compensation segments covering `t`.
/// @param t             A time within the lifeline.
/// @return N(t) + D(t), unwrapped.
Phase renderedPhaseAt(const std::vector<Segment>& segments,
    const std::vector<CompensationSegment>& compensation, double t) {
    for (const CompensationSegment& c : compensation) {
        if (c.startTime.value() <= t && t < c.endTime.value()) {
            return naturalPhaseAt(segments, SampleTime{t}) +
                   compensationAt(compensationFrom(c, SampleTime{t}), SampleDuration{0.0});
        }
    }
    ADD_FAILURE() << "no compensation segment covers " << t;
    return Phase{0.0};
}

/// The brief's frequency: from 10 000 to 30 000 samples the envelope adds
/// three cycles and a quarter, so a partial at phase 0 at 10 000 runs free to
/// 90 degrees at 30 000.
constexpr double kBriefFrequency = ((kPi / 2.0) + (6.0 * kPi)) / 20000.0;

TEST(ValidatePhaseTimeline, AcceptsTargetsAndNullPoints) {
    const std::vector<PhasePointRecord> points{
        phaseRecord(0.0, std::nullopt), phaseRecord(48000.0, 1.0), phaseRecord(120000.0, -kPi)};
    const std::vector<PhaseTarget> targets = validatePhaseTimeline(points, workedExampleEnvelope());
    ASSERT_EQ(targets.size(), 3U);
    EXPECT_FALSE(targets[0].phase.has_value()) << "NULL means free-running, not absent";
    EXPECT_EQ(targets[1].phase->value(), 1.0);
    EXPECT_EQ(targets[2].time.value(), 120000.0) << "a point at the lifeline's end is allowed";
}

TEST(ValidatePhaseTimeline, NeedsAPointAtTheLifelinesStart) {
    EXPECT_THROW(validatePhaseTimeline({}, workedExampleEnvelope()), PartialPrecomputationError);
    const std::vector<PhasePointRecord> late{phaseRecord(10.0, 0.0)};
    EXPECT_THROW(validatePhaseTimeline(late, workedExampleEnvelope()), PartialPrecomputationError);
}

TEST(ValidatePhaseTimeline, TimesMustIncreaseWithinTheLifeline) {
    const std::vector<PhasePointRecord> repeated{phaseRecord(0.0, 0.0), phaseRecord(0.0, 1.0)};
    EXPECT_THROW(
        validatePhaseTimeline(repeated, workedExampleEnvelope()), PartialPrecomputationError);
    const std::vector<PhasePointRecord> after{phaseRecord(0.0, 0.0), phaseRecord(130000.0, 1.0)};
    EXPECT_THROW(validatePhaseTimeline(after, workedExampleEnvelope()), PartialPrecomputationError);
    const std::vector<PhasePointRecord> unset{PhasePointRecord{.id = 0, .time = {}, .phase = 0.0}};
    EXPECT_THROW(validatePhaseTimeline(unset, workedExampleEnvelope()), PartialPrecomputationError);
}

TEST(ValidatePhaseTimeline, PhaseMustBeWrappedRadians) {
    const std::vector<PhasePointRecord> outside{phaseRecord(0.0, 4.0)};
    EXPECT_THROW(
        validatePhaseTimeline(outside, workedExampleEnvelope()), PartialPrecomputationError);
    const std::vector<PhasePointRecord> nan{
        phaseRecord(0.0, std::numeric_limits<double>::quiet_NaN())};
    EXPECT_THROW(validatePhaseTimeline(nan, workedExampleEnvelope()), PartialPrecomputationError);
}

TEST(NaturalPhaseAt, IsPhaseAtOnTheCoveringSegment) {
    const std::vector<FrequencyPoint> envelope = workedExampleEnvelope();
    std::vector<Segment> segments = buildSegments(gatherBoundaries(envelope), envelope);
    integrate(segments);

    EXPECT_EQ(naturalPhaseAt(segments, SampleTime{0.0}).value(), 0.0);
    EXPECT_EQ(naturalPhaseAt(segments, SampleTime{48000.0}), segments[1].startPhase)
        << "a boundary is evaluated on the segment it starts";
    EXPECT_NEAR(naturalPhaseAt(segments, SampleTime{120000.0}).value(), 16200.0, 1e-6)
        << "the lifeline's end is the last segment evaluated at its length";
    EXPECT_THROW(naturalPhaseAt(segments, SampleTime{120001.0}), std::invalid_argument);
}

TEST(InitialPhaseOf, IsTheFirstTargetWhenItIsSet) {
    const std::vector<Segment> segments =
        integratedConstant(0.0, 200.0, 0.01, targetsOf({{0.0, 0.5}}));
    EXPECT_EQ(segments[0].startPhase.value(), 0.5);
}

TEST(InitialPhaseOf, IsZeroWhenEveryPointIsNull) {
    const std::vector<Segment> segments = integratedConstant(
        0.0, 200.0, 0.01, targetsOf({{0.0, std::nullopt}, {100.0, std::nullopt}}));
    EXPECT_EQ(segments[0].startPhase.value(), 0.0);
}

// The brief's NULL start: phase 180 degrees at 30 000 with no ramp, by
// starting where running free arrives there. From 0 the envelope reaches
// 90 degrees (mod 2π), so φ₀ is 90 degrees.
TEST(InitialPhaseOf, IsWorkedBackwardsFromTheFirstTargetAfterANullStart) {
    const std::vector<PhaseTarget> targets = targetsOf({{10000.0, std::nullopt}, {30000.0, kPi}});
    const std::vector<Segment> segments =
        integratedConstant(10000.0, 40000.0, kBriefFrequency, targets);

    EXPECT_NEAR(segments[0].startPhase.value(), kPi / 2.0, 1e-12);
    EXPECT_NEAR(wrapHalfOpen(naturalPhaseAt(segments, SampleTime{30000.0}) - Phase{kPi}).value(),
        0.0, 1e-12);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);
    ASSERT_EQ(compensation.size(), 2U);
    for (const CompensationSegment& c : compensation) {
        EXPECT_EQ(c.startDelta.value(), 0.0) << "the anchor is met by φ₀, not by a ramp";
        EXPECT_EQ(c.endDelta.value(), 0.0);
    }
}

// The brief: 0 degrees at 10 s, 180 degrees at 30 s, and running free the
// partial would be at 90 degrees. The ramp adds 90 degrees over the 20 s, so
// 45 degrees at 20 s.
TEST(BuildCompensation, TheBriefsExample) {
    const std::vector<PhaseTarget> targets = targetsOf({{10000.0, 0.0}, {30000.0, kPi}});
    const std::vector<Segment> segments =
        integratedConstant(10000.0, 40000.0, kBriefFrequency, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 2U);
    EXPECT_EQ(compensation[0].startTime.value(), 10000.0);
    EXPECT_EQ(compensation[0].endTime.value(), 30000.0);
    EXPECT_EQ(compensation[0].startDelta.value(), 0.0);
    EXPECT_NEAR(compensation[0].endDelta.value(), kPi / 2.0, 1e-9);
    EXPECT_NEAR(
        compensationFrom(compensation[0], SampleTime{20000.0}).startDelta.value(), kPi / 4.0, 1e-9);
    // After the last target the compensation holds.
    EXPECT_EQ(compensation[1].startTime.value(), 30000.0);
    EXPECT_EQ(compensation[1].endTime.value(), 40000.0);
    EXPECT_EQ(compensation[1].startDelta, compensation[0].endDelta);
    EXPECT_EQ(compensation[1].endDelta, compensation[0].endDelta);
    // And the rendered phase meets the target.
    EXPECT_NEAR(wrapHalfOpen(renderedPhaseAt(segments, compensation, 30000.0) - Phase{kPi}).value(),
        0.0, 1e-9);
}

// The brief again, with a NULL point at 20 s: free-running to there, then the
// same 90 degrees over the remaining 10 s, so 45 degrees at 25 s.
TEST(BuildCompensation, ANullPointStartsTheRampLater) {
    const std::vector<PhaseTarget> targets =
        targetsOf({{10000.0, 0.0}, {20000.0, std::nullopt}, {30000.0, kPi}});
    const std::vector<Segment> segments =
        integratedConstant(10000.0, 40000.0, kBriefFrequency, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 3U);
    EXPECT_EQ(compensation[0].startDelta.value(), 0.0);
    EXPECT_EQ(compensation[0].endDelta.value(), 0.0) << "free-running up to the NULL point";
    EXPECT_EQ(compensation[1].startTime.value(), 20000.0);
    EXPECT_NEAR(compensation[1].endDelta.value(), kPi / 2.0, 1e-9);
    EXPECT_NEAR(
        compensationFrom(compensation[1], SampleTime{25000.0}).startDelta.value(), kPi / 4.0, 1e-9);
    EXPECT_NEAR(wrapHalfOpen(renderedPhaseAt(segments, compensation, 30000.0) - Phase{kPi}).value(),
        0.0, 1e-9);
}

TEST(BuildCompensation, OnePointIsOneFlatSegment) {
    const std::vector<PhaseTarget> targets = targetsOf({{0.0, 0.5}});
    const std::vector<Segment> segments = integratedConstant(0.0, 200.0, 0.01, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 1U);
    EXPECT_EQ(compensation[0].startTime.value(), 0.0);
    EXPECT_EQ(compensation[0].endTime.value(), 200.0);
    EXPECT_EQ(compensation[0].startDelta.value(), 0.0);
    EXPECT_EQ(compensation[0].endDelta.value(), 0.0);
}

// At zero frequency the natural phase stays at φ₀, so the adjustments are
// exactly the differences between targets.
TEST(BuildCompensation, AnExactHalfCycleGoesNegative) {
    const std::vector<PhaseTarget> targets = targetsOf({{0.0, 0.0}, {100.0, kPi}});
    const std::vector<Segment> segments = integratedConstant(0.0, 200.0, 0.0, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 2U);
    EXPECT_EQ(compensation[0].endDelta.value(), -kPi);
}

TEST(BuildCompensation, DeltasAreRunningTotalsThatDoNotWrap) {
    const std::vector<PhaseTarget> targets = targetsOf({{0.0, 0.0}, {100.0, 2.0}, {200.0, -2.0}});
    const std::vector<Segment> segments = integratedConstant(0.0, 300.0, 0.0, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 3U);
    EXPECT_EQ(compensation[0].endDelta.value(), 2.0);
    // From 2 to -2 is -4, which is nearer as +(2π − 4): a total of 2π − 2,
    // beyond π and kept that way.
    EXPECT_NEAR(compensation[1].endDelta.value(), (2.0 * kPi) - 2.0, 1e-12);
    EXPECT_GT(compensation[1].endDelta.value(), kPi);
}

TEST(BuildCompensation, ATargetAtTheLifelinesEndNeedsNoTrailingSegment) {
    const std::vector<PhaseTarget> targets = targetsOf({{0.0, 0.0}, {200.0, 1.0}});
    const std::vector<Segment> segments = integratedConstant(0.0, 200.0, 0.0, targets);

    const std::vector<CompensationSegment> compensation = buildCompensation(segments, targets);

    ASSERT_EQ(compensation.size(), 1U);
    EXPECT_EQ(compensation[0].endTime.value(), 200.0);
    EXPECT_EQ(compensation[0].endDelta.value(), 1.0);
}

TEST(CompensationFrom, RebasesTheRamp) {
    const CompensationSegment segment{.startTime = SampleTime{100.0},
        .endTime = SampleTime{300.0},
        .startDelta = Phase{1.0},
        .endDelta = Phase{3.0}};

    const NominalCompensation atStart = compensationFrom(segment, SampleTime{100.0});
    EXPECT_EQ(atStart.startDelta.value(), 1.0) << "exactly the stored start";
    EXPECT_EQ(atStart.slope.value(), 0.01);
    EXPECT_DOUBLE_EQ(compensationFrom(segment, SampleTime{200.0}).startDelta.value(), 2.0);
    EXPECT_DOUBLE_EQ(compensationAt(atStart, SampleDuration{200.0}).value(), 3.0);
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
    // Its one phase point gives one flat compensation segment, which every
    // entry also leads to.
    EXPECT_EQ(report.partials[0].compensationCount, 1U);
    EXPECT_EQ(
        count(db, "MATCH (:Partial)-[:HAS_PHYSICAL_TIMELINE]->(:PartialSegmentPhysicalTimeline)"
                  "-[:NEXT_POINT]->(c:PartialPhaseCompensationSegment) "
                  "WHERE c.start_time = 0.0 AND c.end_time = 120000.0 "
                  "AND c.start_phase_delta = 0.0 AND c.end_phase_delta = 0.0 "
                  "RETURN count(c);"),
        1);
    EXPECT_EQ(count(db, "MATCH (:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->"
                        "(:PartialPhaseCompensationSegment) RETURN count(*);"),
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
    EXPECT_EQ(count(db, "MATCH (c:PartialPhaseCompensationSegment) RETURN count(c);"), 1);
    // Six per entry: segment, amplitude point, compensation segment, and the
    // source's azimuth, elevation and distance points.
    EXPECT_EQ(count(db, "MATCH ()-[r:FRAME_ENTRY_PHYSICAL_TIMELINE]->() RETURN count(r);"), 18);
}

TEST_F(PrecomputationDb, RebuildingReplacesTheCompensationSegments) {
    addPartial(
        db, "P", {at(0.0, 0.1), at(144000.0, 0.1)}, Amplitude::Matching, {}, PhaseTimeline::None);
    addPhaseTimeline(db, "P", {{0.0, 0.0}, {60000.0, std::nullopt}, {100000.0, 1.0}});

    ASSERT_TRUE(precomputeAll(db).ok());
    ASSERT_TRUE(precomputeAll(db).ok());

    EXPECT_EQ(count(db, "MATCH (c:PartialPhaseCompensationSegment) RETURN count(c);"), 3);
}

TEST_F(PrecomputationDb, WritesPhaseCompensationSegments) {
    addPartial(
        db, "P", {at(0.0, 0.1), at(144000.0, 0.1)}, Amplitude::Matching, {}, PhaseTimeline::None);
    addPhaseTimeline(db, "P", {{0.0, 0.0}, {60000.0, std::nullopt}, {100000.0, 1.0}});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_TRUE(report.ok()) << *report.partials[0].error;
    EXPECT_EQ(report.partials[0].compensationCount, 3U);
    const std::vector<DbRow> rows =
        db.query("MATCH (c:PartialPhaseCompensationSegment) "
                 "RETURN c.start_time, c.end_time, c.start_phase_delta, c.end_phase_delta "
                 "ORDER BY c.start_time;");
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(optionalDouble(rows[0].at(1)).value(), 60000.0);
    EXPECT_EQ(optionalDouble(rows[0].at(3)).value(), 0.0) << "free-running to the NULL point";
    EXPECT_EQ(optionalDouble(rows[1].at(0)).value(), 60000.0);
    EXPECT_EQ(optionalDouble(rows[2].at(1)).value(), 144000.0);
    // 0.1 rad/sample for 100000 samples is 10000 rad; the ramp makes up the
    // difference to 1.0 (mod 2π), the smaller way round.
    const double expected = wrapHalfOpen(Phase{1.0} - Phase{0.1 * 100000.0}).value();
    EXPECT_NEAR(optionalDouble(rows[1].at(3)).value(), expected, 1e-9);
    EXPECT_EQ(optionalDouble(rows[2].at(2)), optionalDouble(rows[1].at(3)));
}

TEST_F(PrecomputationDb, EachFrameEntryLeadsToTheCoveringCompensationSegment) {
    addPartial(
        db, "P", {at(0.0, 0.1), at(144000.0, 0.1)}, Amplitude::Matching, {}, PhaseTimeline::None);
    addPhaseTimeline(db, "P", {{0.0, 0.0}, {60000.0, std::nullopt}, {100000.0, 1.0}});
    ASSERT_TRUE(precomputeAll(db).ok());

    // Frames 0 and 1 enter the first compensation segment (frame 1 in its
    // middle), frame 2 the second, which covers its start at 96000.
    const auto entryStart = [&](int frame) {
        return optionalDouble(
            db.query("MATCH (:Frame {frame_number: CAST($frame AS INT32)})-[:FRAME_ENTRY]->"
                     "(:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->"
                     "(c:PartialPhaseCompensationSegment) RETURN c.start_time;",
                  {{"frame", static_cast<std::int64_t>(frame)}})
                .at(0)
                .at(0))
            .value();
    };
    EXPECT_EQ(entryStart(0), 0.0);
    EXPECT_EQ(entryStart(1), 0.0);
    EXPECT_EQ(entryStart(2), 60000.0);
}

/// Creates the source and set that addPartial() puts its partials in, with
/// the given spatial timelines, before any partial is added.
///
/// @param[in,out] db  Database holding no partial yet.
/// @param azimuth     (time, radians) points; empty for no timeline.
/// @param elevation   (time, radians) points; empty for no timeline.
/// @param distance    (time, metres) points; empty for no timeline.
void addSourceAndSet(EngineDb& db, const std::vector<SpatialTestPoint>& azimuth,
    const std::vector<SpatialTestPoint>& elevation, const std::vector<SpatialTestPoint>& distance) {
    addSoundSource(db, "Source", azimuth, elevation, distance);
    db.execute("MATCH (s:SoundSource {name: 'Source'}) "
               "CREATE (s)-[:HAS_PARTIALSET]->(:PartialSet {name: 'Set'});");
}

/// The time of the spatial point a frame's entry leads to.
///
/// @param db      The precomputed database, holding one partial.
/// @param kind    "Azimuth", "Elevation" or "Distance".
/// @param frame   The frame.
/// @return The time of the point.
double spatialEntryTime(EngineDb& db, const std::string& kind, int frame) {
    return optionalDouble(
        db.query("MATCH (:Frame {frame_number: CAST($frame AS INT32)})-[:FRAME_ENTRY]->"
                 "(:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:" +
                     kind + "PhysicalTimelinePoint) RETURN a.time;",
              {{"frame", static_cast<std::int64_t>(frame)}})
            .at(0)
            .at(0))
        .value();
}

// Each frame's entry leads to the last point of each of the source's
// timelines at or before the frame's entry time, as for amplitude; the
// timelines span more than the partial.
TEST_F(PrecomputationDb, EachFrameEntryLeadsToTheSourcesSpatialPoints) {
    addSourceAndSet(db,
        {{0.0, 0.0}, {500.0, 0.5}, {48000.0, 1.0}, {100000.0, 2.0}, {500000.0, 2.0}},
        {{-10.0, 0.0}, {500000.0, 0.0}}, {{0.0, 1.0}, {96000.5, 2.0}, {200000.0, 3.0}});
    addPartial(db, "P", {at(1000.0, 0.1), at(144000.0, 0.1)});
    ASSERT_TRUE(precomputeAll(db).ok()) << precomputeAll(db).partials[0].error.value_or("");

    // Frames 0, 1 and 2 start at 0, 48000 and 96000. Frame 0's azimuth entry
    // is the point at 0, not the one at 500, although the partial starts at
    // 1000: the entry is the frame's, not the partial's.
    EXPECT_EQ(spatialEntryTime(db, "Azimuth", 0), 0.0);
    EXPECT_EQ(spatialEntryTime(db, "Azimuth", 1), 48000.0);
    EXPECT_EQ(spatialEntryTime(db, "Azimuth", 2), 48000.0);
    EXPECT_EQ(spatialEntryTime(db, "Elevation", 2), -10.0);
    EXPECT_EQ(spatialEntryTime(db, "Distance", 2), 0.0) << "96000.5 is after 96000";
    EXPECT_EQ(count(db, "MATCH ()-[r:FRAME_ENTRY_PHYSICAL_TIMELINE]->() RETURN count(r);"), 18);
}

// Two partials of one source share its timelines: both have entries into
// them, and neither copies them.
TEST_F(PrecomputationDb, PartialsOfOneSourceShareItsTimelines) {
    addPartial(db, "A", {at(0.0, 0.1), at(48000.0, 0.1)});
    addPartial(db, "B", {at(24000.0, 0.1), at(96000.0, 0.1)});
    ASSERT_TRUE(precomputeAll(db).ok());
    EXPECT_EQ(count(db, "MATCH (a:AzimuthPhysicalTimelinePoint) RETURN count(a);"), 2);
    EXPECT_EQ(count(db, "MATCH (:FrameEntry)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->"
                        "(:AzimuthPhysicalTimelinePoint) RETURN count(*);"),
        3);
}

/// The single partial's precomputation error, with the source set up first.
///
/// @param db         An empty database with the fixture's config.
/// @param azimuth    The source's azimuth points.
/// @param elevation  The source's elevation points.
/// @param distance   The source's distance points.
/// @return The error; empty if the partial succeeded.
std::string spatialFailure(EngineDb& db, const std::vector<SpatialTestPoint>& azimuth,
    const std::vector<SpatialTestPoint>& elevation, const std::vector<SpatialTestPoint>& distance) {
    addSourceAndSet(db, azimuth, elevation, distance);
    addPartial(db, "P", {at(100.0, 0.1), at(1000.0, 0.1)});
    const PrecomputationReport report = precomputeAll(db);
    return report.partials.at(0).error.value_or("");
}

TEST_F(PrecomputationDb, ASourceWithoutASpatialTimelineFailsItsPartials) {
    const std::string error = spatialFailure(db, stillAzimuth(), {}, stillDistance());
    EXPECT_NE(error.find("SoundSource Source"), std::string::npos) << error;
    EXPECT_NE(error.find("no ElevationPhysicalTimeline"), std::string::npos) << error;
    EXPECT_EQ(count(db, "MATCH (e:FrameEntry) RETURN count(e);"), 0);
}

TEST_F(PrecomputationDb, ASpatialValueOutOfRangeFailsThePartial) {
    const std::string error =
        spatialFailure(db, stillAzimuth(), stillElevation(), {{0.0, 1.0}, {2000.0, 0.5}});
    EXPECT_NE(error.find("distance 0.5 is outside"), std::string::npos) << error;
}

TEST_F(PrecomputationDb, ASpatialTimelineThatDoesNotCoverThePartialFails) {
    const std::string late =
        spatialFailure(db, {{200.0, 0.0}, {2000.0, 0.0}}, stillElevation(), stillDistance());
    EXPECT_NE(late.find("starts at 200"), std::string::npos) << late;
}

TEST_F(PrecomputationDb, ASpatialTimelineEndingTooEarlyFails) {
    const std::string early =
        spatialFailure(db, stillAzimuth(), {{0.0, 0.0}, {999.0, 0.0}}, stillDistance());
    EXPECT_NE(early.find("ends at 999"), std::string::npos) << early;
}

TEST_F(PrecomputationDb, ASpatialCurveOtherThanLinearFails) {
    addSourceAndSet(db, stillAzimuth(), stillElevation(), stillDistance());
    db.execute("MATCH (a:AzimuthPhysicalTimelinePoint {time: 0.0}) SET a.azimuth_curve_type = 2;");
    addPartial(db, "P", {at(100.0, 0.1), at(1000.0, 0.1)});
    const std::string error = precomputeAll(db).partials.at(0).error.value_or("");
    EXPECT_NE(error.find("azimuth_curve_type is 2"), std::string::npos) << error;
}

TEST_F(PrecomputationDb, ANullStartPutsTheBackDerivedPhaseInTheFirstSegment) {
    addPartial(db, "P", {at(0.0, 0.001), at(96000.0, 0.001)}, Amplitude::Matching, {},
        PhaseTimeline::None);
    addPhaseTimeline(db, "P", {{0.0, std::nullopt}, {48000.0, 1.0}});

    ASSERT_TRUE(precomputeAll(db).ok());

    const std::vector<double> phases = startPhases(db, "P");
    ASSERT_EQ(phases.size(), 1U);
    EXPECT_NEAR(phases[0], wrapHalfOpen(Phase{1.0} - Phase{0.001 * 48000.0}).value(), 1e-12);
    EXPECT_EQ(count(db, "MATCH (c:PartialPhaseCompensationSegment) "
                        "WHERE c.start_phase_delta = 0.0 AND c.end_phase_delta = 0.0 "
                        "RETURN count(c);"),
        2);
}

TEST_F(PrecomputationDb, APartialWithoutAPhasePhysicalTimelineFails) {
    addPartial(
        db, "P", {at(0.0, 0.1), at(100.0, 0.2)}, Amplitude::Matching, {}, PhaseTimeline::None);

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_EQ(report.partials.size(), 1U);
    ASSERT_FALSE(report.partials[0].ok());
    EXPECT_NE(report.partials[0].error->find("PhasePhysicalTimeline"), std::string::npos);
    EXPECT_EQ(count(db, "MATCH (s:PartialIntegrableSegment) RETURN count(s);"), 0);
}

TEST_F(PrecomputationDb, APhasePhysicalTimelineWithoutPointsFails) {
    addPartial(
        db, "P", {at(0.0, 0.1), at(100.0, 0.2)}, Amplitude::Matching, {}, PhaseTimeline::None);
    addPhaseTimeline(db, "P", {});

    const PrecomputationReport report = precomputeAll(db);

    ASSERT_FALSE(report.partials[0].ok());
    EXPECT_NE(report.partials[0].error->find("PhasePhysicalTimeline"), std::string::npos);
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
