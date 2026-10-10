// SPDX-License-Identifier: Apache-2.0

// Tests for the batch-mode render request.
//
// The properties that matter: a well-formed request yields exactly the frames
// and label sets it says; "no labels" in either spelling means every partial;
// and every malformed request is refused with the line that is wrong, all
// problems at once, rather than being half-read or quietly reinterpreted.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::findUnknownLabels;
using rainbohz::rendering_engine::parseRenderRequest;
using rainbohz::rendering_engine::RenderRequest;
using rainbohz::rendering_engine::RenderRequestError;
using rainbohz::rendering_engine::RequestProblem;

constexpr std::string_view kFile = "request.yaml";

RenderRequest parse(std::string_view yaml) {
    return parseRenderRequest(yaml, kFile);
}

/// The problems a request is refused with. Fails the test if it is accepted.
std::vector<RequestProblem> problemsWith(std::string_view yaml) {
    try {
        parse(yaml);
    } catch (const RenderRequestError& e) {
        EXPECT_EQ(e.file(), kFile);
        return e.problems();
    }
    ADD_FAILURE() << "request was accepted:\n" << yaml;
    return {};
}

/// Refused with exactly one problem, at `line`, whose message contains `text`.
void expectOneProblem(std::string_view yaml, std::size_t line, std::string_view text) {
    const auto problems = problemsWith(yaml);
    ASSERT_EQ(problems.size(), 1U) << yaml;
    EXPECT_EQ(problems[0].line, line) << problems[0].message;
    EXPECT_NE(problems[0].message.find(text), std::string::npos) << problems[0].message;
}

// The example from the proposal, verbatim in substance.
TEST(RenderRequest, ParsesFramesAndLabels) {
    const RenderRequest request = parse("frames:\n"                      // 1
                                        "  - frame: 1\n"                 // 2
                                        "    labels: [strings, brass]\n" // 3
                                        "  - frame: 2\n"                 // 4
                                        "    labels:\n"                  // 5
                                        "      - strings\n"              // 6
                                        "  - frame: 3\n"                 // 7
                                        "  - frame: 4\n"                 // 8
                                        "    labels: []\n");             // 9

    EXPECT_EQ(request.file, kFile);
    ASSERT_EQ(request.frames.size(), 4U);

    EXPECT_EQ(request.frames[0].frameNumber, 1);
    EXPECT_EQ(request.frames[0].labels, (std::set<std::string>{"brass", "strings"}));
    EXPECT_EQ(request.frames[0].line, 2U);

    EXPECT_EQ(request.frames[1].frameNumber, 2);
    EXPECT_EQ(request.frames[1].labels, (std::set<std::string>{"strings"}));

    EXPECT_EQ(request.frames[2].frameNumber, 3);
    EXPECT_TRUE(request.frames[2].allPartials());
    EXPECT_EQ(request.frames[2].line, 7U);

    EXPECT_EQ(request.frames[3].frameNumber, 4);
    EXPECT_TRUE(request.frames[3].allPartials());
}

// Omitted, empty and null are three spellings of one request.
TEST(RenderRequest, NoLabelsInAnySpellingMeansAllPartials) {
    for (const std::string_view yaml : {"frames: [{frame: 1}]", "frames: [{frame: 1, labels: []}]",
             "frames:\n  - frame: 1\n    labels:\n"}) {
        const RenderRequest request = parse(yaml);
        ASSERT_EQ(request.frames.size(), 1U) << yaml;
        EXPECT_TRUE(request.frames[0].allPartials()) << yaml;
    }
}

TEST(RenderRequest, LabelsAreASet) {
    const RenderRequest request = parse("frames: [{frame: 1, labels: [b, a, b]}]");
    EXPECT_EQ(request.frames[0].labels, (std::set<std::string>{"a", "b"}));
}

