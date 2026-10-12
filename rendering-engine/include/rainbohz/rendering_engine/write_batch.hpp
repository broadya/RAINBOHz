// SPDX-License-Identifier: Apache-2.0

#pragma once

// Batched writes to engine-db: many rows per statement, one transaction per
// flush.
//
// Executing a statement costs Ladybug far more than handling one row inside
// it: binding, physical planning and scheduling are paid per statement, and a
// MATCH by id per statement besides. Writing one node or one relationship per
// statement therefore spends almost all its time on overhead. A WriteBatch
// collects rows instead, and on flush() sends each table's rows as one
// statement over a list of records (@see ADR 0022).
//
// Rows are staged against operations registered up front, one per table (or
// per FROM/TO pair of a relationship table), and run in a fixed order that
// respects every reference between them:
//
//   1. delete statements, which clear earlier output;
//   2. node statements: inserts, which report the id each new node was
//      given, and create-if-missing statements;
//   3. relationship inserts, whose ends may be nodes staged in step 2.
//
// Two phases of writes are always enough because node properties never hold
// another node's id: every reference between rows is a relationship, which is
// a junction table in relational terms. A new node is referred to by a
// NodeHandle until its id is known.
//
// Rows are grouped in units, which are all-or-nothing. flush() first tries
// every unit in one transaction; if that fails, it rolls back and retries
// each unit in a transaction of its own, so that one bad unit costs only
// itself. Precomputation uses one unit per partial.

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rainbohz::rendering_engine {

/// A node staged in a WriteBatch, standing in for the id the database will
/// give it.
///
/// Valid only until the next flush(), and only within the unit that staged
/// it.
struct NodeHandle {
    std::size_t value = 0; ///< Index of the node among the batch's staged nodes.
};

/// One end of a staged relationship: a node staged in the same unit, or a
/// node already in the database, given by its primary key.
using EdgeEnd = std::variant<NodeHandle, std::int64_t>;

/// When a row statement runs within a flush.
enum class WritePhase : std::uint8_t {
    Delete, ///< Before any node is written.
    Node,   ///< With the node inserts, before any relationship.
};

/// Identifies an operation registered with a WriteBatch.
struct WriteOperation {
    std::size_t value = 0; ///< Index of the operation, in registration order.
};

/// Stages rows for engine-db and writes them in as few statements as the
/// tables allow.
///
/// Not thread-safe. Holds a reference to its EngineDb, which must outlive it.
class WriteBatch {
public:
    /// Creates an empty batch that will write to `db`.
    ///
    /// @param[in,out] db  Database every flush writes to. A reference is
    ///                    kept: it must outlive this object.
    explicit WriteBatch(EngineDb& db);

    /// Registers a statement run once per flush over every row staged for it.
    ///
    /// The statement must begin by unwinding the rows, `UNWIND $rows AS r`,
    /// and refers to a row's fields as `r.<column>`. It returns nothing that
    /// the batch reads.
    ///
    /// @param phase     When it runs: with the deletes, or with the node
    ///                  inserts. Within a phase, operations run in the order
    ///                  they were registered.
    /// @param cypher    The whole statement.
    /// @param columns   The row's fields, in the order stageRow() takes them.
    /// @param distinct  Whether identical rows are sent once. A
    ///                  create-if-missing (MERGE) statement needs this, since
    ///                  several units may stage the same key.
    /// @return The operation, for stageRow().
    WriteOperation addStatement(WritePhase phase, std::string cypher,
        std::vector<std::pair<std::string, DbColumnType>> columns, bool distinct = false);

    /// Registers an insert of nodes into one table.
    ///
    /// The batch runs `UNWIND $rows AS r <createClause> RETURN r.k, n.id`,
    /// so the clause must bind the new node as `n`, and `k` is reserved as a
    /// column name. Constant properties may be written into the clause.
    ///
    /// @param createClause  e.g. `CREATE (n:Segment {start: r.start})`.
    /// @param columns       The row's fields, in the order stageNode() takes
    ///                      them; may be empty.
    /// @return The operation, for stageNode().
    WriteOperation addNodeInsert(
        std::string_view createClause, std::vector<std::pair<std::string, DbColumnType>> columns);

    /// Registers an insert of relationships between one pair of node tables.
    ///
    /// The batch runs a `COPY` into `relTable` from
    /// `(UNWIND $rows AS r RETURN r.from, r.to)`, naming `fromTable` and
    /// `toTable` as the pair, which finds both ends by primary-key index.
    /// The relationship carries no properties.
    ///
    /// @param relTable   Relationship table.
    /// @param fromTable  Node table at its start.
    /// @param toTable    Node table at its end.
    /// @return The operation, for stageEdge().
    WriteOperation addEdgeInsert(
        std::string_view relTable, std::string_view fromTable, std::string_view toTable);

    /// Starts a new unit: every row staged until the next call belongs to it.
    ///
    /// @return The unit's index among those staged since the last flush,
    ///         which is its position in what flush() returns.
    std::size_t beginUnit();

    /// Stages a row for a statement registered by addStatement().
    ///
    /// @param operation  A statement operation of this batch.
    /// @param values     One value per column, of the column's type.
    /// @throws std::invalid_argument  If no unit has been begun, `operation`
    ///         is not a statement, or `values` has the wrong length.
    void stageRow(WriteOperation operation, std::vector<DbScalar> values);

    /// Stages a node for an insert registered by addNodeInsert().
    ///
    /// @param operation  A node insert of this batch.
    /// @param values     One value per column, of the column's type.
    /// @return The handle by which relationships staged in the same unit
    ///         refer to the node.
    /// @throws std::invalid_argument  If no unit has been begun, `operation`
    ///         is not a node insert, or `values` has the wrong length.
    NodeHandle stageNode(WriteOperation operation, std::vector<DbScalar> values);

    /// Stages a relationship for an insert registered by addEdgeInsert().
    ///
    /// @param operation  A relationship insert of this batch.
    /// @param from       Its start: a node staged in the current unit, or the
    ///                   primary key of a node in `fromTable`.
    /// @param to         Its end, likewise in `toTable`.
    /// @throws std::invalid_argument  If no unit has been begun, `operation`
    ///         is not a relationship insert, or a handle belongs to another
    ///         unit or to no staged node.
    void stageEdge(WriteOperation operation, EdgeEnd from, EdgeEnd to);

    /// Writes every staged row, then empties the batch.
    ///
    /// All units are tried in one transaction. If any statement fails, that
    /// transaction is rolled back and each unit is retried in a transaction
    /// of its own, so that every unit is either wholly written or not at all,
    /// and a failure in one leaves the others written. Handles are invalid
    /// afterwards; registered operations remain.
    ///
    /// @return One entry per unit, in the order they were begun: nullopt
    ///         when the unit was written, otherwise the error text. Every
    ///         engine-db failure is reported here, not thrown, including a
    ///         transaction that cannot be begun because the caller left one
    ///         open, which fails every unit.
    std::vector<std::optional<std::string>> flush();

    /// Counts the units begun since the last flush.
    ///
    /// @return How many units flush() would report on now.
    std::size_t unitCount() const noexcept;

private:
    /// What an operation is, and so how its rows are sent.
    enum class Kind : std::uint8_t {
        Statement,  ///< addStatement().
        NodeInsert, ///< addNodeInsert().
        EdgeInsert, ///< addEdgeInsert().
    };

    /// A registered operation and the rows staged for it.
    struct Operation {
        Kind kind = Kind::Statement;         ///< How the rows are sent.
        WritePhase phase = WritePhase::Node; ///< When a Statement runs.
        std::string cypher;                  ///< The whole statement, with `$rows`.
        std::vector<std::pair<std::string, DbColumnType>> columns; ///< Row fields, as sent.
        bool distinct = false;                          ///< Whether identical rows are sent once.
        std::vector<std::size_t> rowUnits;              ///< Each staged row's unit.
        std::vector<std::vector<DbScalar>> rows;        ///< Staged rows, without `k`.
        std::vector<std::size_t> rowHandles;            ///< NodeInsert: each row's handle.
        std::vector<std::pair<EdgeEnd, EdgeEnd>> edges; ///< EdgeInsert: staged ends.
    };

    /// Sends a statement operation's rows for the selected units, as one
    /// statement; identical rows once each when the operation is distinct.
    ///
    /// @param operation  A Statement operation.
    /// @param units      Mask over the staged units: true for those to write.
    /// @throws EngineDbError  If the statement fails.
    void runStatement(const Operation& operation, const std::vector<bool>& units);

    /// Inserts a node operation's rows for the selected units, as one
    /// statement, and records the id each node was given.
    ///
    /// @param operation  A NodeInsert operation.
    /// @param units      Mask over the staged units: true for those to write.
    /// @param[in,out] ids  Ids by handle: set for every node inserted.
    /// @throws EngineDbError  If the statement fails, or returns other than
    ///         one row per node sent.
    void runNodeInsert(const Operation& operation, const std::vector<bool>& units,
        std::vector<std::optional<std::int64_t>>& ids);

    /// Copies a relationship operation's rows for the selected units in, as
    /// one statement, with handles resolved to ids.
    ///
    /// @param operation  An EdgeInsert operation.
    /// @param units      Mask over the staged units: true for those to write.
    /// @param ids        Ids by handle, from runNodeInsert().
    /// @throws EngineDbError  If the statement fails, or an end is a handle
    ///         whose node was not inserted.
    void runEdgeInsert(const Operation& operation, const std::vector<bool>& units,
        const std::vector<std::optional<std::int64_t>>& ids);

    /// Writes the rows of the units in `units` in one transaction.
    ///
    /// @param units  Mask over the staged units: true for those to write.
    /// @throws EngineDbError  If any statement fails. The transaction has
    ///         been rolled back.
    void write(const std::vector<bool>& units);

    /// Checks that a row may be staged for `operation`.
    ///
    /// @param operation  The operation the row is for.
    /// @param kind       What it must be.
    /// @param values     The row's values: their count is checked.
    /// @return The operation.
    /// @throws std::invalid_argument  If no unit has been begun, the
    ///         operation does not exist or is of another kind, or the row has
    ///         the wrong length.
    Operation& checkedOperation(
        WriteOperation operation, Kind kind, const std::vector<DbScalar>& values);

    /// Checks that a relationship end may be staged in the current unit.
    ///
    /// @param end  A staged node or an existing key.
    /// @throws std::invalid_argument  If `end` is a handle that no staged
    ///         node has, or one staged in another unit.
    void checkEnd(const EdgeEnd& end) const;

    /// The database flushes write to. Never null.
    EngineDb* db_;
    /// Registered operations, in registration order.
    std::vector<Operation> operations_;
    /// Units begun since the last flush.
    std::size_t units_ = 0;
    /// The unit each staged node belongs to, by handle.
    std::vector<std::size_t> handleUnits_;
};

} // namespace rainbohz::rendering_engine
