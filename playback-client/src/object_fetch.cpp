// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/object_fetch.hpp"

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

/// @file
/// @brief Fetches objects by URI; `file://` only in this version.
///
/// The URI decoding mirrors rendering-engine's src/file_uri.cpp, which
/// writes the URIs this reads. It is reimplemented rather than shared because
/// components do not link each other's code; the tests pin the two together
/// through the end-to-end check.
///
/// @see object_fetch.hpp

namespace rainbohz::playback_client {

namespace {

/// The scheme and authority separator of a file URI.
constexpr std::string_view kFileScheme = "file://";

/// Compares two ASCII strings ignoring case.
///
/// @param left   One string.
/// @param right  The other.
/// @return True when they have the same length and match byte for byte after
///         `std::tolower`.
bool equalsIgnoringCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

/// The value of one hex digit.
///
/// @param digit  A character.
/// @return 0 to 15, or nullopt when `digit` is not a hex digit.
std::optional<int> hexValue(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return std::nullopt;
}

/// The scheme of a URI, up to its first `:`.
///
/// @param uri  The URI.
/// @return The scheme, or the whole text when there is no `:`.
std::string_view schemeOf(std::string_view uri) {
    return uri.substr(0, uri.find(':'));
}

/// Fetches `file://` objects by copying the file.
class FileObjectFetcher final : public ObjectFetcher {
public:
    void fetch(std::string_view uri, const std::filesystem::path& destination) override {
        if (!equalsIgnoringCase(schemeOf(uri), "file")) {
            throw ObjectFetchError{"cannot fetch '" + std::string{uri} + "': the '" +
                                   std::string{schemeOf(uri)} +
                                   "' scheme is not in this version; only file:// is"};
        }
        const std::filesystem::path source = fileUriToPath(uri);
        std::error_code error;
        std::filesystem::copy_file(
            source, destination, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            throw ObjectFetchError{"cannot copy " + source.string() + " to " +
                                   destination.string() + ": " + error.message() +
                                   " (playback-client must run where it can read the "
                                   "server's --out-dir)"};
        }
    }
};

} // namespace

std::filesystem::path fileUriToPath(std::string_view uri) {
    if (uri.size() < kFileScheme.size() ||
        !equalsIgnoringCase(uri.substr(0, kFileScheme.size()), kFileScheme)) {
        throw ObjectFetchError{"'" + std::string{uri} + "' is not a file:// URI"};
    }
    if (uri.find_first_of("?#") != std::string_view::npos) {
        throw ObjectFetchError{
            "'" + std::string{uri} + "' has a query or fragment; a file URI has neither"};
    }

    const std::string_view rest = uri.substr(kFileScheme.size());
    const std::size_t slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    if (!authority.empty() && !equalsIgnoringCase(authority, "localhost")) {
        throw ObjectFetchError{"'" + std::string{uri} + "' names host '" + std::string{authority} +
                               "'; only local files are supported"};
    }
    if (slash == std::string_view::npos) {
        throw ObjectFetchError{"'" + std::string{uri} + "' has no path"};
    }

    const std::string_view encoded = rest.substr(slash);
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            decoded += encoded[i];
            continue;
        }
        const std::optional<int> high =
            i + 1 < encoded.size() ? hexValue(encoded[i + 1]) : std::nullopt;
        const std::optional<int> low =
            i + 2 < encoded.size() ? hexValue(encoded[i + 2]) : std::nullopt;
        if (!high.has_value() || !low.has_value()) {
            throw ObjectFetchError{
                "'" + std::string{uri} + "' has a '%' not followed by two hex digits"};
        }
        const int byte = (*high * 16) + *low;
        if (byte == 0) {
            throw ObjectFetchError{"'" + std::string{uri} + "' encodes a NUL byte"};
        }
        decoded += static_cast<char>(byte);
        i += 2;
    }

    std::filesystem::path path{decoded};
    if (!path.is_absolute()) {
        throw ObjectFetchError{"'" + std::string{uri} + "' does not name an absolute path"};
    }
    return path;
}

std::unique_ptr<ObjectFetcher> makeObjectFetcher() {
    return std::make_unique<FileObjectFetcher>();
}

} // namespace rainbohz::playback_client