TEST(RenderRequest, KeepsFileOrder) {
    const RenderRequest request = parse("frames: [{frame: 9}, {frame: 2}, {frame: 5}]");
    ASSERT_EQ(request.frames.size(), 3U);
    EXPECT_EQ(request.frames[0].frameNumber, 9);
    EXPECT_EQ(request.frames[1].frameNumber, 2);
    EXPECT_EQ(request.frames[2].frameNumber, 5);
}

// Frames are indexed from 0: frame 0 is the first second of the score.
TEST(RenderRequest, AcceptsFrameZero) {
    EXPECT_EQ(parse("frames: [{frame: 0}]").frames[0].frameNumber, 0);
}

TEST(RenderRequest, AcceptsTheLargestInt32Frame) {
    EXPECT_EQ(parse("frames: [{frame: 2147483647}]").frames[0].frameNumber, 2147483647);
}

TEST(RenderRequest, RefusesFrameNumbersThatAreNotNonNegativeDecimalIntegers) {
    // Each of these is a frame number yaml-cpp would convert to *something*;
    // none is what the request is allowed to say.
    for (const std::string_view value :
        {"-1", "-0", "1.5", "0x10", "1e3", "+3", "one", "\"3\"", "[1]", "{}", "''"}) {
        const std::string yaml = "frames:\n  - frame: " + std::string{value} + "\n";
        expectOneProblem(yaml, 2, "frame must be an integer, 0 or greater");
    }
}

TEST(RenderRequest, RefusesAFrameNumberBeyondInt32) {
    expectOneProblem("frames:\n  - frame: 2147483648\n", 2, "too large");
}

TEST(RenderRequest, RefusesAMissingFrameNumber) {
    expectOneProblem("frames:\n  - frame:\n", 2, "frame must be an integer");
    expectOneProblem("frames:\n  - labels: [a]\n", 2, "no 'frame' key");
}

// One frame is one output file; two entries for it would collide.
TEST(RenderRequest, RefusesARepeatedFrame) {
    expectOneProblem("frames:\n"                // 1
                     "  - frame: 5\n"           // 2
                     "  - frame: 6\n"           // 3
                     "  - frame: 5\n"           // 4
                     "    labels: [strings]\n", // 5
        4, "frame 5 is already requested at line 2");
}

TEST(RenderRequest, RefusesUnknownKeys) {
    expectOneProblem("frames:\n  - frame: 1\n    lables: [a]\n", 3, "unknown key 'lables'");
    expectOneProblem(
        "frames: [{frame: 1}]\nsource_version: 3\n", 2, "unknown key 'source_version'");
}

// yaml-cpp accepts a repeated key and a lookup finds only the first, which
// would make one of the two silently disappear.
TEST(RenderRequest, RefusesARepeatedKey) {
    expectOneProblem("frames:\n  - frame: 1\n    frame: 2\n", 3, "'frame' appears twice");
}

TEST(RenderRequest, RefusesLabelsThatAreNotAListOfNames) {
    expectOneProblem("frames:\n  - frame: 1\n    labels: strings\n", 3, "write [strings]");
    expectOneProblem("frames:\n  - frame: 1\n    labels: {a: b}\n", 3, "labels must be a list");
    expectOneProblem("frames:\n  - frame: 1\n    labels: ['']\n", 3, "non-empty name");
    expectOneProblem("frames:\n  - frame: 1\n    labels: [[a]]\n", 3, "non-empty name");
}

TEST(RenderRequest, RefusesADocumentThatIsNotARequest) {
    expectOneProblem("", 0, "empty");
    expectOneProblem("# only a comment\n", 0, "empty");
    expectOneProblem("- frame: 1\n", 1, "must be a mapping with a 'frames' key");
    expectOneProblem("frames:\n", 1, "frames must be a list, not nothing");
    expectOneProblem("frames: 1\n", 1, "frames must be a list");
    expectOneProblem("frames: []\n", 1, "renders nothing");
    expectOneProblem("frames: [3]\n", 1, "must be a mapping with a 'frame' key");
}

