// SPDX-License-Identifier: Apache-2.0

#pragma once

// engine-db: the Ladybug graph database hosted inside rendering-engine.
//
// architecture/overview.md, section 5.2: `engine-db` is the one physical
// database in the system, and it is the engine's whole database. Its scope is
// everything in the canonical model, plus everything the engine needs in order
// to determine dependencies, precompute and render. In the distributed
// deployment it is sharded by partial set, one shard per engine instance, and
// a shard's data stays in one place rather than moving between stores.
//
// The canonical/rendering separation is conceptual, not physical. The schema
// this database loads is `engine-db/schema/engine-schema.cypher`, and it is
// under construction: it currently holds the rendering-model concepts, and the
// canonical-model concepts are still to be merged in. `canonical-db/schema/` is
// a design artefact describing the composer-facing perspective; nothing loads
// it. The former `rendering-db/schema/` was retired once `engine-db` held its
// concepts -- see docs/adr/0006.
//
// This type owns the database and mediates access to it. The interface is
// low-level -- Cypher in, rows of plain values out -- and domain code such as
// precomputation builds on it. Values cross this interface as raw
// representations (bool, int64, double, string); wrapping them in unit types
// (units.hpp) is the caller's job.
//
// Ladybug headers are deliberately kept out of this header. Callers of
// rendering-engine should not acquire a transitive dependency on Ladybug's
// public API just by including this file; when the real interface arrives it
// should speak in domain types, not in lbug::main types.

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lbug::main {
class Connection;
class Database;
class PreparedStatement;
} // namespace lbug::main

namespace rainbohz::rendering_engine {

/// Thrown when the underlying graph database rejects a query or fails to open.
class EngineDbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The result of a statement submitted through `tryExecute`.
struct ExecuteOutcome {
    /// True when the database accepted the statement.
    bool ok = false;

    /// Ladybug's error text when `ok` is false; empty otherwise.
    std::string message;

    /// True when the failure was a declaration that is already in the catalog.
    /// Re-applying a schema to a populated database produces one of these per
    /// table, and a loader running idempotently treats them as "present"
    /// rather than as failures.
    bool alreadyExists = false;
};

/// One value in a query result.
///
/// Ladybug's column types are narrowed to the few the engine needs: NULL
/// (std::monostate), BOOL, every signed integer type including SERIAL
/// (std::int64_t), FLOAT and DOUBLE (double), and STRING. A FLOAT is widened
/// to double exactly. Any other column type is refused by query().
using DbValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// One row of a query result, one DbValue per returned column, in order.
using DbRow = std::vector<DbValue>;

/// The value bound to one query parameter. There is no NULL: a property that
/// should be NULL is left out of the statement instead.
///
/// A list of integers binds as a Cypher LIST of INT64, for
/// `UNWIND $ids AS id MATCH (n:Table {id: id})`: one statement then serves
/// many nodes, each by primary-key lookup, which is how rendering walks every
/// partial's list one hop at a time (docs/rendering.md section 6.3). Note that
/// `WHERE n.id IN $ids` is refused by Ladybug 0.19.1 for a SERIAL id, because
/// it will not compare a SERIAL[] with an INT64[]; the UNWIND form is the one
/// that works (established by experiment).
using DbParameter =
    std::variant<bool, std::int64_t, double, std::string, std::vector<std::int64_t>>;

/// Named query parameters. Each name is referenced in the Cypher as `$name`.
using DbParameters = std::vector<std::pair<std::string, DbParameter>>;

/// Reads a nullable integer column.
///
/// @param value  One value from a DbRow.
/// @return The integer, or nullopt if `value` is NULL.
/// @throws EngineDbError  If `value` is neither NULL nor an integer.
std::optional<std::int64_t> optionalInt64(const DbValue& value);

/// Reads a nullable floating-point column (DOUBLE or FLOAT).
///
/// @param value  One value from a DbRow.
/// @return The number, or nullopt if `value` is NULL.
/// @throws EngineDbError  If `value` is neither NULL nor a floating-point
///         number. An integer is refused rather than converted, because a
///         column of the wrong type is a schema mismatch worth hearing about.
std::optional<double> optionalDouble(const DbValue& value);

/// Reads a nullable boolean column.
///
/// @param value  One value from a DbRow.
/// @return The boolean, or nullopt if `value` is NULL.
/// @throws EngineDbError  If `value` is neither NULL nor a boolean.
std::optional<bool> optionalBool(const DbValue& value);

/// Reads a nullable string column.
///
/// @param value  One value from a DbRow.
/// @return The string, or nullopt if `value` is NULL.
/// @throws EngineDbError  If `value` is neither NULL nor a string.
std::optional<std::string> optionalString(const DbValue& value);

/// Reads an integer column that must not be NULL, such as a SERIAL id.
///
/// @param value  One value from a DbRow.
/// @return The integer.
/// @throws EngineDbError  If `value` is NULL or not an integer.
std::int64_t requireInt64(const DbValue& value);

/// One row of the table catalog: the table's name, and "NODE" or "REL".
using TableEntry = std::pair<std::string, std::string>;

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

/// Owns the process-local `engine-db` instance.
///
/// Non-copyable and non-movable: the Ladybug Connection holds a raw pointer
/// back to its Database, so the pair must not be relocated.
class EngineDb {
public:
    /// Opens an in-memory database. This is the intended production mode.
    ///
    /// Equivalent to `EngineDb(":memory:")`.
    ///
    /// @throws EngineDbError  If Ladybug fails to create the database or to
    ///                        connect to it.
    EngineDb();

