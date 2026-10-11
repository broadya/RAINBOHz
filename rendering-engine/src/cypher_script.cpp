// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/cypher_script.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief Splits a Cypher script into individually submittable statements.
///
/// Implements splitStatements() and CypherStatement's reporting helpers,
/// label() and lineSpan(). A small scanner knows just enough Cypher to find
/// where one statement ends: line and block comments, string literals and
/// backtick-quoted identifiers. It records the source lines of each
/// statement's first executable character and its terminating semicolon, so
/// that a failure can be attributed to a line. Labels name DDL by kind and
/// table and elide anything else.
///
/// This is a port of split_statements() in scripts/ladybug_ddl.py, the
/// reference implementation. The two must agree on statement boundaries, line
/// numbers and labels (docs/adr/0005). Pure string processing: no database is
/// involved.
///
/// @see cypher_script.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Beyond this a label stops identifying the statement and starts quoting it.
/// Matches the elision width in scripts/ladybug_ddl.py.
constexpr std::size_t kLabelMaxLength = 60;

/// Tests for whitespace, as std::isspace does in the current C locale.
///
/// std::isspace and friends take an int whose value must be representable as
/// unsigned char; passing a negative char (any byte of a UTF-8 multibyte
/// sequence, where char is signed) is undefined. This and the other character
/// wrappers below are the only places that cast.
///
/// @param character  Any byte of the source.
/// @return True for space, tab, newline, vertical tab, form feed and carriage
///         return in the default "C" locale.
bool isSpace(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

/// Tests whether a character can appear in a table name.
///
/// Stands in for \\w in DDL_HEAD_RE in scripts/ladybug_ddl.py. Python's \\w is
/// Unicode-aware, so the two disagree on non-ASCII names; the schemas do not
/// use any. Casts as isSpace() does.
///
/// @param character  Any byte of the source.
/// @return True for ASCII letters, digits and underscore (in the "C" locale).
bool isWordCharacter(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

/// Upper-cases one character, as std::toupper does in the current C locale.
///
/// Casts as isSpace() does.
///
/// @param character  Any byte of the source.
/// @return The upper-case form of an ASCII letter (in the "C" locale);
///         any other character unchanged.
char toUpper(char character) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
}

/// Compares two strings for equality, ignoring ASCII case.
///
/// Used to match Cypher keywords, which Ladybug accepts in any case.
///
/// @param left   First string.
/// @param right  Second string.
/// @return True when both have the same length and match character by
///         character after toUpper().
bool equalsIgnoringCase(std::string_view left, std::string_view right) {
    return std::ranges::equal(left, right, [](char a, char b) { return toUpper(a) == toUpper(b); });
}

/// Strips leading and trailing whitespace.
///
/// The character set is the one isSpace() accepts in the "C" locale, and the
/// one Python's str.strip() uses for ASCII.
///
/// @param text  Text to trim.
/// @return A view into `text` without its surrounding whitespace, or an empty
///         view when `text` is empty or all whitespace. Valid only as long as
///         the storage `text` refers to.
std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\n\v\f\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\n\v\f\r") - first + 1);
}

/// Splits text into whitespace-separated tokens.
///
/// Equivalent to Python's str.split() with no arguments, which label() relies
/// on both for DDL matching and for collapsing a statement onto one line.
/// Punctuation is not a separator, so `Partial(id` is a single token.
///
/// @param text  Text to split.
/// @return The non-empty tokens in order, as views into `text`. Valid only as
///         long as the storage `text` refers to.
std::vector<std::string_view> tokenise(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && isSpace(text[index])) {
            ++index;
        }
        const std::size_t start = index;
        while (index < text.size() && !isSpace(text[index])) {
            ++index;
        }
        if (index > start) {
            tokens.push_back(text.substr(start, index - start));
        }
    }
    return tokens;
}

/// Extracts a table name from the start of a token.
///
/// Mirrors the tail of DDL_HEAD_RE, an optional backtick or double quote then
/// \\w+: a table name is followed immediately by its parenthesis often enough
/// that whitespace alone does not delimit it. A closing quote is not required, and is not included.
///
/// @param token  Token that should begin with the table name, optionally
///               preceded by a backtick or double quote.
/// @return The leading run of word characters after any opening quote, as a
///         view into `token`; empty if there is none.
std::string_view tableName(std::string_view token) {
    if (!token.empty() && (token.front() == '`' || token.front() == '"')) {
        token.remove_prefix(1);
    }
    std::size_t length = 0;
    while (length < token.size() && isWordCharacter(token[length])) {
        ++length;
    }
    return token.substr(0, length);
}

