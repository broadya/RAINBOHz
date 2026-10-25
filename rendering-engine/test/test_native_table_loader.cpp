// SPDX-License-Identifier: Apache-2.0

// Tests for the native backend's table loader: what it accepts, what it
// refuses, and that it loads every example score as Ladybug does.
//
// Small Parquet files are written by Ladybug itself (COPY TO), so the
// native reader is also checked against a second writer besides pyarrow.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/native_table_loader.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/table_loader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using rainbohz::rendering_engine::Census;
using rainbohz::rendering_engine::DbRow;
using rainbohz::rendering_engine::DbValue;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::LoadReport;
using rainbohz::rendering_engine::loadTableManifest;
using rainbohz::rendering_engine::ManifestError;
using rainbohz::rendering_engine::native::censusOf;
using rainbohz::rendering_engine::native::loadNativeScore;
using rainbohz::rendering_engine::native::NativeLoad;
using rainbohz::rendering_engine::native::NativeModel;

/// A directory under the system temporary directory, removed on destruction.
class TempDirectory {
public:
    TempDirectory() {
        static std::random_device entropy;
        path_ = std::filesystem::temp_directory_path() /
                ("rainbohz-native-loader-test-" + std::to_string(entropy()) + "-" +
                    std::to_string(entropy()));
        std::filesystem::create_directories(path_);
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

    /// Writes a text file into the directory.
    std::filesystem::path write(std::string_view name, std::string_view contents) const {
        const std::filesystem::path file = path_ / name;
        std::ofstream stream{file};
        stream << contents;
        return file;
    }

    /// Writes a Parquet file with Ladybug, from the rows `query` returns.
    void parquet(std::string_view name, std::string_view query) const {
        EngineDb scratch;
        scratch.execute("COPY (" + std::string{query} + ") TO '" + (path_ / name).string() + "'");
    }

private:
    std::filesystem::path path_;
};

/// A manifest of `nodes` and `relationships` entries, with an EngineConfig.
std::string manifest(std::string_view nodes, std::string_view relationships) {
    return R"({"format": "rainbohz-score-tables", "version": 1,
  "engine_config": {"sampleRate": 48000, "channelMode": 1},
  "nodes": [)" +
           std::string{nodes} + R"(],
  "relationships": [)" +
           std::string{relationships} + "]}";
}

/// Two sets and three partials: partial 0 in set 1, partials 1 and 2 in set
/// 0, so that an edge that ignored the ids could not land right by accident.
void writeSetsAndPartials(const TempDirectory& temp) {
    temp.parquet("PartialSet.parquet", "UNWIND ['s0', 's1'] AS n RETURN n AS name");
    temp.parquet(
        "Partial.parquet", "UNWIND ['p0', CAST(NULL AS STRING), 'p2'] AS n RETURN n AS name");
    temp.parquet("HAS_PARTIAL.PartialSet.Partial.parquet",
        "UNWIND [[1, 0], [0, 1], [0, 2]] AS e RETURN e[1] AS `from`, e[2] AS `to`");
}

/// The nodes entry for writeSetsAndPartials()'s tables.
constexpr std::string_view kSetsAndPartials =
    R"({"table": "PartialSet", "rows": 2, "file": "PartialSet.parquet", "columns": ["name"]},
       {"table": "Partial", "rows": 3, "file": "Partial.parquet", "columns": ["name"]},
       {"table": "PartialStateSet", "rows": 4})";

/// The relationships entry for writeSetsAndPartials()'s edges.
constexpr std::string_view kHasPartial =
    R"({"table": "HAS_PARTIAL", "from": "PartialSet", "to": "Partial", "rows": 3,
        "file": "HAS_PARTIAL.PartialSet.Partial.parquet"})";

/// Expects loading `file` to throw a ManifestError whose message holds
/// `expected`.
void expectRefused(const std::filesystem::path& file, std::string_view expected) {
    try {
        static_cast<void>(loadNativeScore(file));
        ADD_FAILURE() << "loaded; expected a refusal mentioning: " << expected;
    } catch (const ManifestError& e) {
        EXPECT_NE(std::string{e.what()}.find(expected), std::string::npos) << e.what();
    }
}

