#include "rainbohz/rendering_engine/server.hpp"

#include "rainbohz/rendering_engine/file_uri.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

/// Ladybug compiles its own, older cpp-httplib into the engine (for its
/// extension installer), in the same namespace. Two versions of the same
/// inline classes and functions in one binary are an ODR violation: the linker
/// keeps one definition of each and the program calls a mix of both, which
/// crashed the first version of this file. Renaming the namespace for this one
/// inclusion keeps the two copies apart. See ADR 0016.
#define httplib rainbohz_httplib // NOLINT(readability-identifier-naming): renames the namespace
#include <httplib.h>
#undef httplib
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

/// @file
/// @brief Server mode: ServerCore's handlers, and the HTTP server around them.
///
/// The only file that includes cpp-httplib or nlohmann/json. ServerCore turns
/// a request body into a reply by running the steps of score_pipeline.hpp;
/// runServer() binds the three endpoints to it on an httplib::Server whose
/// task queue runs every connection inline on the accepting thread.
///
/// @see server.hpp, docs/server.md

namespace rainbohz::rendering_engine {

namespace {

/// The JSON value type of nlohmann/json.
using Json = nlohmann::json;

/// cpp-httplib, under the namespace it is included as here.
namespace http = rainbohz_httplib;

/// Splits a narrative into lines for a response's `log`.
///
/// @param text  The narrative, as written to a stream.
/// @return One string per line, without newlines. Blank lines are kept, so
///         the log reads as the stream did; a final newline does not add an
///         empty line.
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream{text};
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    return lines;
}

/// Builds a reply.
///
/// @param status     The HTTP status. Below 400 is reported as `"ok"`, the
///                   rest as `"error"`.
/// @param message    The one-line summary.
/// @param narrative  The call's narrative, split into `log`.
/// @param extra      Further keys to add to the object, e.g. `uri`.
/// @return The reply, its body serialised as one line of JSON.
HttpReply reply(int status, const std::string& message, const std::string& narrative,
    Json extra = Json::object()) {
    Json body = std::move(extra);
    body["status"] = status < 400 ? "ok" : "error";
    body["message"] = message;
    body["log"] = splitLines(narrative);
    return {.status = status, .body = body.dump()};
}

/// Ends a call: echoes its narrative and verdict, and builds its reply.
///
/// @param echo       Where the narrative is echoed, followed by the message --
///                   prefixed "rendering-engine: " on a failure, as batch mode
///                   prints its verdict -- and a blank line. Flushed, so that
///                   whoever watches the server sees each call as it ends.
/// @param narrative  The call's narrative.
/// @param status     The HTTP status.
/// @param message    The one-line summary.
/// @param extra      Further keys for the reply.
/// @return The reply, as reply() builds it.
HttpReply conclude(std::ostream& echo, const std::string& narrative, int status,
    const std::string& message, Json extra) {
    echo << narrative << '\n'
         << (status < 400 ? "" : "rendering-engine: ") << message << "\n\n"
         << std::flush;
    return reply(status, message, narrative, std::move(extra));
}

/// Parses a request body as a JSON object.
///
/// @param body  The request body.
/// @return The object; or nullopt when the body is not JSON or is JSON but
///         not an object.
std::optional<Json> parseObject(std::string_view body) {
    Json parsed = Json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

/// Lists the keys of `object` that are not in `allowed`, as problems.
///
/// @param object   A JSON object.
/// @param allowed  The keys the object may have.
/// @return One problem per unknown key, in the object's order.
std::vector<std::string> unknownKeys(const Json& object, const std::set<std::string>& allowed) {
    std::vector<std::string> problems;
    for (const auto& [key, value] : object.items()) {
        if (!allowed.contains(key)) {
            problems.push_back("unknown key '" + key + "'");
        }
    }
    return problems;
}

/// Writes problems as FAILED lines, in the layout of report.hpp.
///
/// @param problems  What is wrong, one per entry.
/// @return The lines, ready for reply()'s narrative.
std::string describeProblems(const std::vector<std::string>& problems) {
    std::string text;
    for (const std::string& problem : problems) {
        text += "    FAILED   " + problem + '\n';
    }
    return text;
}

/// Tests whether a path lies within a directory.
///
/// Both are compared component by component, as given: the caller resolves
/// them first, so that neither `..` nor a symbolic link can lead out.
///
/// @param path  A resolved path.
/// @param root  A resolved directory, without a trailing separator, as
///              std::filesystem::canonical() gives it.
/// @return True when `path` is `root` or lies beneath it.
bool isWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
    return std::ranges::mismatch(root, path).in1 == root.end();
}

/// Tests whether a JSON value is a frame number: an integer from 0 to the
/// INT32 maximum, the range of Frame.frame_number.
///
/// @param value  The value of `frame`.
/// @return True when it is. A number with a fraction or exponent is not, even
///         when its value is whole, as a quoted or `1e3` frame is not in the
///         batch request.
bool isFrameNumber(const Json& value) {
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    // nlohmann/json stores a non-negative integer as unsigned, and a negative
    // one as signed.
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() <= kMax;
    }
    return false;
}