/// Matches a DDL head at one token position and builds its label.
///
/// Recognises `CREATE {NODE|REL} TABLE [GROUP] [IF NOT EXISTS] <name>`,
/// keywords in any case. Anchoring is the one deliberate difference from the
/// Python regex, which searches the whole statement. Here the caller tries
/// every token position, which comes to the same thing.
///
/// @param tokens  The statement's tokens, from tokenise().
/// @param start   Index of the token that should be CREATE.
/// @pre `start < tokens.size()`. The length check is unsigned, so a larger
///      `start` wraps around it and indexes out of bounds.
/// @return "CREATE NODE TABLE <name>" or "CREATE REL TABLE <name>", with
///         " GROUP" after TABLE when present and the keywords upper-cased;
///         the name keeps its source case. Empty if the tokens do not match.
std::string ddlLabelAt(const std::vector<std::string_view>& tokens, std::size_t start) {
    if (tokens.size() - start < 4) {
        return {};
    }
    if (!equalsIgnoringCase(tokens[start], "CREATE")) {
        return {};
    }
    const bool isNode = equalsIgnoringCase(tokens[start + 1], "NODE");
    if (!isNode && !equalsIgnoringCase(tokens[start + 1], "REL")) {
        return {};
    }
    if (!equalsIgnoringCase(tokens[start + 2], "TABLE")) {
        return {};
    }

    std::string kind = isNode ? "CREATE NODE TABLE" : "CREATE REL TABLE";
    std::size_t index = start + 3;
    if (equalsIgnoringCase(tokens[index], "GROUP")) {
        kind += " GROUP";
        ++index;
    }
    if (index + 2 < tokens.size() && equalsIgnoringCase(tokens[index], "IF") &&
        equalsIgnoringCase(tokens[index + 1], "NOT") &&
        equalsIgnoringCase(tokens[index + 2], "EXISTS")) {
        index += 3;
    }
    if (index >= tokens.size()) {
        return {};
    }
    const std::string_view name = tableName(tokens[index]);
    if (name.empty()) {
        return {};
    }
    return kind + " " + std::string{name};
}

} // namespace

std::string CypherStatement::label() const {
    const std::vector<std::string_view> tokens = tokenise(text);
    for (std::size_t start = 0; start < tokens.size(); ++start) {
        std::string ddl = ddlLabelAt(tokens, start);
        if (!ddl.empty()) {
            return ddl;
        }
    }

    // Not DDL we recognise: the statement itself, collapsed onto one line.
    std::string collapsed;
    for (const std::string_view token : tokens) {
        if (!collapsed.empty()) {
            collapsed += ' ';
        }
        collapsed += token;
    }
    if (collapsed.size() > kLabelMaxLength) {
        collapsed.resize(kLabelMaxLength);
        collapsed += "...";
    }
    return collapsed;
}

std::string CypherStatement::lineSpan() const {
    if (startLine == endLine) {
        return "line " + std::to_string(startLine);
    }
    return "lines " + std::to_string(startLine) + "-" + std::to_string(endLine);
}

