// Tests for the transform stage.
//
// Two halves. The pure stages are tested directly on in-memory data. The
// whole run is then tested against the real engine-db schema, in memory,
// with small canonical-model scores built here.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::optionalDouble;
using rainbohz::rendering_engine::optionalInt64;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::transform::fitToLifeline;
using rainbohz::rendering_engine::transform::Knot;
using rainbohz::rendering_engine::transform::PartialSetTransformOutcome;
using rainbohz::rendering_engine::transform::PartialTransformError;
using rainbohz::rendering_engine::transform::PartialTransformOutcome;
using rainbohz::rendering_engine::transform::PhysicalPoint;
using rainbohz::rendering_engine::transform::placeKnots;
using rainbohz::rendering_engine::transform::requireSharedEndPoints;
using rainbohz::rendering_engine::transform::transformAll;
using rainbohz::rendering_engine::transform::TransformError;
using rainbohz::rendering_engine::transform::TransformReport;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// A knot for the pure-stage tests.
///
/// @param point  Index of the set point.
/// @param value  Value in canonical units.
/// @param curve  Curve type; linear by default.
/// @return The knot, with curve amount 0.
Knot knot(std::size_t point, double value, std::int64_t curve = 0) {
    return Knot{.point = point, .value = value, .curveType = curve, .curveAmount = 0.0};
}

/// A physical point for an expectation.
///
/// @param time   Samples.
/// @param value  Value.
/// @param curve  Curve type.
/// @return The point, with curve amount 0.
PhysicalPoint pp(double time, double value, std::int64_t curve = 0) {
    return PhysicalPoint{
        .time = SampleTime{time}, .value = value, .curveType = curve, .curveAmount = 0.0};
}

/// Formats a double as a Cypher literal that reads back exactly.
///
/// @param value  A finite value.
/// @return The value with enough digits to round-trip, always typed DOUBLE.
std::string literal(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << std::showpoint << value;
    return out.str();
}

/// One point of a musical timeline built by addMusicalSet().
struct MusicalPoint {
    /// POINT_AT.sync_point, ticks.
    std::int64_t sync = 0;

    /// POINT_AT.offset_seconds; nullopt leaves the property NULL.
    std::optional<double> offset = 0.0;

    /// Each partial's (frequency, amplitude) at this point, by partial name;
    /// nullopt for NULL. A partial absent from the list has no state here.
    std::vector<std::pair<std::string, std::pair<std::optional<double>, std::optional<double>>>>
        states;
};

/// One partial's state at a point, for MusicalPoint::states.
///
/// @param name       Partial.name.
/// @param frequency  Hz; nullopt for NULL.
/// @param amplitude  dB; nullopt for NULL.
/// @return The entry.
std::pair<std::string, std::pair<std::optional<double>, std::optional<double>>> state(
    const std::string& name, std::optional<double> frequency, std::optional<double> amplitude) {
    return {name, {frequency, amplitude}};
}

/// Adds a Score with a one-point TempoMap at `tempo` BPM and 960 ticks per
/// beat, and the SoundEntity and SoundSource every set hangs from.
///
/// @param[in,out] db  Database with the schema applied.
/// @param tempo       The held tempo, BPM.
/// @param lastHolds   TempoMapPoint.hold of the one point; false makes the
///                    map invalid.
void addScore(EngineDb& db, double tempo = 120.0, bool lastHolds = true) {
    db.execute("CREATE (s:Score {name: 'S'})-[:NEXT_POINT]->(m:TempoMap {name: 'M', "
               "ticks_per_beat: 960})-[:NEXT_POINT]->(:TempoMapPoint {position: 0, start_tempo: " +
               literal(tempo) + ", hold: " + (lastHolds ? "true" : "false") +
               "}) CREATE (s)-[:HAS_SOUNDENTITY]->(:SoundEntity {name: 'E'})"
               "-[:HAS_SOUNDSOURCE]->(:SoundSource {name: 'SS'});");
}

