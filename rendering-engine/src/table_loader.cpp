// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/table_loader.hpp"

#include "rainbohz/rendering_engine/cypher_script.hpp"
#include "rainbohz/rendering_engine/score_manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief Loads a score's Parquet tables through COPY FROM, and chooses
/// between a manifest and a Cypher script.
///
/// The manifest itself is read and checked by score_manifest.cpp, which the
/// native backend's loader shares.
///
/// @see table_loader.hpp, ADR 0025

namespace rainbohz::rendering_engine {

namespace {

/// Runs a statement and records its outcome, as loadCypherFile() does.
class Runner {
public:
    /// A runner over one database, recording into one report.
    ///
    /// @param[in,out] db      The database the statements run against.
    /// @param observe         Called once per statement; may be empty.
    /// @param[in,out] report  Gains one count or failure per statement.
    Runner(EngineDb& db, const StatementObserver& observe, LoadReport& report)
        : db_{&db}, observe_{&observe}, report_{&report} {}

    /// Runs `text`, attributing it to `file`.
    ///
    /// @param file  The file the statement reads, for the report.
    /// @param text  The statement.
    /// @return Whether it applied.
    bool run(const std::filesystem::path& file, std::string text) {
        const CypherStatement statement{.text = std::move(text), .startLine = 0, .endLine = 0};
        const ExecuteOutcome outcome = db_->tryExecute(statement.text);
        record(file, statement, outcome);
        return outcome.ok;
    }

    /// Records a failure the loader found itself, as a statement that failed.
    ///
    /// @param file     The file it concerns.
    /// @param text     The statement it stands for.
    /// @param message  What is wrong.
    void refuse(const std::filesystem::path& file, std::string text, std::string message) {
        const CypherStatement statement{.text = std::move(text), .startLine = 0, .endLine = 0};
        record(file, statement, ExecuteOutcome{.ok = false, .message = std::move(message)});
    }

    /// How many rows a node table holds.
    ///
    /// @param file   The file it concerns, for a failure.
    /// @param table  The node table.
    /// @return The count; nullopt, recorded as a failure, if it cannot be
    ///         read.
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
    /// Tells the observer, and counts the outcome in the report.
    ///
    /// @param file       The file the statement concerns.
    /// @param statement  The statement.
    /// @param outcome    How it went.
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

    EngineDb* db_;                     ///< The database; not owned.
    const StatementObserver* observe_; ///< The observer; not owned.
    LoadReport* report_;               ///< The report; not owned.
};

/// Joins names with ", ".
///
/// @param names  The names.
/// @return e.g. "a, b, c".
std::string joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

/// Refuses a data file whose path a COPY statement cannot quote: it has no
/// escape for a quote or a backslash.
///
/// @param manifest  The manifest, for the message.
/// @param file      The resolved data file.
/// @throws ManifestError  If the path holds either.
void requireQuotable(const std::filesystem::path& manifest, const std::filesystem::path& file) {
    const std::string text = file.string();
    if (text.find('\'') != std::string::npos || text.find('\\') != std::string::npos) {
        throw ManifestError{manifest.string() + ": the path " + text +
                            " holds a quote or backslash, which COPY cannot name"};
    }
}

/// Loads one node table: checks it is empty, copies or creates its rows,
/// and checks the count.
///
/// @param[in,out] runner  Runs the statements and records their outcomes.
/// @param manifest        The manifest, named for a table with no file.
/// @param node            The table.
void loadNodeTable(
    Runner& runner, const std::filesystem::path& manifest, const ManifestNodeTable& node) {
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
    const ScoreManifest tables = readScoreManifest(manifest);
    for (const ManifestNodeTable& node : tables.nodes) {
        if (node.file.has_value()) {
            requireQuotable(manifest, *node.file);
        }
    }
    for (const ManifestRelationship& rel : tables.relationships) {
        requireQuotable(manifest, rel.file);
    }

    LoadReport report;
    Runner runner{db, observe, report};
    if (!tables.engineConfig.empty()) {
        std::string properties;
        for (const auto& [key, value] : tables.engineConfig) {
            properties += (properties.empty() ? "" : ", ") + key + ": " + std::to_string(value);
        }
        runner.run(manifest, "CREATE (n:EngineConfig {" + properties + "});");
    }
    for (const ManifestNodeTable& node : tables.nodes) {
        loadNodeTable(runner, manifest, node);
    }
    for (const ManifestRelationship& rel : tables.relationships) {
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
