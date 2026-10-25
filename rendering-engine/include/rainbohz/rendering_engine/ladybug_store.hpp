// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Ladybug backend of engine-db (@see ADR 0028): every store interface
// the stages use, implemented by fixed Cypher queries over an EngineDb, and
// writes batched through WriteBatch (@see ADR 0022).
//
// A LadybugStore does not own its EngineDb. Loading a score into it, the
// integrity checks and the schema census stay operations on the EngineDb
// itself (script_loader.hpp, table_loader.hpp, integrity.hpp), because they
// exist only for this backend.

#include "rainbohz/rendering_engine/engine_store.hpp"
#include "rainbohz/rendering_engine/store_types.hpp"
#include "rainbohz/rendering_engine/transform/store_handles.hpp"

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine {

class EngineDb;

/// engine-db held in a Ladybug database.
class LadybugStore final : public EngineStore {
public:
    /// Serves every stage from `db`.
    ///
    /// @param[in,out] db  The database. It must outlive the store, which
    ///                    keeps a pointer to it.
    explicit LadybugStore(EngineDb& db);

    ~LadybugStore() override;

    LadybugStore(const LadybugStore&) = delete;
    LadybugStore& operator=(const LadybugStore&) = delete;
    LadybugStore(LadybugStore&&) = delete;
    LadybugStore& operator=(LadybugStore&&) = delete;

    std::string_view backend() const noexcept override;
    transform::TransformStore& transformStore() override;
    transform::TimingSource& timingSource() override;
    PrecomputationStore& precomputationStore() override;
    FrameSource& frameSource() override;
    Census census() override;
    std::set<std::string> labelNames() override;

    /// The database this store reads and writes.
    ///
    /// @return The database given to the constructor.
    EngineDb& db() noexcept { return *db_; }

private:
    EngineDb* db_;                                        ///< The database; not owned.
    transform::TransformStorePtr transform_;              ///< The transform's part.
    std::unique_ptr<transform::TimingSource> timing_;     ///< The timing queries' part.
    std::unique_ptr<PrecomputationStore> precomputation_; ///< Precomputation's part.
    std::unique_ptr<FrameSource> frames_;                 ///< Rendering's part.
};

/// Reads every EngineConfig of a Ladybug engine-db, for the stores' parts.
///
/// @param[in,out] db  The database. Not modified.
/// @return One record per EngineConfig node.
/// @throws EngineDbError  If the query fails.
std::vector<EngineConfigRecord> readLadybugEngineConfigs(EngineDb& db);

/// Makes precomputation's store over a Ladybug engine-db.
///
/// @param[in,out] db  The database. It must outlive the store.
/// @return The store.
std::unique_ptr<PrecomputationStore> makeLadybugPrecomputationStore(EngineDb& db);

/// Makes rendering's frame source over a Ladybug engine-db.
///
/// @param[in,out] db  The database. It must outlive the source.
/// @return The source.
std::unique_ptr<FrameSource> makeLadybugFrameSource(EngineDb& db);

} // namespace rainbohz::rendering_engine
