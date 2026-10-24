// SPDX-License-Identifier: Apache-2.0

// Tests for server mode's handlers, driven directly through ServerCore.
//
// No socket is opened: ServerCore takes a call's path segments, query
// parameters and body and returns a status and a JSON body, which is
// everything the server decides. That HTTP carries those faithfully, and
// routes each path and method to the right handler, is checked end to end by
// test/server/run_server_check.py.
//
// Each test gets its own input root, holding copies of the example scores and
// a deliberately broken one, and its own output directory, so that tests can
// run in parallel and the repository's own files are never inside a root.

#include "rainbohz/rendering_engine/file_uri.hpp"
#include "rainbohz/rendering_engine/server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

// Injected by CMake: the real engine-db schema, and two example scores in
// musical time and one in physical time.
#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_TEMPO_MAP_STATE
#define RE_TEST_TEMPO_MAP_STATE ""
#endif
#ifndef RE_TEST_TWO_SETS_STATE
#define RE_TEST_TWO_SETS_STATE ""
#endif

namespace {

using Json = nlohmann::json;
using rainbohz::rendering_engine::fileUriToPath;
using rainbohz::rendering_engine::HttpReply;
using rainbohz::rendering_engine::pathToFileUri;
using rainbohz::rendering_engine::PipelineSettings;
using rainbohz::rendering_engine::QueryParameters;
using rainbohz::rendering_engine::ServerCore;
using rainbohz::rendering_engine::ServerSettings;

/// A fresh directory under the system temporary directory, removed with
/// everything in it on destruction.
class TempDirectory {
public:
    /// @param prefix  Start of the directory's name, to say what it is for.
    explicit TempDirectory(const std::string& prefix) {
        // ctest may run several test processes at once, so a per-process
        // counter would not keep two directories apart on its own.
        static std::random_device entropy;
        path_ = std::filesystem::temp_directory_path() /
                (prefix + std::to_string(entropy()) + "-" + std::to_string(entropy()));
        std::filesystem::create_directories(path_);
        // Resolved, so that comparisons with what the server reports hold on
        // macOS, where the temporary directory is reached through a symlink.
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

    /// @return The directory.
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// A score with a long tempo map and no partials: 75 points, every beat of
/// 960 ticks at 60 BPM, more than two walk chunks.
std::string longMapScore() {
    std::string cypher =
        "CREATE (:EngineConfig {sampleRate: 48000, frameSize: 1000, channelMode: 1});\n"
        "CREATE (s:Score {name: 'Long'})\n"
        "CREATE (m:TempoMap {name: 'Long', ticks_per_beat: 960})\n"
        "CREATE (s)-[:NEXT_POINT]->(m)\n"
        "CREATE (m)";
    for (int i = 0; i < 75; ++i) {
        cypher += "-[:NEXT_POINT]->(:TempoMapPoint {position: " + std::to_string(960 * i) +
                  ", start_tempo: 60.0, hold: true})";
    }
    return cypher + ";\n";
}

/// A server over a private input root and output directory.
class ServerTest : public ::testing::Test {
protected:
    ServerTest() {
        std::filesystem::copy_file(RE_TEST_TEMPO_MAP_STATE, inputRoot_.path() / "tempo-map.cypher");
        std::filesystem::copy_file(RE_TEST_TWO_SETS_STATE, inputRoot_.path() / "two sets.cypher");
        std::ofstream{inputRoot_.path() / "broken.cypher"} << "CREATE (n:NoSuchTable {x: 1});\n";
        std::ofstream{inputRoot_.path() / "long-map.cypher"} << longMapScore();
    }

    /// Makes the server; called by each test, so that a test can change the
    /// settings first.
    ServerCore& server() {
        if (!server_) {
            server_ = std::make_unique<ServerCore>(ServerSettings{.schemaFile = RE_TEST_SCHEMA_FILE,
                                                       .inputRoot = inputRoot_.path(),
                                                       .outputDir = output_.path() / "frames",
                                                       .pipeline = pipeline_},
                echo_);
        }
        return *server_;
    }

    /// @param name  A file in the input root.
    /// @return A PUT /v1/score body naming it.
    std::string scoreBody(const std::string& name) const {
        return Json{{"uri", pathToFileUri(inputRoot_.path() / name)}}.dump();
    }

    /// Loads a file from the input root and expects it to succeed.
    ///
    /// @param name  A file in the input root.
    /// @return The parsed response.
    Json loadOk(const std::string& name) {
        const HttpReply reply = server().putScore(scoreBody(name));
        EXPECT_EQ(reply.status, 200) << reply.body;
        EXPECT_EQ(reply.contentType, "application/json");
        return Json::parse(reply.body);
    }

    /// Whether the echoed narrative contains `text`.
    bool echoMentions(const std::string& text) const {
        return echo_.str().find(text) != std::string::npos;
    }

    TempDirectory inputRoot_{"rainbohz-server-input-"};
    TempDirectory output_{"rainbohz-server-output-"};
    PipelineSettings pipeline_;
    std::ostringstream echo_;
    std::unique_ptr<ServerCore> server_;
};

/// Checks that a reply is a problem document with `status`, and returns it.
///
/// The shape is compared as one value, so that a failure shows every field
/// that differs at once.
Json expectProblem(const HttpReply& reply, int status) {
    const Json problem = Json::parse(reply.body);
    const auto isString = [&](const char* key) {
        return problem.contains(key) && problem.at(key).is_string();
    };
    const Json shape{{"http_status", reply.status}, {"content_type", reply.contentType},
        {"type", problem.value("type", "")}, {"status", problem.value("status", 0)},
        {"title", isString("title")}, {"detail", isString("detail")},
        {"log", problem.contains("log")}};
    const Json expected{{"http_status", status}, {"content_type", "application/problem+json"},
        {"type", "about:blank"}, {"status", status}, {"title", true}, {"detail", true},
        {"log", false}};
    EXPECT_EQ(shape, expected) << reply.body;
    return problem;
}

/// Checks that a reply is a 200 resource, and returns it.
Json expectResource(const HttpReply& reply) {
    EXPECT_EQ(reply.status, 200) << reply.body;
    EXPECT_EQ(reply.contentType, "application/json");
    return Json::parse(reply.body);
}

// ---------------------------------------------------------------------------
// PUT /v1/score
// ---------------------------------------------------------------------------

TEST_F(ServerTest, ItStartsWithNoScore) {
    EXPECT_FALSE(server().hasScore());
}

TEST_F(ServerTest, ALoadReportsWhatLoaded) {
    const Json response = loadOk("tempo-map.cypher");
    EXPECT_TRUE(server().hasScore());
    EXPECT_EQ(response.at("uri"), pathToFileUri(inputRoot_.path() / "tempo-map.cypher"));
    EXPECT_EQ(response.at("partials"), 2);
    EXPECT_GT(response.at("nodes").get<int>(), 0);
    EXPECT_GT(response.at("relationships").get<int>(), 0);
    EXPECT_FALSE(response.contains("log"));
    // The narrative is batch mode's, on the echo only: the summary of each
    // file, and the census.
    EXPECT_TRUE(echoMentions("PUT /v1/score\n"));
    EXPECT_TRUE(echoMentions("schema: applied"));
    EXPECT_TRUE(echoMentions("precomputed 2 of 2 partial(s)"));
}

TEST_F(ServerTest, ABadScoreBodyIsABadRequest) {
    for (const char* body : {"", "{}", R"({"uri": 1})", R"({"uri": "x", "extra": 1})",
             R"({"uri": "http://host/a.cypher"})", R"({"uri": "file://host/a.cypher"})",
             R"({"uri": "file:///a%zz"})"}) {
        expectProblem(server().putScore(body), 400);
    }
}

TEST_F(ServerTest, EveryProblemInAScoreBodyIsListed) {
    const Json problem = expectProblem(server().putScore(R"({"uri": 1, "extra": 0})"), 400);
    EXPECT_EQ(problem.at("errors").size(), 2);
}

TEST_F(ServerTest, AFileOutsideTheInputRootIsForbidden) {
    expectProblem(server().putScore(Json{{"uri", pathToFileUri(RE_TEST_SCHEMA_FILE)}}.dump()), 403);
}

TEST_F(ServerTest, DotDotCannotLeaveTheInputRoot) {
    const std::filesystem::path escape =
        inputRoot_.path() / ".." / inputRoot_.path().filename() / ".." / "elsewhere.cypher";
    expectProblem(server().putScore(Json{{"uri", pathToFileUri(escape)}}.dump()), 403);

    // A `..` that stays inside is fine.
    const std::filesystem::path inside = inputRoot_.path() / "sub" / ".." / "tempo-map.cypher";
    EXPECT_EQ(server().putScore(Json{{"uri", pathToFileUri(inside)}}.dump()).status, 200);
}

TEST_F(ServerTest, ASymbolicLinkCannotLeaveTheInputRoot) {
    std::filesystem::create_symlink(RE_TEST_SCHEMA_FILE, inputRoot_.path() / "link.cypher");
    expectProblem(server().putScore(scoreBody("link.cypher")), 403);
}

TEST_F(ServerTest, ANameThatNeedsEncodingLoads) {
    // "two sets.cypher" has a space, so its URI has %20.
    EXPECT_NE(scoreBody("two sets.cypher").find("%20"), std::string::npos);
    EXPECT_EQ(loadOk("two sets.cypher").at("partials"), 5);
}

TEST_F(ServerTest, AMissingFileIsNotFound) {
    expectProblem(server().putScore(scoreBody("no-such-score.cypher")), 404);
    std::filesystem::create_directory(inputRoot_.path() / "a-directory");
    expectProblem(server().putScore(scoreBody("a-directory")), 404);
}

// A score's tables load as its Cypher does. The manifest here holds only the
// EngineConfig; test_table_loader.cpp covers the tables themselves.
TEST_F(ServerTest, AManifestOfTablesLoads) {
    std::ofstream{inputRoot_.path() / "manifest.json"}
        << R"({"format": "rainbohz-score-tables", "version": 1,
               "engine_config": {"sampleRate": 48000, "channelMode": 0},
               "nodes": [], "relationships": []})";
    const Json response = loadOk("manifest.json");
    EXPECT_EQ(response.at("partials"), 0);
    EXPECT_EQ(response.at("nodes"), 1);
}

// A manifest that names a file outside its directory is a fault in the
// score, like a statement that does not apply, and leaves no score behind.
TEST_F(ServerTest, AManifestNamingAFileOutsideItIsUnprocessable) {
    loadOk("tempo-map.cypher");
    std::ofstream{inputRoot_.path() / "manifest.json"}
        << R"({"format": "rainbohz-score-tables", "version": 1, "engine_config": {},
               "nodes": [{"table": "Partial", "rows": 1, "file": "../Partial.parquet",
                          "columns": ["name"]}],
               "relationships": []})";
    const Json problem = expectProblem(server().putScore(scoreBody("manifest.json")), 422);
    EXPECT_NE(
        problem.at("detail").get<std::string>().find("../Partial.parquet"), std::string::npos);
    expectProblem(server().engineConfig({}), 409);
}

