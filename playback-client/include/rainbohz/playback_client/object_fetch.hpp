// SPDX-License-Identifier: Apache-2.0

#pragma once

// Fetching a rendered frame's object into the client's buffer.
//
// A render answers with the URI of an object rather than with audio: in the
// target architecture the object is in object storage, and URLs, not audio
// payloads, are what travel between components
// (architecture/overview.md §5.2, `object-storage`). Today the URI is a
// `file://` one on the server's machine, so fetching it is a copy, and the
// client has to run where it can read the server's --out-dir.
//
// The fetcher is an interface so that another scheme -- `s3://` against an
// S3 simulator, say, or `https://` -- can be added without touching the lane
// logic. This version implements `file://` only and refuses anything else.

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace rainbohz::playback_client {

/// Thrown when an object cannot be fetched.
class ObjectFetchError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Copies objects named by URI to local files.
class ObjectFetcher {
public:
    /// Destroys the fetcher.
    virtual ~ObjectFetcher() = default;

    ObjectFetcher() = default;
    ObjectFetcher(const ObjectFetcher&) = delete;
    ObjectFetcher& operator=(const ObjectFetcher&) = delete;
    ObjectFetcher(ObjectFetcher&&) = delete;
    ObjectFetcher& operator=(ObjectFetcher&&) = delete;

    /// Copies the object at `uri` to `destination`.
    ///
    /// @param uri          The object's URI, as a render returned it.
    /// @param destination  The local file to write, replaced if it exists.
    ///                     Its directory must exist.
    /// @throws ObjectFetchError if the URI's scheme is not supported, the URI
    ///         is malformed, or the object cannot be read or written.
    /// @note Side effect: writes `destination`. Not atomic; the caller
    ///       renames the file into place once it is complete.
    virtual void fetch(std::string_view uri, const std::filesystem::path& destination) = 0;
};

/// The local path a `file://` URI names.
///
/// Accepts what rendering-engine returns (rendering-engine/docs/server.md
/// §4): `file://`, an empty or `localhost` authority, an absolute,
/// percent-encoded path, and no query or fragment. The scheme and `localhost`
/// match case-insensitively.
///
/// @param uri  The URI.
/// @return The path, decoded.
/// @throws ObjectFetchError if `uri` is not such a URI, has a malformed or NUL
///         percent-escape, or does not name an absolute path.
std::filesystem::path fileUriToPath(std::string_view uri);

/// Makes the fetcher for this version: `file://` URIs only.
///
/// @return The fetcher. Stateless, so it may be used from any thread.
std::unique_ptr<ObjectFetcher> makeObjectFetcher();

} // namespace rainbohz::playback_client