/// Adds a PartialSet under the SoundSource, with partials and a musical
/// timeline.
///
/// @param[in,out] db   Database with addScore() applied.
/// @param setName      PartialSet.name. Unique in the test.
/// @param partials     Partial names, created in this order so that their ids
///                     ascend.
/// @param points       The timeline's points, in order. Every POINT_AT leads
///                     to the Score's TempoMap.
/// @param withTimeline Whether to give the set a musical timeline at all.
void addMusicalSet(EngineDb& db, const std::string& setName,
    const std::vector<std::string>& partials, const std::vector<MusicalPoint>& points,
    bool withTimeline = true) {
    const std::string set = "MATCH (ps:PartialSet {name: '" + setName + "'}) ";
    db.execute("MATCH (ss:SoundSource {name: 'SS'}) CREATE (ss)-[:HAS_PARTIALSET]->(:PartialSet "
               "{name: '" +
               setName + "'});");
    for (const std::string& partial : partials) {
        std::string create = set;
        create += "CREATE (ps)-[:HAS_PARTIAL]->(:Partial {name: '" + partial + "'});";
        db.execute(create);
    }
    if (!withTimeline) {
        return;
    }
    db.execute(set + "CREATE (ps)-[:HAS_MUSICAL_TIMELINE]->(:PartialSetMusicalTimeline);");

    // Points have no name, so a point is found by its depth in the list:
    // this pattern binds the point `depth` hops from the timeline as `as`.
    const auto pointAtDepth = [&set](std::size_t depth, const std::string& as) {
        const std::string hops = std::to_string(depth);
        std::string pattern = set;
        pattern += "MATCH (ps)-[:HAS_MUSICAL_TIMELINE]->(:PartialSetMusicalTimeline)-[:NEXT_POINT*";
        pattern += hops + ".." + hops + "]->(" + as + ":PartialSetMusicalTimelinePoint) ";
        return pattern;
    };

    for (std::size_t i = 0; i < points.size(); ++i) {
        const MusicalPoint& point = points[i];
        // The point, linked from its predecessor (the timeline for the
        // first), with its POINT_AT and an empty PartialStateSet. Ladybug
        // needs the predecessor's label to create the relationship.
        std::string create = i == 0 ? set : pointAtDepth(i, "prev");
        if (i == 0) {
            create += "MATCH (ps)-[:HAS_MUSICAL_TIMELINE]->(prev:PartialSetMusicalTimeline) ";
        }
        create += "MATCH (m:TempoMap {name: 'M'}) CREATE (prev)-[:NEXT_POINT]->"
                  "(q:PartialSetMusicalTimelinePoint {phase_lock: false})-[:POINT_AT {sync_point: ";
        create += std::to_string(point.sync);
        if (point.offset.has_value()) {
            create += ", offset_seconds: " + literal(*point.offset);
        }
        create += "}]->(m) CREATE (q)-[:POINT_STATE]->(:PartialStateSet);";
        db.execute(create);

        for (const auto& [partial, values] : point.states) {
            std::string props;
            if (values.first.has_value()) {
                props += "frequency: " + literal(*values.first);
            }
            if (values.second.has_value()) {
                props += props.empty() ? "" : ", ";
                props += "amplitude: " + literal(*values.second);
            }
            std::string state = pointAtDepth(i + 1, "p");
            state += "MATCH (p)-[:POINT_STATE]->(ss:PartialStateSet) ";
            state += "MATCH (partial:Partial {name: '" + partial + "'}) ";
            state += "CREATE (:PartialState {" + props + "})-[:MEMBER_OF_SET_MANY_ONE]->(ss) ";
            state += "WITH partial, ss MATCH (st:PartialState)-[:MEMBER_OF_SET_MANY_ONE]->(ss) ";
            state += "WHERE NOT EXISTS { MATCH (st)-[:STATE_OF]->(:Partial) } ";
            state += "CREATE (st)-[:STATE_OF]->(partial);";
            db.execute(state);
        }
    }
}