TEST_F(ServerTest, AFailedLoadLeavesNoScore) {
    loadOk("tempo-map.cypher");
    const Json problem = expectProblem(server().putScore(scoreBody("broken.cypher")), 422);
    EXPECT_EQ(problem.at("detail"), "1 statement(s) did not apply. engine-db is partially loaded.");
    EXPECT_TRUE(echoMentions("FAILED   broken.cypher, line 1"));

    EXPECT_FALSE(server().hasScore());
    expectProblem(server().renderFrame("0", ""), 409);
    expectProblem(server().engineConfig({}), 409);
}

TEST_F(ServerTest, ARefusedLoadKeepsTheScore) {
    // Only a load that gets as far as loading replaces the score; one refused
    // for its request does not.
    loadOk("tempo-map.cypher");
    expectProblem(server().putScore(scoreBody("no-such-score.cypher")), 404);
    EXPECT_TRUE(server().hasScore());
}

TEST_F(ServerTest, ASecondLoadReplacesTheFirst) {
    const Json first = loadOk("tempo-map.cypher");
    const Json second = loadOk("two sets.cypher");
    EXPECT_EQ(second.at("partials"), 5);

    // Loading the first again gives exactly the counts it had: nothing of the
    // second score is left behind.
    const Json again = loadOk("tempo-map.cypher");
    EXPECT_EQ(again.at("nodes"), first.at("nodes"));
    EXPECT_EQ(again.at("relationships"), first.at("relationships"));
}

