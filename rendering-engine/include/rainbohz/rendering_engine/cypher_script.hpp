// SPDX-License-Identifier: Apache-2.0

#pragma once

// Splitting a Cypher script into individually submittable statements.
//
// Statements are submitted to Ladybug one at a time so that a failure can be
// attributed to a source line. The schema files are hand-edited deliberately
// (AGENTS.md), so a diagnostic that does not say *where* is of little use.
//
// This is a port of split_statements() in scripts/ladybug_ddl.py, which is the
// reference implementation. The two must agree: the Python tooling validates
// and loads the same files through the `lbug` CLI, and a disagreement about
// where one statement ends would show up as the two tools reporting different
// line numbers for the same error. See docs/adr/0005.
//
// Cypher lexical rules are reimplemented well enough for DDL and simple DML
// rather than completely -- comments, string literals and quoted identifiers.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine {

/// One executable statement, with the source lines it came from.
struct CypherStatement {
    /// Comment-stripped statement text, with no terminating semicolon.
    std::string text;

    /// 1-based line carrying the first executable character.
    std::size_t startLine = 0;

    /// 1-based line carrying the terminating semicolon.
    std::size_t endLine = 0;

    /// Builds a short identifier for the statement, for use in reports.
    ///
    /// Recognises the DDL forms `CREATE {NODE|REL} TABLE [GROUP]
    /// [IF NOT EXISTS] <name>`, case-insensitively and anywhere in the
    /// statement, and names them by kind and table ("CREATE NODE TABLE
    /// Partial"); the optional IF NOT EXISTS is dropped from the label.
    /// Anything else is the statement text collapsed onto one line and elided.
    /// Agrees with the labels produced by scripts/ladybug_ddl.py, so that both
    /// tools name a failing statement the same way.
    ///
    /// @return The DDL label, or the statement's whitespace-separated tokens
    ///         joined by single spaces. The latter is truncated to 60
    ///         characters with "..." appended when longer. Empty only when
    ///         `text` is empty or all whitespace.
    std::string label() const;

    /// Formats the statement's source line range, for use in reports.
    ///
    /// @return "line 42" when `startLine` equals `endLine`, otherwise
    ///         "lines 42-58". The numbers are printed as stored, so a
    ///         default-constructed statement gives "line 0".
    std::string lineSpan() const;
};

/// Splits a Cypher script into statements, tracking source line numbers.
///
/// Comments (`//` to end of line, and `/*` block comments) are stripped, and
/// semicolons or comment markers inside string literals or quoted identifiers
/// are not treated as such. A missing semicolon therefore merges two
/// statements into one -- which is exactly what Ladybug itself sees, so the
/// resulting error is attributed to the correct line range.
///
/// Must agree with split_statements() in scripts/ladybug_ddl.py, the
/// reference implementation; see the file comment and docs/adr/0005.
///
/// @param source  Complete script text. Lines are counted by `\n` alone, so a
///                CRLF file numbers correctly and a CR-only file does not.
///                Not retained: the returned statements own their text.
/// @return The non-empty statements in source order, each with its text
///         trimmed of surrounding whitespace and its terminating semicolon.
///         A trailing statement with no terminating semicolon is included;
///         empty statements (a stray semicolon, a comment-only file) are not.
///         Malformed input -- an unterminated string or block comment -- is
///         not an error: it consumes the rest of the source, as it would in
///         Ladybug.
std::vector<CypherStatement> splitStatements(std::string_view source);

} // namespace rainbohz::rendering_engine
