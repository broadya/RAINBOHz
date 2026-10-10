// SPDX-License-Identifier: Apache-2.0

#pragma once

// Server mode: the engine as a long-running REST service (docs/server.md).
//
// The server holds at most one score in an in-memory engine-db between calls.
// Its calls, all under /v1, are described in full by the OpenAPI document
// api/openapi.yaml:
//
//   PUT  /v1/score                          {"uri": "file:///…/score.cypher"}
//        Discards the score it holds, then loads, transforms and precomputes
//        the named file into a fresh engine-db. If that fails, it holds no
//        score.
//   POST /v1/frames/{frame}/renderings      {"labels": ["bell"]}, or no body
//        Renders one frame of the score it holds to a WAV file in --out-dir
//        and returns the file's URI.
//   POST /v1/shutdown
//        Answers, then stops the server; the process exits 0.
//   GET  /v1/tempo-maps/root                the Score's TempoMap and children
//   GET  /v1/tempo-maps/{id}/children       a map's children
//   GET  /v1/tempo-maps/{id}/points         a page of a map's points
//   GET  /v1/tempo-maps/{id}/frame-position ?at=beats:ticks, the sample there
//   GET  /v1/engine-config                  sample rate, frame size, channels
//
// A successful response is the resource as a JSON object. A failure is an
// RFC 9457 problem document (application/problem+json) whose `detail` is the
// one-line verdict; the call's full narrative goes to standard output only.
//
// Calls are handled strictly one at a time, on the thread that accepts them:
// there is no worker thread and no pool. With one engine-db and one score,
// concurrency would buy contention and nothing else (ADR 0016).
//
// The split between ServerCore and runServer() keeps HTTP out of everything
// that can be tested without a socket. ServerCore takes a call's path
// parameters, query parameters and body as strings and returns a status code
// and a response body; runServer() is the only code that knows about HTTP, and
// cpp-httplib and nlohmann/json are confined to the implementation.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/options.hpp"
#include "rainbohz/rendering_engine/score_pipeline.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace rainbohz::rendering_engine {

/// What a server needs to know, taken from the command line.
struct ServerSettings {
    /// Cypher file of schema DDL, applied to every fresh engine-db.
    std::filesystem::path schemaFile;

    /// The directory every /load must read from.
    std::filesystem::path inputRoot;

    /// Where /render writes its WAV files.
    std::filesystem::path outputDir;

    /// Applied to every load and render.
    PipelineSettings pipeline;
};

/// An HTTP response, before it is HTTP: a status code, a JSON body and its
/// media type.
struct HttpReply {
    /// The HTTP status code.
    int status = 200;

    /// A JSON object, serialised.
    std::string body;

    /// `application/json` for a resource, `application/problem+json` for a
    /// problem.
    std::string contentType = "application/json";
};

/// A call's query parameters, as the URL carries them: a name may repeat.
using QueryParameters = std::multimap<std::string, std::string>;

/// The server's state and its handling of each call, without HTTP.
///
/// Every handler returns a reply rather than throwing: an exception from the
/// engine becomes a 500 whose detail is the exception's text, so no request
/// can stop the server. Every failure is a problem document with the status
/// listed for it here and in api/openapi.yaml. Not thread-safe, and not meant
/// to be: the server calls it from one thread.
class ServerCore {
public:
    /// Prepares a server that holds no score.
    ///
    /// @param settings  The server's settings. inputRoot is resolved to its
    ///                  canonical form here and outputDir made absolute, so
    ///                  that later changes of working directory do not move
    ///                  them.
    /// @param echo      Where each call's narrative is also written, with a
    ///                  line naming the call before it and the verdict after
    ///                  it. Must outlive this object.
    /// @throws std::filesystem::filesystem_error  If inputRoot does not exist.
    ServerCore(ServerSettings settings, std::ostream& echo);

    /// Destroys the server and the score it holds.
    ~ServerCore();

    ServerCore(const ServerCore&) = delete;
    ServerCore& operator=(const ServerCore&) = delete;
    ServerCore(ServerCore&&) = delete;
    ServerCore& operator=(ServerCore&&) = delete;

    /// Handles PUT /v1/score.
    ///
    /// The body must be a JSON object whose only key is `uri`, a string
    /// accepted by fileUriToPath(). The path is resolved, through any
    /// symbolic links, and must lie within the input root and name an
    /// existing regular file. Only then is the held score discarded and the
    /// file loaded into a fresh in-memory engine-db with loadScore().
    ///
    /// @param body  The request body.
    /// @return 200 with `uri`, `nodes`, `relationships` and `partials` (the
    ///         row totals and the Partial count) when the score loaded; 400
    ///         when the body is not such an object or the URI is unusable;
    ///         403 when the path lies outside the input root; 404 when it
    ///         names no regular file; 422 when the load stopped short, with
    ///         its verdict as the detail; 500 when the engine failed
    ///         otherwise. After a 422 or a 500 the server holds no score.
    HttpReply putScore(std::string_view body);