// ---------------------------------------------------------------------------
// POST /v1/frames/{frame}/renderings
// ---------------------------------------------------------------------------

TEST_F(ServerTest, RenderingBeforeALoadIsAConflict) {
    const Json problem = expectProblem(server().renderFrame("0", ""), 409);
    EXPECT_EQ(problem.at("title"), "Conflict");
}

TEST_F(ServerTest, ARenderReturnsTheFramesFile) {
    loadOk("tempo-map.cypher");
    const Json response = expectResource(server().renderFrame("1", ""));
    EXPECT_EQ(response.at("frame"), 1);
    const std::filesystem::path file = fileUriToPath(response.at("uri").get<std::string>());
    EXPECT_EQ(file, output_.path() / "frames" / "frame-000001.wav");
    EXPECT_TRUE(std::filesystem::is_regular_file(file));
    EXPECT_FALSE(response.contains("set_uris"));
}

TEST_F(ServerTest, NoBodyAnEmptyObjectAndEmptyOrNullLabelsMeanEveryPartial) {
    loadOk("tempo-map.cypher");
    for (const char* body : {"", " \n", "{}", R"({"labels": []})", R"({"labels": null})"}) {
        EXPECT_EQ(server().renderFrame("0", body).status, 200) << body;
    }
}

TEST_F(ServerTest, PerSetWavAddsTheSetFiles) {
    pipeline_.perSetWav = true;
    loadOk("two sets.cypher");
    const Json response = expectResource(server().renderFrame("1", ""));
    ASSERT_FALSE(response.at("set_uris").empty());
    for (const Json& uri : response.at("set_uris")) {
        EXPECT_TRUE(std::filesystem::is_regular_file(fileUriToPath(uri.get<std::string>())));
    }
}

