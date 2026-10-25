// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/native_store.hpp"

#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/precomputation_store.hpp"
#include "rainbohz/rendering_engine/transform/native_transform_store.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include <set>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief NativeStore: the native backend's parts, over the model it owns.
///
/// @see native_store.hpp

namespace rainbohz::rendering_engine {

NativeStore::NativeStore(native::NativeModel model)
    : model_{std::move(model)}, transform_{transform::makeNativeTransformStore(model_)},
      timing_{transform::makeNativeTimingSource(model_)},
      precomputation_{makeNativePrecomputationStore(model_)},
      frames_{makeNativeFrameSource(model_)} {}

NativeStore::~NativeStore() = default;

std::string_view NativeStore::backend() const noexcept {
    return "native";
}

transform::TransformStore& NativeStore::transformStore() {
    return *transform_;
}

transform::TimingSource& NativeStore::timingSource() {
    return *timing_;
}

PrecomputationStore& NativeStore::precomputationStore() {
    return *precomputation_;
}

FrameSource& NativeStore::frameSource() {
    return *frames_;
}

Census NativeStore::census() {
    return native::censusOf(model_);
}

std::set<std::string> NativeStore::labelNames() {
    return native::labelNamesOf(model_);
}

} // namespace rainbohz::rendering_engine