/// The points of a partial's physical timeline of one kind, in list order.
///
/// @param db        Database to query.
/// @param partial   Partial.name.
/// @param timeline  "FrequencyPhysicalTimeline" or "AmplitudePhysicalTimeline".
/// @param column    "frequency" or "amplitude".
/// @return (time, value, curve type) per point, ordered by time.
std::vector<std::tuple<double, double, std::int64_t>> physicalPoints(EngineDb& db,
    const std::string& partial, const std::string& timeline, const std::string& column) {
    const std::vector<DbRow> rows =
        db.query("MATCH (:Partial {name: $name})-[:HAS_PHYSICAL_TIMELINE]->(:" + timeline +
                     ")-[:NEXT_POINT*1..30]->(x:" + timeline + "Point) RETURN x.time, x." + column +
                     ", x." + column + "_curve_type ORDER BY x.time;",
            {{"name", partial}});
    std::vector<std::tuple<double, double, std::int64_t>> points;
    points.reserve(rows.size());
    for (const DbRow& row : rows) {
        points.emplace_back(optionalDouble(row.at(0)).value(), optionalDouble(row.at(1)).value(),
            optionalInt64(row.at(2)).value());
    }
    return points;
}

/// Counts the rows of a query that returns one count.
///
/// @param db      Database to query.
/// @param cypher  A query returning a single integer.
/// @return That integer.
std::int64_t count(EngineDb& db, const std::string& cypher) {
    return requireInt64(db.query(cypher).at(0).at(0));
}

/// The outcome of the one partial named `name` in a report.
///
/// @param report  A report.
/// @param name    Partial.name.
/// @return The outcome. Fails the test and returns an empty one if absent.
PartialTransformOutcome outcomeOf(const TransformReport& report, const std::string& name) {
    for (const PartialSetTransformOutcome& set : report.sets) {
        for (const PartialTransformOutcome& partial : set.partials) {
            if (partial.partialName == name) {
                return partial;
            }
        }
    }
    ADD_FAILURE() << "no outcome for partial " << name;
    return {};
}

/// 440 Hz in radians per sample at 48 kHz.
constexpr double kA440 = 2.0 * std::numbers::pi * 440.0 / 48000.0;

/// An in-memory engine-db with the real schema and a 48 kHz EngineConfig.
class TransformDb : public ::testing::Test {
protected:
    /// Applies the schema and the EngineConfig.
    void SetUp() override {
        const auto report = loadCypherFile(db, RE_TEST_SCHEMA_FILE);
        ASSERT_TRUE(report.ok()) << "the engine schema did not load";
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    }

    EngineDb db;
};

// ---------------------------------------------------------------------------
// Pure stages
// ---------------------------------------------------------------------------

TEST(RequireSharedEndPoints, AcceptsSharedEnds) {
    const std::vector<Knot> frequency{knot(0, 440.0), knot(2, 440.0), knot(3, 220.0)};
    const std::vector<Knot> amplitude{knot(0, -6.0), knot(1, -6.0), knot(3, -144.0)};
    EXPECT_NO_THROW(requireSharedEndPoints(frequency, amplitude));
}

TEST(RequireSharedEndPoints, NeedsTwoKnotsOfEach) {
    const std::vector<Knot> one{knot(0, 440.0)};
    const std::vector<Knot> two{knot(0, -6.0), knot(1, -6.0)};
    EXPECT_THROW(requireSharedEndPoints(one, two), PartialTransformError);
    EXPECT_THROW(requireSharedEndPoints(two, one), PartialTransformError);
    const std::vector<Knot> none;
    EXPECT_THROW(requireSharedEndPoints(none, two), PartialTransformError);
}

TEST(RequireSharedEndPoints, RefusesDifferentFirstOrLastPoints) {
    const std::vector<Knot> frequency{knot(0, 440.0), knot(3, 220.0)};
    const std::vector<Knot> laterStart{knot(1, -6.0), knot(3, -6.0)};
    const std::vector<Knot> earlierEnd{knot(0, -6.0), knot(2, -6.0)};
    EXPECT_THROW(requireSharedEndPoints(frequency, laterStart), PartialTransformError);
    EXPECT_THROW(requireSharedEndPoints(frequency, earlierEnd), PartialTransformError);
}

