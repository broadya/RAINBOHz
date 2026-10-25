// SPDX-License-Identifier: Apache-2.0

#pragma once

// The rendering-engine command line.
//
// Three run modes are anticipated by the architecture; `batch` and `server`
// are implemented. The third is recognised here so that the mode axis exists
// in the code from the start rather than being retrofitted around a binary
// that only ever did one thing.
//
//   batch        Empty database, schema applied from one Cypher file,
//                initial state loaded from a second Cypher file, every
//                partial precomputed, the frames named by a YAML render
//                request written to a directory as WAV files, terminate.
//                This is the harness docs/design.md section 3 describes.
//   server       Long-running, driven over REST (docs/server.md). Holds one
//                score in memory between calls: PUT /v1/score replaces it, a
//                rendering writes one frame to a directory, POST
//                /v1/shutdown exits. Files are
//                read only from beneath --input-root.
//   distributed  Long-running, request-driven over message-bus, writing to
//                object-storage. Not implemented.
//
// The parser is CLI11, and it is deliberately confined to the implementation:
// no third-party type appears in this header, the same rule Ladybug is held
// to.

#include "rainbohz/rendering_engine/engine_store.hpp"
#include "rainbohz/rendering_engine/render_kernel.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rainbohz::rendering_engine {

enum class Mode : std::uint8_t { Batch, Server, Distributed };

/// Returns the name used for a mode on the command line and in diagnostics.
///
/// This is the reverse of the name lookup parseCommandLine() does for
/// `--mode`, so a name printed in a diagnostic can be typed back in.
///
/// @param mode  Mode to name.
/// @return The lower-case command-line name (`batch`, `server` or
///         `distributed`). A value outside the enumerators, which only a cast
///         can produce, gives `unknown`. The view refers to a string literal,
///         so it stays valid for the whole program.
std::string_view modeName(Mode mode) noexcept;

/// The rendering-engine's settings, as parsed from the command line by
/// parseCommandLine().
struct Options {
    /// The run mode. `batch` when --mode is not given.
    Mode mode = Mode::Batch;

    /// Cypher file of schema DDL, applied to the empty database before
    /// anything else. Defaults to the in-repo
    /// `engine-db/schema/engine-schema.cypher`, baked in at configure time --
    /// a development affordance. A deployed binary passes --schema.
    std::filesystem::path schemaFile;

    /// Cypher file holding the initial database state, applied on top of the
    /// schema. Required in batch mode, refused in server mode (a score arrives
    /// by PUT /v1/score); there is no default, because a run must not silently load a
    /// score nobody asked for.
    std::filesystem::path stateFile;

    /// YAML render request: which frames to render, and which labels select
    /// the partials for each. Required in batch mode, refused in server mode.
    /// See render_request.hpp.
    std::filesystem::path requestFile;

    /// Where rendered WAV files are written, one per frame, and created if
    /// absent. Defaults to the current directory in batch mode; required in
    /// server mode.
    std::filesystem::path outputDir = ".";

    /// Server mode: the address of the interface to listen on, such as
    /// `127.0.0.1` or `0.0.0.0`. Required; there is no default, so that which
    /// interfaces the server is exposed on is always a deliberate choice.
    std::string host;

    /// Server mode: the TCP port to listen on. Required. 0 asks the operating
    /// system for a free port, which the server reports once it is listening.
    std::uint16_t port = 0;

    /// Server mode: the directory every score load must read from. A URI that
    /// resolves outside it is refused. Required, and must exist.
    std::filesystem::path inputRoot;

    /// Where engine-db lives. Empty means in memory, which is the deployed
    /// configuration; a path is a debugging affordance, so that a run can be
    /// opened afterwards in the `lbug` shell. Batch mode only: a server
    /// replaces its database on every PUT /v1/score.
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

    /// How many threads render each frame's partial sets: 0 (the default)
    /// for one per hardware thread, otherwise that many, at most
    /// kMaxThreads. The files written do not depend on it, only how fast
    /// they are written (ADR 0026).
    std::size_t renderThreads = 0;

    /// Which kernel renders the partial sets: SIMD, the default, or the
    /// scalar reference (ADR 0027). The two agree within the bound of
    /// docs/rendering.md section 10; the reference is there to render a
    /// reference signal from any score.
    RenderKernel renderKernel = RenderKernel::Simd;

    /// Which engine-db backend loads the score (@see ADR 0028): native or
    /// Ladybug. nullopt, the default, lets the score decide: hzlc's tables
    /// load natively, and a Cypher script, `--validate-db` or `--db-path`
    /// need Ladybug (backendFor() in score_pipeline.hpp).
    std::optional<Backend> backend = std::nullopt;
};

/// The outcome of parsing a command line.
///
/// `options` is empty when the program should exit immediately with
/// `exitCode` -- --help and --version (0), or a usage error (non-zero). CLI11
/// has already written the corresponding output.
struct ParseResult {
    /// The parsed options; empty when the program should exit with `exitCode`.
    std::optional<Options> options;
    /// The status to exit with when `options` is empty: 0 for --help and
    /// --version, non-zero for a usage error.
    int exitCode = 0;
};

/// Parses and validates the rendering-engine command line.
///
/// CLI11 does the parsing and the per-option checks (`--schema`, `--state`
/// and `--request` must name existing files, `--input-root` an existing
/// directory, and `--port` a number from 0 to 65535). This function then applies the
/// checks CLI11 is not asked to make, so that each is reported in the engine's
/// own words: the mode name is known (matched case-insensitively); batch mode
/// has both `--state` and `--request` and none of the server options; server
/// mode has `--host`, `--port`, `--input-root` and `--out-dir`, and none of
/// `--state`, `--request` or `--db-path`; `--out-dir` is not an existing
/// non-directory; and a schema file is known, from `--schema` or the
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
