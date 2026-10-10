// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/options.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/report.hpp"
#include "rainbohz/rendering_engine/score_pipeline.hpp"
#include "rainbohz/rendering_engine/server.hpp"

#include <exception>
#include <iostream>

/// @file
/// @brief Entry point of the rendering-engine executable.
///
/// Parses the command line (options.hpp) and dispatches on the run mode. Three
/// run modes are anticipated; two are implemented.
///
/// `batch` reads the render request, stands up engine-db by applying the
/// schema from one Cypher file, loads an initial database state from a second,
/// optionally checks its integrity (`--validate-db`), transforms every partial
/// set with a musical timeline into physical timelines, precomputes every
/// partial's integrable segments and frame entries, checks the request's
/// labels against what loaded, and renders the requested frames to WAV files.
/// Labels do not yet select partials. The steps are those of
/// score_pipeline.hpp; this file adds the request, the verdict on standard
/// error and the exit code.
///
/// `server` hands over to runServer() (server.hpp), which runs the same steps
/// per REST call until it is told to shut down.

namespace {

using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::LoadResult;
using rainbohz::rendering_engine::Mode;
using rainbohz::rendering_engine::modeName;
using rainbohz::rendering_engine::Options;
using rainbohz::rendering_engine::PipelineSettings;
using rainbohz::rendering_engine::RenderRequest;
using rainbohz::rendering_engine::RenderRequestError;
using rainbohz::rendering_engine::RenderResult;

/// @name Process exit codes
/// @{

/// The run completed: in batch mode, everything loaded and every frame
/// rendered; in server mode, the server shut down when asked to.
constexpr int kExitOk = 0;

/// Something did not work. In batch mode: a bad request, a statement that did
/// not apply, an integrity violation under --validate-db, a partial set or
/// partial that could not be transformed, a partial that could not be
/// precomputed, an unknown label, or a frame that could not be rendered. In
/// server mode: the server could not start. In either: an exception that
/// reached main().
constexpr int kExitFailed = 1;

/// The requested mode exists on the command line but has no implementation
/// yet. Distinct from kExitFailed so a caller can tell "not yet" from "broken".
constexpr int kExitNotImplemented = 2;

/// @}

/// Runs the `batch` mode.
///
/// Loads the render request, then loads the score into a fresh engine-db with
/// loadScore() and, if that completes, renders the requested frames into
/// --out-dir with renderRequestedFrames(). Progress goes to standard output
/// and the closing verdict on a failure to standard error.
///
/// @param options  Validated command-line options, with `mode` equal to
///                 Mode::Batch.
/// @return kExitOk if everything loaded, every partial was precomputed, every
///         requested label exists and every requested frame was rendered;
///         kExitFailed if the request is unusable, or either operation
///         stopped short (score_pipeline.hpp lists where it can).
/// @throws EngineDbError    If engine-db cannot be opened or inspected.
/// @throws ScriptLoadError  If the schema or state file cannot be read.
/// @note Side effect: creates `options.outputDir` and writes WAV files into
///       it.
int runBatchMode(const Options& options) {
    EngineDb db{options.dbPath};

    std::cout << "rendering-engine, mode " << modeName(options.mode) << '\n'
              << "  engine-db: " << db.path() << '\n'
              << "  schema:    " << options.schemaFile.string() << '\n'
              << "  state:     " << options.stateFile.string() << '\n'
              << "  request:   " << options.requestFile.string() << '\n'
              << "  out-dir:   " << options.outputDir.string() << '\n'
              << '\n';

    // The request is read first: its shape needs nothing from the database,
    // and a malformed request is not worth loading a score for.
    RenderRequest request;
    try {
        request = rainbohz::rendering_engine::loadRenderRequest(options.requestFile);
    } catch (const RenderRequestError& e) {
        rainbohz::rendering_engine::reportProblems(std::cout, e.file(), e.problems());
        std::cerr << "\nrendering-engine: the render request is not usable.\n";
        return kExitFailed;
    }

    const PipelineSettings settings{.verbose = options.verbose,
        .validateDb = options.validateDb,
        .perSetWav = options.perSetWav};

    const LoadResult loaded = rainbohz::rendering_engine::loadScore(
        db, options.schemaFile, options.stateFile, settings, std::cout);
    if (!loaded.ok()) {
        std::cerr << "\nrendering-engine: " << loaded.verdict << '\n';
        return kExitFailed;
    }

    const RenderResult rendered = rainbohz::rendering_engine::renderRequestedFrames(
        db, request, options.outputDir, settings, std::cout);
    if (!rendered.ok()) {
        std::cerr << "\nrendering-engine: " << rendered.verdict << '\n';
        return kExitFailed;
    }

    std::cout << "\nLoaded, transformed, precomputed, and rendered "
              << rendered.report.frames.size() << " frame(s) to " << options.outputDir.string()
              << ". Labels do not yet select partials.\n";
    return kExitOk;
}

/// Reports that a run mode has no implementation yet.
///
/// @param mode  The mode that was asked for.
/// @return kExitNotImplemented, always.
int notImplemented(Mode mode) {
    std::cerr << "rendering-engine: mode '" << modeName(mode)
              << "' is not implemented.\n"
                 "                  'batch' and 'server' are available; see "
                 "rendering-engine/docs/design.md section 3.\n";
    return kExitNotImplemented;
}

} // namespace

/// Parses the command line and dispatches to the requested run mode.
///
/// Any exception that escapes a mode is reported on standard error as a
/// single line and turned into kExitFailed, so the process never ends by
/// std::terminate().
///
/// @param argc  Argument count, as passed by the runtime.
/// @param argv  Argument vector, as passed by the runtime.
/// @return The run mode's exit code; or, when the command line did not yield
///         options (--help, --version, a usage error), the code
///         parseCommandLine() chose.
int main(int argc, char** argv) {
    try {
        const auto parsed = rainbohz::rendering_engine::parseCommandLine(argc, argv);
        if (!parsed.options.has_value()) {
            return parsed.exitCode;
        }
        const Options& options = *parsed.options;

        switch (options.mode) {
        case Mode::Batch:
            return runBatchMode(options);
        case Mode::Server:
            return rainbohz::rendering_engine::runServer(options) ? kExitOk : kExitFailed;
        case Mode::Distributed:
            return notImplemented(options.mode);
        }
        return kExitFailed;
    } catch (const std::exception& e) {
        std::cerr << "rendering-engine: " << e.what() << '\n';
        return kExitFailed;
    }
}
