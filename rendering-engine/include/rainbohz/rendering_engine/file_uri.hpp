// SPDX-License-Identifier: Apache-2.0

#pragma once

// `file://` URIs: the form in which server mode names the files it reads and
// writes.
//
// The server takes a score as a URI and hands back each rendered frame as a
// URI, so that a caller does not change when, later, frames go to object
// storage and are named by `https://` or `s3://` instead. For now every URI
// is a local file, and these two functions are the whole of the translation.
//
// The reading side is strict, because what it accepts decides which file the
// server opens: only `file://` with an empty or `localhost` authority and an
// absolute path, no query or fragment, and well-formed percent-encoding.
// RFC 8089 allows more (`file:/path`, other hosts, Windows drive letters);
// none of it is needed yet, and each is easier to add than to take back.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rainbohz::rendering_engine {

/// Thrown when a URI is not a local `file://` URI this component accepts.
/// The message says what is wrong with it.
class FileUriError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Converts a `file://` URI to the local path it names.
///
/// The scheme and the `localhost` authority match case-insensitively. Every
/// `%XX` in the path is decoded, so `file:///a%20b` names `/a b`. The path is
/// returned as written, neither normalised nor resolved: a caller that means
/// to confine it to a directory must resolve it first.
///
/// @param uri  The URI, e.g. `file:///scores/demo.cypher`.
/// @return The absolute path the URI names.
/// @throws FileUriError  If the scheme is not `file`; the authority is not
///         empty or `localhost`; the path is empty or not absolute; the URI
///         has a query or fragment; or a `%` is not followed by two hex
///         digits, or decodes to a NUL byte.
std::filesystem::path fileUriToPath(std::string_view uri);

/// Converts an absolute local path to a `file://` URI.
///
/// The path is written in generic form (`/` separators) with an empty
/// authority, and every byte other than an unreserved character (RFC 3986:
/// letters, digits, `-`, `.`, `_`, `~`) or `/` is percent-encoded with
/// upper-case hex digits. fileUriToPath() of the result gives the path back.
///
/// @param path  An absolute path. It is not normalised or resolved.
/// @return The URI, e.g. `file:///out/frame-000003.wav`.
/// @throws FileUriError  If `path` is not absolute.
std::string pathToFileUri(const std::filesystem::path& path);

} // namespace rainbohz::rendering_engine
