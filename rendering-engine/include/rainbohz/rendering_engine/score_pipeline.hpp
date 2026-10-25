// SPDX-License-Identifier: Apache-2.0

#pragma once

// The engine's two operations, as sequences of steps: load a score, and render
// frames from it.
//
// Batch mode runs both once and exits. Server mode runs a load per PUT /v1/score
// and a render per rendering call against the score it holds. Both modes run the
// same steps in the same order because both call these.
//
// Each operation narrates what it does to a stream, using report.hpp, and
// returns how far it got with a one-line verdict when it stopped short. The
// caller decides what a failure means: batch mode prints the verdict and
// exits non-zero, server mode puts it in the response.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/engine_store.hpp"
#include "rainbohz/rendering_engine/native_store.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>

namespace rainbohz::rendering_engine {

/// The command-line switches that change what the operations do or say.
struct PipelineSettings {
    /// Report every statement, check, partial and frame, not just failures.
    bool verbose = false;

    /// Run the integrity checks of integrity.hpp in two passes: the canonical
    /// pass after the state has loaded, stopping before the transform if any
    /// fails, and the physical pass after the transform, stopping before
    /// precomputation if any fails.
    bool validateDb = false;

    /// Also write each partial set's contribution to a frame as its own WAV
    /// file.
    bool perSetWav = false;

    /// Threads rendering each frame's partial sets, as renderFrames() takes
    /// them: 0 for one per hardware thread. 1 by default, so that a caller
    /// gets threads only when it asks; the command line's default is 0.
    std::size_t renderThreads = 1;

    /// Which kernel renders the partial sets, as renderFrames() takes it.
    RenderKernel renderKernel = RenderKernel::Simd;

    /// Which engine-db backend loads a score (@see ADR 0028); nullopt to
    /// let the score decide (backendFor()).
    std::optional<Backend> backend = std::nullopt;
};

/// The backend a score loads into, or why it cannot be loaded.
struct BackendChoice {
    /// The backend to load the score into.
    Backend backend = Backend::Native;

    /// Why the score cannot be loaded as asked; nullopt when it can.
    std::optional<std::string> refusal;
};

/// Chooses the backend a score loads into.
///
/// A backend given in the settings is used. Without one, the score decides:
/// hzlc's tables (a `.json` manifest) load natively, and a `.cypher` script,
/// which only Ladybug reads, loads into Ladybug. The integrity checks are
/// Cypher queries, so settings.validateDb also chooses Ladybug. Native is
/// refused for a Cypher score and for validateDb, saying which backend to
/// use instead.
///
/// @param settings   backend and validateDb are used.
/// @param stateFile  The score.
/// @return The backend, or why the score cannot be loaded as asked.
BackendChoice backendFor(const PipelineSettings& settings, const std::filesystem::path& stateFile);

/// The step at which a load stopped, or Complete.
enum class LoadStage : std::uint8_t {
    Complete,       ///< Every step ran and succeeded.
    Statements,     ///< A schema or state statement did not apply.
    Integrity,      ///< An integrity check failed (only with validateDb).
    Transform,      ///< The transform could not run, or a set or partial failed it.
    Precomputation, ///< Precomputation could not run, or a partial failed it.
};

/// How a load ended.
struct LoadResult {
    /// The step that stopped the load, or Complete.
    LoadStage stage = LoadStage::Complete;

    /// What went wrong, in one line, without a "rendering-engine:" prefix or
    /// a newline. Empty when the stage is Complete.
    std::string verdict;

