#include "rainbohz/playback_client/playback_api.hpp"

#include "rainbohz/playback_client/log.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/// @file
/// @brief The HTTP implementation of PlaybackApi, with cpp-httplib and
/// nlohmann/json.
///
/// This is the only translation unit that includes either library. Each call
/// is one request on a fresh connection, as the server expects (it serves one
/// call per connection; rendering-engine/docs/server.md §5). Every call is
/// timed and logged at debug level.
///
/// Answers are checked against what rendering-engine/api/openapi.yaml says
/// they hold: a field that is missing or of the wrong type is a
/// PlaybackApiError, not a crash or a silent default. Error answers are RFC
/// 9457 problem documents; their `detail` and `errors` are quoted in the
/// exception. A 409 adds the hint that a score has to be loaded, and a 429,
/// which a gateway in front of a future playback-server may give, is reported
/// as rate limiting; this version does not retry.
///
/// @see playback_api.hpp

namespace rainbohz::playback_client {

namespace {

using Json = nlohmann::json;

/// How long to wait for a connection to the server.
constexpr std::chrono::seconds kConnectTimeout{5};

/// How long to wait for any one answer. A render of a large frame in a debug
/// build can take a while; a call that takes longer than this has failed.
constexpr std::chrono::seconds kReadTimeout{300};

/// The parts of a server URL.
struct ServerUrl {
    /// `http://host:port`, for httplib::Client.
    std::string origin;
    /// The path every call is under, e.g. `/v1`; empty for the root.
    std::string basePath;
};

/// Splits a server URL into its origin and base path.
///
/// @param url  `http://host[:port][/path]`.
/// @return The parts, with any trailing `/` removed from the path.
/// @throws PlaybackApiError if `url` is not `http://` with a host, or has a
///         query or fragment.
ServerUrl splitServerUrl(std::string_view url) {
    constexpr std::string_view kScheme = "http://";
    if (!url.starts_with(kScheme)) {
        throw PlaybackApiError{
            "server URL '" + std::string{url} + "' is not an http:// URL (https is not supported)",
            0};
    }
    if (url.find_first_of("?#") != std::string_view::npos) {
        throw PlaybackApiError{"server URL '" + std::string{url} + "' has a query or fragment", 0};
    }
    const std::size_t pathStart = url.find('/', kScheme.size());
    const std::string_view origin = url.substr(0, pathStart);
    if (origin.size() == kScheme.size()) {
        throw PlaybackApiError{"server URL '" + std::string{url} + "' has no host", 0};
    }
    std::string basePath =
        pathStart == std::string_view::npos ? std::string{} : std::string{url.substr(pathStart)};
    while (!basePath.empty() && basePath.back() == '/') {
        basePath.pop_back();
    }
    return ServerUrl{.origin = std::string{origin}, .basePath = std::move(basePath)};
}

/// Percent-encodes a query parameter value.
///
/// @param value  The value.
/// @return `value` with every byte other than RFC 3986's unreserved
///         characters encoded.
std::string encodeQueryValue(std::string_view value) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string encoded;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        if (unreserved) {
            encoded += character;
        } else {
            encoded += '%';
            encoded += kHex[byte / 16];
            encoded += kHex[byte % 16];
        }
    }
    return encoded;
}

/// Builds the exception for an error answer.
///
/// @param what    The call, e.g. `GET /v1/engine-config`.
/// @param status  The HTTP status, 400 or more.
/// @param body    The answer's body, normally a problem document.
/// @return The exception, quoting the problem's `detail` and each of its
///         `errors`, or the raw body when it is not a problem document.
PlaybackApiError problemError(const std::string& what, int status, const std::string& body) {
    std::string message = what + " answered " + std::to_string(status);
    const Json problem = Json::parse(body, nullptr, false);
    if (problem.is_object() && problem.contains("detail") && problem["detail"].is_string()) {
        message += ": " + problem["detail"].get<std::string>();
        if (problem.contains("errors") && problem["errors"].is_array()) {
            for (const Json& error : problem["errors"]) {
                if (error.is_string()) {
                    message += "; " + error.get<std::string>();
                }
            }
        }
    } else if (!body.empty()) {
        message += ": " + body.substr(0, 200);
    }
    if (status == 409) {
        message += " (is a score loaded? PUT /v1/score first)";
    } else if (status == 429) {
        message += " (rate limited; this version does not retry)";
    }
    return PlaybackApiError{message, status};
}

/// Reads a required field of an answer.
///
/// @tparam T     The C++ type to read it as.
/// @param what   The call, for the diagnostic.
/// @param body   The answer, a JSON object.
/// @param key    The field.
/// @return The field's value.
/// @throws PlaybackApiError if the field is missing or not of type `T`.
template <typename T>
T requireField(const std::string& what, const Json& body, const char* key) {
    if (!body.contains(key)) {
        throw PlaybackApiError{what + ": the answer has no '" + key + "'", 200};
    }
    try {
        return body.at(key).get<T>();
    } catch (const Json::exception&) {
        throw PlaybackApiError{
            what + ": the answer's '" + key + "' is not of the expected type", 200};
    }
}