TEST_F(ServerTest, AnUnknownLabelIsUnprocessable) {
    loadOk("tempo-map.cypher");
    expectProblem(server().renderFrame("1", R"({"labels": ["no-such-label"]})"), 422);
    EXPECT_TRUE(echoMentions("FAILED   request body: frame 1 names label 'no-such-label'"));
    // The score is still loaded: a bad request does not cost the server it.
    EXPECT_TRUE(server().hasScore());
}

TEST_F(ServerTest, ABadRenderIsABadRequest) {
    loadOk("tempo-map.cypher");
    for (const auto& [frame, body] : std::vector<std::pair<std::string, std::string>>{{"-1", ""},
             {"1.0", ""}, {"+1", ""}, {"x", ""}, {"2147483648", ""}, {"", ""}, {"1", "not json"},
             {"1", "[1]"}, {"1", R"({"labels": "a"})"}, {"1", R"({"labels": [""]})"},
             {"1", R"({"labels": [1]})"}, {"1", R"({"frame": 1})"}}) {
        expectProblem(server().renderFrame(frame, body), 400);
    }
}

TEST_F(ServerTest, EveryProblemInARenderIsListed) {
    const Json problem =
        expectProblem(server().renderFrame("-1", R"({"labels": [""], "extra": 0})"), 400);
    EXPECT_EQ(problem.at("errors").size(), 3);
}

TEST_F(ServerTest, TheLargestFrameNumberIsAccepted) {
    loadOk("tempo-map.cypher");
    // Far beyond the score, so silent; the point is that it is not refused.
    EXPECT_EQ(server().renderFrame("2147483647", "").status, 200);
}

// ---------------------------------------------------------------------------
// POST /v1/shutdown
// ---------------------------------------------------------------------------

TEST_F(ServerTest, ShutdownIsAcknowledged) {
    EXPECT_EQ(expectResource(server().shutdown()).at("message"), "shutting down");
}

TEST_F(ServerTest, EachCallIsEchoed) {
    loadOk("tempo-map.cypher");
    static_cast<void>(server().renderFrame("0", ""));
    static_cast<void>(server().engineConfig({}));
    EXPECT_TRUE(echoMentions("PUT /v1/score\n"));
    EXPECT_TRUE(echoMentions("POST /v1/frames/0/renderings\n"));
    EXPECT_TRUE(echoMentions("rendered file://"));
    EXPECT_TRUE(echoMentions("GET /v1/engine-config\n"));
}