    /// Whether the load ran to the end.
    ///
    /// @return True when the stage is Complete.
    bool ok() const noexcept { return stage == LoadStage::Complete; }
};

/// Loads a score into `db` and makes it ready to render.
///
/// In order: applies `schemaFile`, then `stateFile`; reports the schema
/// census; and, only if every statement applied, checks the canonical
/// model's integrity if settings.validateDb is set, transforms every partial
/// set and SoundSource in musical time, checks the physical timelines'
/// integrity if settings.validateDb is set, and precomputes every partial. The state file is
/// applied even when the schema did not load whole, since the errors it then produces say which
/// tables are missing. Each later step runs only if the one before succeeded.
/// What the database holds is reported after loading fails, after the
/// transform fails, or after precomputation, so that the census includes
/// its output.
///
/// @param db          Database to load into. Expected to be empty; a load does
///                    not clear what is already there.
/// @param schemaFile  Cypher file of schema DDL.
/// @param stateFile   The score: a Cypher file, or hzlc's manifest of tables.
/// @param settings    verbose and validateDb are used; perSetWav and
///                    renderThreads are not.
/// @param log         Where the narrative goes.
/// @return The step that stopped the load, with its verdict, or Complete.
/// @throws ScriptLoadError  If either file cannot be read.
/// @throws EngineDbError    If engine-db cannot be inspected.
/// @note Side effect: writes the score, its physical timelines and its
///       precomputation to `db`. A load that stops short leaves what it had
///       written.
LoadResult loadScore(EngineDb& db, const std::filesystem::path& schemaFile,
    const std::filesystem::path& stateFile, const PipelineSettings& settings, std::ostream& log);

/// A native load: how it ended, and the store holding the score.
struct NativeScoreLoad {
    /// The step that stopped the load, with its verdict, or Complete.
    LoadResult result;

    /// The store holding the score as far as it loaded; null when the
    /// tables could not be read at all.
    std::unique_ptr<NativeStore> store;
};

/// Loads a score into a fresh native engine-db and makes it ready to render.
///
/// In order: reads the score's tables (native::loadNativeScore()), then, as
/// loadScore() does for Ladybug, transforms every partial set and
/// SoundSource and precomputes every partial, each step only if the one
/// before succeeded, reporting what engine-db holds as loadScore() does.
/// There are no integrity checks: they run on the Ladybug backend only
/// (@see ADR 0028).
///
/// @param stateFile  hzlc's `manifest.json` of the score's tables.
/// @param settings   verbose is used.
/// @param log        Where the narrative goes.
/// @return How the load ended, and the store.
/// @throws ScriptLoadError  If the manifest cannot be read.
/// @throws ManifestError    If the manifest or one of its tables is unusable.
NativeScoreLoad loadNativeScore(
    const std::filesystem::path& stateFile, const PipelineSettings& settings, std::ostream& log);

/// The step at which rendering stopped, or Complete.
enum class RenderStage : std::uint8_t {
    Complete,        ///< Every requested frame was rendered.
    UnknownLabels,   ///< The request names a label engine-db does not hold.
    OutputDirectory, ///< The output directory could not be created.
    Rendering,       ///< Rendering could not run, or a frame failed.
};

/// How rendering ended.
struct RenderResult {
    /// The step that stopped rendering, or Complete.
    RenderStage stage = RenderStage::Complete;

    /// What went wrong, in one line, as LoadResult::verdict. Empty when the
    /// stage is Complete.
    std::string verdict;

    /// What renderFrames() reported. Empty unless rendering ran, which it
    /// does at the Rendering and Complete stages, except when it could not
    /// run at all.
    RenderingReport report;

    /// Whether every frame was rendered.
    ///
    /// @return True when the stage is Complete.
    bool ok() const noexcept { return stage == RenderStage::Complete; }
};

/// Renders the frames a request asks for from a loaded, precomputed score.
///
/// Lists the request's frames; checks every label it names against the
/// labels engine-db holds; creates `outputDir` if it is absent; and renders
/// the frames with renderFrames(). Labels select nothing yet: every partial
/// in a frame's scope is rendered.
///
/// @param[in,out] store  engine-db holding a score that loadScore() completed.
///                   Read only.
/// @param request    The frames to render.
/// @param outputDir  Where the WAV files go; created if absent.
/// @param settings   verbose, perSetWav and renderThreads are used;
///                   validateDb is not.
/// @param log        Where the narrative goes.
/// @return The step that stopped rendering, with its verdict and whatever
///         renderFrames() reported, or Complete.
/// @throws EngineDbError  If engine-db cannot be read.
/// @note Side effect: creates `outputDir` and writes WAV files into it,
///       replacing any of the same names.
RenderResult renderRequestedFrames(EngineStore& store, const RenderRequest& request,
    const std::filesystem::path& outputDir, const PipelineSettings& settings, std::ostream& log);

} // namespace rainbohz::rendering_engine
