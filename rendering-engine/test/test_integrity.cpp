// Tests for the engine-db integrity checks.
//
// The list analysis is tested directly on in-memory ids. The checks are then
// tested against the real engine-db schema, in memory, with small scores
// built here -- including the shapes the schema accepts but the data model
// forbids, which is the point of the checks.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/integrity.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif
#ifndef RE_TEST_TEMPO_MAP_STATE
#define RE_TEST_TEMPO_MAP_STATE ""
#endif
#ifndef RE_TEST_OFFSET_COLLISION_STATE
#define RE_TEST_OFFSET_COLLISION_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::checkIntegrity;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::findListDefects;
using rainbohz::rendering_engine::IntegrityCheck;
using rainbohz::rendering_engine::IntegrityReport;
using rainbohz::rendering_engine::kMaxReportedViolations;
using rainbohz::rendering_engine::ListDefects;
using rainbohz::rendering_engine::loadCypherFile;

using IdMap = std::unordered_map<std::int64_t, std::int64_t>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// The one check whose name starts with `prefix`.
///
/// @param report  A report from checkIntegrity().
/// @param prefix  The start of the check's name, e.g. "FrequencyPhysicalTimeline lists
///                have no cycles".
/// @return The check. Fails the test, and returns an empty check, if no check
///         or more than one matches.
IntegrityCheck find(const IntegrityReport& report, std::string_view prefix) {
    std::vector<IntegrityCheck> matches;
    for (const IntegrityCheck& check : report.checks) {
        if (std::string_view{check.name}.starts_with(prefix)) {
            matches.push_back(check);
        }
    }
    EXPECT_EQ(matches.size(), 1U) << "checks named '" << prefix << "...'";
    return matches.empty() ? IntegrityCheck{} : matches.front();
}

/// The names of the checks that failed, for a readable assertion message.
///
/// @param report  A report from checkIntegrity().
/// @return Each failed check's name and first violation, one per line.
std::string failures(const IntegrityReport& report) {
    std::string text;
    for (const IntegrityCheck& check : report.checks) {
        if (!check.ok()) {
            text += check.name + ": " + check.violations.front() + "\n";
        }
    }
    return text;
}

/// An in-memory engine-db with the real schema, one EngineConfig, and one
/// partial in one PartialSet whose FrequencyPhysicalTimeline has three sound points
/// at times 0, 10 and 20, and whose AmplitudePhysicalTimeline has two, at 0 and 20.
class IntegrityDb : public ::testing::Test {
protected:
    /// Applies the schema and builds the sound score described above.
    void SetUp() override {
        const auto report = loadCypherFile(db, RE_TEST_SCHEMA_FILE);
        ASSERT_TRUE(report.ok()) << "the engine schema did not load";
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
        db.execute(
            "CREATE (:PartialSet {name: 'S'})-[:HAS_PARTIAL]->"
            "(:Partial {name: 'A'})-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline)"
            "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 0.0, frequency: 0.1})"
            "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 10.0, frequency: 0.2})"
            "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 20.0, frequency: 0.1});");
        db.execute(
            "MATCH (p:Partial {name: 'A'}) CREATE "
            "(p)-[:HAS_PHYSICAL_TIMELINE]->(:AmplitudePhysicalTimeline)"
            "-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 0.0, amplitude: -6.0})"
            "-[:NEXT_POINT]->(:AmplitudePhysicalTimelinePoint {time: 20.0, amplitude: -6.0});");
    }

    /// Sets one property of the point at `time`.
    ///
    /// @param time        Time of the point to change: 0, 10 or 20.
    /// @param assignment  The SET clause's assignment, on `f`, e.g.
    ///                    "f.frequency = NULL".
    void setOnPoint(double time, const std::string& assignment) {
        db.query(
            "MATCH (f:FrequencyPhysicalTimelinePoint) WHERE f.time = $time SET " + assignment + ";",
            {{"time", time}});
    }

    EngineDb db;
};

