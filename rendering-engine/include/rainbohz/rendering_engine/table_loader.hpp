// SPDX-License-Identifier: Apache-2.0

#pragma once

// Loading a score's tables into engine-db, and choosing between the two
// forms a score's state can take.
//
// hzlc writes a score as Parquet files, one per node table and one per
// relationship (FROM, TO) pair of tables, listed by a JSON manifest. Ladybug
// reads each file with COPY FROM, as data: nothing is parsed as Cypher, which
// is what makes this form fast to load (about 0.11 s for echoes' 82,005 rows,
// against 9.2 s for the same rows as a Cypher script of literals; macos-max,
// 2026-10-21). The Cypher script remains the debugging form, and a state file
// written by hand is Cypher too. Both forms load the same database.
//
// See ADR 0025 and hzl-compiler/docs/design.md section 7.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/score_manifest.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include <filesystem>

namespace rainbohz::rendering_engine {

/// Loads the tables a manifest lists into `db`, nodes first, in the
/// manifest's order.
///
/// The manifest is read and checked whole (readScoreManifest()) before
/// anything is executed, so a bad manifest leaves `db` untouched. A table or column name must be an
/// identifier, and a data file a plain `.parquet` file name that resolves,
/// symlinks followed, to a regular file in the manifest's own directory: a
/// manifest names nothing outside it.
///
/// Relationships name their ends by id, and the ids must be the row indices
/// the files were written with. That holds only for a node table that is
/// empty when its rows are copied in, so a table that already holds rows is
/// a failure and is not written to. After each node table is copied, its row
/// count is checked against the manifest's.
///
/// Every statement is attempted after that, as loadCypherFile() does, and a
/// failure is recorded and reported, not thrown. A failure's file is the data
/// file the statement read (the manifest for a statement that read none), and
/// its lines are 0.
///
/// @param[in,out] db  Database to load. Every statement it accepts is
///                    committed; nothing is rolled back.
/// @param manifest    The manifest: a JSON file of `format`
///                    "rainbohz-score-tables", `version` 1.
/// @param observe     Called once per statement, as for loadCypherFile(),
///                    with the statement's lines 0. Optional.
/// @return Counts of applied statements and one ScriptFailure per failed one.
/// @throws ScriptLoadError  If the manifest cannot be read.
/// @throws ManifestError  If it is not valid JSON of the expected shape, or
///         names a data file that is missing or outside its directory, or
///         whose path holds a quote or backslash, which COPY cannot name.
///         Nothing has been executed at any of these points.
/// @throws Anything `observe` throws.
[[nodiscard]] LoadReport loadTableManifest(
    EngineDb& db, const std::filesystem::path& manifest, const StatementObserver& observe = {});

/// Loads a score's state from either of its forms, chosen by the file's
/// extension: a `.json` file is a table manifest (loadTableManifest()), a
/// `.cypher` file a Cypher script (loadCypherFile()).
///
/// @param[in,out] db  Database to load, as the loader chosen loads it.
/// @param file        The state file.
/// @param observe     Called once per statement. Optional.
/// @return What the loader chosen reports.
/// @throws ScriptLoadError  For any other extension, or as the loader chosen
///         throws.
/// @throws Anything `observe` throws.
[[nodiscard]] LoadReport loadStateFile(
    EngineDb& db, const std::filesystem::path& file, const StatementObserver& observe = {});

} // namespace rainbohz::rendering_engine
