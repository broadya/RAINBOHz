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
/// @param prefix  The start of the check's name, e.g. "FrequencyTimeline lists
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
/// partial in one PartialSet whose FrequencyTimeline has three sound points
/// at times 0, 10 and 20, and whose AmplitudeTimeline has two, at 0 and 20.
class IntegrityDb : public ::testing::Test {
protected:
    /// Applies the schema and builds the sound score described above.
    void SetUp() override {
        const auto report = loadCypherFile(db, RE_TEST_SCHEMA_FILE);
        ASSERT_TRUE(report.ok()) << "the engine schema did not load";
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
        db.execute("CREATE (:PartialSet {name: 'S'})-[:AGGREGATE_OF_ONE_MANY]->"
                   "(:Partial {name: 'A'})-[:HAS_TIMELINE]->(:FrequencyTimeline)"
                   "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: 0.0, frequency: 0.1})"
                   "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: 10.0, frequency: 0.2})"
                   "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: 20.0, frequency: 0.1});");
        db.execute("MATCH (p:Partial {name: 'A'}) CREATE (p)-[:HAS_TIMELINE]->(:AmplitudeTimeline)"
                   "-[:NEXT_POINT]->(:AmplitudeTimelinePoint {time: 0.0, amplitude: -6.0})"
                   "-[:NEXT_POINT]->(:AmplitudeTimelinePoint {time: 20.0, amplitude: -6.0});");
    }

