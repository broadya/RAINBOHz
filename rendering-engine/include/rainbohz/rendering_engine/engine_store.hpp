// SPDX-License-Identifier: Apache-2.0

#pragma once

// engine-db as the engine's stages see it, whichever backend holds the data
// (@see ADR 0028).
//
// engine-db has one schema, `engine-db/schema/engine-schema.cypher`, and two
// implementations: the Ladybug graph database (ladybug_store.hpp) and a
// native in-memory model. A run uses exactly one of them. The stages never
// see which: each reaches engine-db only through the store interface made for
// it, speaking in the records it reads and the units it writes.
//
//   - transform::TransformStore    the musical-to-physical transform
//   - transform::TimingSource      the server's tempo-map and position calls
//   - PrecomputationStore          precomputation
//   - FrameSource                  rendering's per-frame gather
//
// The transform's two live in the transform namespace, which alone reads
// musical time; this header only names them.

#include "rainbohz/rendering_engine/store_types.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace rainbohz::rendering_engine {

/// Which implementation of engine-db a run uses (@see ADR 0028).
enum class Backend : std::uint8_t {
    Native,  ///< The native in-memory model (native_store.hpp).
    Ladybug, ///< The Ladybug graph database (ladybug_store.hpp).
};

/// The name `--backend` takes for a backend.
///
/// @param backend  The backend.
/// @return "native" or "ladybug".
constexpr std::string_view backendName(Backend backend) noexcept {
    return backend == Backend::Native ? "native" : "ladybug";
}

namespace transform {
class TransformStore;
class TimingSource;
} // namespace transform

class PrecomputationStore;
class FrameSource;

/// One engine-db, through the interfaces the stages use.
///
/// Every reference returned refers to a part of this store and lives as long
/// as it does. A store is used from one thread at a time.
class EngineStore {
public:
    virtual ~EngineStore() = default;

    EngineStore(const EngineStore&) = delete;
    EngineStore& operator=(const EngineStore&) = delete;
    EngineStore(EngineStore&&) = delete;
    EngineStore& operator=(EngineStore&&) = delete;

    /// The backend's name, as `--backend` takes it.
    ///
    /// @return "ladybug" or "native".
    virtual std::string_view backend() const noexcept = 0;

    /// The transform's reads and writes.
    ///
    /// @return This store's part for the transform.
    virtual transform::TransformStore& transformStore() = 0;

    /// The server's timing reads.
    ///
    /// @return This store's part for the timing queries.
    virtual transform::TimingSource& timingSource() = 0;

    /// Precomputation's reads and writes.
    ///
    /// @return This store's part for precomputation.
    virtual PrecomputationStore& precomputationStore() = 0;

    /// Rendering's per-frame reads.
    ///
    /// @return This store's part for rendering.
    virtual FrameSource& frameSource() = 0;

    /// Counts what engine-db holds, per node table and per relationship
    /// table, under the schema's table names.
    ///
    /// @return The census.
    /// @throws EngineDbError  If the count cannot be read.
    virtual Census census() = 0;

    /// The names of every Label in engine-db. A Label with no name is left
    /// out, since a request cannot select it.
    ///
    /// @return The names, sorted.
    /// @throws EngineDbError  If the read fails.
    virtual std::set<std::string> labelNames() = 0;

protected:
    EngineStore() = default;
};

} // namespace rainbohz::rendering_engine
