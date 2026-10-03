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
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::transform::collapseFreeRuns;
using rainbohz::rendering_engine::transform::fitToLifeline;
using rainbohz::rendering_engine::transform::Knot;
using rainbohz::rendering_engine::transform::PartialSetTransformOutcome;
using rainbohz::rendering_engine::transform::PartialTransformError;
using rainbohz::rendering_engine::transform::PartialTransformOutcome;
using rainbohz::rendering_engine::transform::PhaseKnot;
using rainbohz::rendering_engine::transform::phaseKnotsOf;
using rainbohz::rendering_engine::transform::PhasePoint;
using rainbohz::rendering_engine::transform::PhysicalPoint;
using rainbohz::rendering_engine::transform::placeKnots;
using rainbohz::rendering_engine::transform::placePhaseKnots;
using rainbohz::rendering_engine::transform::requireSharedEndPoints;
using rainbohz::rendering_engine::transform::requireStatesWithinLifeline;
using rainbohz::rendering_engine::transform::requireValidPhase;
using rainbohz::rendering_engine::transform::StatePhase;
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

/// A phase point for the pure-stage tests.
///
/// @param point    Index of the set point.
/// @param degrees  Target phase; nullopt for a NULL point.
/// @return The phase knot.
PhaseKnot pk(std::size_t point, std::optional<double> degrees) {
    return PhaseKnot{.point = point, .degrees = degrees};
}

/// A state's phase fields for the pure-stage tests.
///
/// @param point    Index of the set point.
/// @param degrees  PartialState.phase; nullopt for NULL.
/// @param freeRun  PartialState.phase_free_run; nullopt for NULL.
/// @return The fields.
StatePhase sp(std::size_t point, std::optional<double> degrees, std::optional<bool> freeRun) {
    return StatePhase{.point = point, .degrees = degrees, .freeRun = freeRun};
}

/// A target state's phase fields: `degrees` with phase_free_run false.
StatePhase target(std::size_t point, double degrees) {
    return sp(point, degrees, false);
}

/// A free-running state's phase fields: NULL with phase_free_run true.
StatePhase freeRun(std::size_t point) {
    return sp(point, std::nullopt, true);
}