TEST(PlaceKnots, UsesThePointTimesAndKeepsValuesAndCurves) {
    const std::vector<SampleTime> times{
        SampleTime{0.0}, SampleTime{100.0}, SampleTime{200.0}, SampleTime{300.0}};
    const std::vector<Knot> knots{knot(0, 440.0, 1), knot(2, 441.0), knot(3, 442.0, 3)};
    EXPECT_EQ(placeKnots(knots, times),
        (std::vector{pp(0.0, 440.0, 1), pp(200.0, 441.0), pp(300.0, 442.0, 3)}));
}

TEST(PlaceKnots, SquashesCollisions) {
    const std::vector<SampleTime> times{SampleTime{0.0}, SampleTime{100.0}, SampleTime{50.0}};
    const std::vector<Knot> knots{knot(0, 1.0), knot(1, 2.0), knot(2, 3.0)};
    EXPECT_EQ(placeKnots(knots, times), (std::vector{pp(0.0, 1.0), pp(100.0, 3.0)}));
}

TEST(FitToLifeline, MovesTheLastPointToTheEnd) {
    std::vector<PhysicalPoint> amplitude{pp(0.0, -6.0), pp(50.0, -6.0), pp(80.0, -90.0)};
    fitToLifeline(amplitude, SampleTime{100.0});
    EXPECT_EQ(amplitude, (std::vector{pp(0.0, -6.0), pp(50.0, -6.0), pp(100.0, -90.0)}));
}

TEST(FitToLifeline, DropsPointsAtOrAfterTheEnd) {
    std::vector<PhysicalPoint> amplitude{
        pp(0.0, -6.0), pp(100.0, -12.0), pp(120.0, -24.0), pp(150.0, -90.0)};
    fitToLifeline(amplitude, SampleTime{100.0});
    EXPECT_EQ(amplitude, (std::vector{pp(0.0, -6.0), pp(100.0, -90.0)}));
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

TEST_F(TransformDb, TransformsASimpleScore) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .offset = 0.0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .offset = 0.0, .states = {state("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.sampleRate.value(), 48000);
    ASSERT_EQ(report.tempoMaps.size(), 1U);
    EXPECT_EQ(report.tempoMaps[0].ticksPerBeat, 960);
    EXPECT_EQ(report.tempoMaps[0].pointCount, 1U);
    EXPECT_EQ(report.tempoMaps[0].name, "M");
    ASSERT_EQ(report.sets.size(), 1U);
    ASSERT_EQ(report.sets[0].partials.size(), 1U);
    EXPECT_EQ(report.sets[0].partials[0].frequencyPoints, 2U);
    EXPECT_EQ(report.sets[0].partials[0].amplitudePoints, 2U);

    // Two beats at 120 BPM is one second: 48000 samples.
    const auto frequency = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_EQ(frequency.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[0]), 0.0);
    EXPECT_DOUBLE_EQ(std::get<1>(frequency[0]), kA440);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[1]), 48000.0);
    EXPECT_EQ(std::get<2>(frequency[1]), 0);
    const auto amplitude = physicalPoints(db, "A", "AmplitudePhysicalTimeline", "amplitude");
    ASSERT_EQ(amplitude.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<1>(amplitude[0]), -6.0);
    EXPECT_DOUBLE_EQ(std::get<0>(amplitude[1]), 48000.0);
    EXPECT_DOUBLE_EQ(std::get<1>(amplitude[1]), -12.0);
}

TEST_F(TransformDb, ANullPropertyOrAMissingStateIsNotAKnot) {
    addScore(db);
    addMusicalSet(db, "P", {"A", "B"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0), state("B", 880.0, -6.0)}},
            // A: frequency NULL here; B: no state at all here.
            MusicalPoint{.sync = 960, .states = {state("A", std::nullopt, -12.0)}},
            MusicalPoint{
                .sync = 1920, .states = {state("A", 220.0, -18.0), state("B", 880.0, -144.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(outcomeOf(report, "A").frequencyPoints, 2U);
    EXPECT_EQ(outcomeOf(report, "A").amplitudePoints, 3U);
    EXPECT_EQ(outcomeOf(report, "B").frequencyPoints, 2U);
    EXPECT_EQ(outcomeOf(report, "B").amplitudePoints, 2U);
    const auto amplitudeA = physicalPoints(db, "A", "AmplitudePhysicalTimeline", "amplitude");
    ASSERT_EQ(amplitudeA.size(), 3U);
    EXPECT_DOUBLE_EQ(std::get<0>(amplitudeA[1]), 24000.0);
    EXPECT_DOUBLE_EQ(std::get<1>(amplitudeA[1]), -12.0);
}

TEST_F(TransformDb, ANullOffsetMeansZero) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .offset = std::nullopt, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 960, .offset = std::nullopt, .states = {state("A", 440.0, -6.0)}},
        });
    ASSERT_TRUE(transformAll(db).ok());
    const auto frequency = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_EQ(frequency.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[1]), 24000.0);
}

