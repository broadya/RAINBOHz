// SPDX-License-Identifier: Apache-2.0

#pragma once

// Loading a score's tables into the native engine-db (native_model.hpp), by
// reading hzlc's Parquet files directly, without Ladybug (@see ADR 0028).
//
// The manifest is the same one the Ladybug backend loads, read by the same
// code (score_manifest.hpp). The files are then read whole, one at a time, by
// Apache Arrow's Parquet reader. Arrow types appear only in the loader's
// source file.
//
// The native loader accepts what hzlc writes and refuses, with a
// ManifestError naming the file, the column and the problem, anything else:
//
//   - only the canonical node tables and relationship pairs native_model.hpp
//     holds; the physical tables are the engine's own output;
//   - in a node file, the columns the manifest lists, by name and in order,
//     each a property of its table;
//   - in a relationship file, `from` and `to` and then every property of the
//     relationship in the schema's order, which is what Ladybug's COPY reads
//     by position;
//   - Arrow's int64, double, bool and string columns, as hzlc writes them,
//     narrowed to the schema's INT32, INT8 and FLOAT with a range check;
//   - every edge's ends among the rows loaded, and every relationship's
//     multiplicity.
//
// Unlike the Ladybug loader, which records each failed statement and goes
// on, this one stops at the first problem: a score that does not load whole
// is not used, so carrying on would only add messages about its consequences.

#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/score_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace rainbohz::rendering_engine::native {

/// A score loaded into a native model, and what was loaded.
struct NativeLoad {
    /// The model, holding the score's canonical tables and nothing else.
    NativeModel model;

    /// How many node tables the manifest listed, the EngineConfig not
    /// counted.
    std::size_t nodeTables = 0;

    /// How many relationship pairs it listed.
    std::size_t relationshipPairs = 0;

    /// How many node rows and edges were loaded altogether, the EngineConfig
    /// included.
    std::int64_t rows = 0;
};

/// Loads the tables a manifest lists into a fresh native model.
///
/// @param manifest  The manifest: a JSON file of `format`
///                  "rainbohz-score-tables", `version` 1.
/// @return The model and the counts.
/// @throws ScriptLoadError  If the manifest cannot be read.
/// @throws ManifestError  If the manifest or one of its files is unusable
///         (see the header comment for what is accepted), naming the file.
///         Nothing is returned: a score loads whole or not at all.
NativeLoad loadNativeScore(const std::filesystem::path& manifest);

} // namespace rainbohz::rendering_engine::native
