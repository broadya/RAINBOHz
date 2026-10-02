#pragma once

// Server mode: the engine as a long-running REST service (docs/server.md).
//
// The server holds at most one score in an in-memory engine-db between calls,
// and answers three:
//
//   POST /load      {"uri": "file:///…/score.cypher"}
//                   Discards the score it holds, then loads, transforms and
//                   precomputes the named file into a fresh engine-db. If that
//                   fails, it holds no score.
//   POST /render    {"frame": 3, "labels": ["bell"]}
//                   Renders one frame of the score it holds to a WAV file in
//                   --out-dir and returns the file's URI.
//   POST /shutdown  Answers, then stops the server; the process exits 0.
//
// Every response is a JSON object with `status` ("ok" or "error"), `message`
// and `log`, which is the narrative of the call line by line, exactly as
// batch mode would print it. The narrative also goes to standard output.
//
// Calls are handled strictly one at a time, on the thread that accepts them:
// there is no worker thread and no pool. With one engine-db and one score,
// concurrency would buy contention and nothing else (ADR 0016).
//
// The split between ServerCore and runServer() keeps HTTP out of everything
// that can be tested without a socket. ServerCore takes a request body and
// returns a status code and a response body; runServer() is the only code
// that knows about HTTP, and cpp-httplib and nlohmann/json are confined to the
// implementation.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/options.hpp"
#include "rainbohz/rendering_engine/score_pipeline.hpp"

#include <filesystem>
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

/// An HTTP response, before it is HTTP: a status code and a JSON body.
struct HttpReply {
    /// The HTTP status code.
    int status = 200;

    /// A JSON object, serialised.
    std::string body;
};

/// The server's state and its handling of each call, without HTTP.
///
/// Every handler returns a reply rather than throwing: an exception from the
/// engine becomes a 500 whose message is the exception's text, so no request
/// can stop the server. Not thread-safe, and not meant to be: the server calls
/// it from one thread.
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

    /// Handles POST /load.
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
    ///         its verdict as the message; 500 when the engine failed
    ///         otherwise. After a 422 or a 500 the server holds no score.
    HttpReply load(std::string_view body);

    /// Handles POST /render.
    ///
    /// The body must be a JSON object with `frame`, an integer from 0 to the
    /// INT32 maximum, and optionally `labels`, an array of non-empty strings;
    /// an omitted, null or empty `labels` means every partial, as in the
    /// batch request. No other key is accepted. The frame is rendered with
    /// renderRequestedFrames(), which replaces any earlier file for it.
    ///
    /// @param body  The request body.
    /// @return 200 with `frame` and `uri`, and `set_uris` under --per-set-wav,
    ///         when the frame was rendered; 400 when the body is not such an
    ///         object, with every problem in `log`; 409 when no score is
    ///         loaded; 422 when a label is unknown or the frame could not be
    ///         rendered; 500 when the output directory cannot be created or
    ///         the engine failed otherwise.
    HttpReply render(std::string_view body);

    /// Handles POST /shutdown.
    ///
    /// Only answers: stopping the server is runServer()'s business, once the
    /// answer has been sent. The body is ignored.
    ///
    /// @return 200, always.
    HttpReply shutdown();

    /// Whether a score is loaded and ready to render.
    ///
    /// @return True after a /load that succeeded, until the next /load.
    bool hasScore() const noexcept { return db_ != nullptr; }

private:
    /// The settings, with the input root resolved and the output directory
    /// made absolute.
    ServerSettings settings_;

    /// Where each call's narrative is echoed.
    std::ostream& echo_;

    /// The loaded score's database; null when no score is loaded.
    std::unique_ptr<EngineDb> db_;
};

/// Runs server mode until a /shutdown call.
///
/// Creates --out-dir, binds --host and --port (0 picks a free port), writes a
/// line `listening on http://<host>:<port>` to standard output once
/// connections can be accepted, and then serves calls one at a time until
/// POST /shutdown has been answered.
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
