// SPDX-License-Identifier: Apache-2.0

#pragma once

// The stages' entry points over a Ladybug EngineDb, for the tests.
//
// The stages reach engine-db through store interfaces (engine_store.hpp,
// @see ADR 0028). Most tests build their database by loading Cypher into an
// EngineDb, which only the Ladybug backend can hold, and then call a stage on
// it. These overloads wrap the database in a LadybugStore for the length of
// one call, so that those tests read as before. They are declared in the
// stages' own namespaces, so a test's using-declaration of a stage brings
// them in with it.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/score_pipeline.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <vector>

namespace rainbohz::rendering_engine {

/// precomputeAll() over `db`.
///
/// @param[in,out] db  The database.
/// @return As precomputeAll().
inline PrecomputationReport precomputeAll(EngineDb& db) {
    LadybugStore store{db};
    return precomputeAll(store.precomputationStore());
}

/// renderFrames() over `db`.
///
/// @param[in,out] db  The database.
/// @param request     As renderFrames().
/// @param outputDir   As renderFrames().
/// @param perSetWav   As renderFrames().
/// @param threads     As renderFrames().
/// @param kernel      As renderFrames().
/// @return As renderFrames().
inline RenderingReport renderFrames(EngineDb& db, const RenderRequest& request,
    const std::filesystem::path& outputDir, bool perSetWav, std::size_t threads = 1,
    RenderKernel kernel = RenderKernel::Simd) {
    LadybugStore store{db};
    return renderFrames(store.frameSource(), request, outputDir, perSetWav, threads, kernel);
}

/// renderRequestedFrames() over `db`.
///
/// @param[in,out] db  The database.
/// @param request     As renderRequestedFrames().
/// @param outputDir   As renderRequestedFrames().
/// @param settings    As renderRequestedFrames().
/// @param log         As renderRequestedFrames().
/// @return As renderRequestedFrames().
inline RenderResult renderRequestedFrames(EngineDb& db, const RenderRequest& request,
    const std::filesystem::path& outputDir, const PipelineSettings& settings, std::ostream& log) {
    LadybugStore store{db};
    return renderRequestedFrames(store, request, outputDir, settings, log);
}

} // namespace rainbohz::rendering_engine

namespace rainbohz::rendering_engine::transform {

/// transformAll() over `db`.
///
/// @param[in,out] db  The database.
/// @return As transformAll().
inline TransformReport transformAll(EngineDb& db) {
    LadybugStore store{db};
    return transformAll(store.transformStore());
}

/// readRootTempoMap() over `db`.
///
/// @param[in,out] db  The database.
/// @return As readRootTempoMap().
inline TempoMapNode readRootTempoMap(EngineDb& db) {
    LadybugStore store{db};
    return readRootTempoMap(store.timingSource());
}

/// readTempoMapChildren() over `db`.
///
/// @param[in,out] db  The database.
/// @param mapId       As readTempoMapChildren().
/// @return As readTempoMapChildren().
inline std::vector<TempoMapChild> readTempoMapChildren(EngineDb& db, std::int64_t mapId) {
    LadybugStore store{db};
    return readTempoMapChildren(store.timingSource(), mapId);
}

/// readTempoMapPoints() over `db`.
///
/// @param[in,out] db  The database.
/// @param mapId       As readTempoMapPoints().
/// @param after       As readTempoMapPoints().
/// @param limit       As readTempoMapPoints().
/// @return As readTempoMapPoints().
inline TempoMapPointPage readTempoMapPoints(
    EngineDb& db, std::int64_t mapId, std::optional<std::int64_t> after, std::int32_t limit) {
    LadybugStore store{db};
    return readTempoMapPoints(store.timingSource(), mapId, after, limit);
}

/// readEngineParameters() over `db`.
///
/// @param[in,out] db  The database.
/// @return As readEngineParameters().
inline EngineParameters readEngineParameters(EngineDb& db) {
    LadybugStore store{db};
    return readEngineParameters(store.timingSource());
}

/// framePosition() over `db`.
///
/// @param[in,out] db  The database.
/// @param mapId       As framePosition().
/// @param at          As framePosition().
/// @return As framePosition().
inline FramePosition framePosition(EngineDb& db, std::int64_t mapId, BeatsTicks at) {
    LadybugStore store{db};
    return framePosition(store.timingSource(), mapId, at);
}

} // namespace rainbohz::rendering_engine::transform
