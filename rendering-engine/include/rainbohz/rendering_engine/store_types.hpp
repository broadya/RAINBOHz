// SPDX-License-Identifier: Apache-2.0

#pragma once

// Types every implementation of engine-db shares, whichever backend holds the
// data (@see ADR 0028): the error a store reports, the data census, and the
// EngineConfig as read.
//
// They speak in raw representations, as EngineDb does; wrapping values in
// unit types (units.hpp) is the caller's job.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine {

/// Thrown when engine-db rejects a query or fails to open: the underlying
/// graph database for the Ladybug backend, the in-memory model for the native
/// one.
class EngineDbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One line of a data census: a table's name, and how many rows it holds.
using RowCount = std::pair<std::string, std::int64_t>;

/// How much data the database actually holds, as opposed to how many tables
/// were declared in it.
///
/// Tables with no rows are omitted. A schema declares far more tables than any
/// one score populates, so listing the empty ones would bury the loaded ones.
struct Census {
    /// Populated node tables, sorted by name.
    std::vector<RowCount> nodes;

    /// Populated relationship tables, sorted by name.
    std::vector<RowCount> relationships;

    /// Row totals across all tables, including any omitted above (there are
    /// none, but the totals do not depend on that).
    std::int64_t totalNodes = 0;
    /// Relationship rows across all tables, counted the same way.
    std::int64_t totalRelationships = 0;

    /// Tests whether the database holds any data at all.
    ///
    /// @return True when there are no node rows and no relationship rows,
    ///         whether because nothing was loaded or because no schema has
    ///         been applied yet.
    bool empty() const noexcept { return totalNodes == 0 && totalRelationships == 0; }
};

/// One EngineConfig node as read, before validation. Every property is
/// nullable in the schema; each stage checks the ones it needs.
struct EngineConfigRecord {
    std::optional<std::int64_t> frameSize;   ///< frameSize, samples; NULL means one second.
    std::optional<std::int64_t> sampleRate;  ///< sampleRate, Hz.
    std::optional<std::int64_t> channelMode; ///< channelMode, 0 to 7 when valid.
};

} // namespace rainbohz::rendering_engine
