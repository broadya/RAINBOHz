// SPDX-License-Identifier: Apache-2.0

#pragma once

// The native backend's parts for the transform: its TransformStore and its
// TimingSource, over a NativeModel (@see ADR 0028). NativeStore
// (native_store.hpp) owns one of each.

#include "rainbohz/rendering_engine/transform/store_handles.hpp"

#include <memory>

namespace rainbohz::rendering_engine::native {
struct NativeModel;
} // namespace rainbohz::rendering_engine::native

namespace rainbohz::rendering_engine::transform {

class TimingSource;

/// Makes the transform's store over a native model.
///
/// @param[in,out] model  The model holding the score. It must outlive the
///                       store, which keeps a pointer to it, and writes the
///                       physical timelines into it.
/// @return The store.
TransformStorePtr makeNativeTransformStore(native::NativeModel& model);

/// Makes the timing queries' source over a native model.
///
/// @param model  The model holding the score, its canonical tables loaded.
///               It must outlive the source, which keeps a pointer to it and
///               indexes the tempo maps' lists once, when it is made.
/// @return The source.
std::unique_ptr<TimingSource> makeNativeTimingSource(const native::NativeModel& model);

} // namespace rainbohz::rendering_engine::transform
