#pragma once

// The rendering-engine command line.
//
// Three run modes are anticipated by the architecture; only `test` is
// implemented. The other two are recognised here so that the mode axis exists
// in the code from the start rather than being retrofitted around a binary
// that only ever did one thing.
//
//   test         Batch. Empty database, schema applied from one Cypher file,
//                initial state loaded from a second Cypher file, every
//                partial precomputed, the frames named by a YAML render
//                request written to a directory as WAV files, terminate.
//                This is the harness docs/design.md section 3 describes, and
//                until the orchestration subsystem is more than a sketch it is
//                the only path that can actually be run.
//   distributed  Long-running server, request-driven, writing to
//                object-storage. Not implemented.
//   standalone   As distributed, but writing to a directory. Not implemented.
//
// The parser is CLI11, and it is deliberately confined to the implementation:
// no third-party type appears in this header, the same rule Ladybug is held
// to.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rainbohz::rendering_engine {

enum class Mode : std::uint8_t { Test, Distributed, Standalone };

/// Returns the name used for a mode on the command line and in diagnostics.
///
/// This is the reverse of the name lookup parseCommandLine() does for
/// `--mode`, so a name printed in a diagnostic can be typed back in.
///
/// @param mode  Mode to name.
/// @return The lower-case command-line name (`test`, `distributed` or
///         `standalone`). A value outside the enumerators, which only a cast
///         can produce, gives `unknown`. The view refers to a string literal,
///         so it stays valid for the whole program.
std::string_view modeName(Mode mode) noexcept;

struct Options {
    Mode mode = Mode::Test;

    /// Cypher file of schema DDL, applied to the empty database before
    /// anything else. Defaults to the in-repo
    /// `engine-db/schema/engine-schema.cypher`, baked in at configure time --
    /// a development affordance. A deployed binary passes --schema.
    std::filesystem::path schemaFile;

    /// Cypher file holding the initial database state, applied on top of the
    /// schema. Required in test mode; there is no default, because a run must
    /// not silently load a score nobody asked for.
    std::filesystem::path stateFile;

    /// YAML render request: which frames to render, and which labels select
    /// the partials for each. Required in test mode. See render_request.hpp.
    std::filesystem::path requestFile;

    /// Where rendered WAV files are written, one per frame. Defaults to the
    /// current directory, and is created if absent.
    std::filesystem::path outputDir = ".";

    /// Where engine-db lives. Empty means in memory, which is the deployed
    /// configuration; a path is a debugging affordance, so that a run can be
    /// opened afterwards in the `lbug` shell.
    std::string dbPath;

    /// Report every statement, not just the failures.
    bool verbose = false;

    /// Run the integrity checks of integrity.hpp once the state has loaded,
    /// and stop before precomputation if any fails. Off by default: the checks
    /// read every table they cover, and the upstream components are expected
    /// to build a sound database.
    bool validateDb = false;

    /// Also write each partial set's contribution to a frame as its own WAV
    /// file, before summing (docs/rendering.md section 5). A debugging
    /// affordance, off by default.
    bool perSetWav = false;
};

/// The outcome of parsing a command line.
///
/// `options` is empty when the program should exit immediately with
/// `exitCode` -- --help and --version (0), or a usage error (non-zero). CLI11
/// has already written the corresponding output.
struct ParseResult {
    std::optional<Options> options;
    int exitCode = 0;
};

/// Parses and validates the rendering-engine command line.
///
/// CLI11 does the parsing and the per-option checks (`--schema`, `--state`
/// and `--request` must name existing files). This function then applies the
/// checks CLI11 is not asked to make, so that each is reported in the engine's
/// own words: the mode name is known (matched case-insensitively), test mode
/// has both `--state` and `--request`, `--out-dir` is not an existing
/// non-directory, and a schema file is known, from `--schema` or the
/// configure-time default. It checks the command line only: it does not
/// open the database, read any of the files, or create the output directory.
///
/// @param argc  Argument count, as passed to `main`.
/// @param argv  Argument vector, as passed to `main`. `argv[0]` is the
///              program name. Not kept beyond the call.
/// @return The validated Options, or an empty `options` and the exit code to
///         use when the program should stop now: 0 after `--help` or
///         `--version`, non-zero after a usage error. Usage errors are
///         reported here, not thrown.
/// @note Side effect: when `options` is empty, the corresponding output has
///       already been written. CLI11 writes help and version text to stdout
///       and its own parse errors to stderr; this function writes its
///       validation errors to stderr.
ParseResult parseCommandLine(int argc, const char* const* argv);

} // namespace rainbohz::rendering_engine
