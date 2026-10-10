// SPDX-License-Identifier: Apache-2.0

#pragma once

// The engine's reports: what loading, checking, transforming, precomputing and
// rendering did, written as text.
//
// The library modules return reports and never write output themselves; these
// functions turn those reports into the narrative an operator reads. Batch
// mode writes it to standard output. Server mode writes each call's narrative
// to a string, which goes into the response and is echoed to standard output.
// Both say the same things in the same words, because both call these.
//
// The format deliberately follows scripts/load_schema.py. The two tools apply
// the same DDL to the same database for the same reasons, and a reader should
// not have to learn two output formats to compare them. Everything is one
// narrative on one stream: splitting it across two only scrambles the order
// when the output is redirected.

#include "rainbohz/rendering_engine/cypher_script.hpp"
#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/integrity.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"

#include <filesystem>
#include <ostream>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine {

/// Reports statements as the loader applies them, in source order.
///
/// A failure is always reported, with its diagnostic, because a statement that
/// did not run is the thing worth knowing about. Successes are reported only
/// under --verbose.
///
/// @param out        Where the report goes.
/// @param verbose    Whether to report statements that applied or were already
///                   present, as well as those that failed.
/// @param file       Script the statement came from. Only its filename is
///                   printed.
/// @param statement  Statement just executed; supplies the line span and label.
/// @param outcome    What Ladybug made of it. A multi-line diagnostic is printed
///                   one indented line at a time beneath the FAILED line.
void narrateStatement(std::ostream& out, bool verbose, const std::filesystem::path& file,
    const CypherStatement& statement, const ExecuteOutcome& outcome);

/// Prints the one-line summary of loading a single Cypher file.
///
/// @param out     Where the report goes.
/// @param what    Name of the file's role ("schema", "state "), padded by the
///                caller so the summaries line up.
/// @param report  Result of loadCypherFile() for that file.
void reportLoad(std::ostream& out, std::string_view what, const LoadReport& report);

/// Reports how many NODE and REL tables engine-db declares.
///
/// Other table kinds in the catalog are not counted.
///
/// @param out  Where the report goes.
/// @param db   Database to inspect. Non-const only because querying it is.
/// @throws EngineDbError  If Ladybug rejects the catalog query.
void reportSchemaCensus(std::ostream& out, EngineDb& db);

/// Reports what the database actually holds, table by table.
///
/// This is the only part of the output that distinguishes a loaded engine-db
/// from an empty one. Statement counts say the state file parsed and was
/// accepted; a statement that matched nothing is accepted too, and applies
/// nothing. Row counts are what say the score arrived.
///
/// Prints the totals, then, unless the database is empty, the node tables and
/// the relationship tables as two blocks with their counts in one column.
///
/// @param out  Where the report goes.
/// @param db   Database to inspect. Non-const only because querying it is.
/// @throws EngineDbError  If Ladybug rejects either scan.
void reportDataCensus(std::ostream& out, EngineDb& db);

/// Prints each problem as a FAILED line, in the same layout narrateStatement()
/// uses for a statement that did not apply.
///
/// @param out       Where the report goes.
/// @param file      File the problems were found in, passed to describeProblem().
/// @param problems  Problems to print, in the given order.
void reportProblems(std::ostream& out, const std::filesystem::path& file,
    const std::vector<RequestProblem>& problems);

/// Reports the outcome of the integrity checks, one line per violation.
///
/// Every violation a check kept gets a FAILED line, in the layout
/// narrateStatement() uses; a check with more violations than it kept says how
/// many more. A check that passed gets an `ok` line under --verbose only. A
/// closing line gives the totals.
///
/// @param out      Where the report goes.
/// @param report   Result of checkIntegrity().
/// @param verbose  Whether to list the checks that passed.
void reportIntegrity(std::ostream& out, const IntegrityReport& report, bool verbose);

/// Reports the outcome of the transform: the tempo maps found, then one line
/// per set or partial that failed, and under --verbose one per partial that
/// succeeded, in the layout reportPrecomputation() uses. A closing line
/// gives the totals.
///
/// @param out      Where the report goes.
/// @param report   Result of transformAll().
/// @param verbose  Whether to list the partials that succeeded.
void reportTransform(std::ostream& out, const transform::TransformReport& report, bool verbose);

/// Reports the outcome of precomputation, one line per partial.
///
/// A partial that failed always gets a FAILED line with the reason, in the
/// layout narrateStatement() uses. One that succeeded gets an `ok` line with
/// its segment count and frame range under --verbose only, as a statement
/// that applied does. A closing line gives the totals.
///
/// @param out      Where the report goes.
/// @param report   Result of precomputeAll().
/// @param verbose  Whether to list the partials that succeeded.
void reportPrecomputation(std::ostream& out, const PrecomputationReport& report, bool verbose);

/// Reports the outcome of rendering, one line per frame.
///
/// In request order: every partial that first failed validation in a frame
/// gets a FAILED line with its reason, then the frame gets its own line -- an
/// `ok` line with its set and partial counts, peak and file under --verbose
/// only, a FAILED line always. With --per-set-wav, each set's file is an
/// indented `ok` line under its frame's. A closing line gives the totals.
///
/// @param out      Where the report goes.
/// @param report   Result of renderFrames().
/// @param verbose  Whether to list the frames that rendered.
void reportRendering(std::ostream& out, const RenderingReport& report, bool verbose);

/// Lists the frames a request asks for, and what selects each one's partials.
///
/// @param out      Where the report goes.
/// @param request  A request that has already been loaded and validated.
void reportRequest(std::ostream& out, const RenderRequest& request);

} // namespace rainbohz::rendering_engine
