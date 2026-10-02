// Tests for server mode's handlers, driven directly through ServerCore.
//
// No socket is opened: ServerCore takes a request body and returns a status
// and a JSON body, which is everything the server decides. That HTTP carries
// those faithfully is checked end to end by test/server/run_server_check.py.
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

/// A server over a private input root and output directory.
class ServerTest : public ::testing::Test {
protected:
    ServerTest() {
        std::filesystem::copy_file(RE_TEST_TEMPO_MAP_STATE, inputRoot_.path() / "tempo-map.cypher");
        std::filesystem::copy_file(RE_TEST_TWO_SETS_STATE, inputRoot_.path() / "two sets.cypher");
        std::ofstream{inputRoot_.path() / "broken.cypher"} << "CREATE (n:NoSuchTable {x: 1});\n";
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
    /// @return A /load body naming it.
    std::string loadBody(const std::string& name) const {
        return Json{{"uri", pathToFileUri(inputRoot_.path() / name)}}.dump();
    }

    /// Loads a file from the input root and expects it to succeed.
    ///
    /// @param name  A file in the input root.
    /// @return The parsed response.
    Json loadOk(const std::string& name) {
        const HttpReply reply = server().load(loadBody(name));
        EXPECT_EQ(reply.status, 200) << reply.body;
        return Json::parse(reply.body);
    }

    TempDirectory inputRoot_{"rainbohz-server-input-"};
    TempDirectory output_{"rainbohz-server-output-"};
    PipelineSettings pipeline_;
    std::ostringstream echo_;
    std::unique_ptr<ServerCore> server_;
};

/// Whether any line of a response's log contains `text`.
bool logMentions(const Json& response, const std::string& text) {
    return std::ranges::any_of(response.at("log"),
        [&](const Json& line) { return line.get<std::string>().find(text) != std::string::npos; });
}

TEST_F(ServerTest, ItStartsWithNoScore) {
    EXPECT_FALSE(server().hasScore());
}

TEST_F(ServerTest, RenderingBeforeALoadIsAConflict) {
    const HttpReply reply = server().render(R"({"frame": 0})");
    EXPECT_EQ(reply.status, 409);
    EXPECT_EQ(Json::parse(reply.body).at("status"), "error");
}

TEST_F(ServerTest, ALoadReportsWhatLoaded) {
    const Json response = loadOk("tempo-map.cypher");
    EXPECT_TRUE(server().hasScore());
    EXPECT_EQ(response.at("status"), "ok");
    EXPECT_EQ(response.at("uri"), pathToFileUri(inputRoot_.path() / "tempo-map.cypher"));
    EXPECT_EQ(response.at("partials"), 2);
    EXPECT_GT(response.at("nodes").get<int>(), 0);
    EXPECT_GT(response.at("relationships").get<int>(), 0);
    // The narrative is batch mode's: the summary of each file, and the census.
    EXPECT_TRUE(logMentions(response, "schema: applied"));
    EXPECT_TRUE(logMentions(response, "precomputed 2 of 2 partial(s)"));
}

TEST_F(ServerTest, ARenderReturnsTheFramesFile) {
    loadOk("tempo-map.cypher");
    const HttpReply reply = server().render(R"({"frame": 1})");
    ASSERT_EQ(reply.status, 200) << reply.body;
    const Json response = Json::parse(reply.body);
    EXPECT_EQ(response.at("frame"), 1);
    const std::filesystem::path file = fileUriToPath(response.at("uri").get<std::string>());
    EXPECT_EQ(file, output_.path() / "frames" / "frame-000001.wav");
    EXPECT_TRUE(std::filesystem::is_regular_file(file));
    EXPECT_FALSE(response.contains("set_uris"));
}

TEST_F(ServerTest, EmptyAndNullLabelsMeanEveryPartial) {
    loadOk("tempo-map.cypher");
    EXPECT_EQ(server().render(R"({"frame": 0, "labels": []})").status, 200);
    EXPECT_EQ(server().render(R"({"frame": 0, "labels": null})").status, 200);
}

TEST_F(ServerTest, PerSetWavAddsTheSetFiles) {
    pipeline_.perSetWav = true;
    loadOk("two sets.cypher");
    const HttpReply reply = server().render(R"({"frame": 1})");
    ASSERT_EQ(reply.status, 200) << reply.body;
    const Json response = Json::parse(reply.body);
    ASSERT_FALSE(response.at("set_uris").empty());
    for (const Json& uri : response.at("set_uris")) {
        EXPECT_TRUE(std::filesystem::is_regular_file(fileUriToPath(uri.get<std::string>())));
    }
}

TEST_F(ServerTest, AnUnknownLabelIsUnprocessable) {
    loadOk("tempo-map.cypher");
    const HttpReply reply = server().render(R"({"frame": 1, "labels": ["no-such-label"]})");
    EXPECT_EQ(reply.status, 422);
    const Json response = Json::parse(reply.body);
    EXPECT_TRUE(
        logMentions(response, "FAILED   request body: frame 1 names label 'no-such-label'"));
    // The score is still loaded: a bad request does not cost the server it.
    EXPECT_TRUE(server().hasScore());
}

TEST_F(ServerTest, ABadRenderBodyIsABadRequest) {
    loadOk("tempo-map.cypher");
    for (const char* body : {"", "not json", "[1]", "{}", R"({"frame": -1})", R"({"frame": 1.0})",
             R"({"frame": "1"})", R"({"frame": 2147483648})", R"({"frame": 1, "labels": "a"})",
             R"({"frame": 1, "labels": [""]})", R"({"frame": 1, "labels": [1]})",
             R"({"frame": 1, "extra": true})"}) {
        const HttpReply reply = server().render(body);
        EXPECT_EQ(reply.status, 400) << body;
        EXPECT_EQ(Json::parse(reply.body).at("status"), "error") << body;
    }
}

TEST_F(ServerTest, EveryProblemInABodyIsReported) {
    const HttpReply reply = server().render(R"({"frame": -1, "labels": [""], "extra": 0})");
    ASSERT_EQ(reply.status, 400);
    EXPECT_EQ(Json::parse(reply.body).at("log").size(), 3);
}

TEST_F(ServerTest, TheLargestFrameNumberIsAccepted) {
    loadOk("tempo-map.cypher");
    // Far beyond the score, so silent; the point is that it is not refused.
    EXPECT_EQ(server().render(R"({"frame": 2147483647})").status, 200);
}

TEST_F(ServerTest, ABadLoadBodyIsABadRequest) {
    for (const char* body : {"", "{}", R"({"uri": 1})", R"({"uri": "x", "extra": 1})",
             R"({"uri": "http://host/a.cypher"})", R"({"uri": "file://host/a.cypher"})",
             R"({"uri": "file:///a%zz"})"}) {
        EXPECT_EQ(server().load(body).status, 400) << body;
    }
}

TEST_F(ServerTest, AFileOutsideTheInputRootIsForbidden) {
    const HttpReply reply = server().load(Json{{"uri", pathToFileUri(RE_TEST_SCHEMA_FILE)}}.dump());
    EXPECT_EQ(reply.status, 403);
}

TEST_F(ServerTest, DotDotCannotLeaveTheInputRoot) {
    const std::filesystem::path escape =
        inputRoot_.path() / ".." / inputRoot_.path().filename() / ".." / "elsewhere.cypher";
    EXPECT_EQ(server().load(Json{{"uri", pathToFileUri(escape)}}.dump()).status, 403);

    // A `..` that stays inside is fine.
    const std::filesystem::path inside = inputRoot_.path() / "sub" / ".." / "tempo-map.cypher";
    EXPECT_EQ(server().load(Json{{"uri", pathToFileUri(inside)}}.dump()).status, 200);
}

TEST_F(ServerTest, ASymbolicLinkCannotLeaveTheInputRoot) {
    std::filesystem::create_symlink(RE_TEST_SCHEMA_FILE, inputRoot_.path() / "link.cypher");
    EXPECT_EQ(server().load(loadBody("link.cypher")).status, 403);
}

TEST_F(ServerTest, ANameThatNeedsEncodingLoads) {
    // "two sets.cypher" has a space, so its URI has %20.
    EXPECT_NE(loadBody("two sets.cypher").find("%20"), std::string::npos);
    EXPECT_EQ(loadOk("two sets.cypher").at("partials"), 5);
}

TEST_F(ServerTest, AMissingFileIsNotFound) {
    EXPECT_EQ(server().load(loadBody("no-such-score.cypher")).status, 404);
    std::filesystem::create_directory(inputRoot_.path() / "a-directory");
    EXPECT_EQ(server().load(loadBody("a-directory")).status, 404);
}

TEST_F(ServerTest, AFailedLoadLeavesNoScore) {
    loadOk("tempo-map.cypher");
    const HttpReply reply = server().load(loadBody("broken.cypher"));
    EXPECT_EQ(reply.status, 422);
    const Json response = Json::parse(reply.body);
    EXPECT_EQ(
        response.at("message"), "1 statement(s) did not apply. engine-db is partially loaded.");
    EXPECT_TRUE(logMentions(response, "FAILED   broken.cypher, line 1"));

    EXPECT_FALSE(server().hasScore());
    EXPECT_EQ(server().render(R"({"frame": 0})").status, 409);
}

TEST_F(ServerTest, ARefusedLoadKeepsTheScore) {
    // Only a load that gets as far as loading replaces the score; one refused
    // for its request does not.
    loadOk("tempo-map.cypher");
    EXPECT_EQ(server().load(loadBody("no-such-score.cypher")).status, 404);
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

TEST_F(ServerTest, ShutdownIsAcknowledged) {
    const HttpReply reply = server().shutdown();
    EXPECT_EQ(reply.status, 200);
    EXPECT_EQ(Json::parse(reply.body).at("status"), "ok");
}

TEST_F(ServerTest, EachCallIsEchoed) {
    loadOk("tempo-map.cypher");
    static_cast<void>(server().render(R"({"frame": 0})"));
    const std::string echoed = echo_.str();
    EXPECT_NE(echoed.find("POST /load\n"), std::string::npos);
    EXPECT_NE(echoed.find("POST /render\n"), std::string::npos);
    EXPECT_NE(echoed.find("rendered file://"), std::string::npos);
}

} // namespace
