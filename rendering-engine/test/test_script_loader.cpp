// Tests for the Cypher script loader.
//
// These drive a real in-memory engine-db. The properties that matter are that
// every statement is attempted, that a failure is attributed to the right file
// and source lines, and that re-applying a schema is idempotent rather than an
// error -- the three things scripts/load_schema.py had to get right and that
// this loader now has to get right independently.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// Injected by CMake: the repository's real engine-db schema file, and the
// example score that sits beside it.
#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::Census;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::LoadReport;
using rainbohz::rendering_engine::RowCount;
using rainbohz::rendering_engine::ScriptLoadError;

/// A directory under the system temporary directory, removed on destruction.
class TempDirectory {
public:
    TempDirectory() {
        // gtest_discover_tests runs each test in its own process, and ctest may
        // run several at once, so a per-process counter would not keep two
        // directories apart on its own.
        static std::random_device entropy;
        const auto unique =
            "rainbohz-loader-test-" + std::to_string(entropy()) + "-" + std::to_string(entropy());
        path_ = std::filesystem::temp_directory_path() / unique;
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    const std::filesystem::path& path() const { return path_; }

    /// Writes `contents` to `name` inside the directory and returns its path.
    std::filesystem::path write(std::string_view name, std::string_view contents) const {
        const std::filesystem::path file = path_ / name;
        std::ofstream stream{file};
        stream << contents;
        return file;
    }

private:
    std::filesystem::path path_;
};

/// The minimal schema these tests need, when the real one is beside the point.
constexpr std::string_view kToySchema = "CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));\n"
                                        "CREATE NODE TABLE Label(id INT64, name STRING, "
                                        "PRIMARY KEY(id));\n";

TEST(ScriptLoader, AppliesEveryStatementInAFile) {
    EngineDb db;
    const TempDirectory temp;
    const auto file = temp.write("schema.cypher", kToySchema);

    const LoadReport report = loadCypherFile(db, file);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.applied, 2);
    EXPECT_EQ(report.alreadyPresent, 0);
}

TEST(ScriptLoader, MissingFileThrows) {
    EngineDb db;
    const TempDirectory temp;
    EXPECT_THROW(
        static_cast<void>(loadCypherFile(db, temp.path() / "absent.cypher")), ScriptLoadError);
}

// A directory is not a script. The engine names one schema file and one state
// file, so there is no directory sweep to fall back on and this must fail
// rather than quietly load nothing.
TEST(ScriptLoader, DirectoryIsNotALoadableScript) {
    EngineDb db;
    const TempDirectory temp;
    EXPECT_THROW(static_cast<void>(loadCypherFile(db, temp.path())), ScriptLoadError);
}

// The point of "report all": a bad statement must not hide the good ones after
// it, and it must carry the source lines that identify it.
TEST(ScriptLoader, ContinuesPastAFailureAndAttributesIt) {
    EngineDb db;
    const TempDirectory temp;
    const auto file = temp.write("state.cypher",
        "CREATE NODE TABLE Partial(id INT64, PRIMARY KEY(id));\n" // 1
        "CREATE (:NoSuchTable {id: 1});\n"                        // 2
        "CREATE (:Partial {id: 1});\n");                          // 3

    const LoadReport report = loadCypherFile(db, file);

    EXPECT_FALSE(report.ok());
    ASSERT_EQ(report.failures.size(), 1U);
    EXPECT_EQ(report.failures[0].startLine, 2U);
    EXPECT_EQ(report.failures[0].endLine, 2U);
    EXPECT_EQ(report.failures[0].lineSpan(), "line 2");
    EXPECT_EQ(report.failures[0].file, file);
    EXPECT_FALSE(report.failures[0].message.empty());

    // The statement after the failure still ran.
    EXPECT_EQ(report.applied, 2);
    EXPECT_NE(db.queryToString("MATCH (p:Partial) RETURN p.id;").find('1'), std::string::npos);
}

