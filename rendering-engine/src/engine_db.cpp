// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <main/lbug.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief The Ladybug-backed implementation of EngineDb and Transaction.
///
/// This is the only translation unit that includes Ladybug headers. It opens
/// the embedded database and its connection, converts values between
/// Ladybug's types and DbValue / DbParameter, and turns Ladybug's result-object
/// error reporting into EngineDbError exceptions or ExecuteOutcome values. A
/// declaration that is "already in the catalog" is reported as such rather than
/// as a failure, so re-applying a schema is idempotent.
///
/// Also implements catalog introspection (tables(), census(), labelNames()),
/// parameterised queries with a cache of prepared statements, explicit
/// transactions, the RAII Transaction guard that rolls back unless committed,
/// and the typed readers for DbValue. Values cross the interface as raw
/// representations; wrapping them in unit types is the caller's job.
///
/// @see engine_db.hpp

namespace rainbohz::rendering_engine {

namespace {

// Ladybug treats an empty path and ":memory:" identically, but being explicit
// here keeps the in-memory default visible at the call site rather than
// depending on a library convention.
constexpr std::string_view kInMemoryPath = ":memory:";

// How Ladybug reports a declaration that is already in the catalog. Matched
// case-insensitively, as ALREADY_EXISTS_RE in scripts/ladybug_ddl.py is.
constexpr std::string_view kAlreadyExistsMarker = "already exists in catalog";

/// Tests whether one string occurs within another, ignoring ASCII case.
///
/// Characters are compared after std::toupper in the current C locale, with
/// the cast to unsigned char that std::toupper requires for bytes of UTF-8
/// multibyte sequences.
///
/// @param haystack  Text to search.
/// @param needle    Text to search for.
/// @return True when `needle` occurs as a contiguous run in `haystack`.
///         False when `needle` is empty, since std::ranges::search then
///         returns an empty match; the only needle passed here is
///         kAlreadyExistsMarker, which is not.
bool containsIgnoringCase(std::string_view haystack, std::string_view needle) {
    const auto found = std::ranges::search(haystack, needle, [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) ==
               std::toupper(static_cast<unsigned char>(b));
    });
    return !found.empty();
}

/// Converts one value of a Ladybug result row to a DbValue.
///
/// @param value  A value read from a Ladybug FlatTuple.
/// @return The value, narrowed as DbValue documents: integers widened to
///         int64, FLOAT widened to double, NULL as std::monostate.
/// @throws EngineDbError  If the column type is one DbValue does not
///         represent (dates, lists, nodes, and so on). The message names the
///         type.
DbValue toDbValue(const lbug::common::Value& value) {
    if (value.isNull()) {
        return std::monostate{};
    }
    using lbug::common::LogicalTypeID;
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::BOOL:
        return value.getValue<bool>();
    case LogicalTypeID::INT8:
        return static_cast<std::int64_t>(value.getValue<std::int8_t>());
    case LogicalTypeID::INT16:
        return static_cast<std::int64_t>(value.getValue<std::int16_t>());
    case LogicalTypeID::INT32:
        return static_cast<std::int64_t>(value.getValue<std::int32_t>());
    case LogicalTypeID::INT64:
    case LogicalTypeID::SERIAL:
        return value.getValue<std::int64_t>();
    case LogicalTypeID::FLOAT:
        return static_cast<double>(value.getValue<float>());
    case LogicalTypeID::DOUBLE:
        return value.getValue<double>();
    case LogicalTypeID::STRING:
        return value.getValue<std::string>();
    default:
        throw EngineDbError{"query returned a column of type " + value.getDataType().toString() +
                            ", which engine-db's query interface does not represent"};
    }
}

/// Converts a DbParameter to the Ladybug value bound to a `$name`.
///
/// @param parameter  The parameter's value.
/// @return A new Ladybug value of the matching type: BOOL, INT64, DOUBLE,
///         STRING, or a LIST of INT64.
std::unique_ptr<lbug::common::Value> toLadybugValue(const DbParameter& parameter) {
    return std::visit(
        [](const auto& v) -> std::unique_ptr<lbug::common::Value> {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::vector<std::int64_t>>) {
                using lbug::common::LogicalType;
                std::vector<std::unique_ptr<lbug::common::Value>> children;
                children.reserve(v.size());
                for (const std::int64_t id : v) {
                    children.push_back(std::make_unique<lbug::common::Value>(id));
                }
                return std::make_unique<lbug::common::Value>(
                    LogicalType::LIST(LogicalType::INT64()), std::move(children));
            } else {
                return std::make_unique<lbug::common::Value>(v);
            }
        },
        parameter);
}