/// Finds the row count of one node table in a census.
///
/// @param census  What the database holds.
/// @param table   The node table's name.
/// @return Its row count; 0 when the table holds no rows, since a census
///         omits empty tables.
std::int64_t nodeCount(const Census& census, std::string_view table) {
    for (const auto& [name, count] : census.nodes) {
        if (name == table) {
            return count;
        }
    }
    return 0;
}

/// Reads a /render body into a frame request, checking it as the batch
/// request is checked (render_request.hpp).
///
/// Every problem is collected rather than the first: a frame that is exactly
/// an integer in range, labels that are a set of non-empty names, and no
/// other key.
///
/// @param request    The body, already known to be a JSON object.
/// @param[out] frame Receives the frame number and labels that were usable.
/// @return Everything wrong with the body; empty when `frame` is complete.
std::vector<std::string> readFrameRequest(const Json& request, FrameRequest& frame) {
    std::vector<std::string> problems = unknownKeys(request, {"frame", "labels"});
    const auto number = request.find("frame");
    if (number == request.end()) {
        problems.emplace_back("missing key 'frame'");
    } else if (!isFrameNumber(*number)) {
        problems.push_back("'frame' must be an integer from 0 to " +
                           std::to_string(std::numeric_limits<std::int32_t>::max()) + ", not " +
                           number->dump());
    } else {
        frame.frameNumber = number->get<std::int32_t>();
    }

    const auto labels = request.find("labels");
    if (labels == request.end() || labels->is_null()) {
        return problems;
    }
    if (!labels->is_array()) {
        problems.emplace_back("'labels' must be an array of strings");
        return problems;
    }
    for (const Json& label : *labels) {
        if (!label.is_string() || label.get<std::string>().empty()) {
            problems.push_back("every label must be a non-empty string, not " + label.dump());
        } else {
            frame.labels.insert(label.get<std::string>());
        }
    }
    return problems;
}

/// A task queue that runs each task at once, on the thread that enqueues it.
///
/// httplib::Server hands every accepted connection to its task queue; its
/// default is a pool of worker threads. This one makes the accept loop handle
/// each connection itself before accepting the next, so calls are served
/// strictly one at a time and nothing runs on another thread (ADR 0016).
class InlineTaskQueue final : public http::TaskQueue {
public:
    /// Runs the task.
    ///
    /// @param task  The connection to handle.
    /// @return True, always: the task has run.
    bool enqueue(std::function<void()> task) override {
        task();
        return true;
    }

    /// Nothing to stop: there are no workers.
    void shutdown() override {}
};

} // namespace

ServerCore::ServerCore(ServerSettings settings, std::ostream& echo)
    : settings_{std::move(settings)}, echo_{echo} {
    settings_.inputRoot = std::filesystem::canonical(settings_.inputRoot);
    settings_.outputDir = std::filesystem::absolute(settings_.outputDir);
}

