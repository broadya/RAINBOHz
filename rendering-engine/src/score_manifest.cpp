// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/score_manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

/// @file
/// @brief Reads and checks a score's table manifest, for either backend.
///
/// @see score_manifest.hpp, ADR 0025

namespace rainbohz::rendering_engine {

namespace {

/// The JSON library's value type.
using Json = nlohmann::json;

/// Reads and checks a manifest. Every problem is a ManifestError naming the
/// manifest (a ScriptLoadError when the file cannot be read at all), thrown
/// before anything is loaded.
class ManifestReader {
public:
    /// A reader of one manifest.
    ///
    /// @param manifest  The manifest file.
    explicit ManifestReader(std::filesystem::path manifest) : manifest_{std::move(manifest)} {}

    /// Reads the manifest whole.
    ///
    /// @return The manifest, checked.
    /// @throws ScriptLoadError  If the file cannot be read.
    /// @throws ManifestError  If it is not of the expected shape.
    ScoreManifest read() {
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
        if (root.value("format", "") != kScoreManifestFormat) {
            fail("'format' is not \"" + std::string{kScoreManifestFormat} + "\"");
        }
        if (!root.contains("version") || !root["version"].is_number_integer() ||
            root["version"].get<std::int64_t>() != kScoreManifestVersion) {
            fail("'version' is not " + std::to_string(kScoreManifestVersion) +
                 ", the version this engine reads");
        }

        ScoreManifest manifest;
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
    /// Refuses the manifest.
    ///
    /// @param problem  What is wrong with it.
    /// @throws ManifestError  Always, naming the manifest.
    [[noreturn]] void fail(const std::string& problem) const {
        throw ManifestError{manifest_.string() + ": " + problem};
    }

    /// Reports that the manifest cannot be read.
    ///
    /// @param problem  Why not.
    /// @throws ScriptLoadError  Always, naming the manifest.
    [[noreturn]] void failToRead(const std::string& problem) const {
        throw ScriptLoadError{manifest_.string() + ": " + problem};
    }

    /// Parses the file as JSON.
    ///
    /// @return The document.
    /// @throws ScriptLoadError  If the file cannot be opened.
    /// @throws ManifestError  If it is not valid JSON.
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

    /// A required member of a JSON object, of one type.
    ///
    /// @param object  The object.
    /// @param key     The member's name.
    /// @param type    Its type: an array or an object.
    /// @return The member.
    /// @throws ManifestError  If it is missing or of another type.
    const Json& member(const Json& object, const std::string& key, Json::value_t type) const {
        const auto found = object.find(key);
        if (found == object.end() || found->type() != type) {
            fail("'" + key + "' is missing or not " +
                 (type == Json::value_t::array ? "an array" : "an object"));
        }
        return *found;
    }

    /// A required member that must be an identifier.
    ///
    /// @param entry  The entry holding it.
    /// @param key    The member's name.
    /// @param where  The entry, for the message.
    /// @return The identifier.
    /// @throws ManifestError  If it is missing, not a string, or not an
    ///         identifier.
    std::string identifier(
        const Json& entry, const std::string& key, const std::string& where) const {
        const auto found = entry.find(key);
        if (found == entry.end() || !found->is_string() ||
            !isIdentifier(found->get<std::string>())) {
            fail(where + ": '" + key + "' is missing or not an identifier");
        }
        return found->get<std::string>();
    }

    /// An entry's required row count.
    ///
    /// @param entry  The entry.
    /// @param where  The entry, for the message.
    /// @return The count, 0 or more.
    /// @throws ManifestError  If it is missing, not an integer, or negative.
    std::int64_t rowCount(const Json& entry, const std::string& where) const {
        const auto found = entry.find("rows");
        if (found == entry.end() || !found->is_number_integer() || found->get<std::int64_t>() < 0) {
            fail(where + ": 'rows' is missing or not a count");
        }
        return found->get<std::int64_t>();
    }

    /// The data file `entry` names, which must be a plain `.parquet` name
    /// resolving to a regular file in the manifest's directory.
    ///
    /// @param entry  The entry.
    /// @param where  The entry, for the message.
    /// @return The file, resolved.
    /// @throws ManifestError  If the name is missing or not plain, or the
    ///         file is missing or outside the directory.
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
        return resolved;
    }

    /// Reads a `nodes` entry.
    ///
    /// @param entry  The entry.
    /// @return The node table.
    /// @throws ManifestError  If the entry is not of the expected shape.
    ManifestNodeTable readNode(const Json& entry) const {
        if (!entry.is_object()) {
            fail("a 'nodes' entry is not an object");
        }
        ManifestNodeTable node;
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

    /// Reads a `relationships` entry.
    ///
    /// @param entry  The entry.
    /// @return The relationship pair.
    /// @throws ManifestError  If the entry is not of the expected shape.
    ManifestRelationship readRel(const Json& entry) const {
        if (!entry.is_object()) {
            fail("a 'relationships' entry is not an object");
        }
        ManifestRelationship rel;
        rel.table = identifier(entry, "table", "a 'relationships' entry");
        const std::string where = "relationship " + rel.table;
        rel.from = identifier(entry, "from", where);
        rel.to = identifier(entry, "to", where);
        rel.rows = rowCount(entry, where);
        rel.file = dataFile(entry, where);
        return rel;
    }

    std::filesystem::path manifest_;  ///< The manifest file.
    std::filesystem::path directory_; ///< Its directory, resolved.
};

} // namespace

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

ScoreManifest readScoreManifest(const std::filesystem::path& manifest) {
    return ManifestReader{manifest}.read();
}

} // namespace rainbohz::rendering_engine
