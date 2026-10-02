#include "rainbohz/rendering_engine/server.hpp"

#include "rainbohz/rendering_engine/file_uri.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

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
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
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
/// a call's parameters and body into a reply, running the steps of
/// score_pipeline.hpp or the queries of timing_query.hpp; runServer() binds
/// the routes to it on an httplib::Server whose task queue runs every
/// connection inline on the accepting thread.
///
/// @see server.hpp, api/openapi.yaml, docs/server.md

namespace rainbohz::rendering_engine {

namespace {

using transform::BeatsTicks;
using transform::channelModeName;
using transform::EngineParameters;
using transform::FramePosition;
using transform::parseBeatsTicks;
using transform::readEngineParameters;
using transform::readRootTempoMap;
using transform::readTempoMapChildren;
using transform::readTempoMapPoints;
using transform::TempoMapChild;
using transform::TempoMapInfo;
using transform::TempoMapNode;
using transform::TempoMapPointInfo;
using transform::TempoMapPointPage;
using transform::TimingQueryError;

/// The JSON value type of nlohmann/json.
using Json = nlohmann::json;

/// cpp-httplib, under the namespace it is included as here.
namespace http = rainbohz_httplib;

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------

/// The reason phrase of a status the server sends (RFC 9110), used as a
/// problem's `title`.
///
/// @param status  An HTTP status code.
/// @return Its reason phrase; "Error" for one the server never sends.
std::string_view reasonPhrase(int status) noexcept {
    switch (status) {
    case 400:
        return "Bad Request";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 422:
        return "Unprocessable Content";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

/// Builds a successful reply: the resource itself.
///
/// @param status    The HTTP status, below 400.
/// @param resource  The resource, a JSON object.
/// @return The reply, its body serialised as one line of JSON.
HttpReply resource(int status, const Json& resource) {
    return {.status = status, .body = resource.dump(), .contentType = "application/json"};
}

/// Builds a failed reply: an RFC 9457 problem document.
///
/// The type is `about:blank`, so the title is the status's reason phrase and
/// `detail` says what went wrong in this call.
///
/// @param status  The HTTP status, 400 or above.
/// @param detail  The one-line verdict.
/// @param errors  Each thing wrong with the request, when there are several;
///                added as the `errors` extension member when not empty.
/// @return The reply, as application/problem+json.
HttpReply problem(
    int status, const std::string& detail, const std::vector<std::string>& errors = {}) {
    Json body{{"type", "about:blank"}, {"title", reasonPhrase(status)}, {"status", status},
        {"detail", detail}};
    if (!errors.empty()) {
        body["errors"] = errors;
    }
    return {.status = status, .body = body.dump(), .contentType = "application/problem+json"};
}

/// Ends a call: echoes its narrative and verdict, and passes its reply on.
///
/// @param echo       Where the narrative is echoed, followed by the verdict --
///                   prefixed "rendering-engine: " on a failure, as batch mode
///                   prints its verdict -- and a blank line. Flushed, so that
///                   whoever watches the server sees each call as it ends.
/// @param narrative  The call's narrative; may be empty.
/// @param verdict    The one-line summary.
/// @param answer     The reply.
/// @return `answer`.
HttpReply conclude(std::ostream& echo, const std::string& narrative, const std::string& verdict,
    HttpReply answer) {
    if (!narrative.empty()) {
        echo << narrative << '\n';
    }
    echo << (answer.status < 400 ? "" : "rendering-engine: ") << verdict << "\n\n" << std::flush;
    return answer;
}

/// The status a TimingQueryError stands for.
///
/// @param kind  Whose fault the failure is.
/// @return 400, 404 or 422.
int statusOf(TimingQueryError::Kind kind) noexcept {
    switch (kind) {
    case TimingQueryError::Kind::BadRequest:
        return 400;
    case TimingQueryError::Kind::NotFound:
        return 404;
    case TimingQueryError::Kind::Unprocessable:
        return 422;
    }
    return 500;
}

/// A JSON value for a nullable property: the value, or null.
///
/// @param value  The property as read.
/// @return The value, or JSON null when it is nullopt.
template <typename T>
Json nullable(const std::optional<T>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

/// The JSON form of a TempoMap.
///
/// @param map  The map.
/// @return `{id, name, ticks_per_beat}`.
Json mapJson(const TempoMapInfo& map) {
    return Json{{"id", map.id}, {"name", nullable(map.name)},
        {"ticks_per_beat", nullable(map.ticksPerBeat)}};
}

/// The JSON form of a list of children.
///
/// @param children  The children of one map.
/// @return An array of `{id, name, ticks_per_beat, sync_point_parent,
///         sync_point_child}`.
Json childrenJson(const std::vector<TempoMapChild>& children) {
    Json list = Json::array();
    for (const TempoMapChild& child : children) {
        Json entry = mapJson(child.map);
        entry["sync_point_parent"] = nullable(child.syncPointParent);
        entry["sync_point_child"] = nullable(child.syncPointChild);
        list.push_back(std::move(entry));
    }
    return list;
}

// ---------------------------------------------------------------------------
// Reading requests
// ---------------------------------------------------------------------------

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

/// Lists what is wrong with a call's query parameters, as problems.
///
/// @param query    The query parameters.
/// @param allowed  The names the call accepts, each at most once.
/// @return One problem per unknown name and per name given more than once.
std::vector<std::string> checkQuery(
    const QueryParameters& query, const std::set<std::string>& allowed) {
    std::vector<std::string> problems;
    for (auto at = query.begin(); at != query.end(); at = query.upper_bound(at->first)) {
        if (!allowed.contains(at->first)) {
            problems.push_back("unknown query parameter '" + at->first + "'");
        } else if (query.count(at->first) > 1) {
            problems.push_back("query parameter '" + at->first + "' is given more than once");
        }
    }
    return problems;
}

/// The value of a query parameter given at most once.
///
/// @param query  The query parameters, already checked by checkQuery().
/// @param name   The parameter's name.
/// @return Its value; nullopt when it is absent.
std::optional<std::string> queryValue(const QueryParameters& query, const std::string& name) {
    const auto at = query.find(name);
    return at == query.end() ? std::nullopt : std::optional<std::string>{at->second};
}

/// Writes problems as FAILED lines, in the layout of report.hpp.
///
/// @param problems  What is wrong, one per entry.
/// @return The lines, for the call's narrative.
std::string describeProblems(const std::vector<std::string>& problems) {
    std::string text;
    for (const std::string& problem : problems) {
        text += "    FAILED   " + problem + '\n';
    }
    return text;
}

/// Parses an unsigned decimal integer, digits only, no larger than `max`.
///
/// @param text  The text, e.g. a path segment.
/// @param max   The largest value accepted.
/// @return The value; nullopt when `text` is empty, holds anything but
///         digits, or exceeds `max`.
std::optional<std::int64_t> parseUnsigned(std::string_view text, std::int64_t max) noexcept {
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* const last = std::to_address(text.cend());
    const auto [end, error] = std::from_chars(std::to_address(text.cbegin()), last, value);
    if (error != std::errc{} || end != last || value > max) {
        return std::nullopt;
    }
    return value;
}

/// Reads the `{id}` of a tempo-map path.
///
/// @param id             The path segment.
/// @param[out] problems  Receives what is wrong with it, if anything.
/// @return The TempoMap.id; nullopt when the segment is not one.
std::optional<std::int64_t> readMapId(std::string_view id, std::vector<std::string>& problems) {
    const std::optional<std::int64_t> mapId =
        parseUnsigned(id, std::numeric_limits<std::int64_t>::max());
    if (!mapId.has_value()) {
        problems.push_back(
            "tempo map id '" + std::string{id} + "' is not a non-negative decimal integer");
    }
    return mapId;
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

/// Reads a render call into a frame request, checking it as the batch
/// request is checked (render_request.hpp).
///
/// Every problem is collected rather than the first: a frame that is exactly
/// an integer in range, a body that is empty or an object, labels that are a
/// set of non-empty names, and no other key.
///
/// @param frameText  The `{frame}` path segment.
/// @param body       The request body.
/// @param[out] frame Receives the frame number and labels that were usable.
/// @return Everything wrong with the call; empty when `frame` is complete.
std::vector<std::string> readFrameRequest(
    std::string_view frameText, std::string_view body, FrameRequest& frame) {
    std::vector<std::string> problems;
    const std::optional<std::int64_t> number =
        parseUnsigned(frameText, std::numeric_limits<std::int32_t>::max());
    if (number.has_value()) {
        frame.frameNumber = static_cast<std::int32_t>(*number);
    } else {
        problems.push_back("frame '" + std::string{frameText} + "' must be an integer from 0 to " +
                           std::to_string(std::numeric_limits<std::int32_t>::max()));
    }

    // No body at all asks for every partial, as an empty object does.
    if (body.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return problems;
    }
    const std::optional<Json> request = parseObject(body);
    if (!request.has_value()) {
        problems.emplace_back("the body is neither empty nor a JSON object");
        return problems;
    }
    const std::vector<std::string> unknown = unknownKeys(*request, {"labels"});
    problems.insert(problems.end(), unknown.begin(), unknown.end());

    const auto labels = request->find("labels");
    if (labels == request->end() || labels->is_null()) {
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

/// Answers a read call: one that queries the loaded score and changes
/// nothing.
///
/// Refuses the call with 400 when its request has problems, then with 409
/// when no score is loaded; otherwise runs `answer`, turning a
/// TimingQueryError into the status of its kind and any other exception into
/// 500.
///
/// @tparam Answer  Callable as `Json(EngineDb&, std::string& verdict)`,
///                 returning the resource and setting the verdict.
/// @param echo      Where the call and its verdict are echoed.
/// @param call      The call's method and path, e.g. "GET /v1/engine-config".
/// @param db        The loaded score; null when none is loaded.
/// @param problems  Everything wrong with the request.
/// @param answer    Builds the resource.
/// @return The reply.
template <typename Answer>
HttpReply answerRead(std::ostream& echo, const std::string& call, EngineDb* db,
    const std::vector<std::string>& problems, Answer answer) {
    echo << call << '\n';
    if (!problems.empty()) {
        const std::string detail =
            problems.size() == 1 ? problems.front() : "the request is not usable";
        return conclude(echo, describeProblems(problems), detail, problem(400, detail, problems));
    }
    if (db == nullptr) {
        const std::string detail = "no score is loaded; PUT /v1/score first";
        return conclude(echo, "", detail, problem(409, detail));
    }
    try {
        std::string verdict;
        const Json body = answer(*db, verdict);
        return conclude(echo, "", verdict, resource(200, body));
    } catch (const TimingQueryError& e) {
        return conclude(echo, "", e.what(), problem(statusOf(e.kind()), e.what()));
    } catch (const std::exception& e) {
        return conclude(echo, "", e.what(), problem(500, e.what()));
    }
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

HttpReply ServerCore::putScore(std::string_view body) {
    echo_ << "PUT /v1/score\n";
    std::ostringstream log;
    const auto fail = [&](int status, const std::string& detail,
                          const std::vector<std::string>& errors = {}) {
        return conclude(echo_, log.str(), detail, problem(status, detail, errors));
    };

    const std::optional<Json> request = parseObject(body);
    if (!request.has_value()) {
        return fail(400, "the body is not a JSON object");
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
        return fail(400, "the body is not a usable score request", problems);
    }

    std::filesystem::path stateFile;
    try {
        stateFile = fileUriToPath(uri->get<std::string>());
    } catch (const FileUriError& e) {
        return fail(400, e.what());
    }

    // Resolved before it is checked, so that neither `..` nor a symbolic link
    // can lead outside the input root. The input root was resolved likewise
    // when the server started.
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(stateFile, error);
    if (error) {
        return fail(404, "cannot resolve " + stateFile.string() + ": " + error.message());
    }
    stateFile = std::move(resolved);
    if (!isWithin(stateFile, settings_.inputRoot)) {
        return fail(
            403, stateFile.string() + " is outside the input root " + settings_.inputRoot.string());
    }
    if (!std::filesystem::is_regular_file(stateFile, error)) {
        return fail(404, stateFile.string() + " is not a file");
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
            return fail(422, loaded.verdict);
        }
        const Census census = db->census();
        db_ = std::move(db);
        const std::string fileUri = pathToFileUri(stateFile);
        return conclude(echo_, log.str(), "loaded " + fileUri,
            resource(200, {{"uri", fileUri}, {"nodes", census.totalNodes},
                              {"relationships", census.totalRelationships},
                              {"partials", nodeCount(census, "Partial")}}));
    } catch (const std::exception& e) {
        return fail(500, e.what());
    }
}

HttpReply ServerCore::renderFrame(std::string_view frameText, std::string_view body) {
    echo_ << "POST /v1/frames/" << frameText << "/renderings\n";
    std::ostringstream log;
    const auto fail = [&](int status, const std::string& detail,
                          const std::vector<std::string>& errors = {}) {
        return conclude(echo_, log.str(), detail, problem(status, detail, errors));
    };

    FrameRequest frame;
    const std::vector<std::string> problems = readFrameRequest(frameText, body, frame);
    if (!problems.empty()) {
        log << describeProblems(problems);
        return fail(
            400, problems.size() == 1 ? problems.front() : "the request is not usable", problems);
    }

    if (!db_) {
        return fail(409, "no score is loaded; PUT /v1/score first");
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
            return fail(500, rendered.verdict);
        case RenderStage::UnknownLabels:
        case RenderStage::Rendering:
            return fail(422, rendered.verdict);
        }

        const FrameOutcome& outcome = rendered.report.frames.front();
        const std::string fileUri = pathToFileUri(outcome.file);
        Json answer{{"frame", outcome.frame.value()}, {"uri", fileUri}};
        if (settings_.pipeline.perSetWav) {
            Json setUris = Json::array();
            for (const auto& [setId, file] : outcome.setFiles) {
                setUris.push_back(pathToFileUri(file));
            }
            answer["set_uris"] = std::move(setUris);
        }
        return conclude(echo_, log.str(), "rendered " + fileUri, resource(200, answer));
    } catch (const std::exception& e) {
        return fail(500, e.what());
    }
}

HttpReply ServerCore::shutdown() {
    echo_ << "POST /v1/shutdown\n";
    return conclude(echo_, "", "shutting down", resource(200, {{"message", "shutting down"}}));
}

HttpReply ServerCore::tempoMapRoot(const QueryParameters& query) {
    return answerRead(echo_, "GET /v1/tempo-maps/root", db_.get(), checkQuery(query, {}),
        [](EngineDb& db, std::string& verdict) {
            const TempoMapNode root = readRootTempoMap(db);
            Json body = mapJson(root.map);
            body["children"] = childrenJson(root.children);
            verdict = "root TempoMap " + std::to_string(root.map.id) + ", " +
                      std::to_string(root.children.size()) + " child(ren)";
            return body;
        });
}

HttpReply ServerCore::tempoMapChildren(std::string_view id, const QueryParameters& query) {
    std::vector<std::string> problems;
    const std::optional<std::int64_t> mapId = readMapId(id, problems);
    const std::vector<std::string> queryProblems = checkQuery(query, {});
    problems.insert(problems.end(), queryProblems.begin(), queryProblems.end());
    return answerRead(echo_, "GET /v1/tempo-maps/" + std::string{id} + "/children", db_.get(),
        problems, [&](EngineDb& db, std::string& verdict) {
            const std::vector<TempoMapChild> children = readTempoMapChildren(db, *mapId);
            verdict = "TempoMap " + std::to_string(*mapId) + ": " +
                      std::to_string(children.size()) + " child(ren)";
            return Json{{"tempo_map_id", *mapId}, {"children", childrenJson(children)}};
        });
}

HttpReply ServerCore::tempoMapPoints(std::string_view id, const QueryParameters& query) {
    std::vector<std::string> problems;
    const std::optional<std::int64_t> mapId = readMapId(id, problems);
    const std::vector<std::string> queryProblems = checkQuery(query, {"after", "limit"});
    problems.insert(problems.end(), queryProblems.begin(), queryProblems.end());

    std::optional<std::int64_t> after;
    if (const std::optional<std::string> text = queryValue(query, "after"); text.has_value()) {
        after = parseUnsigned(*text, std::numeric_limits<std::int64_t>::max());
        if (!after.has_value()) {
            problems.push_back(
                "'after' must be a position in ticks, a non-negative integer, not '" + *text + "'");
        }
    }
    std::int32_t limit = kDefaultPointsLimit;
    if (const std::optional<std::string> text = queryValue(query, "limit"); text.has_value()) {
        const std::optional<std::int64_t> parsed = parseUnsigned(*text, kMaxPointsLimit);
        if (parsed.has_value() && *parsed >= 1) {
            limit = static_cast<std::int32_t>(*parsed);
        } else {
            problems.push_back("'limit' must be an integer from 1 to " +
                               std::to_string(kMaxPointsLimit) + ", not '" + *text + "'");
        }
    }

    return answerRead(echo_, "GET /v1/tempo-maps/" + std::string{id} + "/points", db_.get(),
        problems, [&](EngineDb& db, std::string& verdict) {
            const TempoMapPointPage page = readTempoMapPoints(db, *mapId, after, limit);
            Json points = Json::array();
            for (const TempoMapPointInfo& point : page.points) {
                points.push_back(Json{{"position", nullable(point.position)},
                    {"start_tempo", nullable(point.startTempo)}, {"hold", nullable(point.hold)}});
            }
            Json next = nullptr;
            if (page.nextAfter.has_value()) {
                next = "/v1/tempo-maps/" + std::to_string(*mapId) +
                       "/points?after=" + std::to_string(*page.nextAfter) +
                       "&limit=" + std::to_string(limit);
            }
            verdict = "TempoMap " + std::to_string(*mapId) + ": " +
                      std::to_string(page.points.size()) + " point(s)" +
                      (page.nextAfter.has_value() ? ", more follow" : "");
            return Json{{"tempo_map_id", *mapId},
                {"ticks_per_beat", nullable(page.map.ticksPerBeat)}, {"points", std::move(points)},
                {"next", std::move(next)}};
        });
}

HttpReply ServerCore::framePosition(std::string_view id, const QueryParameters& query) {
    std::vector<std::string> problems;
    const std::optional<std::int64_t> mapId = readMapId(id, problems);
    const std::vector<std::string> queryProblems = checkQuery(query, {"at"});
    problems.insert(problems.end(), queryProblems.begin(), queryProblems.end());

    std::optional<BeatsTicks> at;
    const std::optional<std::string> text = queryValue(query, "at");
    if (!text.has_value()) {
        problems.emplace_back("missing query parameter 'at'");
    } else {
        at = parseBeatsTicks(*text);
        if (!at.has_value()) {
            problems.push_back(
                "'at' must be beats:ticks, two non-negative integers, or a number of beats, not '" +
                *text + "'");
        }
    }

    return answerRead(echo_, "GET /v1/tempo-maps/" + std::string{id} + "/frame-position", db_.get(),
        problems, [&](EngineDb& db, std::string& verdict) {
            const FramePosition position = transform::framePosition(db, *mapId, *at);
            const std::string written = std::to_string(at->beats) + ":" + std::to_string(at->ticks);
            verdict = "TempoMap " + std::to_string(*mapId) + " at " + written + ": frame " +
                      std::to_string(position.frame.value()) + ", offset " +
                      std::to_string(position.offset.value());
            return Json{{"tempo_map_id", *mapId}, {"at", written},
                {"frame", position.frame.value()}, {"offset", position.offset.value()}};
        });
}

HttpReply ServerCore::engineConfig(const QueryParameters& query) {
    return answerRead(echo_, "GET /v1/engine-config", db_.get(), checkQuery(query, {}),
        [](EngineDb& db, std::string& verdict) {
            const EngineParameters parameters = readEngineParameters(db);
            const std::string_view mode = channelModeName(parameters.channelMode);
            verdict = std::to_string(parameters.sampleRate.value()) + " Hz, frames of " +
                      std::to_string(parameters.frameSize.value()) + " samples, " +
                      std::string{mode};
            return Json{{"sample_rate", parameters.sampleRate.value()},
                {"frame_size", parameters.frameSize.value()}, {"channel_mode", mode}};
        });
}

bool runServer(const Options& options) {
    // The output directory is created before listening, so that a server
    // that cannot write fails at once rather than at its first render.
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
        response.set_content(answer.body, answer.contentType);
    };
    const auto queryOf = [](const http::Request& request) {
        QueryParameters query;
        for (const auto& [name, value] : request.params) {
            query.emplace(name, value);
        }
        return query;
    };
    const auto idOf = [](const http::Request& request) { return request.path_params.at("id"); };

    // Every route, by path and method. Paths are cpp-httplib patterns, in
    // which `:name` is a path parameter.
    using Handler = std::function<void(const http::Request&, http::Response&)>;
    const std::map<std::string, std::map<std::string, Handler>> routes{
        {"/v1/score", {{"PUT",
                          [&](const http::Request& request, http::Response& response) {
                              respond(response, core.putScore(request.body));
                          }}}},
        {"/v1/frames/:frame/renderings",
            {{"POST",
                [&](const http::Request& request, http::Response& response) {
                    respond(
                        response, core.renderFrame(request.path_params.at("frame"), request.body));
                }}}},
        // stop() only closes the listening socket. This response is still
        // written, and the accept loop then finds nothing to accept and
        // returns.
        {"/v1/shutdown", {{"POST",
                             [&](const http::Request&, http::Response& response) {
                                 respond(response, core.shutdown());
                                 server.stop();
                             }}}},
        {"/v1/tempo-maps/root", {{"GET",
                                    [&](const http::Request& request, http::Response& response) {
                                        respond(response, core.tempoMapRoot(queryOf(request)));
                                    }}}},
        {"/v1/tempo-maps/:id/children",
            {{"GET",
                [&](const http::Request& request, http::Response& response) {
                    respond(response, core.tempoMapChildren(idOf(request), queryOf(request)));
                }}}},
        {"/v1/tempo-maps/:id/points",
            {{"GET",
                [&](const http::Request& request, http::Response& response) {
                    respond(response, core.tempoMapPoints(idOf(request), queryOf(request)));
                }}}},
        {"/v1/tempo-maps/:id/frame-position",
            {{"GET",
                [&](const http::Request& request, http::Response& response) {
                    respond(response, core.framePosition(idOf(request), queryOf(request)));
                }}}},
        {"/v1/engine-config", {{"GET",
                                  [&](const http::Request& request, http::Response& response) {
                                      respond(response, core.engineConfig(queryOf(request)));
                                  }}}},
    };

    // Each route's own methods are bound to its handler; every other method
    // on the same path gets 405 and an Allow header naming the right ones.
    // The handlers refer to `allowed` and `binders`, which outlive the server.
    using Binder = http::Server& (http::Server::*)(const std::string&, http::Server::Handler);
    const std::map<std::string, Binder> binders{{"GET", &http::Server::Get},
        {"POST", &http::Server::Post}, {"PUT", &http::Server::Put}, {"PATCH", &http::Server::Patch},
        {"DELETE", &http::Server::Delete}, {"OPTIONS", &http::Server::Options}};
    std::map<std::string, std::string> allowed;
    for (const auto& [path, methods] : routes) {
        std::string& allow = allowed[path];
        for (const auto& [method, handler] : methods) {
            allow += (allow.empty() ? "" : ", ") + method;
        }
    }
    for (const auto& [path, methods] : routes) {
        const std::string* const allow = &allowed.at(path);
        for (const auto& [method, bind] : binders) {
            const auto own = methods.find(method);
            if (own != methods.end()) {
                (server.*bind)(path, own->second);
                continue;
            }
            const std::string* const refused = &method;
            (server.*bind)(
                path, [allow, refused](const http::Request& request, http::Response& response) {
                    std::string detail = request.path;
                    detail += " does not take ";
                    detail += *refused;
                    detail += "; it takes ";
                    detail += *allow;
                    const HttpReply answer = conclude(std::cout, "", detail, problem(405, detail));
                    response.status = answer.status;
                    response.set_header("Allow", *allow);
                    response.set_content(answer.body, answer.contentType);
                });
        }
    }
    // Anything else gets a problem document too, so that a caller can always
    // parse what comes back.
    server.set_error_handler([](const http::Request& request, http::Response& response) {
        if (response.body.empty()) {
            const HttpReply answer =
                problem(response.status, "no endpoint " + request.method + ' ' + request.path +
                                             "; the endpoints are described by api/openapi.yaml");
            response.set_content(answer.body, answer.contentType);
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
