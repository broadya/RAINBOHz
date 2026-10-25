// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Ladybug backend's parts for the transform: its TransformStore and its
// TimingSource (@see ADR 0028). LadybugStore (ladybug_store.hpp) owns one of
// each.

#include "rainbohz/rendering_engine/transform/store_handles.hpp"

#include <memory>

namespace rainbohz::rendering_engine {
class EngineDb;
} // namespace rainbohz::rendering_engine

namespace rainbohz::rendering_engine::transform {

class TimingSource;

/// Makes the transform's store over a Ladybug engine-db.
///
/// @param[in,out] db  Database holding the score. It must outlive the store,
///                    which keeps a pointer to it.
/// @return The store.
TransformStorePtr makeLadybugTransformStore(EngineDb& db);

/// Makes the timing queries' source over a Ladybug engine-db.
///
/// @param[in,out] db  Database holding the score. It must outlive the
///                    source, which keeps a pointer to it.
/// @return The source.
std::unique_ptr<TimingSource> makeLadybugTimingSource(EngineDb& db);

} // namespace rainbohz::rendering_engine::transform
