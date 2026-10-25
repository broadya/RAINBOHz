// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/ladybug_store.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/precomputation_store.hpp"
#include "rainbohz/rendering_engine/transform/ladybug_transform_store.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief LadybugStore: the Ladybug backend's parts, and what they share.
///
/// @see ladybug_store.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Reads every EngineConfig's frame size, sample rate and channel mode.
constexpr std::string_view kReadConfig =
    "MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate, c.channelMode;";

} // namespace

std::vector<EngineConfigRecord> readLadybugEngineConfigs(EngineDb& db) {
    std::vector<EngineConfigRecord> configs;
    for (const DbRow& row : db.query(kReadConfig)) {
        configs.push_back(EngineConfigRecord{.frameSize = optionalInt64(row.at(0)),
            .sampleRate = optionalInt64(row.at(1)),
            .channelMode = optionalInt64(row.at(2))});
    }
    return configs;
}

LadybugStore::LadybugStore(EngineDb& db)
    : db_{&db}, transform_{transform::makeLadybugTransformStore(db)},
      timing_{transform::makeLadybugTimingSource(db)},
      precomputation_{makeLadybugPrecomputationStore(db)}, frames_{makeLadybugFrameSource(db)} {}

LadybugStore::~LadybugStore() = default;

std::string_view LadybugStore::backend() const noexcept {
    return "ladybug";
}

transform::TransformStore& LadybugStore::transformStore() {
    return *transform_;
}

transform::TimingSource& LadybugStore::timingSource() {
    return *timing_;
}

PrecomputationStore& LadybugStore::precomputationStore() {
    return *precomputation_;
}

FrameSource& LadybugStore::frameSource() {
    return *frames_;
}

Census LadybugStore::census() {
    return db_->census();
}

std::set<std::string> LadybugStore::labelNames() {
    return db_->labelNames();
}

} // namespace rainbohz::rendering_engine