TEST(ScriptLoader, AttributesAFailureSpanningSeveralLines) {
    EngineDb db;
    const TempDirectory temp;
    const auto file = temp.write("state.cypher", "\n"                        // 1
                                                 "CREATE NODE TABLE Bad (\n" // 2
                                                 "    id NOT_A_TYPE,\n"      // 3
                                                 "    PRIMARY KEY(id)\n"     // 4
                                                 ");\n");                    // 5

    const LoadReport report = loadCypherFile(db, file);
    ASSERT_EQ(report.failures.size(), 1U);
    EXPECT_EQ(report.failures[0].lineSpan(), "lines 2-5");
    EXPECT_EQ(report.failures[0].label, "CREATE NODE TABLE Bad");
}

// Re-applying a schema is how init_db.sh behaves and how a restarted engine
// would behave against a persisted database: present, not failed.
TEST(ScriptLoader, ReapplyingASchemaReportsAlreadyPresent) {
    EngineDb db;
    const TempDirectory temp;
    const auto file = temp.write("schema.cypher", kToySchema);

    const LoadReport first = loadCypherFile(db, file);
    ASSERT_TRUE(first.ok());

    const LoadReport second = loadCypherFile(db, file);
    EXPECT_TRUE(second.ok());
    EXPECT_EQ(second.applied, 0);
    EXPECT_EQ(second.alreadyPresent, 2);
}

// Ladybug binds rel tables at DDL time, so within a file the node tables must
// land first. The loader must not reorder: source order is what the hand-edited
// schema file relies on.
TEST(ScriptLoader, StatementsAreAppliedInSourceOrder) {
    EngineDb db;
    const TempDirectory temp;
    const auto good = temp.write("in-order.cypher",
        std::string{kToySchema} + "CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);\n");

    const LoadReport report = loadCypherFile(db, good);
    EXPECT_TRUE(report.ok());
    EXPECT_EQ(report.applied, 3);

    // The same statements the other way round must fail, which is what proves
    // the loader is not quietly sorting them.
    EngineDb reversed;
    const auto bad = temp.write("out-of-order.cypher",
        "CREATE REL TABLE HAS_LABEL(FROM Partial TO Label);\n" + std::string{kToySchema});
    EXPECT_FALSE(loadCypherFile(reversed, bad).ok());
}

TEST(ScriptLoader, ReportsAccumulate) {
    LoadReport first;
    first.applied = 2;
    first.alreadyPresent = 1;
    first.failures.push_back(
        {.file = "a.cypher", .startLine = 1, .endLine = 1, .label = "x", .message = "boom"});

    LoadReport second;
    second.applied = 3;
    second.failures.push_back(
        {.file = "b.cypher", .startLine = 4, .endLine = 5, .label = "y", .message = "bang"});

    first += second;
    EXPECT_EQ(first.applied, 5);
    EXPECT_EQ(first.alreadyPresent, 1);
    EXPECT_EQ(first.failures.size(), 2U);
    EXPECT_FALSE(first.ok());
}

// The real thing. `make validate-schemas` proves the schema loads into the
// Homebrew `lbug` CLI; this proves it loads in process, at the Ladybug version
// this binary is actually linked against, which is a different claim.
TEST(ScriptLoader, RealEngineSchemaLoadsInProcess) {
    const std::filesystem::path schemaFile{RE_TEST_SCHEMA_FILE};
    ASSERT_FALSE(schemaFile.empty()) << "RE_TEST_SCHEMA_FILE was not set by the build";
    ASSERT_TRUE(std::filesystem::is_regular_file(schemaFile)) << schemaFile;

    EngineDb db;
    const LoadReport report = loadCypherFile(db, schemaFile);

    for (const auto& failure : report.failures) {
        ADD_FAILURE() << failure.file.filename().string() << ", " << failure.lineSpan() << ": "
                      << failure.label << "\n    " << failure.message;
    }
    EXPECT_TRUE(report.ok());
    EXPECT_GT(report.applied, 0);
    EXPECT_FALSE(db.tables().empty());
}

// The two files test mode applies, named once so that the tests below and the
// build's own defaults cannot drift apart.
std::filesystem::path realSchemaFile() {
    return std::filesystem::path{RE_TEST_SCHEMA_FILE};
}

std::filesystem::path exampleScoreFile() {
    return std::filesystem::path{RE_TEST_EXAMPLE_STATE};
}