TEST_F(TransformDb, OffsetsMoveAndSquashPoints) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            // Pulled before zero: placed at zero.
            MusicalPoint{.sync = 0, .offset = -0.1, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 960, .offset = 0.0, .states = {state("A", 441.0, -6.0)}},
            // One second by the tempo map, pulled back to 0.25 s: it lands
            // before the tail at 0.5 s and replaces it.
            MusicalPoint{.sync = 1920, .offset = -0.75, .states = {state("A", 442.0, -144.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    const auto frequency = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_EQ(frequency.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[0]), 0.0);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[1]), 24000.0);
    EXPECT_DOUBLE_EQ(std::get<1>(frequency[1]), 2.0 * std::numbers::pi * 442.0 / 48000.0);
    const auto amplitude = physicalPoints(db, "A", "AmplitudePhysicalTimeline", "amplitude");
    ASSERT_EQ(amplitude.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<1>(amplitude[1]), -144.0);
}

TEST_F(TransformDb, TheFrequencyEnvelopeDefinesTheLifeline) {
    addScore(db);
    // Frequency collides at its last point and is delayed to 0.5 s; the
    // amplitude envelope, whose middle point is absent for frequency, ends
    // there too.
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .offset = 0.0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 960, .offset = 0.0, .states = {state("A", 441.0, std::nullopt)}},
            MusicalPoint{.sync = 1920, .offset = -0.8, .states = {state("A", std::nullopt, -12.0)}},
            MusicalPoint{.sync = 1920, .offset = -0.75, .states = {state("A", 442.0, -144.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok()) << report.sets[0].partials[0].error.value_or("");
    const auto frequency = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    const auto amplitude = physicalPoints(db, "A", "AmplitudePhysicalTimeline", "amplitude");
    ASSERT_EQ(frequency.size(), 2U);
    ASSERT_EQ(amplitude.size(), 3U);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency.back()), 24000.0);
    EXPECT_DOUBLE_EQ(std::get<0>(amplitude.back()), 24000.0);
    // offset_seconds is a FLOAT column, so -0.8 arrives as its float32
    // neighbour; the time is exact for that value.
    EXPECT_DOUBLE_EQ(
        std::get<0>(amplitude[1]), (1.0 + static_cast<double>(static_cast<float>(-0.8))) * 48000.0);
}

TEST_F(TransformDb, RerunningIsIdempotent) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    ASSERT_TRUE(transformAll(db).ok());
    const auto before = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_TRUE(transformAll(db).ok());
    EXPECT_EQ(physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency"), before);
    EXPECT_EQ(count(db, "MATCH (:FrequencyPhysicalTimeline) RETURN count(*);"), 1);
    EXPECT_EQ(count(db, "MATCH (:AmplitudePhysicalTimeline) RETURN count(*);"), 1);
    EXPECT_EQ(count(db, "MATCH (:FrequencyPhysicalTimelinePoint) RETURN count(*);"), 2);
    EXPECT_EQ(count(db, "MATCH (:AmplitudePhysicalTimelinePoint) RETURN count(*);"), 2);
}

