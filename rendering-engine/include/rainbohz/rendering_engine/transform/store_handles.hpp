// SPDX-License-Identifier: Apache-2.0

#pragma once

// An owning handle to a transform::TransformStore that code outside the
// transform directories can hold.
//
// TransformStore's header names the tempo map's records, which nothing
// outside the transform includes (AGENTS.md). A std::unique_ptr needs the
// complete type wherever it is destroyed, so the handle carries a deleter
// whose body lives in the transform, where the type is complete.

#include <memory>

namespace rainbohz::rendering_engine::transform {

class TransformStore;

/// Deletes a TransformStore from a translation unit that sees its type.
struct TransformStoreDeleter {
    /// Deletes `store`.
    ///
    /// @param store  The store; may be null.
    void operator()(TransformStore* store) const noexcept;
};

/// An owning pointer to a TransformStore of either backend.
using TransformStorePtr = std::unique_ptr<TransformStore, TransformStoreDeleter>;

} // namespace rainbohz::rendering_engine::transform