/// Reads a required integer field and checks that it fits an INT32 within
/// a range.
///
/// @param what     The call, for the diagnostic.
/// @param body     The answer, a JSON object.
/// @param key      The field.
/// @param minimum  The smallest value accepted.
/// @return The value.
/// @throws PlaybackApiError if the field is missing, not an integer, or out
///         of range.
std::int32_t requireInt32(
    const std::string& what, const Json& body, const char* key, std::int32_t minimum) {
    if (!body.contains(key) || !body.at(key).is_number_integer()) {
        throw PlaybackApiError{what + ": the answer has no integer '" + key + "'", 200};
    }
    const auto value = body.at(key).get<std::int64_t>();
    if (value < minimum || value > std::numeric_limits<std::int32_t>::max()) {
        throw PlaybackApiError{
            what + ": the answer's '" + key + "' (" + std::to_string(value) + ") is out of range",
            200};
    }
    return static_cast<std::int32_t>(value);
}

/// PlaybackApi over HTTP.
class HttpPlaybackApi final : public PlaybackApi {
public:
    /// A client for the API at `url`.
    ///
    /// @param url  The parts of the server URL.
    explicit HttpPlaybackApi(ServerUrl url) : url_{std::move(url)} {}

    EngineParameters engineParameters() override {
        const std::string what = "GET " + url_.basePath + "/engine-config";
        const Json body = call(what,
            [&](httplib::Client& client) { return client.Get(url_.basePath + "/engine-config"); });
        return EngineParameters{
            .sampleRate = SampleRate{requireInt32(what, body, "sample_rate", 1)},
            .frameSize = FrameSize{requireInt32(what, body, "frame_size", 1)},
            .channelMode = requireField<std::string>(what, body, "channel_mode")};
    }

    TempoMapSummary rootTempoMap() override {
        const std::string what = "GET " + url_.basePath + "/tempo-maps/root";
        const Json body = call(what, [&](httplib::Client& client) {
            return client.Get(url_.basePath + "/tempo-maps/root");
        });
        TempoMapSummary map{
            .id = requireField<std::int64_t>(what, body, "id"), .name = {}, .ticksPerBeat = {}};
        if (body.contains("name") && body["name"].is_string()) {
            map.name = body["name"].get<std::string>();
        }
        if (body.contains("ticks_per_beat") && !body["ticks_per_beat"].is_null()) {
            map.ticksPerBeat = requireInt32(what, body, "ticks_per_beat", 1);
        }
        return map;
    }

    FrameOffset framePosition(std::int64_t tempoMapId, const MusicalPosition& at) override {
        const std::string path =
            url_.basePath + "/tempo-maps/" + std::to_string(tempoMapId) +
            "/frame-position?at=" + encodeQueryValue(formatMusicalPosition(at));
        const std::string what = "GET " + path;
        const Json body = call(what, [&](httplib::Client& client) { return client.Get(path); });
        return FrameOffset{.frame = FrameNumber{requireInt32(what, body, "frame", 0)},
            .offset = requireInt32(what, body, "offset", 0)};
    }

    std::string renderFrame(FrameNumber frame) override {
        const std::string path =
            url_.basePath + "/frames/" + std::to_string(frame.value()) + "/renderings";
        const std::string what = "POST " + path;
        const Json body = call(what, [&](httplib::Client& client) {
            return client.Post(path, std::string{}, "application/json");
        });
        const auto rendered = requireInt32(what, body, "frame", 0);
        if (rendered != frame.value()) {
            throw PlaybackApiError{
                what + ": the answer is for frame " + std::to_string(rendered), 200};
        }
        return requireField<std::string>(what, body, "uri");
    }

private:
    /// Makes one call, logs it, and returns its parsed answer.
    ///
    /// @param what     The call, for the log and diagnostics.
    /// @param request  Makes the request on the client it is given.
    /// @return The answer's body, a JSON object.
    /// @throws PlaybackApiError if there is no answer, the status is not 2xx,
    ///         or the body is not a JSON object.
    /// @note Side effect: a debug log line with the status and time taken.
    template <typename Request>
    Json call(const std::string& what, const Request& request) {
        httplib::Client client{url_.origin};
        client.set_connection_timeout(kConnectTimeout);
        client.set_read_timeout(kReadTimeout);
        client.set_keep_alive(false);

        const auto started = std::chrono::steady_clock::now();
        const httplib::Result result = request(client);
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);

        if (!result) {
            throw PlaybackApiError{
                what + " to " + url_.origin + " failed: " + httplib::to_string(result.error()), 0};
        }
        logLine(LogLevel::Debug, what + " -> " + std::to_string(result->status) + " in " +
                                     std::to_string(elapsed.count()) + " ms");
        if (result->status < 200 || result->status >= 300) {
            throw problemError(what, result->status, result->body);
        }
        Json body = Json::parse(result->body, nullptr, false);
        if (!body.is_object()) {
            throw PlaybackApiError{what + ": the answer is not a JSON object", result->status};
        }
        return body;
    }

    ServerUrl url_;
};

} // namespace

std::unique_ptr<PlaybackApi> makeHttpPlaybackApi(const std::string& serverUrl) {
    return std::make_unique<HttpPlaybackApi>(splitServerUrl(serverUrl));
}

} // namespace rainbohz::playback_client