TEST_F(TransformDb, ReplacesExistingPhysicalTimelines) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    db.execute("MATCH (p:Partial {name: 'A'}) CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->"
               "(:FrequencyPhysicalTimeline)-[:NEXT_POINT]->"
               "(:FrequencyPhysicalTimelinePoint {time: 1.0, frequency: 9.0})-[:NEXT_POINT]->"
               "(:FrequencyPhysicalTimelinePoint {time: 2.0, frequency: 9.0});");
    ASSERT_TRUE(transformAll(db).ok());
    EXPECT_EQ(count(db, "MATCH (:FrequencyPhysicalTimeline) RETURN count(*);"), 1);
    EXPECT_EQ(count(db, "MATCH (f:FrequencyPhysicalTimelinePoint) WHERE f.frequency = 9.0 "
                        "RETURN count(*);"),
        0);
    const auto frequency = physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_EQ(frequency.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<0>(frequency[1]), 48000.0);
}

TEST_F(TransformDb, ASetWithoutAMusicalTimelineIsUntouched) {
    addScore(db);
    addMusicalSet(db, "Physical", {"X"}, {}, /*withTimeline=*/false);
    db.execute("MATCH (p:Partial {name: 'X'}) CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->"
               "(:FrequencyPhysicalTimeline)-[:NEXT_POINT]->"
               "(:FrequencyPhysicalTimelinePoint {time: 0.0, frequency: 0.1})-[:NEXT_POINT]->"
               "(:FrequencyPhysicalTimelinePoint {time: 10.0, frequency: 0.1});");
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(report.sets.size(), 1U);
    EXPECT_EQ(report.partialCount(), 1U);
    const auto x = physicalPoints(db, "X", "FrequencyPhysicalTimeline", "frequency");
    ASSERT_EQ(x.size(), 2U);
    EXPECT_DOUBLE_EQ(std::get<0>(x[1]), 10.0);
}

TEST_F(TransformDb, NothingToTransformIsFine) {
    addScore(db);
    const TransformReport report = transformAll(db);
    EXPECT_TRUE(report.ok());
    EXPECT_TRUE(report.sets.empty());
    EXPECT_EQ(report.tempoMaps.size(), 1U);
    EXPECT_EQ(report.partialCount(), 0U);
}

TEST(Transform, TheExampleScoreHasNothingToTransform) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_EXAMPLE_STATE).ok());
    const TransformReport report = transformAll(db);
    EXPECT_TRUE(report.ok());
    EXPECT_TRUE(report.sets.empty());
    EXPECT_TRUE(report.tempoMaps.empty());
}

TEST_F(TransformDb, RefusesAMissingOrUnusableEngineConfig) {
    db.execute("MATCH (c:EngineConfig) SET c.sampleRate = NULL;");
    EXPECT_THROW(static_cast<void>(transformAll(db)), TransformError);
    db.execute("MATCH (c:EngineConfig) DELETE c;");
    EXPECT_THROW(static_cast<void>(transformAll(db)), TransformError);
}

TEST_F(TransformDb, AnInvalidTempoMapIsFatal) {
    addScore(db, 120.0, /*lastHolds=*/false);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    EXPECT_THROW(static_cast<void>(transformAll(db)), TransformError);
    EXPECT_EQ(count(db, "MATCH (:FrequencyPhysicalTimeline) RETURN count(*);"), 0);
}

TEST_F(TransformDb, ASynchronisationEdgeIsRefused) {
    addScore(db);
    db.execute("MATCH (m:TempoMap) CREATE (child:TempoMap {name: 'child', ticks_per_beat: 960})"
               "-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: 960, sync_point_child: 0}]"
               "->(m);");
    try {
        static_cast<void>(transformAll(db));
        FAIL() << "accepted a synchronisation edge";
    } catch (const TransformError& e) {
        EXPECT_NE(std::string{e.what()}.find("TEMPO_MAP_SYNCHRONIZATION_POINT"), std::string::npos);
    }
}

TEST_F(TransformDb, ASetWhoseScoreHasNoTempoMapFails) {
    db.execute("CREATE (s:Score {name: 'S'})-[:HAS_SOUNDENTITY]->(:SoundEntity {name: 'E'})"
               "-[:HAS_SOUNDSOURCE]->(:SoundSource {name: 'SS'});");
    db.execute("CREATE (:TempoMap {name: 'M', ticks_per_beat: 960})-[:NEXT_POINT]->"
               "(:TempoMapPoint {position: 0, start_tempo: 120.0, hold: true});");
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_FALSE(report.ok());
    ASSERT_EQ(report.sets.size(), 1U);
    ASSERT_TRUE(report.sets[0].error.has_value());
    EXPECT_NE(report.sets[0].error->find("no TempoMap"), std::string::npos);
    EXPECT_TRUE(report.sets[0].partials.empty());
    EXPECT_EQ(report.failureCount(), 1U);
}