// ---------------------------------------------------------------------------
// The read calls
// ---------------------------------------------------------------------------

TEST_F(ServerTest, EveryReadNeedsAScore) {
    expectProblem(server().tempoMapRoot({}), 409);
    expectProblem(server().tempoMapChildren("0", {}), 409);
    expectProblem(server().tempoMapPoints("0", {}), 409);
    expectProblem(server().framePosition("0", {{"at", "0"}}), 409);
    expectProblem(server().engineConfig({}), 409);
}

TEST_F(ServerTest, TheRootIsTheScoresTempoMap) {
    loadOk("tempo-map.cypher");
    const Json root = expectResource(server().tempoMapRoot({}));
    EXPECT_TRUE(root.at("id").is_number_integer());
    EXPECT_EQ(root.at("name"), "Main");
    EXPECT_EQ(root.at("ticks_per_beat"), 960);
    EXPECT_EQ(root.at("children"), Json::array());

    const std::string id = std::to_string(root.at("id").get<std::int64_t>());
    const Json children = expectResource(server().tempoMapChildren(id, {}));
    EXPECT_EQ(children.at("tempo_map_id"), root.at("id"));
    EXPECT_EQ(children.at("children"), Json::array());
}

TEST_F(ServerTest, AScoreInPhysicalTimeHasNoRoot) {
    loadOk("two sets.cypher");
    expectProblem(server().tempoMapRoot({}), 404);
}

TEST_F(ServerTest, AnUnknownOrMalformedMapId) {
    loadOk("tempo-map.cypher");
    expectProblem(server().tempoMapChildren("999", {}), 404);
    expectProblem(server().tempoMapPoints("999", {}), 404);
    expectProblem(server().framePosition("999", {{"at", "0"}}), 404);
    for (const char* id : {"x", "-1", "1.0", ""}) {
        expectProblem(server().tempoMapChildren(id, {}), 400);
        expectProblem(server().tempoMapPoints(id, {}), 400);
    }
}

TEST_F(ServerTest, UnknownOrRepeatedQueryParametersAreBadRequests) {
    loadOk("tempo-map.cypher");
    expectProblem(server().tempoMapRoot({{"x", "1"}}), 400);
    expectProblem(server().engineConfig({{"x", "1"}}), 400);
    expectProblem(server().tempoMapChildren("0", {{"x", "1"}}), 400);
    expectProblem(server().tempoMapPoints("0", {{"limit", "1"}, {"limit", "2"}}), 400);
    expectProblem(server().framePosition("0", {{"at", "0"}, {"at", "1"}}), 400);
}

/// The query of a points page's `next` link.
///
/// @param next  The link, e.g. `/v1/tempo-maps/0/points?after=960&limit=20`.
/// @param id    The map the link must name.
/// @return `after` and `limit` as query parameters; empty when the link is
///         not of that form.
QueryParameters nextQuery(const std::string& next, const std::string& id) {
    const std::string prefix = "/v1/tempo-maps/" + id + "/points?after=";
    const std::size_t amp = next.find("&limit=");
    if (!next.starts_with(prefix) || amp == std::string::npos) {
        return {};
    }
    return {{"after", next.substr(prefix.size(), amp - prefix.size())},
        {"limit", next.substr(amp + 7)}};
}

/// Pages through a map's points by following `next`, as a client would.
///
/// @param server  A server with a score loaded.
/// @param id      The map's id.
/// @param limit   The page size asked for.
/// @return Every point's position, in order.
std::vector<std::int64_t> pageThrough(ServerCore& server, const std::string& id, int limit) {
    std::vector<std::int64_t> positions;
    QueryParameters query{{"limit", std::to_string(limit)}};
    for (int pages = 0; pages < 100 && !query.empty(); ++pages) {
        const Json page = expectResource(server.tempoMapPoints(id, query));
        for (const Json& point : page.at("points")) {
            positions.push_back(point.at("position").get<std::int64_t>());
        }
        query = page.at("next").is_null() ? QueryParameters{}
                                          : nextQuery(page.at("next").get<std::string>(), id);
        // `next` is this call with the cursor moved on, the page size kept.
        EXPECT_TRUE(
            page.at("next").is_null() || query.find("limit")->second == std::to_string(limit))
            << page.at("next");
    }
    return positions;
}