// Both things wrong with it are reported, not only the first.
TEST(RenderRequest, RefusesAMisnamedFramesKey) {
    const auto problems = problemsWith("frame:\n  - frame: 1\n");
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_NE(problems[0].message.find("unknown key 'frame'"), std::string::npos);
    EXPECT_NE(problems[1].message.find("no 'frames' key"), std::string::npos);
}

// yaml-cpp's Load reads the first document and drops the rest.
TEST(RenderRequest, RefusesASecondDocument) {
    expectOneProblem("frames: [{frame: 1}]\n---\nframes: [{frame: 2}]\n", 3, "one YAML document");
}

TEST(RenderRequest, ReportsSyntaxErrorsWithTheirLine) {
    const auto problems = problemsWith("frames:\n  - frame: 1\n  - frame: [2\n");
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_GE(problems[0].line, 3U);
}

// The point of collecting: one run shows everything that needs fixing.
TEST(RenderRequest, ReportsEveryProblemAtOnce) {
    const auto problems = problemsWith("frames:\n"             // 1
                                       "  - frame: -1\n"       // 2
                                       "  - frame: 2\n"        // 3
                                       "    labels: strings\n" // 4
                                       "  - frame: 3\n"        // 5
                                       "    colour: red\n");   // 6
    ASSERT_EQ(problems.size(), 3U);
    EXPECT_EQ(problems[0].line, 2U);
    EXPECT_EQ(problems[1].line, 4U);
    EXPECT_EQ(problems[2].line, 6U);
}

TEST(RenderRequest, ErrorTextNamesTheFileAndEveryLine) {
    try {
        parse("frames:\n  - frame: one\n  - frame: -2\n");
        FAIL() << "request was accepted";
    } catch (const RenderRequestError& e) {
        const std::string text = e.what();
        EXPECT_NE(text.find("request.yaml, line 2:"), std::string::npos) << text;
        EXPECT_NE(text.find("request.yaml, line 3:"), std::string::npos) << text;
    }
}

TEST(RenderRequest, MissingFileIsRefused) {
    EXPECT_THROW(rainbohz::rendering_engine::loadRenderRequest(
                     std::filesystem::temp_directory_path() / "rainbohz-no-such-request.yaml"),
        RenderRequestError);
}

TEST(RenderRequest, FindsLabelsTheDatabaseDoesNotHold) {
    const RenderRequest request = parse("frames:\n"                         // 1
                                        "  - frame: 1\n"                    // 2
                                        "    labels: [strings, brasss]\n"   // 3
                                        "  - frame: 2\n"                    // 4
                                        "  - frame: 3\n"                    // 5
                                        "    labels: [brasss, strings]\n"); // 6

    const auto problems = findUnknownLabels(request, {"brass", "strings"});
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_EQ(problems[0].line, 2U);
    EXPECT_NE(problems[0].message.find("'brasss'"), std::string::npos);
    EXPECT_EQ(problems[1].line, 5U);

    EXPECT_TRUE(findUnknownLabels(request, {"brass", "brasss", "strings"}).empty());
}

// The names a request is checked against come from engine-db itself.
TEST(RenderRequest, LabelNamesComeFromEngineDb) {
    EngineDb db;
    db.execute("CREATE NODE TABLE Label(id INT64, name STRING, PRIMARY KEY(id));");
    EXPECT_TRUE(db.labelNames().empty());

    db.execute("CREATE (:Label {id: 1, name: 'strings'});");
    db.execute("CREATE (:Label {id: 2, name: 'brass'});");
    db.execute("CREATE (:Label {id: 3, name: 'brass'});");
    db.execute("CREATE (:Label {id: 4});");
    EXPECT_EQ(db.labelNames(), (std::set<std::string>{"brass", "strings"}));
}

} // namespace