TEST_F(TransformDb, ASetLevelFaultFailsTheSetAndNotTheOthers) {
    addScore(db);
    addMusicalSet(db, "Good", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    addMusicalSet(db, "Bad", {"B"},
        {
            MusicalPoint{.sync = 960, .states = {state("B", 440.0, -6.0)}},
            // Musical order goes backwards.
            MusicalPoint{.sync = 0, .states = {state("B", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_FALSE(report.ok());
    ASSERT_EQ(report.sets.size(), 2U);
    EXPECT_TRUE(report.sets[0].ok());
    ASSERT_TRUE(report.sets[1].error.has_value());
    EXPECT_NE(report.sets[1].error->find("non-decreasing"), std::string::npos);
    EXPECT_EQ(physicalPoints(db, "A", "FrequencyPhysicalTimeline", "frequency").size(), 2U);
    EXPECT_EQ(physicalPoints(db, "B", "FrequencyPhysicalTimeline", "frequency").size(), 0U);
}

TEST_F(TransformDb, APointWithoutPointAtFailsTheSet) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    db.execute("MATCH (:PartialSetMusicalTimelinePoint)-[r:POINT_AT]->() WITH r LIMIT 1 DELETE r;");
    const TransformReport report = transformAll(db);
    ASSERT_EQ(report.sets.size(), 1U);
    ASSERT_TRUE(report.sets[0].error.has_value());
    EXPECT_NE(report.sets[0].error->find("no POINT_AT"), std::string::npos);
}

TEST_F(TransformDb, AStateOfAPartialOutsideTheSetFailsTheSet) {
    addScore(db);
    addMusicalSet(db, "Other", {"Z"}, {}, /*withTimeline=*/false);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0), state("Z", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_EQ(report.sets.size(), 1U);
    ASSERT_TRUE(report.sets[0].error.has_value());
    EXPECT_NE(report.sets[0].error->find("not a member"), std::string::npos);
}

TEST_F(TransformDb, PartialLevelFaultsFailOnlyThatPartial) {
    addScore(db);
    addMusicalSet(db, "P", {"A", "B", "C", "D"},
        {
            MusicalPoint{.sync = 0,
                .states = {state("A", 440.0, -6.0), state("B", 440.0, std::nullopt),
                    state("C", 440.0, -6.0), state("D", 440.0, 3.0)}},
            MusicalPoint{.sync = 960, .states = {state("A", 440.0, -6.0), state("B", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920,
                .states = {state("A", 440.0, -12.0), state("B", 440.0, -12.0),
                    state("D", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_FALSE(report.ok());
    EXPECT_TRUE(outcomeOf(report, "A").ok());
    // B: amplitude not defined at the first point.
    EXPECT_NE(outcomeOf(report, "B").error.value_or("").find("first point"), std::string::npos);
    // C: defined at one point only.
    EXPECT_NE(outcomeOf(report, "C").error.value_or("").find("at least two"), std::string::npos);
    // D: amplitude above 0 dB.
    EXPECT_NE(outcomeOf(report, "D").error.value_or("").find("[-144, 0]"), std::string::npos);
    EXPECT_EQ(report.failureCount(), 3U);
    EXPECT_EQ(report.partialCount(), 4U);
    EXPECT_EQ(count(db, "MATCH (:FrequencyPhysicalTimeline) RETURN count(*);"), 1);
}

TEST_F(TransformDb, ACollapsedLifelineFailsThePartial) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 960, .offset = 0.0, .states = {state("A", 440.0, -6.0)}},
            // Pulled back onto the first point exactly.
            MusicalPoint{.sync = 1920, .offset = -0.5, .states = {state("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_NE(outcomeOf(report, "A").error.value_or("").find("collapsed"), std::string::npos);
}

} // namespace
