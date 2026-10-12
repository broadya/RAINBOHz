// SPDX-License-Identifier: Apache-2.0

#pragma once

// The playback-client command line.
//
//   playback-client --start B:T --end B:T
//                   [--server URL] [--loops N]
//                   [--buffer-dir DIR] [--keep-buffer]
//                   [--output-wav FILE] [--channel-mode MODE] [--debug]
//
// The parser is CLI11, confined to the implementation: no third-party type
// appears in this header.

#include "rainbohz/playback_client/musical_position.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rainbohz::playback_client {

/// The server URL used when --server is not given: the default of
/// `make run-rendering-engine-server`, with the API's `/v1`.
inline constexpr const char* kDefaultServerUrl = "http://127.0.0.1:8080/v1";

/// A validated command line.
struct Options {
    /// The REST API's base URL, `/v1` included.
    std::string serverUrl = kDefaultServerUrl;

    /// Where the range starts, on the score's root tempo map.
    MusicalPosition start;

    /// Where the range ends, exclusive; after `start`.
    MusicalPosition end;

    /// How many times to play the range; at least 1. Every loop is rendered
    /// afresh.
    std::int64_t loops = 1;

    /// The directory the run's buffer directory is made in. Defaults to the
    /// system's temporary directory.
    std::filesystem::path bufferRoot;

    /// Keep the run's buffer directory at exit, for debugging. By default it
    /// is removed.
    bool keepBuffer = false;

    /// Write what would be played to this WAV file instead of the audio
    /// device: unpaced, without gaps. For tests.
    std::optional<std::filesystem::path> outputWav;

    /// The channel mode the score must have, by the server's name; one of
    /// kPlayableChannelModes. The score decides the mode; this only checks
    /// it. Empty to play whichever playable mode the score has.
    std::optional<std::string> channelMode;

    /// Write debug lines to the log: every REST call and its time, every
    /// frame, every wait.
    bool debug = false;
};

/// The outcome of parsing a command line.
///
/// `options` is empty when the program should exit at once with `exitCode`:
/// 0 after --help or --version, 2 after a usage error. The output has already
/// been written.
struct ParseResult {
    /// The validated options; empty when the program should exit at once.
    std::optional<Options> options;
    /// The exit status when `options` is empty: 0 after --help or --version, 2 after
    /// a usage error.
    int exitCode = 0;
};

/// Parses and validates the command line.
///
/// CLI11 parses and checks each option on its own (`--loops` at least 1,
/// `--buffer-dir` an existing directory). This function then checks what
/// CLI11 is not asked to: that `--start` and `--end` are positions,
/// `beats:ticks` or bare beats, and that the end is after the start. Whether
/// their ticks fit the tempo map's resolution is known only from the server,
/// and is checked by planSession().
///
/// @param argc  Argument count, as passed to `main`.
/// @param argv  Argument vector, as passed to `main`. Not kept.
/// @return The validated options, or the exit code to stop with.
/// @note Side effect: when `options` is empty, help or version text has been
///       written to stdout, or a diagnostic to stderr.
ParseResult parseCommandLine(int argc, const char* const* argv);

} // namespace rainbohz::playback_client
