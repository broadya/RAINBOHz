#include "rainbohz/rendering_engine/file_uri.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief Converts between local paths and `file://` URIs.
///
/// Implements fileUriToPath() and pathToFileUri(). Byte-wise throughout: a
/// path's bytes are encoded and decoded as they are, without assuming an
/// encoding, which is what POSIX paths are.
///
/// @see file_uri.hpp

namespace rainbohz::rendering_engine {

namespace {

/// The scheme and authority separator every accepted URI starts with.
constexpr std::string_view kScheme = "file://";

/// Compares two ASCII strings ignoring case.
///
/// @param left   One string.
/// @param right  The other.
/// @return True when they have the same length and match byte for byte
///         after `std::tolower`.
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

/// Whether a byte may appear unencoded in a path written by pathToFileUri().
///
/// @param byte  The byte.
/// @return True for RFC 3986's unreserved characters and `/`.
bool isLiteral(unsigned char byte) {
    return std::isalnum(byte) != 0 || byte == '-' || byte == '.' || byte == '_' || byte == '~' ||
           byte == '/';
}

} // namespace

std::filesystem::path fileUriToPath(std::string_view uri) {
    if (uri.size() < kScheme.size() ||
        !equalsIgnoringCase(uri.substr(0, kScheme.size()), kScheme)) {
        throw FileUriError{"'" + std::string{uri} + "' is not a file:// URI"};
    }
    if (uri.find_first_of("?#") != std::string_view::npos) {
        throw FileUriError{
            "'" + std::string{uri} + "' has a query or fragment; a file URI has neither"};
    }

    const std::string_view rest = uri.substr(kScheme.size());
    const std::size_t slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    if (!authority.empty() && !equalsIgnoringCase(authority, "localhost")) {
        throw FileUriError{"'" + std::string{uri} + "' names host '" + std::string{authority} +
                           "'; only local files are supported"};
    }
    if (slash == std::string_view::npos) {
        throw FileUriError{"'" + std::string{uri} + "' has no path"};
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
            throw FileUriError{
                "'" + std::string{uri} + "' has a '%' not followed by two hex digits"};
        }
        const int byte = (*high * 16) + *low;
        if (byte == 0) {
            throw FileUriError{"'" + std::string{uri} + "' encodes a NUL byte"};
        }
        decoded += static_cast<char>(byte);
        i += 2;
    }

    std::filesystem::path path{decoded};
    if (!path.is_absolute()) {
        throw FileUriError{"'" + std::string{uri} + "' does not name an absolute path"};
    }
    return path;
}

std::string pathToFileUri(const std::filesystem::path& path) {
    if (!path.is_absolute()) {
        throw FileUriError{"'" + path.string() + "' is not an absolute path"};
    }
    static constexpr std::array<char, 16> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    std::string uri{kScheme};
    for (const char character : path.generic_string()) {
        const auto byte = static_cast<unsigned char>(character);
        if (isLiteral(byte)) {
            uri += character;
            continue;
        }
        uri += '%';
        uri += kHex.at(byte / 16);
        uri += kHex.at(byte % 16);
    }
    return uri;
}

} // namespace rainbohz::rendering_engine