namespace {

/// Where the scanner is in the source, and what it has accumulated so far.
struct ScanState {
    std::size_t index = 0;     ///< Offset of the next character to read.
    std::size_t line = 1;      ///< 1-based line containing `index`.
    std::size_t startLine = 0; ///< 0 stands for "no executable text seen yet"
    std::string buffer;        ///< Text of the current statement so far.
};

/// Skips a `//` comment without copying it into the statement.
///
/// Stops *at* the newline, not after it, so that the caller's general path
/// appends it and counts the line, and the statement keeps its shape.
///
/// @param source        Script being scanned.
/// @param[in,out] scan  `index` must be at the opening `//`. On return it is
///                      at the terminating newline, or at the end of
///                      `source`. `line` and `buffer` are unchanged.
void skipLineComment(std::string_view source, ScanState& scan) {
    while (scan.index < source.size() && source[scan.index] != '\n') {
        ++scan.index;
    }
}

/// Skips a block comment without copying it into the statement.
///
/// Nothing is appended, not even a space, so a block comment between two
/// tokens joins them, as it does in ladybug_ddl.py. Block comments do not
/// nest: the first closing marker ends the comment.
///
/// (The closing marker is not written out in these comments: it would end
/// them early in Doxygen's scanner.)
///
/// @param source        Script being scanned.
/// @param[in,out] scan  `index` must be at the opening `/*`. On return it is
///                      just past the closing marker, and `line` has advanced
///                      by the newlines inside the comment. `buffer` is
///                      unchanged.
/// @throws CypherScriptError  If the comment is never closed, naming the line
///         it opened on.
void skipBlockComment(std::string_view source, ScanState& scan) {
    const std::size_t openedOn = scan.line;
    scan.index += 2;
    while (scan.index < source.size() && source.substr(scan.index, 2) != "*/") {
        if (source[scan.index] == '\n') {
            ++scan.line;
        }
        ++scan.index;
    }
    if (scan.index >= source.size()) {
        throw CypherScriptError{"line " + std::to_string(openedOn) +
                                ": unterminated block comment; it hides the rest of the script"};
    }
    scan.index += 2;
}

/// Copies a quoted run -- a string literal or a quoted identifier -- verbatim.
///
/// Copying it whole means a semicolon or a comment marker inside it is not
/// read as one. A backslash escapes the next character in `'` and `"` runs,
/// but not in backticked ones. An unterminated quote consumes the rest of the
/// source, which is what Ladybug would see too.
///
/// @param source        Script being scanned.
/// @param[in,out] scan  `index` must be at the opening quote. On return it is
///                      just past the closing quote, the run including both
///                      quotes has been appended to `buffer`, `line` has
///                      advanced by the newlines inside it (escaped ones
///                      included), and `startLine` is set if this run is the
///                      statement's first executable text.
/// @post A backslash as the last character of `source` leaves `index` one
///       past the end. Callers compare with `<`, so this ends the scan.
void consumeQuoted(std::string_view source, ScanState& scan) {
    if (scan.startLine == 0) {
        scan.startLine = scan.line;
    }
    const char quote = source[scan.index];
    scan.buffer += quote;
    ++scan.index;

    while (scan.index < source.size()) {
        // Backticked identifiers do not honour backslash escapes.
        if (source[scan.index] == '\\' && quote != '`') {
            const std::string_view escape = source.substr(scan.index, 2);
            scan.buffer += escape;
            if (escape.size() == 2 && escape[1] == '\n') {
                ++scan.line;
            }
            scan.index += 2;
            continue;
        }
        const char character = source[scan.index];
        scan.buffer += character;
        if (character == '\n') {
            ++scan.line;
        }
        ++scan.index;
        if (character == quote) {
            break;
        }
    }
}

} // namespace

std::vector<CypherStatement> splitStatements(std::string_view source) {
    std::vector<CypherStatement> statements;
    ScanState scan;

    const auto flush = [&statements, &scan](std::size_t endLine) {
        const std::string_view text = trim(scan.buffer);
        if (!text.empty()) {
            statements.push_back(CypherStatement{.text = std::string{text},
                .startLine = scan.startLine != 0 ? scan.startLine : endLine,
                .endLine = endLine});
        }
        scan.buffer.clear();
        scan.startLine = 0;
    };

    while (scan.index < source.size()) {
        const char character = source[scan.index];
        const std::string_view pair = source.substr(scan.index, 2);

        if (pair == "//") {
            skipLineComment(source, scan);
        } else if (pair == "/*") {
            skipBlockComment(source, scan);
        } else if (character == '\'' || character == '"' || character == '`') {
            consumeQuoted(source, scan);
        } else if (character == ';') {
            flush(scan.line);
            ++scan.index;
        } else {
            if (!isSpace(character) && scan.startLine == 0) {
                scan.startLine = scan.line;
            }
            scan.buffer += character;
            if (character == '\n') {
                ++scan.line;
            }
            ++scan.index;
        }
    }

    // A trailing statement with no terminating semicolon still has to be
    // submitted: Ladybug would accept it, and dropping it silently is the
    // failure mode ADR 0002 exists to prevent.
    flush(scan.line);

    return statements;
}

} // namespace rainbohz::rendering_engine