TEST(NativeTableLoader, LoadsRowsByIndexEdgesByIdAndTheEngineConfig) {
    const TempDirectory temp;
    writeSetsAndPartials(temp);
    const auto file = temp.write("manifest.json", manifest(kSetsAndPartials, kHasPartial));

    const NativeLoad load = loadNativeScore(file);
    const auto& c = load.model.canonical;
    EXPECT_EQ(load.nodeTables, 3U);
    EXPECT_EQ(load.relationshipPairs, 1U);
    EXPECT_EQ(load.rows, 1 + 2 + 3 + 4 + 3);

    ASSERT_EQ(c.engineConfigs.size(), 1U);
    EXPECT_EQ(c.engineConfigs[0].sampleRate, 48000);
    EXPECT_EQ(c.engineConfigs[0].channelMode, 1);
    EXPECT_FALSE(c.engineConfigs[0].frameSize.has_value());

    ASSERT_EQ(c.partials.size(), 3U);
    EXPECT_EQ(c.partials[0].name, "p0");
    EXPECT_FALSE(c.partials[1].name.has_value());
    EXPECT_EQ(c.partialSets[1].name, "s1");
    EXPECT_EQ(c.partialStateSets, 4U);

    ASSERT_EQ(c.hasPartial.size(), 3U);
    EXPECT_EQ(c.hasPartial.edges()[0].from, 1);
    EXPECT_EQ(c.hasPartial.edges()[0].to, 0);
}

TEST(NativeTableLoader, AColumnTheFileLacksTakesTheSchemaDefault) {
    const TempDirectory temp;
    temp.parquet("PartialSetMusicalTimelinePoint.parquet",
        "UNWIND [2, 3] AS t RETURN t AS frequency_curve_type");
    const auto file = temp.write(
        "manifest.json", manifest(R"({"table": "PartialSetMusicalTimelinePoint", "rows": 2,
                     "file": "PartialSetMusicalTimelinePoint.parquet",
                     "columns": ["frequency_curve_type"]})",
                             ""));
    const NativeModel model = loadNativeScore(file).model;
    const auto& points = model.canonical.partialSetMusicalTimelinePoints;
    ASSERT_EQ(points.size(), 2U);
    EXPECT_EQ(points[1].frequencyCurveType, 3);
    EXPECT_EQ(points[1].amplitudeCurveType, 0);
    EXPECT_EQ(points[1].amplitudeCurveAmount, 0.0F);
}

TEST(NativeTableLoader, RefusesWhatTheNativeModelDoesNotHold) {
    const TempDirectory temp;
    temp.parquet("FrameEntry.parquet", "UNWIND [1] AS x RETURN x AS id");
    expectRefused(temp.write("a.json", manifest(R"({"table": "FrameEntry", "rows": 1})", "")),
        "only a score's canonical tables");
    writeSetsAndPartials(temp);
    expectRefused(temp.write("b.json", manifest(kSetsAndPartials,
                                           R"({"table": "PARTIAL_IN_FRAME", "from": "Partial",
                                               "to": "PartialSet", "rows": 3,
                                               "file": "HAS_PARTIAL.PartialSet.Partial.parquet"})")),
        "only a score's canonical relationships");
}

TEST(NativeTableLoader, RefusesColumnsTheManifestOrTheSchemaDoNotName) {
    const TempDirectory temp;
    temp.parquet("Partial.parquet", "UNWIND ['p'] AS n RETURN n AS name, 1.0 AS level");
    expectRefused(
        temp.write("a.json", manifest(R"({"table": "Partial", "rows": 1, "file": "Partial.parquet",
                                         "columns": ["name"]})",
                                 "")),
        "columns are not the 1 the manifest lists");
    expectRefused(
        temp.write("b.json", manifest(R"({"table": "Partial", "rows": 1, "file": "Partial.parquet",
                                         "columns": ["name", "level"]})",
                                 "")),
        "Partial has no property level");
}