/// A default state's phase fields: both NULL.
StatePhase byDefault(std::size_t point) {
    return sp(point, std::nullopt, std::nullopt);
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

/// A phase point for an expectation.
///
/// @param time     Samples.
/// @param radians  Target phase; nullopt for a NULL point.
/// @return The point.
PhasePoint php(double time, std::optional<double> radians) {
    return PhasePoint{.time = SampleTime{time},
        .phase = radians.has_value() ? std::optional<Phase>{Phase{*radians}} : std::nullopt};
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

/// One partial's state at a point, for MusicalPoint::states.
struct StateSpec {
    /// Partial.name.
    std::string partial;

    /// Hz; nullopt for NULL.
    std::optional<double> frequency;

    /// dB; nullopt for NULL.
    std::optional<double> amplitude;

    /// Degrees; nullopt for NULL.
    std::optional<double> phase;

    /// PartialState.phase_free_run; nullopt for NULL.
    std::optional<bool> freeRun;
};

/// One point of a musical timeline built by addMusicalSet().
struct MusicalPoint {
    /// POINT_AT.sync_point, ticks.
    std::int64_t sync = 0;

    /// POINT_AT.offset_seconds; nullopt leaves the property NULL.
    std::optional<double> offset = 0.0;

    /// Each partial's state at this point. A partial absent from the list
    /// has no state here.
    std::vector<StateSpec> states;
};

/// One partial's state at a point, for MusicalPoint::states, with valid
/// phase fields (transform.md §6.6).
///
/// @param name       Partial.name.
/// @param frequency  Hz; nullopt for NULL.
/// @param amplitude  dB; nullopt for NULL.
/// @param phase      A target, degrees, written with phase_free_run false;
///                   nullopt (the default) for a default state, both NULL.
/// @return The entry.
StateSpec state(const std::string& name, std::optional<double> frequency,
    std::optional<double> amplitude, std::optional<double> phase = std::nullopt) {
    return {.partial = name,
        .frequency = frequency,
        .amplitude = amplitude,
        .phase = phase,
        .freeRun = phase.has_value() ? std::optional<bool>{false} : std::nullopt};
}

/// A free-running state: phase NULL, phase_free_run true.
///
/// @param name       Partial.name.
/// @param frequency  Hz; nullopt for NULL.
/// @param amplitude  dB; nullopt for NULL.
/// @return The entry.
StateSpec freeState(
    const std::string& name, std::optional<double> frequency, std::optional<double> amplitude) {
    return {.partial = name,
        .frequency = frequency,
        .amplitude = amplitude,
        .phase = std::nullopt,
        .freeRun = true};
}

/// A state with the phase fields exactly as given, valid or not.
///
/// @param name       Partial.name.
/// @param frequency  Hz; nullopt for NULL.
/// @param amplitude  dB; nullopt for NULL.
/// @param phase      Degrees; nullopt for NULL.
/// @param freeRun    phase_free_run; nullopt for NULL.
/// @return The entry.
StateSpec rawState(const std::string& name, std::optional<double> frequency,
    std::optional<double> amplitude, std::optional<double> phase, std::optional<bool> freeRun) {
    return {.partial = name,
        .frequency = frequency,
        .amplitude = amplitude,
        .phase = phase,
        .freeRun = freeRun};
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

/// The property map of a PartialState, with NULL values left out.
///
/// @param frequency  Hz; nullopt for NULL.
/// @param amplitude  dB; nullopt for NULL.
/// @param phase      Degrees; nullopt for NULL.
/// @param freeRun    phase_free_run; nullopt for NULL.
/// @return e.g. "frequency: 440.000, phase: 90.0000, phase_free_run: false",
///         without braces.
std::string stateProperties(const std::optional<double>& frequency,
    const std::optional<double>& amplitude, const std::optional<double>& phase,
    const std::optional<bool>& freeRun) {
    std::string props;
    for (const auto& [name, value] : {std::pair{"frequency", frequency},
             std::pair{"amplitude", amplitude}, std::pair{"phase", phase}}) {
        if (value.has_value()) {
            props += props.empty() ? "" : ", ";
            props += std::string{name} + ": " + literal(*value);
        }
    }
    if (freeRun.has_value()) {
        props += props.empty() ? "" : ", ";
        props += std::string{"phase_free_run: "} + (*freeRun ? "true" : "false");
    }
    return props;
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
                  "(q:PartialSetMusicalTimelinePoint)-[:POINT_AT {sync_point: ";
        create += std::to_string(point.sync);
        if (point.offset.has_value()) {
            create += ", offset_seconds: " + literal(*point.offset);
        }
        create += "}]->(m) CREATE (q)-[:POINT_STATE]->(:PartialStateSet);";
        db.execute(create);

        for (const auto& [partial, frequency, amplitude, phase, freeRun] : point.states) {
            const std::string props = stateProperties(frequency, amplitude, phase, freeRun);
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

/// The points of a partial's PhasePhysicalTimeline, in list order.
///
/// @param db       Database to query.
/// @param partial  Partial.name.
/// @return (time, phase or nullopt) per point, ordered by time.
std::vector<std::pair<double, std::optional<double>>> phasePoints(
    EngineDb& db, const std::string& partial) {
    const std::vector<DbRow> rows =
        db.query("MATCH (:Partial {name: $name})-[:HAS_PHYSICAL_TIMELINE]->"
                 "(:PhasePhysicalTimeline)-[:NEXT_POINT*1..30]->(x:PhasePhysicalTimelinePoint) "
                 "RETURN x.time, x.phase ORDER BY x.time;",
            {{"name", partial}});
    std::vector<std::pair<double, std::optional<double>>> points;
    points.reserve(rows.size());
    for (const DbRow& row : rows) {
        points.emplace_back(optionalDouble(row.at(0)).value(), optionalDouble(row.at(1)));
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

TEST(RequireStatesWithinLifeline, AcceptsStatesOnAndBetweenTheEnds) {
    const std::vector<Knot> frequency{knot(1, 440.0), knot(3, 440.0)};
    const std::vector<StatePhase> states{byDefault(1), target(2, 90.0), freeRun(3)};
    EXPECT_NO_THROW(requireStatesWithinLifeline(frequency, states));
}

TEST(RequireStatesWithinLifeline, RefusesAStateBeforeOrAfterTheLifeline) {
    const std::vector<Knot> frequency{knot(1, 440.0), knot(3, 440.0)};
    const std::vector<StatePhase> before{freeRun(0), target(1, 0.0), target(3, 0.0)};
    const std::vector<StatePhase> after{target(1, 0.0), target(3, 0.0), target(4, 0.0)};
    EXPECT_THROW(requireStatesWithinLifeline(frequency, before), PartialTransformError);
    EXPECT_THROW(requireStatesWithinLifeline(frequency, after), PartialTransformError);
}

TEST(RequireStatesWithinLifeline, ADefaultStateOutsideTheLifelineIsRefusedToo) {
    // It plays no role in phase, but it is still a state with no frequency.
    const std::vector<Knot> frequency{knot(1, 440.0), knot(3, 440.0)};
    const std::vector<StatePhase> states{byDefault(1), byDefault(3), byDefault(4)};
    EXPECT_THROW(requireStatesWithinLifeline(frequency, states), PartialTransformError);
}

TEST(RequireValidPhase, AcceptsTheThreeValidCombinations) {
    const std::vector<StatePhase> states{
        target(0, -180.0), freeRun(1), byDefault(2), target(3, 180.0)};
    EXPECT_NO_THROW(requireValidPhase(states));
}

TEST(RequireValidPhase, RefusesEveryOtherCombination) {
    const std::vector<StatePhase> phaseWithFreeRun{target(0, 0.0), sp(1, 90.0, true)};
    const std::vector<StatePhase> phaseWithNull{target(0, 0.0), sp(1, 90.0, std::nullopt)};
    const std::vector<StatePhase> falseWithNoPhase{target(0, 0.0), sp(1, std::nullopt, false)};
    EXPECT_THROW(requireValidPhase(phaseWithFreeRun), PartialTransformError);
    EXPECT_THROW(requireValidPhase(phaseWithNull), PartialTransformError);
    EXPECT_THROW(requireValidPhase(falseWithNoPhase), PartialTransformError);
}

TEST(RequireValidPhase, RefusesATargetOutOfRange) {
    const std::vector<StatePhase> states{target(0, 181.0)};
    try {
        requireValidPhase(states);
        FAIL() << "expected PartialTransformError";
    } catch (const PartialTransformError& error) {
        EXPECT_NE(std::string{error.what()}.find("[-180, 180]"), std::string::npos);
    }
}

TEST(PhaseKnotsOf, TargetsAndFreeRunsArePointsDefaultsAreNot) {
    const std::vector<StatePhase> states{
        target(0, 10.0), byDefault(1), freeRun(2), byDefault(3), target(4, 20.0)};
    EXPECT_EQ(phaseKnotsOf(states), (std::vector{pk(0, 10.0), pk(2, std::nullopt), pk(4, 20.0)}));
}

TEST(PhaseKnotsOf, ADefaultFirstStateRunsFreeToTheFirstTarget) {
    const std::vector<StatePhase> states{byDefault(0), byDefault(1), target(2, 45.0), byDefault(3)};
    EXPECT_EQ(phaseKnotsOf(states), (std::vector{pk(0, std::nullopt), pk(2, 45.0)}));
}

TEST(PhaseKnotsOf, NoPhaseAnywhereGivesOneFreePoint) {
    const std::vector<StatePhase> states{byDefault(0), byDefault(1), byDefault(2)};
    EXPECT_EQ(phaseKnotsOf(states), (std::vector{pk(0, std::nullopt)}));
}

TEST(PlacePhaseKnots, ConvertsDegreesAndKeepsNulls) {
    const std::vector<SampleTime> times{SampleTime{0.0}, SampleTime{100.0}, SampleTime{200.0}};
    const std::vector<PhaseKnot> knots{pk(0, std::nullopt), pk(1, 90.0), pk(2, -180.0)};
    EXPECT_EQ(placePhaseKnots(knots, times),
        (std::vector{php(0.0, std::nullopt), php(100.0, std::numbers::pi / 2.0),
            php(200.0, -std::numbers::pi)}));
}

TEST(PlacePhaseKnots, HalfACycleIsExactlyPi) {
    const std::vector<SampleTime> times{SampleTime{0.0}, SampleTime{100.0}};
    const std::vector<PhaseKnot> knots{pk(0, 180.0), pk(1, -180.0)};
    EXPECT_EQ(placePhaseKnots(knots, times),
        (std::vector{php(0.0, std::numbers::pi), php(100.0, -std::numbers::pi)}));
}

TEST(PlacePhaseKnots, ALaterNullWinsACollision) {
    const std::vector<SampleTime> times{SampleTime{0.0}, SampleTime{100.0}, SampleTime{50.0}};
    const std::vector<PhaseKnot> knots{pk(0, 0.0), pk(1, 90.0), pk(2, std::nullopt)};
    EXPECT_EQ(
        placePhaseKnots(knots, times), (std::vector{php(0.0, 0.0), php(100.0, std::nullopt)}));
}

TEST(FitToLifeline, MovesALatePhasePointBackToTheEnd) {
    std::vector<PhasePoint> phase{php(0.0, 0.0), php(100.0, 1.0), php(150.0, std::nullopt)};
    fitToLifeline(phase, SampleTime{100.0});
    EXPECT_EQ(phase, (std::vector{php(0.0, 0.0), php(100.0, std::nullopt)}));
}

TEST(FitToLifeline, LeavesAnEarlyLastPhasePointWhereItIs) {
    std::vector<PhasePoint> phase{php(0.0, std::nullopt), php(60.0, 1.0)};
    const std::vector<PhasePoint> before = phase;
    fitToLifeline(phase, SampleTime{100.0});
    EXPECT_EQ(phase, before);
}

TEST(FitToLifeline, KeepsASinglePhasePoint) {
    std::vector<PhasePoint> phase{php(0.0, std::nullopt)};
    fitToLifeline(phase, SampleTime{100.0});
    EXPECT_EQ(phase, (std::vector{php(0.0, std::nullopt)}));
}

TEST(CollapseFreeRuns, KeepsOnlyTheEndsOfARunOfNulls) {
    std::vector<PhasePoint> phase{php(0.0, 1.0), php(10.0, std::nullopt), php(20.0, std::nullopt),
        php(30.0, std::nullopt), php(40.0, std::nullopt), php(50.0, 2.0)};
    collapseFreeRuns(phase);
    EXPECT_EQ(phase, (std::vector{php(0.0, 1.0), php(10.0, std::nullopt), php(40.0, std::nullopt),
                         php(50.0, 2.0)}));
}

TEST(CollapseFreeRuns, KeepsTheFirstAndLastPoints) {
    std::vector<PhasePoint> phase{
        php(0.0, std::nullopt), php(10.0, std::nullopt), php(20.0, std::nullopt)};
    collapseFreeRuns(phase);
    EXPECT_EQ(phase, (std::vector{php(0.0, std::nullopt), php(20.0, std::nullopt)}));
}

TEST(CollapseFreeRuns, LeavesRunsOfTwoAndSeparateRunsAlone) {
    std::vector<PhasePoint> phase{php(0.0, std::nullopt), php(10.0, std::nullopt), php(20.0, 1.0),
        php(30.0, std::nullopt), php(40.0, 2.0)};
    const std::vector<PhasePoint> before = phase;
    collapseFreeRuns(phase);
    EXPECT_EQ(phase, before);
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

// ---------------------------------------------------------------------------
// Phase (§6.6)
// ---------------------------------------------------------------------------

TEST_F(TransformDb, WritesPhaseTargetsInRadians) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0, 0.0)}},
            MusicalPoint{.sync = 960, .states = {state("A", std::nullopt, -6.0, -90.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0, 180.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(outcomeOf(report, "A").phasePoints, 3U);
    EXPECT_EQ(phasePoints(db, "A"),
        (std::vector<std::pair<double, std::optional<double>>>{
            {0.0, 0.0}, {24000.0, -std::numbers::pi / 2.0}, {48000.0, std::numbers::pi}}));
}

TEST_F(TransformDb, AFreeRunningStateIsANullPoint) {
    addScore(db);
    // A runs free to the middle point and then to its target. B's middle
    // state is a default one, so B's target is met over the whole span.
    addMusicalSet(db, "P", {"A", "B"},
        {
            MusicalPoint{
                .sync = 0, .states = {freeState("A", 440.0, -6.0), state("B", 440.0, -6.0, 0.0)}},
            MusicalPoint{
                .sync = 960, .states = {freeState("A", 440.0, -6.0), state("B", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920,
                .states = {state("A", 440.0, -12.0, 45.0), state("B", 440.0, -12.0, 45.0)}},
        });
    ASSERT_TRUE(transformAll(db).ok());
    const double target = std::numbers::pi / 4.0;
    EXPECT_EQ(phasePoints(db, "A"),
        (std::vector<std::pair<double, std::optional<double>>>{
            {0.0, std::nullopt}, {24000.0, std::nullopt}, {48000.0, target}}));
    EXPECT_EQ(phasePoints(db, "B"),
        (std::vector<std::pair<double, std::optional<double>>>{{0.0, 0.0}, {48000.0, target}}));
}

TEST_F(TransformDb, DefaultStatesPlayNoRoleInPhase) {
    addScore(db);
    // A starts and ends with default states: it runs free from the start to
    // its one target, and its last phase point is that target, not the end.
    // B says nothing about phase: one NULL point at the start.
    addMusicalSet(db, "P", {"A", "B"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0), state("B", 440.0, -6.0)}},
            MusicalPoint{
                .sync = 960, .states = {state("A", 440.0, -6.0, -90.0), state("B", 440.0, -6.0)}},
            MusicalPoint{
                .sync = 1920, .states = {state("A", 440.0, -12.0), state("B", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(phasePoints(db, "A"), (std::vector<std::pair<double, std::optional<double>>>{
                                        {0.0, std::nullopt}, {24000.0, -std::numbers::pi / 2.0}}));
    EXPECT_EQ(outcomeOf(report, "B").phasePoints, 1U);
    EXPECT_EQ(phasePoints(db, "B"),
        (std::vector<std::pair<double, std::optional<double>>>{{0.0, std::nullopt}}));
}

TEST_F(TransformDb, ADefaultStateNeverWinsACollision) {
    addScore(db);
    // The last state is pulled back onto the 90 degree target at 0.5 s.
    // Being a default state it is no phase point, so the target stays.
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .offset = 0.0, .states = {state("A", 440.0, -6.0, 0.0)}},
            MusicalPoint{.sync = 960, .offset = 0.0, .states = {state("A", 441.0, -6.0, 90.0)}},
            MusicalPoint{.sync = 1920, .offset = -0.75, .states = {state("A", 442.0, -144.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok()) << report.sets[0].partials[0].error.value_or("");
    EXPECT_EQ(phasePoints(db, "A"), (std::vector<std::pair<double, std::optional<double>>>{
                                        {0.0, 0.0}, {24000.0, std::numbers::pi / 2.0}}));
}

TEST_F(TransformDb, RunsOfFreeRunningPointsAreCollapsed) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 480, .states = {freeState("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 960, .states = {state("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1440, .states = {freeState("A", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920, .states = {freeState("A", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok());
    EXPECT_EQ(outcomeOf(report, "A").frequencyPoints, 5U);
    // The default first state's NULL point starts the run; the default at
    // 960 is no point, so the run is 0, 12000, 36000, 48000.
    EXPECT_EQ(outcomeOf(report, "A").phasePoints, 2U);
    EXPECT_EQ(phasePoints(db, "A"), (std::vector<std::pair<double, std::optional<double>>>{
                                        {0.0, std::nullopt}, {48000.0, std::nullopt}}));
}

TEST_F(TransformDb, PhasePointsAreSquashedAndFittedToTheLifeline) {
    addScore(db);
    // As TheFrequencyEnvelopeDefinesTheLifeline: the last frequency point is
    // pulled back onto the tail at 0.5 s. In the phase points, the last
    // state collides with the 90 degree target at 0.625 s and replaces it;
    // the fit then moves it back to the lifeline's end. The default state at
    // 0.5 s is no phase point.
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .offset = 0.0, .states = {state("A", 440.0, -6.0, 0.0)}},
            MusicalPoint{.sync = 960, .offset = 0.0, .states = {state("A", 441.0, -6.0)}},
            MusicalPoint{
                .sync = 1920, .offset = -0.375, .states = {state("A", std::nullopt, -12.0, 90.0)}},
            MusicalPoint{
                .sync = 1920, .offset = -0.75, .states = {state("A", 442.0, -144.0, -90.0)}},
        });
    const TransformReport report = transformAll(db);
    ASSERT_TRUE(report.ok()) << report.sets[0].partials[0].error.value_or("");
    EXPECT_EQ(phasePoints(db, "A"), (std::vector<std::pair<double, std::optional<double>>>{
                                        {0.0, 0.0}, {24000.0, -std::numbers::pi / 2.0}}));
}

TEST_F(TransformDb, RerunningReplacesThePhaseTimeline) {
    addScore(db);
    addMusicalSet(db, "P", {"A"},
        {
            MusicalPoint{.sync = 0, .states = {state("A", 440.0, -6.0, 10.0)}},
            MusicalPoint{.sync = 1920, .states = {state("A", 440.0, -12.0, 20.0)}},
        });
    ASSERT_TRUE(transformAll(db).ok());
    const auto before = phasePoints(db, "A");
    ASSERT_TRUE(transformAll(db).ok());
    EXPECT_EQ(phasePoints(db, "A"), before);
    EXPECT_EQ(count(db, "MATCH (:PhasePhysicalTimeline) RETURN count(*);"), 1);
    EXPECT_EQ(count(db, "MATCH (:PhasePhysicalTimelinePoint) RETURN count(*);"), 2);
}

TEST_F(TransformDb, PhaseFaultsFailOnlyThatPartial) {
    addScore(db);
    addMusicalSet(db, "P", {"A", "B", "C"},
        {
            // C: a state before its lifeline begins.
            MusicalPoint{.sync = 0,
                .states = {state("A", 440.0, -6.0), state("B", 440.0, -6.0, 181.0),
                    state("C", std::nullopt, std::nullopt)}},
            MusicalPoint{.sync = 960, .states = {state("C", 440.0, -6.0)}},
            MusicalPoint{.sync = 1920,
                .states = {state("A", 440.0, -12.0), state("B", 440.0, -12.0),
                    state("C", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_TRUE(outcomeOf(report, "A").ok());
    // B: phase outside [-180, 180].
    EXPECT_NE(outcomeOf(report, "B").error.value_or("").find("[-180, 180]"), std::string::npos);
    EXPECT_NE(outcomeOf(report, "C").error.value_or("").find("lifeline"), std::string::npos);
    EXPECT_EQ(report.failureCount(), 2U);
}

TEST_F(TransformDb, InvalidPhaseCombinationsFailOnlyThatPartial) {
    addScore(db);
    addMusicalSet(db, "P", {"A", "B", "C", "D"},
        {
            MusicalPoint{.sync = 0,
                .states = {state("A", 440.0, -6.0, 0.0), rawState("B", 440.0, -6.0, 90.0, true),
                    rawState("C", 440.0, -6.0, 90.0, std::nullopt),
                    rawState("D", 440.0, -6.0, std::nullopt, false)}},
            MusicalPoint{.sync = 1920,
                .states = {state("A", 440.0, -12.0), state("B", 440.0, -12.0),
                    state("C", 440.0, -12.0), state("D", 440.0, -12.0)}},
        });
    const TransformReport report = transformAll(db);
    EXPECT_TRUE(outcomeOf(report, "A").ok());
    for (const std::string partial : {"B", "C", "D"}) {
        EXPECT_NE(
            outcomeOf(report, partial).error.value_or("").find("phase_free_run"), std::string::npos)
            << partial;
    }
    EXPECT_EQ(report.failureCount(), 3U);
}

} // namespace
