// SPDX-License-Identifier: Apache-2.0

// Tests for the command line: the modes, and what each requires and refuses.
//
// parseCommandLine() writes its diagnostics to stderr; these tests check only
// whether a command line yields options, and which.

#include "rainbohz/rendering_engine/options.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Injected by CMake: an existing file to pass where one is required.
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::Backend;
using rainbohz::rendering_engine::Mode;
using rainbohz::rendering_engine::modeName;
using rainbohz::rendering_engine::parseCommandLine;
using rainbohz::rendering_engine::ParseResult;
using rainbohz::rendering_engine::RenderKernel;

constexpr const char* kFile = RE_TEST_EXAMPLE_STATE;

/// Parses a command line given without the program name.
///
/// @param arguments  The arguments after `argv[0]`.
/// @return What parseCommandLine() made of them.
ParseResult parse(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "rendering-engine");
    std::vector<const char*> argv;
    argv.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    return parseCommandLine(static_cast<int>(argv.size()), argv.data());
}

/// A directory that exists, for --input-root and --out-dir.
std::string existingDirectory() {
    return std::filesystem::temp_directory_path().string();
}

/// A complete server command line.
std::vector<std::string> serverLine() {
    return {"--mode", "server", "--host", "127.0.0.1", "--port", "8080", "--input-root",
        existingDirectory(), "--out-dir", existingDirectory()};
}

TEST(Options, TheModesAreBatchServerAndDistributed) {
    EXPECT_EQ(modeName(Mode::Batch), "batch");
    EXPECT_EQ(modeName(Mode::Server), "server");
    EXPECT_EQ(modeName(Mode::Distributed), "distributed");
}

TEST(Options, BatchIsTheDefaultMode) {
    const ParseResult result = parse({"--state", kFile, "--request", kFile});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->mode, Mode::Batch);
}

TEST(Options, ModeNamesMatchIgnoringCase) {
    const ParseResult result = parse({"--mode", "BATCH", "--state", kFile, "--request", kFile});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->mode, Mode::Batch);
}

TEST(Options, TheOldModeNamesAreGone) {
    EXPECT_FALSE(
        parse({"--mode", "test", "--state", kFile, "--request", kFile}).options.has_value());
    EXPECT_FALSE(parse({"--mode", "standalone"}).options.has_value());
}

TEST(Options, BatchNeedsStateAndRequest) {
    EXPECT_FALSE(parse({"--mode", "batch", "--state", kFile}).options.has_value());
    EXPECT_FALSE(parse({"--mode", "batch", "--request", kFile}).options.has_value());
}

TEST(Options, BatchRefusesTheServerOptions) {
    const std::vector<std::vector<std::string>> extras{
        {"--host", "127.0.0.1"}, {"--port", "8080"}, {"--input-root", existingDirectory()}};
    for (const auto& extra : extras) {
        std::vector<std::string> line{"--mode", "batch", "--state", kFile, "--request", kFile};
        line.insert(line.end(), extra.begin(), extra.end());
        EXPECT_FALSE(parse(line).options.has_value()) << extra.front();
    }
}

TEST(Options, AServerLineIsParsed) {
    const ParseResult result = parse(serverLine());
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->mode, Mode::Server);
    EXPECT_EQ(result.options->host, "127.0.0.1");
    EXPECT_EQ(result.options->port, 8080);
    EXPECT_EQ(result.options->inputRoot, std::filesystem::path{existingDirectory()});
    EXPECT_EQ(result.options->outputDir, std::filesystem::path{existingDirectory()});
}

TEST(Options, PortZeroIsAccepted) {
    std::vector<std::string> line = serverLine();
    line.at(5) = "0";
    const ParseResult result = parse(line);
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->port, 0);
}

TEST(Options, TheServerNeedsAllFourOfItsOptions) {
    // Each of --host, --port, --input-root and --out-dir in turn is left out,
    // with its value. --out-dir has a default in batch mode, which must not
    // count as having been given.
    for (std::size_t option = 2; option < 10; option += 2) {
        std::vector<std::string> line = serverLine();
        const std::string name = line.at(option);
        line.erase(line.begin() + static_cast<std::ptrdiff_t>(option),
            line.begin() + static_cast<std::ptrdiff_t>(option) + 2);
        EXPECT_FALSE(parse(line).options.has_value()) << "without " << name;
    }
}

TEST(Options, TheServerRefusesTheBatchOptions) {
    const std::vector<std::vector<std::string>> extras{
        {"--state", kFile}, {"--request", kFile}, {"--db-path", "/tmp/engine.lbug"}};
    for (const auto& extra : extras) {
        std::vector<std::string> line = serverLine();
        line.insert(line.end(), extra.begin(), extra.end());
        EXPECT_FALSE(parse(line).options.has_value()) << extra.front();
    }
}

TEST(Options, ThePortMustBeInRange) {
    for (const std::string port : {"65536", "-1", "http"}) {
        std::vector<std::string> line = serverLine();
        line.at(5) = port;
        EXPECT_FALSE(parse(line).options.has_value()) << port;
    }
}