TEST_F(ServerTest, PointsArePagedToTheEnd) {
    loadOk("long-map.cypher");
    const std::string id =
        std::to_string(expectResource(server().tempoMapRoot({})).at("id").get<std::int64_t>());

    // The default page holds them all.
    const Json page = expectResource(server().tempoMapPoints(id, {}));
    const Json shape{{"ticks_per_beat", page.at("ticks_per_beat")},
        {"count", page.at("points").size()}, {"next", page.at("next")},
        {"first", page.at("points").front()}};
    EXPECT_EQ(shape, (Json{{"ticks_per_beat", 960}, {"count", 75}, {"next", nullptr},
                         {"first", {{"position", 0}, {"start_tempo", 60.0}, {"hold", true}}}}));

    std::vector<std::int64_t> expected(75);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = 960 * static_cast<std::int64_t>(i);
    }
    EXPECT_EQ(pageThrough(server(), id, 20), expected);
    EXPECT_EQ(pageThrough(server(), id, 1), expected);

    const Json beyond = expectResource(server().tempoMapPoints(id, {{"after", "1000000"}}));
    EXPECT_EQ(beyond.at("points"), Json::array());
    EXPECT_EQ(beyond.at("next"), nullptr);
}

TEST_F(ServerTest, ThePageSizeIsBounded) {
    loadOk("long-map.cypher");
    for (const char* limit : {"0", "1001", "-1", "x", ""}) {
        expectProblem(server().tempoMapPoints("0", {{"limit", limit}}), 400);
    }
    for (const char* after : {"-1", "x", "1.5", ""}) {
        expectProblem(server().tempoMapPoints("0", {{"after", after}}), 400);
    }
    EXPECT_EQ(server().tempoMapPoints("0", {{"limit", "1000"}}).status, 200);
    EXPECT_EQ(server().tempoMapPoints("0", {{"limit", "1"}}).status, 200);
}

TEST_F(ServerTest, EngineConfigIsResolved) {
    loadOk("tempo-map.cypher");
    const Json config = expectResource(server().engineConfig({}));
    EXPECT_EQ(config.at("sample_rate"), 48000);
    // frameSize is NULL in this score, so a frame is one second.
    EXPECT_EQ(config.at("frame_size"), 48000);
    EXPECT_EQ(config.at("channel_mode"), "mono");

    loadOk("long-map.cypher");
    const Json other = expectResource(server().engineConfig({}));
    EXPECT_EQ(other.at("frame_size"), 1000);
    EXPECT_EQ(other.at("channel_mode"), "stereo-from-mid-side");
}

TEST_F(ServerTest, AFramePositionIsAFrameAndAnOffset) {
    // 60 BPM: beat n is n seconds, sample 48000 n, in frames of 1000 samples.
    // Half a beat is exactly 0.5 s.
    loadOk("long-map.cypher");
    Json at = expectResource(server().framePosition("0", {{"at", "3:480"}}));
    EXPECT_EQ(at.at("tempo_map_id"), 0);
    EXPECT_EQ(at.at("at"), "3:480");
    EXPECT_EQ(at.at("frame"), 168);
    EXPECT_EQ(at.at("offset"), 0);

    // A bare number is beats, and comes back written in full.
    at = expectResource(server().framePosition("0", {{"at", "2"}}));
    EXPECT_EQ(at.at("at"), "2:0");
    EXPECT_EQ(at.at("frame"), 96);
}

TEST_F(ServerTest, ABadFramePositionIsABadRequest) {
    loadOk("long-map.cypher");
    expectProblem(server().framePosition("0", {}), 400);
    for (const char* at : {"", "1.5", "-1", "1:x", "x"}) {
        expectProblem(server().framePosition("0", {{"at", at}}), 400);
    }
    // Ticks must be below the map's 960 per beat.
    const Json problem = expectProblem(server().framePosition("0", {{"at", "1:960"}}), 400);
    EXPECT_NE(
        problem.at("detail").get<std::string>().find("960 ticks per beat"), std::string::npos);
}

} // namespace
