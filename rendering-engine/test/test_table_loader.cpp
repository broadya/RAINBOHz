// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/table_loader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::LoadReport;
using rainbohz::rendering_engine::loadStateFile;
using rainbohz::rendering_engine::loadTableManifest;
using rainbohz::rendering_engine::ManifestError;
using rainbohz::rendering_engine::ScriptLoadError;

/// A directory under the system temporary directory, removed on destruction.
class TempDirectory {
public:
    TempDirectory() {
        static std::random_device entropy;
        path_ =
            std::filesystem::temp_directory_path() /
            ("rainbohz-table-test-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
        std::filesystem::create_directories(path_);
        // The loader resolves symlinks (macOS's /var is one), and so must the
        // paths these tests compare against.
        path_ = std::filesystem::canonical(path_);
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

    std::filesystem::path write(std::string_view name, std::string_view contents) const {
        const std::filesystem::path file = path_ / name;
        std::ofstream stream{file};
        stream << contents;
        return file;
    }

private:
    std::filesystem::path path_;
};

/// The EngineConfig table, as engine-db declares it.
constexpr std::string_view kEngineConfigTable =
    "CREATE NODE TABLE EngineConfig(id SERIAL, sampleRate INT64, channelMode INT64, "
    "PRIMARY KEY(id))";

/// Tables shaped as engine-db's are: SERIAL ids that relationships name.
constexpr std::array<std::string_view, 5> kSchema = {
    kEngineConfigTable,
    "CREATE NODE TABLE PartialSet(id SERIAL, name STRING, PRIMARY KEY(id))",
    "CREATE NODE TABLE Partial(id SERIAL, name STRING, level DOUBLE, PRIMARY KEY(id))",
    "CREATE NODE TABLE PartialStateSet(id SERIAL, PRIMARY KEY(id))",
    "CREATE REL TABLE HAS_PARTIAL(FROM PartialSet TO Partial)",
};

void applySchema(EngineDb& db) {
    for (const std::string_view statement : kSchema) {
        db.execute(statement);
    }
}

/// Writes a Parquet file with Ladybug itself, from the rows `query` returns.
void writeParquet(const std::filesystem::path& file, std::string_view query) {
    EngineDb scratch;
    scratch.execute("COPY (" + std::string{query} + ") TO '" + file.string() + "'");
}

/// Two sets, three partials, a set without columns, and the partials' sets.
/// Partial 0 belongs to set 1, partials 1 and 2 to set 0, so a relationship
/// that ignored the ids could not land right by accident.
void writeScore(const TempDirectory& temp, std::string_view partialRows = "3") {
    writeParquet(temp.path() / "PartialSet.parquet", "UNWIND ['s0', 's1'] AS n RETURN n AS name");
    writeParquet(temp.path() / "Partial.parquet",
        "UNWIND [{name: 'p0', level: -6.0}, {name: 'p1', level: CAST(NULL AS DOUBLE)}, "
        "{name: 'p2', level: 0.1}] AS r RETURN r.name AS name, r.level AS level");
    writeParquet(temp.path() / "HAS_PARTIAL.PartialSet.Partial.parquet",
        "UNWIND [[1, 0], [0, 1], [0, 2]] AS e RETURN e[1] AS `from`, e[2] AS `to`");
    temp.write("manifest.json", R"({
  "format": "rainbohz-score-tables",
  "version": 1,
  "comments": ["test"],
  "engine_config": {"sampleRate": 48000, "channelMode": 1},
  "nodes": [
    {"table": "PartialSet", "rows": 2, "file": "PartialSet.parquet", "columns": ["name"]},
    {"table": "Partial", "rows": )" +
                                    std::string{partialRows} +
                                    R"(, "file": "Partial.parquet", "columns": ["name", "level"]},
    {"table": "PartialStateSet", "rows": 2}
  ],
  "relationships": [
    {"table": "HAS_PARTIAL", "from": "PartialSet", "to": "Partial", "rows": 3,
     "file": "HAS_PARTIAL.PartialSet.Partial.parquet"}
  ]
})");
}

std::int64_t count(EngineDb& db, std::string_view table) {
    const std::vector<DbRow> rows =
        db.query("MATCH (n:" + std::string{table} + ") RETURN count(n)");
    return std::get<std::int64_t>(rows.at(0).at(0));
}

TEST(TableLoader, LoadsNodesThenRelationshipsWithIdsInRowOrder) {
    const TempDirectory temp;
    writeScore(temp);
    EngineDb db;
    applySchema(db);

    const LoadReport report = loadTableManifest(db, temp.path() / "manifest.json");

    ASSERT_TRUE(report.ok()) << report.failures.front().message;
    const std::vector<DbRow> config =
        db.query("MATCH (c:EngineConfig) RETURN c.sampleRate, c.channelMode");
    ASSERT_EQ(config.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(config[0][0]), 48000);
    EXPECT_EQ(std::get<std::int64_t>(config[0][1]), 1);
    EXPECT_EQ(count(db, "PartialStateSet"), 2);

    const std::vector<DbRow> partials =
        db.query("MATCH (s:PartialSet)-[:HAS_PARTIAL]->(p:Partial) "
                 "RETURN p.id, p.name, s.name, p.level ORDER BY p.id");
    ASSERT_EQ(partials.size(), 3U);
    EXPECT_EQ(std::get<std::int64_t>(partials[0][0]), 0);
    EXPECT_EQ(std::get<std::string>(partials[0][1]), "p0");
    EXPECT_EQ(std::get<std::string>(partials[0][2]), "s1");
    EXPECT_EQ(std::get<double>(partials[0][3]), -6.0);
    EXPECT_EQ(std::get<std::string>(partials[1][1]), "p1");
    EXPECT_EQ(std::get<std::string>(partials[1][2]), "s0");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(partials[1][3]));
    EXPECT_EQ(std::get<std::string>(partials[2][2]), "s0");
    EXPECT_EQ(std::get<double>(partials[2][3]), 0.1);
}