// ---------------------------------------------------------------------------
// findListDefects
// ---------------------------------------------------------------------------

TEST(FindListDefects, SoundListsHaveNone) {
    const ListDefects defects =
        findListDefects(IdMap{{10, 1}, {11, 4}}, IdMap{{1, 2}, {2, 3}, {4, 5}});
    EXPECT_TRUE(defects.cycles.empty());
    EXPECT_TRUE(defects.merges.empty());
}

TEST(FindListDefects, FindsACycleThroughTheHead) {
    const ListDefects defects = findListDefects(IdMap{{10, 1}}, IdMap{{1, 2}, {2, 3}, {3, 1}});
    ASSERT_EQ(defects.cycles.size(), 1U);
    EXPECT_EQ(defects.cycles[0].nodes, (std::vector<std::int64_t>{1, 2, 3}));
    EXPECT_EQ(defects.cycles[0].enteredFrom, (std::vector<std::int64_t>{10}));
    EXPECT_TRUE(defects.merges.empty()) << "a head on a cycle is the cycle's entry, not a merge";
}

TEST(FindListDefects, FindsACycleNoTimelineReaches) {
    const ListDefects defects = findListDefects(IdMap{{10, 1}}, IdMap{{1, 2}, {5, 6}, {6, 5}});
    ASSERT_EQ(defects.cycles.size(), 1U);
    EXPECT_EQ(defects.cycles[0].nodes, (std::vector<std::int64_t>{5, 6}));
    EXPECT_TRUE(defects.cycles[0].enteredFrom.empty());
}

TEST(FindListDefects, ACycleStartsAtItsSmallestId) {
    const ListDefects defects = findListDefects(IdMap{}, IdMap{{7, 3}, {3, 9}, {9, 7}});
    ASSERT_EQ(defects.cycles.size(), 1U);
    EXPECT_EQ(defects.cycles[0].nodes, (std::vector<std::int64_t>{3, 9, 7}));
}

TEST(FindListDefects, ASelfLoopIsACycle) {
    const ListDefects defects = findListDefects(IdMap{{10, 1}}, IdMap{{1, 1}});
    ASSERT_EQ(defects.cycles.size(), 1U);
    EXPECT_EQ(defects.cycles[0].nodes, (std::vector<std::int64_t>{1}));
}

TEST(FindListDefects, FindsAMergeAndItsOwner) {
    const ListDefects defects = findListDefects(IdMap{{10, 1}, {11, 3}}, IdMap{{1, 2}, {2, 3}});
    EXPECT_TRUE(defects.cycles.empty());
    ASSERT_EQ(defects.merges.size(), 1U);
    EXPECT_EQ(defects.merges[0].timeline, 11);
    EXPECT_EQ(defects.merges[0].node, 3);
    EXPECT_EQ(defects.merges[0].predecessor, 2);
    EXPECT_EQ(defects.merges[0].owner, 10);
}

TEST(FindListDefects, AMergeIntoAListNoTimelineReaches) {
    const ListDefects defects = findListDefects(IdMap{{11, 2}}, IdMap{{1, 2}});
    ASSERT_EQ(defects.merges.size(), 1U);
    EXPECT_EQ(defects.merges[0].owner, std::nullopt);
}

// ---------------------------------------------------------------------------
// checkIntegrity: sound databases
// ---------------------------------------------------------------------------

TEST_F(IntegrityDb, ASoundScorePasses) {
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(report.ok()) << failures(report);
    // One EngineConfig check, twenty range rules, set membership, lifeline
    // equality, five musical-time checks, and two per NEXT_POINT list type.
    EXPECT_EQ(report.checks.size(), 1U + 20U + 2U + 5U + (2U * 14U));
}

TEST(Integrity, TheExampleScorePasses) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_EXAMPLE_STATE).ok());
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(report.ok()) << failures(report);
}

