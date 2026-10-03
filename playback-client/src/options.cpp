#include "rainbohz/playback_client/options.hpp"

#include <CLI/CLI.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

/// @file
/// @brief Parses the playback-client command line into Options, with CLI11.
///
/// @see options.hpp

namespace rainbohz::playback_client {

namespace {

/// The exit code for a usage error, distinct from a runtime failure (1).
constexpr int kUsageError = 2;

/// The program's description, shown by --help.
constexpr const char* kDescription =
    "Plays a range of the score loaded in rendering-engine's REST service, in a loop,\n"
    "through the default audio device. Every loop is rendered afresh: the network\n"
    "thread fills one lane of the buffer while the other plays.\n"
    "See playback-client/docs/design.md.";

} // namespace

ParseResult parseCommandLine(int argc, const char* const* argv) {
    CLI::App app{kDescription, "playback-client"};
    app.set_version_flag("--version", "playback-client 0.1.0");

    Options options;
    std::string start;
    std::string end;
    std::string bufferRoot;
    std::string outputWav;

    app.add_option("--server", options.serverUrl,
           "The REST API's base URL, /v1 included (default " + std::string{kDefaultServerUrl} + ")")
        ->capture_default_str();
    app.add_option("--start", start, "Start of the range, beats:ticks or beats")->required();
    app.add_option("--end", end, "End of the range, exclusive, beats:ticks or beats")->required();
    app.add_option("--loops", options.loops, "How many times to play the range (default 1)")
        ->check(CLI::Range(std::int64_t{1}, std::int64_t{1'000'000}));
    app.add_option("--buffer-dir", bufferRoot,
           "Directory to make the run's buffer in (default: the system temporary directory)")
        ->check(CLI::ExistingDirectory);
    app.add_flag("--keep-buffer", options.keepBuffer,
        "Keep the run's buffer directory at exit, for debugging");
    app.add_option("--output-wav", outputWav,
        "Write what would be played to this WAV file instead of the audio device");
    app.add_flag("--debug", options.debug, "Log every REST call, frame and wait");

    try {
        app.parse(argc, argv);
    } catch (const CLI::Success& success) {
        return ParseResult{.options = std::nullopt, .exitCode = app.exit(success)};
    } catch (const CLI::ParseError& error) {
        const int code = app.exit(error);
        return ParseResult{.options = std::nullopt, .exitCode = code == 0 ? 0 : kUsageError};
    }

    try {
        options.start = parseMusicalPosition(start);
        options.end = parseMusicalPosition(end);
    } catch (const MusicalPositionError& error) {
        std::cerr << "playback-client: " << error.what() << "\n";
        return ParseResult{.options = std::nullopt, .exitCode = kUsageError};
    }
    if (!(options.start < options.end)) {
        std::cerr << "playback-client: --end " << formatMusicalPosition(options.end)
                  << " must be after --start " << formatMusicalPosition(options.start) << "\n";
        return ParseResult{.options = std::nullopt, .exitCode = kUsageError};
    }

    options.bufferRoot = bufferRoot.empty() ? std::filesystem::temp_directory_path()
                                            : std::filesystem::path{bufferRoot};
    if (!outputWav.empty()) {
        options.outputWav = std::filesystem::path{outputWav};
    }
    return ParseResult{.options = options, .exitCode = 0};
}

} // namespace rainbohz::playback_client
