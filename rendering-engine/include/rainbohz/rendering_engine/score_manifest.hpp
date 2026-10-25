// SPDX-License-Identifier: Apache-2.0

#pragma once

// The manifest of a score's tables, as hzlc writes it (ADR 0025;
// hzl-compiler/docs/design.md section 7), read and checked once for either
// engine-db backend: the Ladybug loader copies the files it lists with COPY
// FROM (table_loader.hpp), the native loader reads them itself
// (native_table_loader.hpp).
//
// The directory holds `manifest.json` and one Parquet file per node table
// with properties and per relationship (FROM, TO) pair. A node table with no
// properties has no file: the manifest gives its row count. Every name in the
// manifest is an identifier, and every file a plain `.parquet` name in the
// manifest's own directory, so a manifest names nothing outside it.

#include "rainbohz/rendering_engine/script_loader.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine {

/// Thrown when a manifest can be read but not used: it is not JSON of the
/// expected shape, or names a data file that is missing or outside its
/// directory, or (for the native loader) a table, column or value the
/// schema does not allow. A fault in the score, as a statement that does not
/// apply is, rather than in reading it.
class ManifestError : public ScriptLoadError {
public:
    using ScriptLoadError::ScriptLoadError;
};

/// A node table the manifest lists.
struct ManifestNodeTable {
    std::string table;     ///< The node table's name.
    std::int64_t rows = 0; ///< How many rows it has.
    /// The Parquet file of its rows, resolved; nullopt for a table with no
    /// properties, whose rows are created by count.
    std::optional<std::filesystem::path> file;
    /// The file's columns, in order: property names. Empty without a file.
    std::vector<std::string> columns;
};

/// A relationship (FROM, TO) pair the manifest lists.
struct ManifestRelationship {
    std::string table;          ///< The relationship table's name.
    std::string from;           ///< The FROM node table.
    std::string to;             ///< The TO node table.
    std::int64_t rows = 0;      ///< How many edges the file holds.
    std::filesystem::path file; ///< The Parquet file of its edges, resolved.
};

/// A manifest, read and checked.
struct ScoreManifest {
    /// The EngineConfig's properties, in the manifest's order; empty when it
    /// lists none, in which case no EngineConfig is created.
    std::vector<std::pair<std::string, std::int64_t>> engineConfig;
    /// The node tables, in the manifest's order.
    std::vector<ManifestNodeTable> nodes;
    /// The relationship pairs, in the manifest's order.
    std::vector<ManifestRelationship> relationships;
};

/// The manifest's `format`.
inline constexpr std::string_view kScoreManifestFormat = "rainbohz-score-tables";

/// The manifest `version` this engine reads.
inline constexpr std::int64_t kScoreManifestVersion = 1;

/// Tests whether a name is one Cypher can take unquoted: a letter or
/// underscore, then letters, digits and underscores.
///
/// @param name  The name.
/// @return True when it is such an identifier.
bool isIdentifier(std::string_view name);

/// Reads and checks a score's manifest.
///
/// @param manifest  The manifest: a JSON file of `format`
///                  "rainbohz-score-tables", `version` 1.
/// @return The manifest, every data file resolved, symlinks followed, to a
///         regular file in the manifest's directory.
/// @throws ScriptLoadError  If the manifest cannot be read.
/// @throws ManifestError  If it is not valid JSON of the expected shape, or
///         names a data file that is not a plain `.parquet` name, is missing,
///         or resolves outside its directory.
ScoreManifest readScoreManifest(const std::filesystem::path& manifest);

} // namespace rainbohz::rendering_engine