    /// Opens a database at `path`, or in memory if `path` is ":memory:" or
    /// empty.
    ///
    /// An on-disk path is a debugging affordance -- being able to open an
    /// engine-db in the `lbug` shell and inspect it is worth the option -- and
    /// is not part of the deployed configuration.
    ///
    /// @param path  Filesystem path of an on-disk database, which Ladybug
    ///              creates if it does not exist; or ":memory:" or empty for
    ///              an in-memory one. Copied: the view need not outlive the
    ///              call.
    /// @throws EngineDbError  If Ladybug fails to open the database or to
    ///                        connect to it -- for example because the path is
    ///                        not writable. The message names the path and
    ///                        carries Ladybug's own error text.
    explicit EngineDb(std::string_view path);

    /// Closes the connection, then the database.
    ///
    /// Members are destroyed in reverse declaration order, so the Connection
    /// goes before the Database it points at. An in-memory database and its
    /// contents are discarded; an on-disk one persists.
    ~EngineDb();

    /// @name Copying and moving
    /// Deleted: the Ladybug Connection holds a raw pointer back to its
    /// Database, so the pair must not be relocated. See the class comment.
    ///@{
    EngineDb(const EngineDb&) = delete;
    EngineDb& operator=(const EngineDb&) = delete;
    EngineDb(EngineDb&&) = delete;
    EngineDb& operator=(EngineDb&&) = delete;
    ///@}

    /// Executes `cypher` for effect, throwing on failure.
    ///
    /// Any rows the statement returns are discarded; use queryToString() to
    /// see them.
    ///
    /// @param cypher  A single Cypher statement. Copied: the view need not
    ///                outlive the call.
    /// @throws EngineDbError  If Ladybug rejects the statement. The message is
    ///                        Ladybug's error text. A statement that fails to
    ///                        parse at all may instead propagate Ladybug's own
    ///                        exception (see tryExecute()).
    void execute(std::string_view cypher);

    /// Executes `cypher` for effect, reporting failure by return value.
    ///
    /// This exists for callers that apply many statements and want to report
    /// all of the failures rather than stopping at the first. Prefer
    /// execute() where a failure really is fatal. Any rows the statement
    /// returns are discarded.
    ///
    /// @param cypher  A single Cypher statement. Copied: the view need not
    ///                outlive the call.
    /// @return `ok` set on success. On failure, `ok` clear, `message` holding
    ///         Ladybug's error text, and `alreadyExists` set when that text
    ///         reports a declaration already in the catalog. Failures are
    ///         never thrown, including those Ladybug reports by exception;
    ///         those are never classed as `alreadyExists`.
    [[nodiscard]] ExecuteOutcome tryExecute(std::string_view cypher);

    /// Lists every table in the catalog.
    ///
    /// Backs the census a loader prints once it has finished, which is the
    /// cheapest available check that a schema arrived whole.
    ///
    /// @return One entry per declared table, node and relationship alike,
    ///         sorted by name (and by kind between equal names). Empty when no
    ///         schema has been applied.
    /// @throws EngineDbError  If Ladybug rejects the catalog query.
    std::vector<TableEntry> tables();

    /// Counts how many rows each populated table holds.
    ///
    /// tables() says what the schema declared; this says what the data
    /// actually is. A state file whose statements all applied has still loaded
    /// nothing if it matched nothing, and only a row count distinguishes the
    /// two cases.
    ///
    /// Implemented as two unlabelled scans rather than one query per table:
    /// the schema declares tens of tables and a count query each would cost
    /// more than the scan does.
    ///
    /// @return Per-table row counts for the non-empty node and relationship
    ///         tables, each list sorted by name, plus the totals. An empty
    ///         catalog yields an empty census rather than an error.
    /// @throws EngineDbError  If Ladybug rejects either scan.
    Census census();

    /// Collects the names of every Label node.
    ///
    /// This is what a render request's labels are checked against.
    ///
    /// @return The distinct names, sorted. Label.name is not a key, so two
    ///         Label nodes may share a name; they count as one here, as they
    ///         do when a request selects by name. A Label with a null name
    ///         cannot be selected by name and is omitted.
    /// @throws EngineDbError  If Ladybug rejects the query -- for example
    ///                        because no schema declaring a Label table has
    ///                        been applied.
    std::set<std::string> labelNames();