TEST(Options, TheInputRootMustBeAnExistingDirectory) {
    std::vector<std::string> line = serverLine();
    line.at(7) = (std::filesystem::temp_directory_path() / "rainbohz-no-such-directory").string();
    EXPECT_FALSE(parse(line).options.has_value());
    line.at(7) = kFile;
    EXPECT_FALSE(parse(line).options.has_value());
}

TEST(Options, RenderThreadsDefaultToAutomatic) {
    const ParseResult result = parse({"--state", kFile, "--request", kFile});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->renderThreads, 0U);
}

TEST(Options, RenderThreadsAreTakenInBothModes) {
    const ParseResult batch =
        parse({"--state", kFile, "--request", kFile, "--render-threads", "4"});
    ASSERT_TRUE(batch.options.has_value());
    EXPECT_EQ(batch.options->renderThreads, 4U);

    std::vector<std::string> line = serverLine();
    line.insert(line.end(), {"--render-threads", "1"});
    const ParseResult server = parse(line);
    ASSERT_TRUE(server.options.has_value());
    EXPECT_EQ(server.options->renderThreads, 1U);
}

TEST(Options, RenderThreadsMustBeInRange) {
    for (const std::string threads : {"257", "-1", "many"}) {
        EXPECT_FALSE(parse({"--state", kFile, "--request", kFile, "--render-threads", threads})
                .options.has_value())
            << threads;
    }
    EXPECT_TRUE(parse({"--state", kFile, "--request", kFile, "--render-threads", "256"})
            .options.has_value());
}

TEST(Options, RenderKernelDefaultsToSimd) {
    const ParseResult result = parse({"--state", kFile, "--request", kFile});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->renderKernel, RenderKernel::Simd);
}

TEST(Options, RenderKernelTakesTheScalarReferenceInBothModes) {
    const ParseResult batch =
        parse({"--state", kFile, "--request", kFile, "--render-kernel", "scalar"});
    ASSERT_TRUE(batch.options.has_value());
    EXPECT_EQ(batch.options->renderKernel, RenderKernel::Scalar);

    std::vector<std::string> line = serverLine();
    line.insert(line.end(), {"--render-kernel", "SIMD"});
    const ParseResult server = parse(line);
    ASSERT_TRUE(server.options.has_value());
    EXPECT_EQ(server.options->renderKernel, RenderKernel::Simd);
}

TEST(Options, RenderKernelMustBeNamed) {
    for (const std::string kernel : {"vector", "0", "1", ""}) {
        EXPECT_FALSE(parse({"--state", kFile, "--request", kFile, "--render-kernel", kernel})
                .options.has_value())
            << kernel;
    }
}

/// An existing file named like a table manifest, for an explicit native
/// backend, which refuses a Cypher score.
std::string manifestFile() {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "rainbohz-options-test-manifest.json";
    std::ofstream{file} << "{}";
    return file.string();
}

TEST(Options, TheBackendIsLeftToTheScoreByDefault) {
    const ParseResult result = parse({"--state", kFile, "--request", kFile});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_FALSE(result.options->backend.has_value());
}

TEST(Options, TheBackendIsNamedIgnoringCaseInBothModes) {
    const ParseResult batch =
        parse({"--state", manifestFile(), "--request", kFile, "--backend", "Native"});
    ASSERT_TRUE(batch.options.has_value());
    EXPECT_EQ(batch.options->backend, Backend::Native);

    std::vector<std::string> line = serverLine();
    line.insert(line.end(), {"--backend", "ladybug"});
    const ParseResult server = parse(line);
    ASSERT_TRUE(server.options.has_value());
    EXPECT_EQ(server.options->backend, Backend::Ladybug);

    EXPECT_FALSE(
        parse({"--state", kFile, "--request", kFile, "--backend", "kuzu"}).options.has_value());
}

TEST(Options, AnExplicitNativeBackendRefusesWhatOnlyLadybugDoes) {
    // A Cypher score, the integrity checks, and a database on disk.
    EXPECT_FALSE(
        parse({"--state", kFile, "--request", kFile, "--backend", "native"}).options.has_value());
    EXPECT_FALSE(parse(
        {"--state", manifestFile(), "--request", kFile, "--backend", "native", "--validate-db"})
            .options.has_value());
    EXPECT_FALSE(parse({"--state", manifestFile(), "--request", kFile, "--backend", "native",
                           "--db-path", existingDirectory() + "/rainbohz-options-db"})
            .options.has_value());
    std::vector<std::string> line = serverLine();
    line.insert(line.end(), {"--backend", "native", "--validate-db"});
    EXPECT_FALSE(parse(line).options.has_value());

    // Left to the score, each of these simply chooses Ladybug.
    EXPECT_TRUE(parse({"--state", kFile, "--request", kFile, "--validate-db"}).options.has_value());
}

} // namespace