TEST(NativeTableLoader, RefusesAValueOfTheWrongTypeOrRange) {
    const TempDirectory temp;
    temp.parquet("TempoMap.parquet", "UNWIND ['m'] AS n RETURN n AS name, 'x' AS ticks_per_beat");
    expectRefused(temp.write("a.json",
                      manifest(R"({"table": "TempoMap", "rows": 1, "file": "TempoMap.parquet",
                                         "columns": ["name", "ticks_per_beat"]})",
                          "")),
        "column ticks_per_beat is string; int64 is expected");
    temp.parquet(
        "TempoMap.parquet", "UNWIND ['m'] AS n RETURN n AS name, 4294967296 AS ticks_per_beat");
    expectRefused(temp.write("b.json",
                      manifest(R"({"table": "TempoMap", "rows": 1, "file": "TempoMap.parquet",
                                         "columns": ["name", "ticks_per_beat"]})",
                          "")),
        "does not fit the schema's INT32");
    temp.parquet("PartialState.parquet", "UNWIND [1.0e300] AS f RETURN f AS frequency");
    expectRefused(temp.write("c.json", manifest(R"({"table": "PartialState", "rows": 1,
                                  "file": "PartialState.parquet", "columns": ["frequency"]})",
                                           "")),
        "beyond the range of the schema's FLOAT");
}

TEST(NativeTableLoader, RefusesARowCountThatDisagreesWithTheManifest) {
    const TempDirectory temp;
    writeSetsAndPartials(temp);
    expectRefused(
        temp.write("a.json", manifest(R"({"table": "Partial", "rows": 4, "file": "Partial.parquet",
                         "columns": ["name"]})",
                                 "")),
        "holds 3 row(s); the manifest lists 4");
}

TEST(NativeTableLoader, RefusesAnEdgeToNoRowOrBreakingItsMultiplicity) {
    const TempDirectory temp;
    writeSetsAndPartials(temp);
    temp.parquet("HAS_PARTIAL.PartialSet.Partial.parquet",
        "UNWIND [[0, 0], [0, 7]] AS e RETURN e[1] AS `from`, e[2] AS `to`");
    expectRefused(temp.write("a.json", manifest(kSetsAndPartials,
                                           R"({"table": "HAS_PARTIAL", "from": "PartialSet",
                                               "to": "Partial", "rows": 2,
                                               "file": "HAS_PARTIAL.PartialSet.Partial.parquet"})")),
        "has Partial 7, which names no row of Partial");
    // HAS_PARTIAL is ONE_MANY: a partial is in one set.
    temp.parquet("HAS_PARTIAL.PartialSet.Partial.parquet",
        "UNWIND [[0, 0], [1, 0]] AS e RETURN e[1] AS `from`, e[2] AS `to`");
    expectRefused(temp.write("b.json", manifest(kSetsAndPartials,
                                           R"({"table": "HAS_PARTIAL", "from": "PartialSet",
                                               "to": "Partial", "rows": 2,
                                               "file": "HAS_PARTIAL.PartialSet.Partial.parquet"})")),
        "edge 1: node 0 already has an edge to it");
}

TEST(NativeTableLoader, RefusesRelationshipColumnsOutOfTheSchemasOrder) {
    const TempDirectory temp;
    temp.parquet("TempoMap.parquet", "UNWIND ['a', 'b'] AS n RETURN n AS name");
    temp.parquet("TEMPO_MAP_SYNCHRONIZATION_POINT.TempoMap.TempoMap.parquet",
        "UNWIND [1] AS x RETURN 1 AS `from`, 0 AS `to`, 0 AS sync_point_child, "
        "0 AS sync_point_parent");
    expectRefused(temp.write("a.json",
                      manifest(R"({"table": "TempoMap", "rows": 2, "file": "TempoMap.parquet",
                         "columns": ["name"]})",
                          R"({"table": "TEMPO_MAP_SYNCHRONIZATION_POINT", "from": "TempoMap",
                    "to": "TempoMap", "rows": 1,
                    "file": "TEMPO_MAP_SYNCHRONIZATION_POINT.TempoMap.TempoMap.parquet"})")),
        "its columns are not from, to, sync_point_parent, sync_point_child");
}