/// Describes what a DbValue holds, for error messages.
///
/// @param value  Any DbValue.
/// @return "NULL", "a boolean", "an integer", "a floating-point number" or "a
///         string".
std::string describe(const DbValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "NULL";
            } else if constexpr (std::is_same_v<T, bool>) {
                return "a boolean";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return "an integer";
            } else if constexpr (std::is_same_v<T, double>) {
                return "a floating-point number";
            } else {
                return "a string";
            }
        },
        value);
}

/// Reads a nullable value of one alternative of DbValue.
///
/// @tparam T     The alternative expected.
/// @param value  One value from a DbRow.
/// @param what   Name of the expected type, for the error message.
/// @return The value, or nullopt if `value` is NULL.
/// @throws EngineDbError  If `value` holds a different, non-NULL alternative.
template <typename T>
std::optional<T> optionalOf(const DbValue& value, std::string_view what) {
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    if (const T* held = std::get_if<T>(&value)) {
        return *held;
    }
    throw EngineDbError{"expected " + std::string{what} + " or NULL, found " + describe(value)};
}

/// Executes a statement that takes no parameters and returns nothing useful.
///
/// @param connection  Connection to run it on.
/// @param cypher      The statement.
/// @throws EngineDbError  If Ladybug rejects it.
void executeOrThrow(lbug::main::Connection& connection, const std::string& cypher) {
    auto result = connection.query(cypher);
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }
}

} // namespace

std::optional<std::int64_t> optionalInt64(const DbValue& value) {
    return optionalOf<std::int64_t>(value, "an integer");
}

std::optional<double> optionalDouble(const DbValue& value) {
    return optionalOf<double>(value, "a floating-point number");
}

std::optional<bool> optionalBool(const DbValue& value) {
    return optionalOf<bool>(value, "a boolean");
}

std::optional<std::string> optionalString(const DbValue& value) {
    return optionalOf<std::string>(value, "a string");
}

std::int64_t requireInt64(const DbValue& value) {
    const std::optional<std::int64_t> held = optionalInt64(value);
    if (!held.has_value()) {
        throw EngineDbError{"expected an integer, found NULL"};
    }
    return *held;
}

EngineDb::EngineDb() : EngineDb(kInMemoryPath) {}

EngineDb::EngineDb(std::string_view path)
    : path_{path.empty() ? std::string{kInMemoryPath} : std::string{path}} {
    try {
        database_ = std::make_unique<lbug::main::Database>(path_);
        connection_ = std::make_unique<lbug::main::Connection>(database_.get());
    } catch (const std::exception& e) {
        throw EngineDbError{std::string{"failed to open engine-db at "} + path_ + ": " + e.what()};
    }
}

// Out of line so that the header never needs Ladybug's definitions in order to
// destroy the unique_ptr members.
EngineDb::~EngineDb() = default;

void EngineDb::execute(std::string_view cypher) {
    auto result = connection_->query(std::string{cypher});
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }
}

ExecuteOutcome EngineDb::tryExecute(std::string_view cypher) {
    // Ladybug reports a rejected statement through the result rather than by
    // throwing, but a statement that fails to parse at all can still throw,
    // and this function promises not to.
    try {
        auto result = connection_->query(std::string{cypher});
        if (result->isSuccess()) {
            return ExecuteOutcome{.ok = true, .message = {}, .alreadyExists = false};
        }
        std::string message = result->getErrorMessage();
        const bool alreadyExists = containsIgnoringCase(message, kAlreadyExistsMarker);
        return ExecuteOutcome{
            .ok = false, .message = std::move(message), .alreadyExists = alreadyExists};
    } catch (const std::exception& e) {
        return ExecuteOutcome{.ok = false, .message = e.what(), .alreadyExists = false};
    }
}

std::vector<TableEntry> EngineDb::tables() {
    auto result = connection_->query(std::string{"CALL SHOW_TABLES() RETURN name, type;"});
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }

    std::vector<TableEntry> entries;
    while (result->hasNext()) {
        const auto row = result->getNext();
        entries.emplace_back(
            row->getValue(0)->getValue<std::string>(), row->getValue(1)->getValue<std::string>());
    }
    std::ranges::sort(entries);
    return entries;
}

