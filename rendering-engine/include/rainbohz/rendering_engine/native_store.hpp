// SPDX-License-Identifier: Apache-2.0

#pragma once

// The native backend of engine-db (@see ADR 0028): every store interface
// the stages use, implemented over a NativeModel held in memory.
//
// A NativeStore owns its model. A score is loaded into a model by
// native::loadNativeScore() (native_table_loader.hpp) and handed over whole;
// the transform and precomputation then write into it through the store, and
// rendering and the timing queries read from it. Nothing here touches
// Ladybug.

#include "rainbohz/rendering_engine/engine_store.hpp"
#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/store_types.hpp"
#include "rainbohz/rendering_engine/transform/store_handles.hpp"

#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace rainbohz::rendering_engine {

/// engine-db held natively, in memory.
class NativeStore final : public EngineStore {
public:
    /// Serves every stage from `model`, which the store takes over.
    ///
    /// @param model  A model with a score's canonical tables loaded.
    explicit NativeStore(native::NativeModel model);

    ~NativeStore() override;

    NativeStore(const NativeStore&) = delete;
    NativeStore& operator=(const NativeStore&) = delete;
    NativeStore(NativeStore&&) = delete;
    NativeStore& operator=(NativeStore&&) = delete;

    std::string_view backend() const noexcept override;
    transform::TransformStore& transformStore() override;
    transform::TimingSource& timingSource() override;
    PrecomputationStore& precomputationStore() override;
    FrameSource& frameSource() override;
    Census census() override;
    std::set<std::string> labelNames() override;

    /// The model this store reads and writes.
    ///
    /// @return The model.
    const native::NativeModel& model() const noexcept { return model_; }

private:
    native::NativeModel model_; ///< The model. Declared first: the parts point into it.
    transform::TransformStorePtr transform_;              ///< The transform's part.
    std::unique_ptr<transform::TimingSource> timing_;     ///< The timing queries' part.
    std::unique_ptr<PrecomputationStore> precomputation_; ///< Precomputation's part.
    std::unique_ptr<FrameSource> frames_;                 ///< Rendering's part.
};

/// Makes precomputation's store over a native model.
///
/// @param[in,out] model  The model. It must outlive the store, which writes
///                       the segment timelines and frames into it.
/// @return The store.
std::unique_ptr<PrecomputationStore> makeNativePrecomputationStore(native::NativeModel& model);

/// Makes rendering's frame source over a native model.
///
/// @param model  The model. It must outlive the source.
/// @return The source.
std::unique_ptr<FrameSource> makeNativeFrameSource(const native::NativeModel& model);

} // namespace rainbohz::rendering_engine