// The ids are the row indices only in a table that starts empty. Copying on
// top of rows would wire every relationship to the wrong node, silently.
TEST(TableLoader, RefusesATableThatAlreadyHoldsRows) {
    const TempDirectory temp;
    writeScore(temp);
    EngineDb db;
    applySchema(db);
    db.execute("CREATE (:Partial {name: 'earlier'})");

    const LoadReport report = loadTableManifest(db, temp.path() / "manifest.json");

    ASSERT_FALSE(report.ok());
    EXPECT_NE(report.failures.front().message.find("already holds 1 row(s)"), std::string::npos);
    EXPECT_EQ(report.failures.front().file, temp.path() / "Partial.parquet");
    EXPECT_EQ(count(db, "Partial"), 1);
}

TEST(TableLoader, ReportsARowCountThatDisagreesWithTheManifest) {
    const TempDirectory temp;
    writeScore(temp, "4");
    EngineDb db;
    applySchema(db);

    const LoadReport report = loadTableManifest(db, temp.path() / "manifest.json");

    ASSERT_EQ(report.failures.size(), 1U);
    EXPECT_NE(
        report.failures[0].message.find("holds 3 row(s) after the copy; the manifest lists 4"),
        std::string::npos);
}

/// A manifest of one node table whose entry is `entry`.
std::string oneNodeManifest(std::string_view entry) {
    return R"({"format": "rainbohz-score-tables", "version": 1, "engine_config": {},
               "nodes": [)" +
           std::string{entry} + R"(], "relationships": []})";
}