Census EngineDb::census() {
    // An unlabelled pattern matches every table of that kind, and yields no
    // rows at all when the catalog holds none -- so this is safe against a
    // database whose schema has not been applied yet. `label()` names the
    // table a row came from, which is what turns one scan into a per-table
    // count.
    const auto tally = [this](std::string_view cypher, std::vector<RowCount>& into) {
        auto result = connection_->query(std::string{cypher});
        if (!result->isSuccess()) {
            throw EngineDbError{result->getErrorMessage()};
        }
        std::int64_t total = 0;
        while (result->hasNext()) {
            const auto row = result->getNext();
            const auto count = row->getValue(1)->getValue<std::int64_t>();
            into.emplace_back(row->getValue(0)->getValue<std::string>(), count);
            total += count;
        }
        std::ranges::sort(into);
        return total;
    };

    Census census;
    census.totalNodes = tally("MATCH (n) RETURN label(n), count(*);", census.nodes);
    census.totalRelationships =
        tally("MATCH ()-[r]->() RETURN label(r), count(*);", census.relationships);
    return census;
}

std::set<std::string> EngineDb::labelNames() {
    auto result = connection_->query(std::string{"MATCH (l:Label) RETURN l.name;"});
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }

    std::set<std::string> names;
    while (result->hasNext()) {
        const auto row = result->getNext();
        // A Label with no name cannot be selected by one, so it is skipped
        // rather than entered as an empty string.
        if (!row->getValue(0)->isNull()) {
            names.insert(row->getValue(0)->getValue<std::string>());
        }
    }
    return names;
}

std::vector<DbRow> EngineDb::query(std::string_view cypher, const DbParameters& parameters) {
    std::string key{cypher};
    auto found = prepared_.find(key);
    if (found == prepared_.end()) {
        auto statement = connection_->prepare(key);
        if (!statement->isSuccess()) {
            // Not cached: a statement that failed to prepare is reported
            // again, the same way, if it is ever retried.
            throw EngineDbError{statement->getErrorMessage()};
        }
        found = prepared_.emplace(std::move(key), std::move(statement)).first;
    }

    std::unordered_map<std::string, std::unique_ptr<lbug::common::Value>> bound;
    for (const auto& [name, value] : parameters) {
        bound.insert_or_assign(name, toLadybugValue(value));
    }

    auto result = connection_->executeWithParams(found->second.get(), std::move(bound));
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }

    std::vector<DbRow> rows;
    const auto columns = result->getNumColumns();
    while (result->hasNext()) {
        const auto tuple = result->getNext();
        DbRow row;
        row.reserve(columns);
        // Ladybug's column index type is narrower than size_t; a result never
        // has more columns than it can index.
        for (lbug::common::idx_t column = 0; column < columns; ++column) {
            row.push_back(toDbValue(*tuple->getValue(column)));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void EngineDb::beginTransaction() {
    executeOrThrow(*connection_, "BEGIN TRANSACTION;");
}

void EngineDb::commit() {
    executeOrThrow(*connection_, "COMMIT;");
}

void EngineDb::rollback() {
    executeOrThrow(*connection_, "ROLLBACK;");
}

const std::string& EngineDb::path() const noexcept {
    return path_;
}

std::string EngineDb::queryToString(std::string_view cypher) {
    auto result = connection_->query(std::string{cypher});
    if (!result->isSuccess()) {
        throw EngineDbError{result->getErrorMessage()};
    }
    return result->toString();
}

std::string EngineDb::ladybugVersion() {
    // Not lbug::main::Version::getVersion(): upstream defines that from
    // CMAKE_PROJECT_VERSION, which in this build is rendering-engine's own
    // version rather than Ladybug's. The build system supplies the real one.
    // See cmake/Dependencies.cmake.
    return RE_LADYBUG_VERSION;
}

Transaction::Transaction(EngineDb& db) : db_{&db} {
    db_->beginTransaction();
}

Transaction::~Transaction() {
    if (committed_) {
        return;
    }
    try {
        db_->rollback();
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Ladybug rolls back on its own when a statement inside the
        // transaction fails, and then refuses an explicit ROLLBACK. Either
        // way nothing was committed, which is all this destructor promises.
    }
}

void Transaction::commit() {
    db_->commit();
    committed_ = true;
}

} // namespace rainbohz::rendering_engine