/// Applies the real schema and then the real example score to `db`, failing
/// the calling test with file and line if any statement is rejected.
void loadExampleScore(EngineDb& db) {
    ASSERT_TRUE(std::filesystem::is_regular_file(realSchemaFile())) << realSchemaFile();
    ASSERT_TRUE(std::filesystem::is_regular_file(exampleScoreFile())) << exampleScoreFile();

    static_cast<void>(loadCypherFile(db, realSchemaFile()));

    const LoadReport report = loadCypherFile(db, exampleScoreFile());
    for (const auto& failure : report.failures) {
        ADD_FAILURE() << failure.file.filename().string() << ", " << failure.lineSpan() << ": "
                      << failure.label << "\n    " << failure.message;
    }
    ASSERT_TRUE(report.ok());
}

/// How many rows a census recorded for `table`, or zero if it recorded none.
std::int64_t rowsIn(const std::vector<RowCount>& counts, std::string_view table) {
    const auto found = std::ranges::find(counts, table, &RowCount::first);
    return found == counts.end() ? 0 : found->second;
}

// The example score names tables; the schema has to declare them. Asserted
// separately from the load so that a missing table says so directly, rather
// than as a Ladybug binder error attributed to a line of the score.
//
// This does not assert that the schema loaded whole -- the test above owns
// that claim, and duplicating it here would mean one schema defect reddening
// two tests that are checking different things.
TEST(ScriptLoader, RealSchemaDeclaresEverythingTheExampleScoreNeeds) {
    ASSERT_TRUE(std::filesystem::is_regular_file(realSchemaFile())) << realSchemaFile();

    EngineDb db;
    static_cast<void>(loadCypherFile(db, realSchemaFile()));
    const auto tables = db.tables();

    for (const std::string_view table :
        {"EngineConfig", "Score", "SoundEntity", "SoundSource", "PartialSet", "Partial",
            "AmplitudeTimeline", "AmplitudeTimelinePoint", "FrequencyTimeline",
            "FrequencyTimelinePoint", "AGGREGATE_OF_ONE_MANY", "HAS_TIMELINE", "NEXT_POINT"}) {
        EXPECT_TRUE(std::ranges::any_of(
            tables, [table](const auto& entry) { return entry.first == table; }))
            << "the example score needs " << table << ", which the schema did not create";
    }
}

// The end-to-end shape of test mode: schema file, then state file, into one
// database. This is exactly what `rendering-engine --mode test` does with its
// defaults, and exactly what `make run-rendering-engine` runs.
TEST(ScriptLoader, ExampleScoreLoadsOnTopOfTheRealSchema) {
    EngineDb db;
    ASSERT_NO_FATAL_FAILURE(loadExampleScore(db));

    // Every statement applying is not the same as the score arriving: a CREATE
    // that matched nothing is accepted and stores nothing. The row census is
    // what distinguishes the two, so the size of the example is asserted here
    // rather than the statement count.
    const Census census = db.census();
    EXPECT_EQ(census.totalNodes, 12);
    EXPECT_EQ(census.totalRelationships, 10);
}

// The example is a score, not a handful of disconnected nodes: one partial,
// reachable from the score through the aggregation chain, carrying an
// amplitude and a frequency timeline of two points each. That is the minimum
// a renderable partial needs.
TEST(ScriptLoader, ExampleScoreHasTheShapeOfAScore) {
    EngineDb db;
    ASSERT_NO_FATAL_FAILURE(loadExampleScore(db));
    const Census census = db.census();

    EXPECT_EQ(rowsIn(census.nodes, "Partial"), 1);
    EXPECT_EQ(rowsIn(census.nodes, "AmplitudeTimelinePoint"), 2);
    EXPECT_EQ(rowsIn(census.nodes, "FrequencyTimelinePoint"), 2);

    // An amplitude timeline and a frequency timeline, on the one partial.
    EXPECT_EQ(rowsIn(census.relationships, "HAS_TIMELINE"), 2);

    // Score -> SoundEntity -> SoundSource -> PartialSet -> Partial.
    EXPECT_EQ(rowsIn(census.relationships, "AGGREGATE_OF_ONE_MANY"), 4);

    // Two linked lists: a head edge and one link edge each.
    EXPECT_EQ(rowsIn(census.relationships, "NEXT_POINT"), 4);
}

} // namespace