    /// Handles POST /v1/frames/{frame}/renderings.
    ///
    /// `frame` must be a decimal integer from 0 to the INT32 maximum, digits
    /// only. The body is either empty, meaning every partial, or a JSON
    /// object with at most `labels`, an array of non-empty strings; an
    /// omitted, null or empty `labels` means every partial, as in the batch
    /// request. The frame is rendered with renderRequestedFrames(), which
    /// replaces any earlier file for it.
    ///
    /// @param frame  The `{frame}` path segment.
    /// @param body   The request body.
    /// @return 200 with `frame` and `uri`, and `set_uris` under --per-set-wav,
    ///         when the frame was rendered; 400 when the frame or the body is
    ///         unusable, with every problem in `errors`; 409 when no score is
    ///         loaded; 422 when a label is unknown or the frame could not be
    ///         rendered; 500 when the output directory cannot be created or
    ///         the engine failed otherwise.
    HttpReply renderFrame(std::string_view frame, std::string_view body);

    /// Handles POST /v1/shutdown.
    ///
    /// Only answers: stopping the server is runServer()'s business, once the
    /// answer has been sent. The body is ignored.
    ///
    /// @return 200, always.
    HttpReply shutdown();

    /// Handles GET /v1/tempo-maps/root.
    ///
    /// @param query  The query parameters; there must be none.
    /// @return 200 with the root TempoMap's `id`, `name`, `ticks_per_beat` and
    ///         `children`; 400 for a query parameter; 404 when there is no
    ///         Score or it has no TempoMap; 409 when no score is loaded; 422
    ///         when there is more than one Score.
    HttpReply tempoMapRoot(const QueryParameters& query);

    /// Handles GET /v1/tempo-maps/{id}/children.
    ///
    /// @param id     The `{id}` path segment: a TempoMap.id, digits only.
    /// @param query  The query parameters; there must be none.
    /// @return 200 with `tempo_map_id` and `children`; 400 when `id` is not
    ///         an id or there is a query parameter; 404 when there is no such
    ///         map; 409 when no score is loaded.
    HttpReply tempoMapChildren(std::string_view id, const QueryParameters& query);

    /// Handles GET /v1/tempo-maps/{id}/points.
    ///
    /// Query parameters, each at most once: `after`, a position in ticks,
    /// digits only, which only later points follow; and `limit`, the page
    /// size, from 1 to kMaxPointsLimit, kDefaultPointsLimit when omitted.
    ///
    /// @param id     The `{id}` path segment.
    /// @param query  The query parameters.
    /// @return 200 with `tempo_map_id`, `ticks_per_beat`, `points` and `next`,
    ///         the relative URL of the next page or null; 400 when `id` or a
    ///         parameter is unusable; 404 when there is no such map; 409 when
    ///         no score is loaded; 422 when the map's list cannot be paged.
    HttpReply tempoMapPoints(std::string_view id, const QueryParameters& query);

    /// Handles GET /v1/tempo-maps/{id}/frame-position.
    ///
    /// The query must hold `at` exactly once: a position `beats:ticks`, or a
    /// bare number of beats.
    ///
    /// @param id     The `{id}` path segment.
    /// @param query  The query parameters.
    /// @return 200 with `tempo_map_id`, `at`, `frame` and `offset`; 400 when
    ///         `id` or `at` is unusable, including ticks beyond the map's
    ///         ticks_per_beat; 404 when there is no such map; 409 when no
    ///         score is loaded; 422 when the map does not convert
    ///         (framePosition()).
    HttpReply framePosition(std::string_view id, const QueryParameters& query);

    /// Handles GET /v1/engine-config.
    ///
    /// @param query  The query parameters; there must be none.
    /// @return 200 with `sample_rate`, `frame_size` (resolved) and
    ///         `channel_mode`; 400 for a query parameter; 409 when no score
    ///         is loaded; 422 when EngineConfig is unusable.
    HttpReply engineConfig(const QueryParameters& query);

    /// Whether a score is loaded and ready to render.
    ///
    /// @return True after a PUT /v1/score that succeeded, until the next one.
    bool hasScore() const noexcept { return db_ != nullptr; }

    /// The page size of GET /v1/tempo-maps/{id}/points when `limit` is
    /// omitted.
    static constexpr int kDefaultPointsLimit = 100;

    /// The largest `limit` GET /v1/tempo-maps/{id}/points accepts.
    static constexpr int kMaxPointsLimit = 1000;

private:
    /// The settings, with the input root resolved and the output directory
    /// made absolute.
    ServerSettings settings_;

    /// Where each call's narrative is echoed.
    std::ostream& echo_;

    /// The loaded score's database; null when no score is loaded.
    std::unique_ptr<EngineDb> db_;
};

/// Runs server mode until a POST /v1/shutdown call.
///
/// Creates --out-dir, binds --host and --port (0 picks a free port), writes a
/// line `listening on http://<host>:<port>` to standard output once
/// connections can be accepted, and then serves calls one at a time until
/// POST /v1/shutdown has been answered. A known path asked with the wrong
/// method gets 405 with an `Allow` header; an unknown path gets 404.
///
/// @param options  Validated command-line options, with `mode` equal to
///                 Mode::Server.
/// @return True when the server shut down because it was asked to; false
///         when it could not start (the reason is on standard error) or
///         its accept loop failed.
/// @note Side effects: listens on the network; reads files beneath
///       --input-root; writes WAV files into --out-dir; writes the narrative
///       of every call to standard output.
bool runServer(const Options& options);

} // namespace rainbohz::rendering_engine