    /// Executes `cypher` with `parameters` bound, and returns every row.
    ///
    /// Statements are prepared once and the prepared form is kept for the
    /// lifetime of this object, keyed by the exact text of `cypher`. Callers
    /// that run the same statement many times -- once per segment, once per
    /// partial -- should therefore pass the same text and vary only the
    /// parameters.
    ///
    /// The whole result is read into memory before returning. Use it for
    /// results of modest size; nothing here streams.
    ///
    /// @param cypher      A single Cypher statement, with parameters written
    ///                    as `$name`. Copied: the view need not outlive the
    ///                    call.
    /// @param parameters  Values for every `$name` the statement uses.
    /// @return The rows, in the order the database returned them. Empty for
    ///         a statement that returns nothing.
    /// @throws EngineDbError  If the statement fails to prepare or to execute,
    ///         with Ladybug's error text; or if a returned column is of a type
    ///         DbValue does not represent.
    std::vector<DbRow> query(std::string_view cypher, const DbParameters& parameters = {});

    /// Starts an explicit read-write transaction on this connection.
    ///
    /// Until commit() or rollback(), every statement runs inside it. Prefer
    /// the Transaction guard, which rolls back on every path that does not
    /// commit.
    ///
    /// @throws EngineDbError  If Ladybug refuses, for example because a
    ///         transaction is already open.
    void beginTransaction();

    /// Commits the open transaction.
    ///
    /// @throws EngineDbError  If there is no open transaction, or the commit
    ///         fails.
    void commit();

    /// Rolls back the open transaction, discarding its writes.
    ///
    /// @throws EngineDbError  If there is no open transaction. Note that
    ///         Ladybug may already have rolled back a transaction in which a
    ///         statement failed, in which case this throws too.
    void rollback();

    /// Reports where this instance was opened.
    ///
    /// @return The path given to the constructor, or ":memory:" for an
    ///         in-memory database, including one opened with an empty path.
    ///         Valid for the lifetime of this object.
    const std::string& path() const noexcept;

    /// Executes `cypher` and returns the result rendered as text.
    ///
    /// A scaffolding convenience: returning formatted text is not a sane
    /// long-term interface, and this goes away once real result handling
    /// exists.
    ///
    /// @param cypher  A single Cypher statement. Copied: the view need not
    ///                outlive the call.
    /// @return The result as Ladybug formats it for display: a header of
    ///         column names followed by one line per row. The layout is
    ///         Ladybug's and is not meant to be parsed.
    /// @throws EngineDbError  If Ladybug rejects the statement. As with
    ///                        execute(), a statement that fails to parse at
    ///                        all may instead propagate Ladybug's own
    ///                        exception.
    std::string queryToString(std::string_view cypher);

    /// Reports the Ladybug library version this binary was linked against.
    ///
    /// @return The version string the build system recorded for the Ladybug
    ///         dependency (see cmake/Dependencies.cmake). Needs no database
    ///         instance.
    static std::string ladybugVersion();

private:
    /// The path the database was opened at; ":memory:" for an in-memory one.
    std::string path_;
    /// The open Ladybug database.
    std::unique_ptr<lbug::main::Database> database_;
    /// The connection every statement runs on.
    std::unique_ptr<lbug::main::Connection> connection_;

    // Declared after connection_ so that they are destroyed before it.
    /// Prepared statements cached by query(), keyed by their Cypher text.
    std::unordered_map<std::string, std::unique_ptr<lbug::main::PreparedStatement>> prepared_;
};

/// Scoped transaction: begins on construction, rolls back on destruction
/// unless commit() was called.
///
/// This is what makes a partial's output all-or-nothing: an exception thrown
/// between construction and commit() leaves the database as it was.
class Transaction {
public:
    /// Begins a transaction on `db`.
    ///
    /// @param[in,out] db  Database to run the transaction on. Must outlive
    ///                    this object; a reference is kept.
    /// @throws EngineDbError  If the transaction cannot be started.
    explicit Transaction(EngineDb& db);

    /// Rolls back, unless commit() has succeeded.
    ///
    /// A failed rollback is swallowed, since a destructor must not throw and
    /// Ladybug may already have rolled back after a failed statement.
    ~Transaction();

    /// @name Copying and moving
    /// Deleted: a transaction is tied to one scope.
    ///@{
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    ///@}

    /// Commits the transaction's writes.
    ///
    /// @throws EngineDbError  If the commit fails. The destructor then still
    ///         attempts a rollback.
    void commit();

private:
    /// The database the transaction runs on.
    EngineDb* db_; // Never null. A pointer, not a reference member (Core Guidelines C.12).
    bool committed_ = false; ///< Whether commit() has succeeded, so no rollback is due.
};

} // namespace rainbohz::rendering_engine
