// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/table_loader.hpp"

#include "rainbohz/rendering_engine/cypher_script.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief Loads a score's Parquet tables through COPY FROM, and chooses
/// between a manifest and a Cypher script.
///
/// @see table_loader.hpp, ADR 0025

namespace rainbohz::rendering_engine {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kFormat = "rainbohz-score-tables";
constexpr std::int64_t kVersion = 1;

/// A node table: its rows come from `file`, or, for a table with no columns,
/// are created by count.
struct NodeTable {
    std::string table;
    std::int64_t rows = 0;
    std::optional<std::filesystem::path> file;
    std::vector<std::string> columns;
};

/// One relationship (FROM, TO) pair of tables and the file of its rows.
struct RelTable {
    std::string table;
    std::string from;
    std::string to;
    std::filesystem::path file;
};

/// A manifest, checked: every name an identifier, every file in its directory.
struct Manifest {
    std::vector<std::pair<std::string, std::int64_t>> engineConfig;
    std::vector<NodeTable> nodes;
    std::vector<RelTable> relationships;
};

/// True for a name Cypher can take unquoted: a letter or underscore, then
/// letters, digits and underscores.
bool isIdentifier(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto letter = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    return letter(name.front()) &&
           std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

/// Reads and checks a manifest. Every problem is a ScriptLoadError naming
/// the manifest, thrown before anything is executed.
class ManifestReader {
public:
    explicit ManifestReader(std::filesystem::path manifest) : manifest_{std::move(manifest)} {}

    Manifest read() {
        std::error_code error;
        if (!std::filesystem::is_regular_file(manifest_, error)) {
            failToRead("not a readable file");
        }
        directory_ = std::filesystem::weakly_canonical(manifest_, error).parent_path();
        if (error) {
            failToRead("cannot resolve its directory: " + error.message());
        }

        const Json root = parse();
        if (!root.is_object()) {
            fail("is not a JSON object");
        }
        if (root.value("format", "") != kFormat) {
            fail("'format' is not \"" + std::string{kFormat} + "\"");
        }
        if (!root.contains("version") || !root["version"].is_number_integer() ||
            root["version"].get<std::int64_t>() != kVersion) {
            fail(
                "'version' is not " + std::to_string(kVersion) + ", the version this engine reads");
        }

        Manifest manifest;
        for (const auto& [key, value] :
            member(root, "engine_config", Json::value_t::object).items()) {
            if (!isIdentifier(key) || !value.is_number_integer()) {
                fail("engine_config." + key + " is not an identifier with an integer value");
            }
            manifest.engineConfig.emplace_back(key, value.get<std::int64_t>());
        }
        for (const Json& entry : member(root, "nodes", Json::value_t::array)) {
            manifest.nodes.push_back(readNode(entry));
        }
        for (const Json& entry : member(root, "relationships", Json::value_t::array)) {
            manifest.relationships.push_back(readRel(entry));
        }
        return manifest;
    }

private:
    [[noreturn]] void fail(const std::string& problem) const {
        throw ManifestError{manifest_.string() + ": " + problem};
    }

    [[noreturn]] void failToRead(const std::string& problem) const {
        throw ScriptLoadError{manifest_.string() + ": " + problem};
    }

    Json parse() const {
        std::ifstream stream{manifest_, std::ios::binary};
        if (!stream) {
            failToRead("could not open");
        }
        try {
            return Json::parse(stream);
        } catch (const Json::parse_error& e) {
            fail(std::string{"not valid JSON: "} + e.what());
        }
    }

    const Json& member(const Json& object, const std::string& key, Json::value_t type) const {
        const auto found = object.find(key);
        if (found == object.end() || found->type() != type) {
            fail("'" + key + "' is missing or not " +
                 (type == Json::value_t::array ? "an array" : "an object"));
        }
        return *found;
    }

    std::string identifier(
        const Json& entry, const std::string& key, const std::string& where) const {
        const auto found = entry.find(key);
        if (found == entry.end() || !found->is_string() ||
            !isIdentifier(found->get<std::string>())) {
            fail(where + ": '" + key + "' is missing or not an identifier");
        }
        return found->get<std::string>();
    }

    std::int64_t rowCount(const Json& entry, const std::string& where) const {
        const auto found = entry.find("rows");
        if (found == entry.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0) {
            fail(where + ": 'rows' is missing or not a count");
        }
        return found->get<std::int64_t>();
    }

    /// The data file `entry` names, which must be a plain `.parquet` name
    /// resolving to a regular file in the manifest's directory.
    std::filesystem::path dataFile(const Json& entry, const std::string& where) const {
        const auto found = entry.find("file");
        if (found == entry.end() || !found->is_string()) {
            fail(where + ": 'file' is missing or not a string");
        }
        const std::string name = found->get<std::string>();
        const std::filesystem::path plain{name};
        if (name.empty() || plain.has_parent_path() || plain.filename() != plain ||
            plain.extension() != ".parquet" || name.find('\\') != std::string::npos) {
            fail(where + ": file '" + name +
                 "' is not a plain .parquet file name in the manifest's directory");
        }
        std::error_code error;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(directory_ / plain, error);
        if (error || resolved.parent_path() != directory_) {
            fail(where + ": file '" + name + "' resolves outside the manifest's directory");
        }
        if (!std::filesystem::is_regular_file(resolved, error)) {
            fail(where + ": file '" + name + "' is missing");
        }
        // The path is quoted in a COPY statement, which has no escape for these.
        const std::string text = resolved.string();
        if (text.find('\'') != std::string::npos || text.find('\\') != std::string::npos) {
            fail(where + ": the path " + text +
                 " holds a quote or backslash, which COPY cannot name");
        }
        return resolved;
    }

    NodeTable readNode(const Json& entry) const {
        if (!entry.is_object()) {
            fail("a 'nodes' entry is not an object");
        }
        NodeTable node;
        node.table = identifier(entry, "table", "a 'nodes' entry");
        const std::string where = "node table " + node.table;
        node.rows = rowCount(entry, where);
        if (!entry.contains("file")) {
            if (entry.contains("columns")) {
                fail(where + ": has 'columns' but no 'file'");
            }
            return node;
        }
        node.file = dataFile(entry, where);
        const auto columns = entry.find("columns");
        if (columns == entry.end() || !columns->is_array() || columns->empty()) {
            fail(where + ": 'columns' is missing, not an array, or empty");
        }
        for (const Json& column : *columns) {
            if (!column.is_string() || !isIdentifier(column.get<std::string>())) {
                fail(where + ": a column is not an identifier");
            }
            node.columns.push_back(column.get<std::string>());
        }
        return node;
    }

    RelTable readRel(const Json& entry) const {
        if (!entry.is_object()) {
            fail("a 'relationships' entry is not an object");
        }
        RelTable rel;
        rel.table = identifier(entry, "table", "a 'relationships' entry");
        const std::string where = "relationship " + rel.table;
        rel.from = identifier(entry, "from", where);
        rel.to = identifier(entry, "to", where);
        rowCount(entry, where);
        rel.file = dataFile(entry, where);
        return rel;
    }

    std::filesystem::path manifest_;
    std::filesystem::path directory_;
};

/// Runs a statement and records its outcome, as loadCypherFile() does.
class Runner {
public:
    Runner(EngineDb& db, const StatementObserver& observe, LoadReport& report)
        : db_{&db}, observe_{&observe}, report_{&report} {}

    /// Runs `text`, attributing it to `file`. Returns whether it applied.
    bool run(const std::filesystem::path& file, std::string text) {
        const CypherStatement statement{.text = std::move(text), .startLine = 0, .endLine = 0};
        const ExecuteOutcome outcome = db_->tryExecute(statement.text);
        record(file, statement, outcome);
        return outcome.ok;
    }

    /// Records a failure the loader found itself, as a statement that failed.
    void refuse(const std::filesystem::path& file, std::string text, std::string message) {
        const CypherStatement statement{.text = std::move(text), .startLine = 0, .endLine = 0};
        record(file, statement, ExecuteOutcome{.ok = false, .message = std::move(message)});
    }

    /// How many rows node table `table` holds; nullopt, recorded as a
    /// failure, if the count cannot be read.
    std::optional<std::int64_t> count(const std::filesystem::path& file, const std::string& table) {
        const std::string text = "MATCH (n:" + table + ") RETURN count(n);";
        try {
            const std::vector<DbRow> rows = db_->query(text);
            if (rows.size() == 1 && rows.front().size() == 1 &&
                std::holds_alternative<std::int64_t>(rows.front().front())) {
                return std::get<std::int64_t>(rows.front().front());
            }
            refuse(file, text, "the count returned no single integer");
        } catch (const EngineDbError& e) {
            refuse(file, text, e.what());
        }
        return std::nullopt;
    }

private:
    void record(const std::filesystem::path& file, const CypherStatement& statement,
        const ExecuteOutcome& outcome) {
        if (*observe_) {
            (*observe_)(file, statement, outcome);
        }
        if (outcome.ok) {
            ++report_->applied;
        } else if (outcome.alreadyExists) {
            ++report_->alreadyPresent;
        } else {
            report_->failures.push_back(ScriptFailure{.file = file,
                .startLine = 0,
                .endLine = 0,
                .label = statement.label(),
                .message = outcome.message});
        }
    }

    EngineDb* db_;
    const StatementObserver* observe_;
    LoadReport* report_;
};

std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

/// Loads one node table: checks it is empty, copies or creates its rows,
/// and checks the count.
void loadNodeTable(Runner& runner, const std::filesystem::path& manifest, const NodeTable& node) {
    const std::filesystem::path& file = node.file.value_or(manifest);
    const std::optional<std::int64_t> before = runner.count(file, node.table);
    if (!before.has_value()) {
        return;
    }
    if (*before != 0) {
        runner.refuse(file, "COPY " + node.table,
            node.table + " already holds " + std::to_string(*before) +
                " row(s). Its ids would not be the row indices the relationships name, so "
                "nothing was copied: a score's tables load into a fresh engine-db only.");
        return;
    }
    if (node.rows == 0) {
        return;
    }
    const std::string statement = node.file.has_value()
                                      ? "COPY " + node.table + "(" + joined(node.columns) +
                                            ") FROM '" + node.file->string() + "';"
                                      : "UNWIND range(1, " + std::to_string(node.rows) +
                                            ") AS r CREATE (n:" + node.table + ");";
    if (!runner.run(file, statement)) {
        return;
    }
    const std::optional<std::int64_t> after = runner.count(file, node.table);
    if (after.has_value() && *after != node.rows) {
        runner.refuse(file, "COPY " + node.table,
            node.table + " holds " + std::to_string(*after) +
                " row(s) after the copy; the "
                "manifest lists " +
                std::to_string(node.rows));
    }
}

} // namespace

LoadReport loadTableManifest(
    EngineDb& db, const std::filesystem::path& manifest, const StatementObserver& observe) {
    const Manifest tables = ManifestReader{manifest}.read();

    LoadReport report;
    Runner runner{db, observe, report};
    if (!tables.engineConfig.empty()) {
        std::string properties;
        for (const auto& [key, value] : tables.engineConfig) {
            properties += (properties.empty() ? "" : ", ") + key + ": " + std::to_string(value);
        }
        runner.run(manifest, "CREATE (n:EngineConfig {" + properties + "});");
    }
    for (const NodeTable& node : tables.nodes) {
        loadNodeTable(runner, manifest, node);
    }
    for (const RelTable& rel : tables.relationships) {
        runner.run(rel.file, "COPY " + rel.table + " FROM '" + rel.file.string() + "' (from='" +
                                 rel.from + "', to='" + rel.to + "');");
    }
    return report;
}

LoadReport loadStateFile(
    EngineDb& db, const std::filesystem::path& file, const StatementObserver& observe) {
    const std::filesystem::path extension = file.extension();
    if (extension == ".json") {
        return loadTableManifest(db, file, observe);
    }
    if (extension == ".cypher") {
        return loadCypherFile(db, file, observe);
    }
    throw ScriptLoadError{
        file.string() + ": a score's state is a .json table manifest or a .cypher script"};
}

} // namespace rainbohz::rendering_engine