ServerCore::~ServerCore() = default;

HttpReply ServerCore::load(std::string_view body) {
    echo_ << "POST /load\n";
    std::ostringstream log;
    const auto finish = [&](int status, const std::string& message, Json extra = Json::object()) {
        return conclude(echo_, log.str(), status, message, std::move(extra));
    };

    const std::optional<Json> request = parseObject(body);
    if (!request.has_value()) {
        return finish(400, "the body is not a JSON object");
    }
    std::vector<std::string> problems = unknownKeys(*request, {"uri"});
    const auto uri = request->find("uri");
    if (uri == request->end()) {
        problems.emplace_back("missing key 'uri'");
    } else if (!uri->is_string()) {
        problems.emplace_back("'uri' is not a string");
    }
    if (!problems.empty()) {
        log << describeProblems(problems);
        return finish(400, "the body is not a usable load request");
    }

    std::filesystem::path stateFile;
    try {
        stateFile = fileUriToPath(uri->get<std::string>());
    } catch (const FileUriError& e) {
        return finish(400, e.what());
    }

    // Resolved before it is checked, so that neither `..` nor a symbolic link
    // can lead outside the input root. The input root was resolved likewise
    // when the server started.
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(stateFile, error);
    if (error) {
        return finish(404, "cannot resolve " + stateFile.string() + ": " + error.message());
    }
    stateFile = std::move(resolved);
    if (!isWithin(stateFile, settings_.inputRoot)) {
        return finish(
            403, stateFile.string() + " is outside the input root " + settings_.inputRoot.string());
    }
    if (!std::filesystem::is_regular_file(stateFile, error)) {
        return finish(404, stateFile.string() + " is not a file");
    }

    // The held score is discarded before the new one is loaded, not after:
    // a load that fails leaves the server holding nothing rather than a score
    // the caller has just asked to replace.
    db_.reset();
    try {
        auto db = std::make_unique<EngineDb>();
        log << "  schema:    " << settings_.schemaFile.string() << '\n'
            << "  state:     " << stateFile.string() << '\n'
            << '\n';
        const LoadResult loaded =
            loadScore(*db, settings_.schemaFile, stateFile, settings_.pipeline, log);
        if (!loaded.ok()) {
            return finish(422, loaded.verdict);
        }
        const Census census = db->census();
        db_ = std::move(db);
        const std::string fileUri = pathToFileUri(stateFile);
        return finish(200, "loaded " + fileUri,
            {{"uri", fileUri}, {"nodes", census.totalNodes},
                {"relationships", census.totalRelationships},
                {"partials", nodeCount(census, "Partial")}});
    } catch (const std::exception& e) {
        return finish(500, e.what());
    }
}

HttpReply ServerCore::render(std::string_view body) {
    echo_ << "POST /render\n";
    std::ostringstream log;
    const auto finish = [&](int status, const std::string& message, Json extra = Json::object()) {
        return conclude(echo_, log.str(), status, message, std::move(extra));
    };

    const std::optional<Json> request = parseObject(body);
    if (!request.has_value()) {
        return finish(400, "the body is not a JSON object");
    }

    FrameRequest frame;
    const std::vector<std::string> problems = readFrameRequest(*request, frame);
    if (!problems.empty()) {
        log << describeProblems(problems);
        return finish(400, "the body is not a usable render request");
    }

    if (!db_) {
        return finish(409, "no score is loaded; POST /load first");
    }

    try {
        // The request's "file" names it in diagnostics, of which only the last
        // path component is shown: "request body: frame 3 names label 'x',
        // which engine-db does not hold".
        const RenderRequest one{.file = std::filesystem::path{"request body"}, .frames = {frame}};
        const RenderResult rendered =
            renderRequestedFrames(*db_, one, settings_.outputDir, settings_.pipeline, log);
        switch (rendered.stage) {
        case RenderStage::Complete:
            break;
        case RenderStage::OutputDirectory:
            return finish(500, rendered.verdict);
        case RenderStage::UnknownLabels:
        case RenderStage::Rendering:
            return finish(422, rendered.verdict);
        }

        const FrameOutcome& outcome = rendered.report.frames.front();
        const std::string fileUri = pathToFileUri(outcome.file);
        Json extra{{"frame", outcome.frame.value()}, {"uri", fileUri}};
        if (settings_.pipeline.perSetWav) {
            Json setUris = Json::array();
            for (const auto& [setId, file] : outcome.setFiles) {
                setUris.push_back(pathToFileUri(file));
            }
            extra["set_uris"] = std::move(setUris);
        }
        return finish(200, "rendered " + fileUri, std::move(extra));
    } catch (const std::exception& e) {
        return finish(500, e.what());
    }
}

