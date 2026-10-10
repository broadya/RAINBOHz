// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/script_loader.hpp"

#include "rainbohz/rendering_engine/cypher_script.hpp"

#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>

/// @file
/// @brief Applies a Cypher script file to engine-db, one statement at a time.
///
/// Implements loadCypherFile() and the report types ScriptFailure and
/// LoadReport. The file is read whole before anything is executed, so a file
/// that cannot be read leaves the database untouched. It is then split by
/// splitStatements() and every statement is attempted. Failures are recorded
/// with their file and line span, and declarations already in the catalog are
/// counted separately. Schema DDL and initial database state both load through
/// here. Nothing is printed: reporting is the caller's business, optionally
/// per statement through a StatementObserver.
///
/// @see script_loader.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Reads a whole script file into memory.
///
/// Reading everything up front is what lets loadCypherFile() promise that a
/// bad file leaves the database untouched. The file is read in binary mode, so
/// line endings reach splitStatements() unchanged and it counts lines itself.
///
/// @param file  Script to read. Symlinks are followed.
/// @return The file's complete contents, byte for byte.
/// @throws ScriptLoadError  If `file` does not exist, is not a regular file
///         (a directory, say), or its status cannot be determined; if it
///         cannot be opened; or if reading it fails part-way. The message
///         names the file.
std::string readFile(const std::filesystem::path& file) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        throw ScriptLoadError{"not a readable file: " + file.string()};
    }

    const std::ifstream stream{file, std::ios::binary};
    if (!stream) {
        throw ScriptLoadError{"could not open: " + file.string()};
    }

    std::ostringstream contents;
    contents << stream.rdbuf();
    if (stream.bad()) {
        throw ScriptLoadError{"could not read: " + file.string()};
    }
    return std::move(contents).str();
}

} // namespace

std::string ScriptFailure::lineSpan() const {
    if (startLine == endLine) {
        return "line " + std::to_string(startLine);
    }
    return "lines " + std::to_string(startLine) + "-" + std::to_string(endLine);
}

LoadReport& LoadReport::operator+=(const LoadReport& other) {
    applied += other.applied;
    alreadyPresent += other.alreadyPresent;
    failures.insert(failures.end(), other.failures.begin(), other.failures.end());
    return *this;
}

LoadReport loadCypherFile(
    EngineDb& db, const std::filesystem::path& file, const StatementObserver& observe) {
    const std::string source = readFile(file);

    LoadReport report;
    for (const CypherStatement& statement : splitStatements(source)) {
        const ExecuteOutcome outcome = db.tryExecute(statement.text);
        if (observe) {
            observe(file, statement, outcome);
        }
        if (outcome.ok) {
            ++report.applied;
        } else if (outcome.alreadyExists) {
            ++report.alreadyPresent;
        } else {
            report.failures.push_back(ScriptFailure{.file = file,
                .startLine = statement.startLine,
                .endLine = statement.endLine,
                .label = statement.label(),
                .message = outcome.message});
        }
    }
    return report;
}

} // namespace rainbohz::rendering_engine