    /// Sets one property of the point at `time`.
    ///
    /// @param time        Time of the point to change: 0, 10 or 20.
    /// @param assignment  The SET clause's assignment, on `f`, e.g.
    ///                    "f.frequency = NULL".
    void setOnPoint(double time, const std::string& assignment) {
        db.query("MATCH (f:FrequencyTimelinePoint) WHERE f.time = $time SET " + assignment + ";",
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
    // One EngineConfig check, eleven range rules, set membership, lifeline
    // equality, and two per NEXT_POINT list type.
    EXPECT_EQ(report.checks.size(), 1U + 11U + 2U + (2U * 10U));
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
    db.execute("MATCH (p:FrequencyTimelinePoint) WHERE p.time = 10.0 "
               "CREATE (:Partial {name: 'B'})-[:HAS_TIMELINE]->(:FrequencyTimeline)"
               "-[:NEXT_POINT]->(p);");

    const IntegrityReport report = checkIntegrity(db);
    const IntegrityCheck merges = find(report, "FrequencyTimeline lists do not merge");
    ASSERT_EQ(merges.violationCount, 1U);
    EXPECT_NE(merges.violations[0].find("in the list of FrequencyTimeline"), std::string::npos)
        << merges.violations[0];
    EXPECT_TRUE(find(report, "FrequencyTimeline lists have no cycles").ok());
}

TEST_F(IntegrityDb, FindsACycleThroughTheHead) {
    db.execute("MATCH (a:FrequencyTimelinePoint), (b:FrequencyTimelinePoint) "
               "WHERE a.time = 20.0 AND b.time = 0.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityReport report = checkIntegrity(db);
    const IntegrityCheck cycles = find(report, "FrequencyTimeline lists have no cycles");
    ASSERT_EQ(cycles.violationCount, 1U);
    EXPECT_NE(
        cycles.violations[0].find("entered from the head of FrequencyTimeline"), std::string::npos)
        << cycles.violations[0];
    EXPECT_TRUE(find(report, "FrequencyTimeline lists do not merge").ok());
}

TEST_F(IntegrityDb, FindsACycleNoTimelineReaches) {
    db.execute("CREATE (a:FrequencyTimelinePoint {time: 1.0, frequency: 0.1})"
               "-[:NEXT_POINT]->(b:FrequencyTimelinePoint {time: 2.0, frequency: 0.1});");
    db.execute("MATCH (a:FrequencyTimelinePoint), (b:FrequencyTimelinePoint) "
               "WHERE a.time = 2.0 AND b.time = 1.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityCheck cycles =
        find(checkIntegrity(db), "FrequencyTimeline lists have no cycles");
    ASSERT_EQ(cycles.violationCount, 1U);
    EXPECT_NE(cycles.violations[0].find("reached from no FrequencyTimeline"), std::string::npos)
        << cycles.violations[0];
}

// Lists are checked for every list type in NEXT_POINT, not just the ones
// precomputation reads.
TEST_F(IntegrityDb, ChecksOtherListTypesToo) {
    db.execute("CREATE (:PhaseTimeline)-[:NEXT_POINT]->(:PhaseTimelinePoint {time: 0.0})"
               "-[:NEXT_POINT]->(:PhaseTimelinePoint {time: 5.0});");
    db.execute("MATCH (a:PhaseTimelinePoint), (b:PhaseTimelinePoint) "
               "WHERE a.time = 5.0 AND b.time = 0.0 CREATE (a)-[:NEXT_POINT]->(b);");

    const IntegrityReport report = checkIntegrity(db);
    EXPECT_EQ(find(report, "PhaseTimeline lists have no cycles").violationCount, 1U);
    EXPECT_EQ(report.violationCount(), 1U) << failures(report);
}

// ---------------------------------------------------------------------------
// checkIntegrity: ranges
// ---------------------------------------------------------------------------

TEST_F(IntegrityDb, TimeMustNotBeNegative) {
    setOnPoint(0.0, "f.time = -0.5");
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyTimelinePoint.time ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("time is -0.5; it must be >= 0"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, ValuesMustBeFinite) {
    db.query("MATCH (f:FrequencyTimelinePoint) WHERE f.time = 10.0 SET f.frequency = $value;",
        {{"value", std::numeric_limits<double>::quiet_NaN()}});
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyTimelinePoint.frequency ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("not finite"), std::string::npos) << check.violations[0];
}

TEST_F(IntegrityDb, RequiredValuesMustNotBeNull) {
    setOnPoint(20.0, "f.frequency = NULL");
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyTimelinePoint.frequency ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("frequency is NULL"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, CurveTypeMustBeAKnownCurve) {
    setOnPoint(0.0, "f.frequency_curve_type = 5");
    setOnPoint(10.0, "f.frequency_curve_type = 4"); // s-curve: valid data, unsupported feature
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyTimelinePoint.frequency_curve_type ");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("is 5; it must be <= 4"), std::string::npos)
        << check.violations[0];
}

TEST_F(IntegrityDb, CurveAmountMustBeBetweenZeroAndOne) {
    setOnPoint(0.0, "f.frequency_curve_amount = 1.5");
    setOnPoint(10.0, "f.frequency_curve_amount = -0.25");
    const IntegrityCheck check =
        find(checkIntegrity(db), "FrequencyTimelinePoint.frequency_curve_amount ");
    EXPECT_EQ(check.violationCount, 2U);
}

TEST_F(IntegrityDb, MessagesAreCappedButCountsAreNot) {
    const std::size_t extra = kMaxReportedViolations + 5;
    for (std::size_t i = 0; i < extra; ++i) {
        db.execute("CREATE (:FrequencyTimelinePoint {time: -1.0, frequency: 0.1});");
    }
    const IntegrityCheck check = find(checkIntegrity(db), "FrequencyTimelinePoint.time ");
    EXPECT_EQ(check.violationCount, extra);
    EXPECT_EQ(check.violations.size(), kMaxReportedViolations);
}

// ---------------------------------------------------------------------------
// checkIntegrity: EngineConfig
// ---------------------------------------------------------------------------

TEST_F(IntegrityDb, AmplitudeMustNotExceedFullScale) {
    db.execute("MATCH (a:AmplitudeTimelinePoint) WHERE a.time = 0.0 SET a.amplitude = 0.5;");
    const IntegrityCheck check =
        find(checkIntegrity(db), "AmplitudeTimelinePoint.amplitude is not NULL, finite and <= 0");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("0.5"), std::string::npos);
}

TEST_F(IntegrityDb, AmplitudeTimeMustNotBeNegative) {
    db.execute("MATCH (a:AmplitudeTimelinePoint) WHERE a.time = 0.0 SET a.time = -1.0;");
    EXPECT_EQ(find(checkIntegrity(db), "AmplitudeTimelinePoint.time is").violationCount, 1U);
}

TEST_F(IntegrityDb, ChannelModeMustBeAKnownMode) {
    db.execute("MATCH (c:EngineConfig) SET c.channelMode = 3;");
    EXPECT_EQ(find(checkIntegrity(db), "EngineConfig.channelMode is").violationCount, 1U);
}

TEST_F(IntegrityDb, EveryPartialMustBeInAPartialSet) {
    db.execute("CREATE (:Partial {name: 'Loose'});");
    const IntegrityCheck check =
        find(checkIntegrity(db), "every Partial is aggregated by exactly one PartialSet");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("0 PartialSet"), std::string::npos);
}

TEST_F(IntegrityDb, TheTwoTimelinesMustStartAndEndTogether) {
    db.execute("MATCH (a:AmplitudeTimelinePoint) WHERE a.time = 20.0 SET a.time = 19.0;");
    const IntegrityCheck check = find(checkIntegrity(db),
        "AmplitudeTimeline and FrequencyTimeline lists start and end at the same times");
    ASSERT_EQ(check.violationCount, 1U);
    EXPECT_NE(check.violations[0].find("ends at 19"), std::string::npos);
}

TEST_F(IntegrityDb, APartialWithOnlyOneTimelineIsNotALifelineViolation) {
    db.execute("CREATE (:PartialSet {name: 'T'})-[:AGGREGATE_OF_ONE_MANY]->(:Partial {name: 'B'})"
               "-[:HAS_TIMELINE]->(:FrequencyTimeline)"
               "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: 0.0, frequency: 0.1})"
               "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: 5.0, frequency: 0.1});");
    EXPECT_TRUE(find(checkIntegrity(db),
        "AmplitudeTimeline and FrequencyTimeline lists start and end at the same times")
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

} // namespace