HttpReply ServerCore::shutdown() {
    echo_ << "POST /shutdown\n\nshutting down\n\n" << std::flush;
    return reply(200, "shutting down", "");
}

bool runServer(const Options& options) {
    // The output directory is created before listening, so that a server
    // that cannot write fails at once rather than at its first /render.
    std::error_code error;
    std::filesystem::create_directories(options.outputDir, error);
    if (error) {
        std::cerr << "rendering-engine: cannot create --out-dir " << options.outputDir.string()
                  << ": " << error.message() << '\n';
        return false;
    }

    ServerCore core{ServerSettings{.schemaFile = options.schemaFile,
                        .inputRoot = options.inputRoot,
                        .outputDir = options.outputDir,
                        .pipeline = {.verbose = options.verbose,
                            .validateDb = options.validateDb,
                            .perSetWav = options.perSetWav}},
        std::cout};

    http::Server server;
    server.new_task_queue = [] {
        // httplib's interface: it takes ownership of the raw pointer.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new InlineTaskQueue;
    };
    // One request per connection. A connection kept alive would hold the
    // accept loop, and with it every other caller, until the client let go.
    server.set_keep_alive_max_count(1);

    const auto respond = [](http::Response& response, const HttpReply& answer) {
        response.status = answer.status;
        response.set_content(answer.body, "application/json");
    };
    server.Post("/load", [&](const http::Request& request, http::Response& response) {
        respond(response, core.load(request.body));
    });
    server.Post("/render", [&](const http::Request& request, http::Response& response) {
        respond(response, core.render(request.body));
    });
    // stop() only closes the listening socket. This response is still
    // written, and the accept loop then finds nothing to accept and returns.
    server.Post("/shutdown", [&](const http::Request&, http::Response& response) {
        respond(response, core.shutdown());
        server.stop();
    });
    // Anything else gets a JSON body too, so that a caller can always parse
    // what comes back.
    server.set_error_handler([](const http::Request& request, http::Response& response) {
        if (response.body.empty()) {
            const HttpReply answer = reply(response.status,
                "no endpoint " + request.method + ' ' + request.path +
                    "; the endpoints are POST /load, POST /render and POST /shutdown",
                "");
            response.set_content(answer.body, "application/json");
        }
    });

    int port = options.port;
    if (port == 0) {
        port = server.bind_to_any_port(options.host);
    } else if (!server.bind_to_port(options.host, port)) {
        port = -1;
    }
    if (port < 0) {
        std::cerr << "rendering-engine: cannot listen on " << options.host << ':' << options.port
                  << '\n';
        return false;
    }

    std::cout << "rendering-engine, mode " << modeName(options.mode) << '\n'
              << "  schema:     " << options.schemaFile.string() << '\n'
              << "  input-root: " << options.inputRoot.string() << '\n'
              << "  out-dir:    " << options.outputDir.string() << '\n'
              << "  listening on http://" << options.host << ':' << port << "\n\n"
              << std::flush;

    if (!server.listen_after_bind()) {
        std::cerr << "rendering-engine: the server stopped accepting connections\n";
        return false;
    }
    std::cout << "rendering-engine: shut down\n";
    return true;
}

} // namespace rainbohz::rendering_engine
