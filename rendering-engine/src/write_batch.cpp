// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/write_batch.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief Stages rows for engine-db and writes each table's rows as one
/// statement.
///
/// A flush runs the delete statements, then the node statements and inserts,
/// then the relationship inserts, each over the rows of the units being
/// written. Node inserts report the id each staged node was given, keyed by
/// its handle, and relationship ends that are handles are resolved through
/// those ids before the relationships are copied in.
///
/// @see write_batch.hpp, ADR 0022

namespace rainbohz::rendering_engine {

namespace {

/// The row field that carries a staged node's handle through an insert.
constexpr std::string_view kHandleColumn = "k";

} // namespace

WriteBatch::WriteBatch(EngineDb& db) : db_{&db} {}

WriteOperation WriteBatch::addStatement(WritePhase phase, std::string cypher,
    std::vector<std::pair<std::string, DbColumnType>> columns, bool distinct) {
    Operation operation;
    operation.kind = Kind::Statement;
    operation.phase = phase;
    operation.cypher = std::move(cypher);
    operation.columns = std::move(columns);
    operation.distinct = distinct;
    operations_.push_back(std::move(operation));
    return WriteOperation{operations_.size() - 1};
}

WriteOperation WriteBatch::addDeleteById(std::string_view table) {
    return addStatement(WritePhase::Delete,
        "UNWIND $rows AS r MATCH (n:" + std::string{table} + " {id: r.id}) DETACH DELETE n;",
        {{"id", DbColumnType::Int64}});
}

WriteOperation WriteBatch::addNodeInsert(
    std::string_view createClause, std::vector<std::pair<std::string, DbColumnType>> columns) {
    Operation operation;
    operation.kind = Kind::NodeInsert;
    operation.cypher = "UNWIND $rows AS r " + std::string{createClause} + " RETURN r." +
                       std::string{kHandleColumn} + ", n.id;";
    operation.columns.emplace_back(std::string{kHandleColumn}, DbColumnType::Int64);
    for (auto& column : columns) {
        if (column.first == kHandleColumn) {
            throw std::invalid_argument{"column name k is reserved by WriteBatch"};
        }
        operation.columns.push_back(std::move(column));
    }
    operations_.push_back(std::move(operation));
    return WriteOperation{operations_.size() - 1};
}

WriteOperation WriteBatch::addEdgeInsert(
    std::string_view relTable, std::string_view fromTable, std::string_view toTable) {
    Operation operation;
    operation.kind = Kind::EdgeInsert;
    operation.cypher = "COPY " + std::string{relTable} +
                       " FROM (UNWIND $rows AS r RETURN r.from, r.to) (from='" +
                       std::string{fromTable} + "', to='" + std::string{toTable} + "');";
    operation.columns = {{"from", DbColumnType::Int64}, {"to", DbColumnType::Int64}};
    operations_.push_back(std::move(operation));
    return WriteOperation{operations_.size() - 1};
}

std::size_t WriteBatch::beginUnit() {
    return units_++;
}

WriteBatch::Operation& WriteBatch::checkedOperation(
    WriteOperation operation, Kind kind, const std::vector<DbScalar>& values) {
    if (units_ == 0) {
        throw std::invalid_argument{"WriteBatch: a row was staged before any unit was begun"};
    }
    if (operation.value >= operations_.size() || operations_[operation.value].kind != kind) {
        throw std::invalid_argument{"WriteBatch: a row was staged for the wrong operation"};
    }
    Operation& found = operations_[operation.value];
    // A node insert's first column is the handle, which the batch supplies.
    const std::size_t expected =
        found.columns.size() - (kind == Kind::NodeInsert ? std::size_t{1} : std::size_t{0});
    if (values.size() != expected) {
        throw std::invalid_argument{"WriteBatch: a row has " + std::to_string(values.size()) +
                                    " values for " + std::to_string(expected) + " columns"};
    }
    return found;
}

void WriteBatch::stageRow(WriteOperation operation, std::vector<DbScalar> values) {
    Operation& found = checkedOperation(operation, Kind::Statement, values);
    found.rowUnits.push_back(units_ - 1);
    found.rows.push_back(std::move(values));
}

NodeHandle WriteBatch::stageNode(WriteOperation operation, std::vector<DbScalar> values) {
    Operation& found = checkedOperation(operation, Kind::NodeInsert, values);
    const NodeHandle handle{handleUnits_.size()};
    handleUnits_.push_back(units_ - 1);
    found.rowUnits.push_back(units_ - 1);
    found.rows.push_back(std::move(values));
    found.rowHandles.push_back(handle.value);
    return handle;
}

void WriteBatch::checkEnd(const EdgeEnd& end) const {
    if (const auto* handle = std::get_if<NodeHandle>(&end)) {
        if (handle->value >= handleUnits_.size()) {
            throw std::invalid_argument{"WriteBatch: a relationship refers to an unknown node"};
        }
        if (handleUnits_[handle->value] != units_ - 1) {
            throw std::invalid_argument{
                "WriteBatch: a relationship refers to a node staged in another unit"};
        }
    }
}

void WriteBatch::stageEdge(WriteOperation operation, EdgeEnd from, EdgeEnd to) {
    Operation& found = checkedOperation(operation, Kind::EdgeInsert, {DbScalar{}, DbScalar{}});
    checkEnd(from);
    checkEnd(to);
    found.rowUnits.push_back(units_ - 1);
    found.edges.emplace_back(from, to);
}

void WriteBatch::runStatement(const Operation& operation, const std::vector<bool>& units) {
    DbRecordList records{.columns = operation.columns, .rows = {}};
    std::set<std::vector<DbScalar>> seen;
    for (std::size_t i = 0; i < operation.rows.size(); ++i) {
        const bool wanted = units[operation.rowUnits[i]] &&
                            (!operation.distinct || seen.insert(operation.rows[i]).second);
        if (wanted) {
            records.rows.push_back(operation.rows[i]);
        }
    }
    if (!records.rows.empty()) {
        db_->query(operation.cypher, {{"rows", std::move(records)}});
    }
}

void WriteBatch::runNodeInsert(const Operation& operation, const std::vector<bool>& units,
    std::vector<std::optional<std::int64_t>>& ids) {
    DbRecordList records{.columns = operation.columns, .rows = {}};
    for (std::size_t i = 0; i < operation.rows.size(); ++i) {
        if (!units[operation.rowUnits[i]]) {
            continue;
        }
        std::vector<DbScalar> row;
        row.reserve(operation.columns.size());
        row.emplace_back(static_cast<std::int64_t>(operation.rowHandles[i]));
        row.insert(row.end(), operation.rows[i].begin(), operation.rows[i].end());
        records.rows.push_back(std::move(row));
    }
    if (records.rows.empty()) {
        return;
    }
    const std::size_t sent = records.rows.size();
    const std::vector<DbRow> created = db_->query(operation.cypher, {{"rows", std::move(records)}});
    if (created.size() != sent) {
        throw EngineDbError{"inserting " + std::to_string(sent) + " nodes returned " +
                            std::to_string(created.size()) + " rows"};
    }
    for (const DbRow& row : created) {
        ids.at(static_cast<std::size_t>(requireInt64(row.at(0)))) = requireInt64(row.at(1));
    }
}

void WriteBatch::runEdgeInsert(const Operation& operation, const std::vector<bool>& units,
    const std::vector<std::optional<std::int64_t>>& ids) {
    const auto resolve = [&](const EdgeEnd& end) -> std::int64_t {
        if (const auto* key = std::get_if<std::int64_t>(&end)) {
            return *key;
        }
        const std::optional<std::int64_t>& id = ids.at(std::get<NodeHandle>(end).value);
        if (!id.has_value()) {
            throw EngineDbError{"a relationship refers to a staged node that was not written"};
        }
        return *id;
    };
    DbRecordList records{.columns = operation.columns, .rows = {}};
    for (std::size_t i = 0; i < operation.edges.size(); ++i) {
        if (units[operation.rowUnits[i]]) {
            records.rows.push_back(
                {resolve(operation.edges[i].first), resolve(operation.edges[i].second)});
        }
    }
    if (!records.rows.empty()) {
        db_->query(operation.cypher, {{"rows", std::move(records)}});
    }
}

void WriteBatch::write(const std::vector<bool>& units) {
    Transaction transaction{*db_};
    std::vector<std::optional<std::int64_t>> ids(handleUnits_.size());
    for (const Operation& operation : operations_) {
        if (operation.kind == Kind::Statement && operation.phase == WritePhase::Delete) {
            runStatement(operation, units);
        }
    }
    for (const Operation& operation : operations_) {
        if (operation.kind == Kind::Statement && operation.phase == WritePhase::Node) {
            runStatement(operation, units);
        } else if (operation.kind == Kind::NodeInsert) {
            runNodeInsert(operation, units, ids);
        }
    }
    for (const Operation& operation : operations_) {
        if (operation.kind == Kind::EdgeInsert) {
            runEdgeInsert(operation, units, ids);
        }
    }
    transaction.commit();
}

std::vector<std::optional<std::string>> WriteBatch::flush() {
    std::vector<std::optional<std::string>> results(units_);
    if (units_ > 0) {
        try {
            write(std::vector<bool>(units_, true));
        } catch (const EngineDbError& batched) {
            if (units_ == 1) {
                results.front() = batched.what();
            } else {
                // Find the failing units by writing each on its own; the
                // others are then written as they would have been.
                for (std::size_t unit = 0; unit < units_; ++unit) {
                    std::vector<bool> only(units_, false);
                    only[unit] = true;
                    try {
                        write(only);
                    } catch (const EngineDbError& alone) {
                        results[unit] = alone.what();
                    }
                }
            }
        }
    }

    for (Operation& operation : operations_) {
        operation.rowUnits.clear();
        operation.rows.clear();
        operation.rowHandles.clear();
        operation.edges.clear();
    }
    units_ = 0;
    handleUnits_.clear();
    return results;
}

std::size_t WriteBatch::unitCount() const noexcept {
    return units_;
}

} // namespace rainbohz::rendering_engine