TEST(NativeTableLoader, RefusesATableListedTwice) {
    const TempDirectory temp;
    writeSetsAndPartials(temp);
    expectRefused(temp.write("a.json", manifest(std::string{kSetsAndPartials} +
                                                    R"(, {"table": "Partial", "rows": 3,
                                                "file": "Partial.parquet", "columns": ["name"]})",
                                           "")),
        "Partial is listed twice");
    expectRefused(temp.write("b.json", manifest(std::string{kSetsAndPartials} +
                                                    R"(, {"table": "PartialStateSet", "rows": 1})",
                                           "")),
        "PartialStateSet is listed twice");
    // Listed first with no rows, which a check on what the table holds would miss.
    expectRefused(temp.write("c.json", manifest(R"({"table": "PartialStateSet", "rows": 0},
                                                  {"table": "PartialStateSet", "rows": 2})",
                                           "")),
        "PartialStateSet is listed twice");
    expectRefused(temp.write("d.json", manifest(kSetsAndPartials, std::string{kHasPartial} + ", " +
                                                                      std::string{kHasPartial})),
        "HAS_PARTIAL PartialSet->Partial is listed twice");
}

// ---------------------------------------------------------------------------
// Against Ladybug, on every example score
// ---------------------------------------------------------------------------

/// The compiled examples, as compile_scores.py names them.
constexpr std::array<std::string_view, 5> kExamples{
    "tempo-map-score", "polytempo-shift-score", "polytempo-stretch-score", "spatial-score", "demo"};

/// An example's manifest, compiled by the `scores.compile` CTest fixture.
///
/// @param name  The example.
/// @return The manifest's path; the test fails when it is missing.
std::filesystem::path compiledScore(std::string_view name) {
    const std::filesystem::path file =
        std::filesystem::path{RE_TEST_COMPILED_SCORES} / name / "manifest.json";
    EXPECT_TRUE(std::filesystem::exists(file))
        << file << " is missing. Run the tests through CTest, whose scores.compile test "
        << "compiles the examples, or run rendering-engine/test/compile_scores.py.";
    return file;
}

/// Loads an example into a fresh Ladybug engine-db, schema first.
void loadIntoLadybug(EngineDb& db, const std::filesystem::path& manifest) {
    const LoadReport schema = loadCypherFile(db, RE_TEST_SCHEMA_FILE);
    ASSERT_TRUE(schema.ok());
    const LoadReport tables = loadTableManifest(db, manifest);
    ASSERT_TRUE(tables.ok()) << tables.failures.front().message;
}

/// Converts a field to what Ladybug's query would return for it: a FLOAT is
/// widened to double, every integer to int64, NULL to monostate.
template <typename T>
DbValue asDb(const std::optional<T>& value) {
    if (!value.has_value()) {
        return std::monostate{};
    }
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
        return *value;
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<double>(*value);
    } else {
        return static_cast<std::int64_t>(*value);
    }
}