// Every problem with a manifest is found before anything is executed, so a
// refused manifest leaves engine-db as it was.
TEST(TableLoader, RefusesABadManifestBeforeExecutingAnything) {
    const TempDirectory temp;
    writeScore(temp);
    const TempDirectory outside;
    writeParquet(outside.path() / "Partial.parquet", "RETURN 'x' AS name");
    std::filesystem::create_symlink(
        outside.path() / "Partial.parquet", temp.path() / "Linked.parquet");

    const std::vector<std::string> manifests = {
        "[]",
        "not json",
        R"({"format": "something-else", "version": 1})",
        R"({"format": "rainbohz-score-tables", "version": 2})",
        oneNodeManifest(R"({"table": "Partial", "rows": 1})"
                        R"(, "file": "../Partial.parquet", "columns": ["name"]})"),
        oneNodeManifest(R"({"table": "Partial", "rows": 1, "file": ")" +
                        (outside.path() / "Partial.parquet").string() +
                        R"(", "columns": ["name"]})"),
        oneNodeManifest(
            R"({"table": "Partial", "rows": 1, "file": "Linked.parquet", "columns": ["name"]})"),
        oneNodeManifest(
            R"({"table": "Partial", "rows": 1, "file": "Absent.parquet", "columns": ["name"]})"),
        oneNodeManifest(
            R"({"table": "Partial", "rows": 1, "file": "manifest.json", "columns": ["name"]})"),
        oneNodeManifest(R"({"table": "Partial) DETACH DELETE (n", "rows": 1})"),
        oneNodeManifest(
            R"({"table": "Partial", "rows": 1, "file": "Partial.parquet", "columns": ["na me"]})"),
        oneNodeManifest(R"({"table": "Partial", "rows": -1})"),
        oneNodeManifest(R"({"table": "Partial", "rows": 1, "columns": ["name"]})"),
    };
    for (const std::string& manifest : manifests) {
        SCOPED_TRACE(manifest);
        const std::filesystem::path file = temp.write("bad.json", manifest);
        EngineDb db;
        applySchema(db);
        EXPECT_THROW(static_cast<void>(loadTableManifest(db, file)), ManifestError);
        EXPECT_EQ(count(db, "Partial"), 0);
        EXPECT_EQ(count(db, "EngineConfig"), 0);
    }
}

// Ladybug numbers rows in file order within a row group; a few thousand rows
// span several of Ladybug's 2,048-row vectors.
TEST(TableLoader, IdsFollowFileOrderPastOneVector) {
    const TempDirectory temp;
    writeParquet(temp.path() / "Partial.parquet",
        "UNWIND range(0, 4999) AS i RETURN 'p' + CAST(i AS STRING) AS name");
    temp.write("manifest.json",
        oneNodeManifest(
            R"({"table": "Partial", "rows": 5000, "file": "Partial.parquet", "columns": ["name"]})"));
    EngineDb db;
    applySchema(db);

    ASSERT_TRUE(loadTableManifest(db, temp.path() / "manifest.json").ok());
    const std::vector<DbRow> rows = db.query("MATCH (p:Partial) RETURN p.id, p.name");
    ASSERT_EQ(rows.size(), 5000U);
    for (const DbRow& row : rows) {
        ASSERT_EQ(
            "p" + std::to_string(std::get<std::int64_t>(row[0])), std::get<std::string>(row[1]));
    }
}

TEST(TableLoader, StateFileIsChosenByExtension) {
    const TempDirectory temp;
    writeScore(temp);
    {
        EngineDb db;
        applySchema(db);
        EXPECT_TRUE(loadStateFile(db, temp.path() / "manifest.json").ok());
        EXPECT_EQ(count(db, "Partial"), 3);
    }
    {
        EngineDb db;
        applySchema(db);
        const std::filesystem::path script =
            temp.write("state.cypher", "CREATE (:Partial {name: 'p'});\n");
        EXPECT_TRUE(loadStateFile(db, script).ok());
        EXPECT_EQ(count(db, "Partial"), 1);
    }
    {
        EngineDb db;
        const std::filesystem::path other = temp.write("state.txt", "CREATE (:Partial);\n");
        EXPECT_THROW(static_cast<void>(loadStateFile(db, other)), ScriptLoadError);
    }
}

} // namespace