TEST_F(IntegrityDb, CheckingDoesNotModifyTheDatabase) {
    setOnPoint(0.0, "f.time = -1.0");
    const auto before = db.census();
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_FALSE(report.ok());
    const auto after = db.census();
    EXPECT_EQ(after.nodes, before.nodes);
    EXPECT_EQ(after.relationships, before.relationships);
}

// ---------------------------------------------------------------------------
// checkIntegrity: linked lists
// ---------------------------------------------------------------------------

// The shape established by experiment: the schema accepts a second timeline
// whose head is a point in the middle of another timeline's list.
TEST_F(IntegrityDb, FindsATimelineMergedIntoAnother) {
    db.execute(
        "MATCH (p:FrequencyPhysicalTimelinePoint) WHERE p.time = 10.0 "
        "CREATE (:Partial {name: 'B'})-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline)"
        "-[:NEXT_POINT]->(p);");

    const IntegrityReport report = checkIntegrity(db);
    const IntegrityCheck merges = find(report, "FrequencyPhysicalTimeline lists do not merge");
    ASSERT_EQ(merges.violationCount, 1U);
    EXPECT_NE(
        merges.violations[0].find("in the list of FrequencyPhysicalTimeline"), std::string::npos)
        << merges.violations[0];
    EXPECT_TRUE(find(report, "FrequencyPhysicalTimeline lists have no cycles").ok());
}