TEST(NativeTableLoader, LoadsEveryExampleAsLadybugDoes) {
    for (const std::string_view name : kExamples) {
        SCOPED_TRACE(std::string{name});
        const std::filesystem::path file = compiledScore(name);
        if (!std::filesystem::exists(file)) {
            continue;
        }
        EngineDb db;
        loadIntoLadybug(db, file);
        const NativeModel model = loadNativeScore(file).model;
        const auto& c = model.canonical;

        const Census ladybug = db.census();
        const Census native = censusOf(model);
        EXPECT_EQ(native.nodes, ladybug.nodes);
        EXPECT_EQ(native.relationships, ladybug.relationships);
        EXPECT_EQ(native.totalNodes, ladybug.totalNodes);
        EXPECT_EQ(native.totalRelationships, ladybug.totalRelationships);

        // Every value whose type the loader narrows, compared exactly.
        const std::vector<DbRow> states = db.query(
            "MATCH (s:PartialState) RETURN s.frequency, s.phase, s.phase_free_run, s.amplitude "
            "ORDER BY s.id");
        ASSERT_EQ(states.size(), c.partialStates.size());
        for (std::size_t i = 0; i < states.size(); ++i) {
            EXPECT_EQ(asDb(c.partialStates[i].frequency), states[i][0]) << "state " << i;
            EXPECT_EQ(asDb(c.partialStates[i].phase), states[i][1]) << "state " << i;
            EXPECT_EQ(asDb(c.partialStates[i].phaseFreeRun), states[i][2]) << "state " << i;
            EXPECT_EQ(asDb(c.partialStates[i].amplitude), states[i][3]) << "state " << i;
        }
        const std::vector<DbRow> points =
            db.query("MATCH (p:PartialSetMusicalTimelinePoint) RETURN p.amplitude_curve_type, "
                     "p.amplitude_curve_amount, p.frequency_curve_type, p.frequency_curve_amount "
                     "ORDER BY p.id");
        ASSERT_EQ(points.size(), c.partialSetMusicalTimelinePoints.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& row = c.partialSetMusicalTimelinePoints[i];
            EXPECT_EQ(asDb(row.amplitudeCurveType), points[i][0]);
            EXPECT_EQ(asDb(row.amplitudeCurveAmount), points[i][1]);
            EXPECT_EQ(asDb(row.frequencyCurveType), points[i][2]);
            EXPECT_EQ(asDb(row.frequencyCurveAmount), points[i][3]);
        }
        const std::vector<DbRow> spatial =
            db.query("MATCH (s:SpatialPositionState) RETURN s.azimuth, s.elevation, s.distance "
                     "ORDER BY s.id");
        ASSERT_EQ(spatial.size(), c.spatialPositionStates.size());
        for (std::size_t i = 0; i < spatial.size(); ++i) {
            EXPECT_EQ(asDb(c.spatialPositionStates[i].azimuth), spatial[i][0]);
            EXPECT_EQ(asDb(c.spatialPositionStates[i].elevation), spatial[i][1]);
            EXPECT_EQ(asDb(c.spatialPositionStates[i].distance), spatial[i][2]);
        }
        const std::vector<DbRow> maps =
            db.query("MATCH (m:TempoMap) RETURN m.name, m.ticks_per_beat ORDER BY m.id");
        ASSERT_EQ(maps.size(), c.tempoMaps.size());
        for (std::size_t i = 0; i < maps.size(); ++i) {
            EXPECT_EQ(asDb(c.tempoMaps[i].name), maps[i][0]);
            EXPECT_EQ(asDb(c.tempoMaps[i].ticksPerBeat), maps[i][1]);
        }
        const std::vector<DbRow> pointAt =
            db.query("MATCH (p:PartialSetMusicalTimelinePoint)-[a:POINT_AT]->(m:TempoMap) "
                     "RETURN p.id, m.id, a.sync_point, a.offset_seconds ORDER BY p.id");
        ASSERT_EQ(pointAt.size(), c.setPointAt.size());
        // A point has at most one POINT_AT (MANY_ONE), so its id keys it.
        std::map<std::int64_t, DbRow> nativePointAt;
        for (const auto& edge : c.setPointAt.edges()) {
            nativePointAt.emplace(
                edge.from, DbRow{edge.from, edge.to, asDb(edge.properties.syncPoint),
                               asDb(edge.properties.offsetSeconds)});
        }
        std::size_t at = 0;
        for (const auto& [point, row] : nativePointAt) {
            EXPECT_EQ(row, pointAt.at(at)) << "POINT_AT of point " << point;
            ++at;
        }
        const std::vector<DbRow> configs =
            db.query("MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate, c.channelMode");
        ASSERT_EQ(configs.size(), c.engineConfigs.size());
        EXPECT_EQ(asDb(c.engineConfigs[0].frameSize), configs[0][0]);
        EXPECT_EQ(asDb(c.engineConfigs[0].sampleRate), configs[0][1]);
        EXPECT_EQ(asDb(c.engineConfigs[0].channelMode), configs[0][2]);
    }
}

} // namespace
