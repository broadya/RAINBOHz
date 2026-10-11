// SPDX-License-Identifier: Apache-2.0

#pragma once

// Applying Cypher scripts to engine-db.
//
// Schema DDL and initial database state are the same operation as far as this
// component is concerned: a file of statements, applied in order, with every
// failure attributable to a source line. Both go through here.
//
// Every statement is attempted. A failure is recorded and the loader moves on,
// so one bad statement in a state file does not hide the rest -- the run
// reports all of them together and the caller decides what to do about it.
// Nothing here writes to standard output; reporting is the caller's business.
//
// Note on ADR 0002: the three `lbug` CLI defects it documents (errors on
// stdout, exit 0 on failure, statements silently skipped when a whole file is
// piped in) are defects of the *shell*. This code drives the embedded library
// directly and is not subject to them. Statements are still submitted one at a
// time, for line attribution rather than for correctness. See ADR 0005.

#include "rainbohz/rendering_engine/cypher_script.hpp"
#include "rainbohz/rendering_engine/engine_db.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rainbohz::rendering_engine {

/// One statement the database rejected.
struct ScriptFailure {
    std::filesystem::path file; ///< The script file the statement came from
    std::size_t startLine = 0;  ///< 1-based line carrying the first executable character
    std::size_t endLine = 0;    ///< 1-based line carrying the terminating semicolon
    std::string label;          ///< CypherStatement::label(), to identify the statement
    std::string message;        ///< Ladybug's error text

    /// Formats the failing statement's source line range, for use in reports.
    ///
    /// Matches CypherStatement::lineSpan(), so a failure and the statement it
    /// came from are described the same way.
    ///
    /// @return "line 42" when `startLine` equals `endLine`, otherwise
    ///         "lines 42-58". The numbers are printed as stored, so a
    ///         default-constructed failure gives "line 0".
    std::string lineSpan() const;
};

/// What one load did.
struct LoadReport {
    /// Statements the database accepted.
    int applied = 0;

    /// Declarations that were already in the catalog. Re-applying a schema is
    /// idempotent and these are not failures -- but note that it applies only
    /// *new* declarations. It does not migrate a table that already exists.
    int alreadyPresent = 0;

    /// Statements the database rejected, in the order they were applied.
    std::vector<ScriptFailure> failures;

    /// True when no statement failed.
    ///
    /// Statements that were already present do not count against this: a
    /// re-applied schema is still ok.
    ///
    /// @return True when `failures` is empty.
    bool ok() const noexcept { return failures.empty(); }

    /// Accumulates another report, for a caller applying several files.
    ///
    /// @param other  Report to add in. Its counts are summed into this one
    ///               and its failures are appended after this report's own,
    ///               so failures stay in the order the files were applied.
    /// @return `*this`, now covering both loads.
    /// @pre `other` is not `*this`. Appending a vector's own range to it is
    ///      undefined behaviour in std::vector::insert.
    LoadReport& operator+=(const LoadReport& other);
};

/// Thrown when a script cannot be read at all: a missing or unreadable file,
/// or one that cannot be split into statements (an unterminated block
/// comment). A statement the database rejects is not this -- that is a
/// ScriptFailure in the report.
class ScriptLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Called with the outcome of every statement, in order, as it is applied.
///
/// The report says what happened in aggregate; this is for a caller that wants
/// to narrate it, which is what --verbose does. Empty by default -- the loader
/// itself writes nothing.
using StatementObserver = std::function<void(const std::filesystem::path& file,
    const CypherStatement& statement, const ExecuteOutcome& outcome)>;

/// Applies every statement in `file` to `db`, in source order.
///
/// Order within the file is load-bearing: Ladybug binds relationship tables at
/// DDL time, so a CREATE REL TABLE must follow the CREATE NODE TABLE for every
/// table it names. That is a property of the file, which is hand-edited, and
/// this function does not reorder anything.
///
/// There is deliberately no directory equivalent. The engine names its schema
/// file and its state file, one each; see rendering-engine/docs/design.md
/// section 3.1.
///
/// The whole file is read before anything is executed, so an unreadable file
/// leaves `db` untouched. After that every statement is attempted, whatever
/// happened to the ones before it.
///
/// @param[in,out] db  Database the statements run against. Every statement it
///                    accepts is committed to it, so a load that reports
///                    failures has still applied the rest. Nothing is rolled
///                    back.
/// @param file        Script to apply. Must be a readable regular file. It is
///                    split with splitStatements(), whose line numbering the
///                    report inherits.
/// @param observe     Called once per statement, in source order, after the
///                    statement has run and before the next one starts.
///                    Optional; empty means no callback. Not retained beyond
///                    the call.
/// @return Counts of applied and already-present statements, plus one
///         ScriptFailure per rejected statement, in source order. Rejections
///         are reported here, not thrown.
/// @throws ScriptLoadError  If `file` is missing, not a regular file, or
///         cannot be opened or read; or if it has an unterminated block
///         comment. Nothing has been executed against `db` at that point.
/// @throws Anything `observe` throws, which ends the load at that statement.
///         Statements already applied stay committed.
/// @see docs/adr/0002, docs/adr/0005
[[nodiscard]] LoadReport loadCypherFile(
    EngineDb& db, const std::filesystem::path& file, const StatementObserver& observe = {});

} // namespace rainbohz::rendering_engine