TEST_F(IntegrityDb, FindsACycleThroughTheHead) {
    db.execute("MATCH (a:FrequencyPhysicalTimelinePoint), (b:FrequencyPhysicalTimelinePoint) "
               "WHERE a.time = 20.0 AND b.time = 0.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityReport report = checkIntegrity(db);
    const IntegrityCheck cycles = find(report, "FrequencyPhysicalTimeline lists have no cycles");
    ASSERT_EQ(cycles.violationCount, 1U);
    EXPECT_NE(cycles.violations[0].find("entered from the head of FrequencyPhysicalTimeline"),
        std::string::npos)
        << cycles.violations[0];
    EXPECT_TRUE(find(report, "FrequencyPhysicalTimeline lists do not merge").ok());
}

TEST_F(IntegrityDb, FindsACycleNoTimelineReaches) {
    db.execute("CREATE (a:FrequencyPhysicalTimelinePoint {time: 1.0, frequency: 0.1})"
               "-[:NEXT_POINT]->(b:FrequencyPhysicalTimelinePoint {time: 2.0, frequency: 0.1});");
    db.execute("MATCH (a:FrequencyPhysicalTimelinePoint), (b:FrequencyPhysicalTimelinePoint) "
               "WHERE a.time = 2.0 AND b.time = 1.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityCheck cycles =
        find(checkIntegrity(db), "FrequencyPhysicalTimeline lists have no cycles");
    ASSERT_EQ(cycles.violationCount, 1U);
    EXPECT_NE(
        cycles.violations[0].find("reached from no FrequencyPhysicalTimeline"), std::string::npos)
        << cycles.violations[0];
}

// Lists are checked for every list type in NEXT_POINT, not just the ones
// precomputation reads.
TEST_F(IntegrityDb, ChecksOtherListTypesToo) {
    db.execute(
        "CREATE (:PhasePhysicalTimeline)-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: 0.0})"
        "-[:NEXT_POINT]->(:PhasePhysicalTimelinePoint {time: 5.0});");
    db.execute("MATCH (a:PhasePhysicalTimelinePoint), (b:PhasePhysicalTimelinePoint) "
               "WHERE a.time = 5.0 AND b.time = 0.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityReport report = checkIntegrity(db);
    EXPECT_EQ(find(report, "PhasePhysicalTimeline lists have no cycles").violationCount, 1U);
    EXPECT_EQ(report.violationCount(), 1U) << failures(report);
}

// ---------------------------------------------------------------------------
// checkIntegrity: ranges
// ---------------------------------------------------------------------------

TEST_F(IntegrityDb, TimeMustNotBeNegative) {
    setOnPoint(0.0, "f.time = -0.5");
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.time ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("time is -0.5; it must be >= 0"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, ValuesMustBeFinite) {
    db.query(
        "MATCH (f:FrequencyPhysicalTimelinePoint) WHERE f.time = 10.0 SET f.frequency = $value;",
        {{"value", std::numeric_limits<double>::quiet_NaN()}});
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.frequency ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not finite"), std::string::npos) << check.violations[0];
}

TEST_F(IntegrityDb, RequiredValuesMustNotBeNull) {
    setOnPoint(20.0, "f.frequency = NULL");
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.frequency ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("frequency is NULL"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, CurveTypeMustBeAKnownCurve) {
    setOnPoint(0.0, "f.frequency_curve_type = 5");
    setOnPoint(10.0, "f.frequency_curve_type = 4"); // s-curve: valid data, unsupported feature
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.frequency_curve_type ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("is 5; it must be <= 4"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, CurveAmountMustBeBetweenZeroAndOne) {
    setOnPoint(0.0, "f.frequency_curve_amount = 1.5");
    setOnPoint(10.0, "f.frequency_curve_amount = -0.25");
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.frequency_curve_amount ");
    EXPECT_EQ(check.violationCount, 2U);
}

TEST_F(IntegrityDb, MessagesAreCappedButCountsAreNot) {
    const std::size_t extra = kMaxReportedViolations + 5;
    for (std::size_t i = 0; i < extra; ++i) {
        db.execute("CREATE (:FrequencyPhysicalTimelinePoint {time: -1.0, frequency: 0.1});");
    }
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyPhysicalTimelinePoint.time ");
    EXPECT_EQ(check.violationCount, extra);
    EXPECT_EQ(check.violations.size(), kMaxReportedViolations);
}

// ---------------------------------------------------------------------------
// checkIntegrity: EngineConfig
// ---------------------------------------------------------------------------

TEST_F(IntegrityDb, AmplitudeMustNotExceedFullScale) {
    db.execute(
        "MATCH (a:AmplitudePhysicalTimelinePoint) WHERE a.time = 0.0 SET a.amplitude = 0.5;");
    const IntegrityCheck check = find(checkIntegrity(db),
        "AmplitudePhysicalTimelinePoint.amplitude is not NULL, finite and <= 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("0.5"), std::string::npos);
}

TEST_F(IntegrityDb, AmplitudeTimeMustNotBeNegative) {
    db.execute("MATCH (a:AmplitudePhysicalTimelinePoint) WHERE a.time = 0.0 SET a.time = -1.0;");
    EXPECT_EQ(
        find(checkIntegrity(db), "AmplitudePhysicalTimelinePoint.time is").violationCount, 1U);
}

TEST_F(IntegrityDb, ChannelModeMustBeAKnownMode) {
    db.execute("MATCH (c:EngineConfig) SET c.channelMode = 5;");
    EXPECT_EQ(find(checkIntegrity(db), "EngineConfig.channelMode is").violationCount, 1U);
}

TEST_F(IntegrityDb, EveryPartialMustBeInAPartialSet) {
    db.execute("CREATE (:Partial {name: 'Loose'});");
    const IntegrityCheck check =
        find(checkIntegrity(db), "every Partial is aggregated by exactly one PartialSet");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("0 PartialSet"), std::string::npos);
}

TEST_F(IntegrityDb, TheTwoPhysicalTimelinesMustStartAndEndTogether) {
    db.execute("MATCH (a:AmplitudePhysicalTimelinePoint) WHERE a.time = 20.0 SET a.time = 19.0;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "AmplitudePhysicalTimeline and FrequencyPhysicalTimeline lists "
                                 "start and end at the same times");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("ends at 19"), std::string::npos);
}

TEST_F(IntegrityDb, APartialWithOnlyOnePhysicalTimelineIsNotALifelineViolation) {
    db.execute("CREATE (:PartialSet {name: 'T'})-[:HAS_PARTIAL]->(:Partial {name: 'B'})"
               "-[:HAS_PHYSICAL_TIMELINE]->(:FrequencyPhysicalTimeline)"
               "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 0.0, frequency: 0.1})"
               "-[:NEXT_POINT]->(:FrequencyPhysicalTimelinePoint {time: 5.0, frequency: 0.1});");
    EXPECT_TRUE(find(checkIntegrity(db), "AmplitudePhysicalTimeline and FrequencyPhysicalTimeline "
                                         "lists start and end at the same times")
            .ok());
}

TEST_F(IntegrityDb, EngineConfigMustExist) {
    db.execute("MATCH (c:EngineConfig) DELETE c;");
    EXPECT_EQ(find(checkIntegrity(db), "EngineConfig is a singleton").violationCount, 1U);
}

TEST_F(IntegrityDb, EngineConfigMustBeASingleton) {
    db.execute("CREATE (:EngineConfig {sampleRate: 44100, channelMode: 0});");
    EXPECT_EQ(find(checkIntegrity(db), "EngineConfig is a singleton").violationCount, 1U);
}

TEST_F(IntegrityDb, EngineConfigMustYieldAFrameSize) {
    db.execute("MATCH (c:EngineConfig) SET c.sampleRate = NULL;");
    EXPECT_EQ(find(checkIntegrity(db), "EngineConfig is a singleton").violationCount, 1U);
}

TEST_F(IntegrityDb, FrameSizeMustBePositive) {
    db.execute("MATCH (c:EngineConfig) SET c.frameSize = 0;");
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_EQ(find(report, "EngineConfig.frameSize ").violationCount, 1U);
    EXPECT_TRUE(find(report, "EngineConfig.sampleRate ").ok());
}

// ---------------------------------------------------------------------------
// checkIntegrity: the musical-time tables the transform reads
// ---------------------------------------------------------------------------

/// An in-memory engine-db holding the tempo-map example score, which is in
/// the canonical model: a Score with a two-point TempoMap, one PartialSet
/// "Chord" with partials A and B and a four-point musical timeline at ticks
/// 0, 1920, 3840 and 7680.
class MusicalIntegrityDb : public ::testing::Test {
protected:
    /// Applies the schema and loads the example.
    void SetUp() override {
        ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok())
            << "the engine schema did not load";
        ASSERT_TRUE(loadCypherFile(db, RE_TEST_TEMPO_MAP_STATE).ok()) << "the example did not load";
    }

    EngineDb db;
};

TEST_F(MusicalIntegrityDb, TheTempoMapExamplePasses) {
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(report.ok()) << failures(report);
}

TEST(Integrity, TheOffsetCollisionExamplePasses) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_OFFSET_COLLISION_STATE).ok());
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(report.ok()) << failures(report);
}

TEST_F(MusicalIntegrityDb, TheLastTempoMapPointMustHold) {
    db.execute("MATCH (p:TempoMapPoint) WHERE p.position = 3840 SET p.hold = false;");
    const IntegrityCheck check = find(checkIntegrity(db), "TempoMap lists start at position 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("must hold"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, TheFirstTempoMapPointIsAtZero) {
    db.execute("MATCH (p:TempoMapPoint) WHERE p.position = 0 SET p.position = 1;");
    const IntegrityCheck check = find(checkIntegrity(db), "TempoMap lists start at position 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("position 0"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, TempoMapPositionsStrictlyIncrease) {
    db.execute("MATCH (p:TempoMapPoint) WHERE p.position = 3840 SET p.position = 0;");
    const IntegrityCheck check = find(checkIntegrity(db), "TempoMap lists start at position 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not after"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, ATempoMapNeedsAPoint) {
    db.execute("CREATE (:TempoMap {name: 'Empty', ticks_per_beat: 960});");
    const IntegrityCheck check = find(checkIntegrity(db), "TempoMap lists start at position 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("no TempoMapPoint"), std::string::npos);
}

TEST_F(MusicalIntegrityDb, HoldMustNotBeNull) {
    db.execute("MATCH (p:TempoMapPoint) WHERE p.position = 0 SET p.hold = NULL;");
    EXPECT_EQ(find(checkIntegrity(db), "TempoMap lists start at position 0").violationCount, 1U);
}

TEST_F(MusicalIntegrityDb, TicksPerBeatMustBePositive) {
    db.execute("MATCH (m:TempoMap) SET m.ticks_per_beat = 0;");
    EXPECT_EQ(find(checkIntegrity(db), "TempoMap.ticks_per_beat ").violationCount, 1U);
}

TEST_F(MusicalIntegrityDb, TempoStaysInTheSchemaRange) {
    db.execute("MATCH (p:TempoMapPoint) WHERE p.position = 0 SET p.start_tempo = 2000.0;");
    const IntegrityCheck check = find(checkIntegrity(db), "TempoMapPoint.start_tempo ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("<= 1000"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, EveryMusicalPointNeedsAPointAt) {
    db.execute("MATCH (:PartialSetMusicalTimelinePoint)-[a:POINT_AT {sync_point: 1920}]->() "
               "DELETE a;");
    const IntegrityReport report = checkIntegrity(db);
    const IntegrityCheck check =
        find(report, "every PartialSetMusicalTimelinePoint has one POINT_AT");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("no POINT_AT"), std::string::npos);
    EXPECT_TRUE(find(report, "PartialSetMusicalTimeline points are in non-decreasing").ok());
}

TEST_F(MusicalIntegrityDb, SyncPointMustNotBeNegativeOrNull) {
    db.execute("MATCH ()-[a:POINT_AT {sync_point: 1920}]->() SET a.sync_point = -1;");
    db.execute("MATCH ()-[a:POINT_AT {sync_point: 3840}]->() SET a.sync_point = NULL;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "every PartialSetMusicalTimelinePoint has one POINT_AT");
    EXPECT_EQ(check.violationCount, 2U);
}

TEST_F(MusicalIntegrityDb, OffsetMustBeFiniteIfSet) {
    db.query("MATCH ()-[a:POINT_AT {sync_point: 1920}]->() SET a.offset_seconds = $value;",
        {{"value", std::numeric_limits<double>::quiet_NaN()}});
    db.execute("MATCH ()-[a:POINT_AT {sync_point: 3840}]->() SET a.offset_seconds = NULL;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "every PartialSetMusicalTimelinePoint has one POINT_AT");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not finite"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, MusicalPointsMustNotGoBackwards) {
    db.execute("MATCH ()-[a:POINT_AT {sync_point: 1920}]->() SET a.sync_point = 5000;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "PartialSetMusicalTimeline points are in non-decreasing");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("before the previous point's 5000"), std::string::npos)
        << check.violations[0];
}

TEST_F(MusicalIntegrityDb, EqualSyncPointsAreInOrder) {
    db.execute("MATCH ()-[a:POINT_AT {sync_point: 1920}]->() SET a.sync_point = 0;");
    EXPECT_TRUE(
        find(checkIntegrity(db), "PartialSetMusicalTimeline points are in non-decreasing").ok());
}

TEST_F(MusicalIntegrityDb, PointAtMustLeadToTheScoresTempoMap) {
    db.execute("CREATE (:TempoMap {name: 'Other', ticks_per_beat: 960})-[:NEXT_POINT]->"
               "(:TempoMapPoint {position: 0, start_tempo: 100.0, hold: true});");
    db.execute("MATCH (:PartialSetMusicalTimelinePoint)-[a:POINT_AT {sync_point: 1920}]->() "
               "DELETE a;");
    db.execute("MATCH (p:PartialSetMusicalTimelinePoint) "
               "WHERE NOT EXISTS { MATCH (p)-[:POINT_AT]->() } "
               "MATCH (m:TempoMap {name: 'Other'}) "
               "CREATE (p)-[:POINT_AT {sync_point: 1920, offset_seconds: 0.0}]->(m);");
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(find(report, "every PartialSetMusicalTimelinePoint has one POINT_AT").ok());
    const IntegrityCheck check =
        find(report, "PartialSetMusicalTimeline points are in non-decreasing");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not TempoMap"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, ASetInMusicalTimeNeedsAScoreWithATempoMap) {
    db.execute("MATCH (:Score)-[r:NEXT_POINT]->(:TempoMap) DELETE r;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "PartialSetMusicalTimeline points are in non-decreasing");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("has no TempoMap"), std::string::npos)
        << check.violations[0];
}

TEST_F(MusicalIntegrityDb, AStateNeedsASetAndAPartial) {
    db.execute("CREATE (:PartialState {frequency: 1.0, amplitude: -6.0});");
    const IntegrityCheck check = find(checkIntegrity(db), "every PartialState is in one");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("0 PartialStateSet(s)"), std::string::npos)
        << check.violations[0];
}

TEST_F(MusicalIntegrityDb, AStateOfAPartialOutsideTheSetIsReported) {
    db.execute("MATCH (ss:SoundSource) CREATE (ss)-[:HAS_PARTIALSET]->(:PartialSet {name: 'Other'})"
               "-[:HAS_PARTIAL]->(:Partial {name: 'Z'});");
    db.execute("MATCH (:PartialSetMusicalTimeline)-[:NEXT_POINT]->"
               "(:PartialSetMusicalTimelinePoint)-[:POINT_STATE]->(set:PartialStateSet) "
               "CREATE (:PartialState {frequency: 1.0, amplitude: -6.0})"
               "-[:MEMBER_OF_SET_MANY_ONE]->(set);");
    db.execute("MATCH (st:PartialState) WHERE NOT EXISTS { MATCH (st)-[:STATE_OF]->() } "
               "MATCH (z:Partial {name: 'Z'}) CREATE (st)-[:STATE_OF]->(z);");
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_TRUE(find(report, "every PartialState is in one").ok());
    const IntegrityCheck check = find(report, "a PartialStateSet used by a timeline point");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not in PartialSet"), std::string::npos)
        << check.violations[0];
}

TEST_F(MusicalIntegrityDb, TwoStatesOfOnePartialAtOnePointAreReported) {
    db.execute("MATCH (:PartialSetMusicalTimeline)-[:NEXT_POINT]->"
               "(:PartialSetMusicalTimelinePoint)-[:POINT_STATE]->(set:PartialStateSet) "
               "CREATE (:PartialState {frequency: 1.0, amplitude: -6.0})"
               "-[:MEMBER_OF_SET_MANY_ONE]->(set);");
    db.execute("MATCH (st:PartialState) WHERE NOT EXISTS { MATCH (st)-[:STATE_OF]->() } "
               "MATCH (a:Partial {name: 'A'}) CREATE (st)-[:STATE_OF]->(a);");
    const IntegrityCheck check =
        find(checkIntegrity(db), "a PartialStateSet used by a timeline point");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("second state"), std::string::npos) << check.violations[0];
}

TEST_F(MusicalIntegrityDb, PartialStateValuesMayBeNullButMustBeInRange) {
    db.execute("MATCH (:Partial {name: 'B'})<-[:STATE_OF]-(st:PartialState) "
               "WHERE st.amplitude < -20.0 AND st.amplitude > -30.0 SET st.amplitude = 1.0;");
    db.query("MATCH (:Partial {name: 'A'})<-[:STATE_OF]-(st:PartialState) "
             "WHERE st.frequency > 400.0 SET st.frequency = $value;",
        {{"value", std::numeric_limits<double>::quiet_NaN()}});
    const IntegrityReport report = checkIntegrity(db);
    EXPECT_EQ(find(report, "PartialState.amplitude ").violationCount, 1U);
    EXPECT_EQ(find(report, "PartialState.frequency ").violationCount, 2U);
}

} // namespace
