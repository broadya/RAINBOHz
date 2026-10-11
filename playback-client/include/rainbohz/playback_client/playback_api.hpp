// SPDX-License-Identifier: Apache-2.0

#pragma once

// The REST calls the client makes.
//
// Named for the API, not for the component that answers it. Today the server
// is rendering-engine in server mode (rendering-engine/api/openapi.yaml). In
// the target architecture it is playback-server: it sums the audio of many
// rendering-engine instances, and has a REST interface very close to the
// engine's, behind gateway features such as rate limiting. Keeping the
// client's view of the calls behind this interface means that moving to it
// should be a change of --server, or of one implementation of this class,
// and nothing in the lane logic. See docs/design.md §1.
//
// The interface is abstract so that the tests can drive the threads with a
// scripted fake. The HTTP implementation is the only code that includes
// cpp-httplib or nlohmann/json.

#include "rainbohz/playback_client/frame_plan.hpp"
#include "rainbohz/playback_client/musical_position.hpp"
#include "rainbohz/playback_client/units.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rainbohz::playback_client {

/// Thrown when a call fails: the server could not be reached, it answered
/// with an error, or its answer is not what the API document says.
///
/// what() is a complete one-line explanation, including the server's problem
/// detail and every entry of its `errors` list, and for a 409 the hint that a
/// score has to be loaded first.
class PlaybackApiError : public std::runtime_error {
public:
    /// An error with explanation `message`.
    ///
    /// @param message  The explanation.
    /// @param status   The HTTP status, or 0 when there was no answer.
    PlaybackApiError(const std::string& message, int status)
        : std::runtime_error{message}, status_{status} {}

    /// The HTTP status of the answer.
    ///
    /// @return The status, or 0 when there was no answer.
    int status() const noexcept { return status_; }

private:
    /// The HTTP status of the answer; 0 when there was no answer.
    int status_;
};

/// The engine parameters, GET /engine-config.
struct EngineParameters {
    /// Samples per second.
    SampleRate sampleRate{1};
    /// Samples per frame, resolved.
    FrameSize frameSize{1};
    /// `mono`, `stereo`, `mid-side`, `foa-a-format` or `foa-b-format`.
    std::string channelMode;
};

/// The root tempo map, GET /tempo-maps/root.
struct TempoMapSummary {
    /// TempoMap.id, valid for the loaded score.
    std::int64_t id = 0;
    /// The map's name; empty when NULL.
    std::string name;
    /// Ticks per beat; empty when NULL.
    std::optional<std::int32_t> ticksPerBeat;
};

/// The REST calls of a playback session.
class PlaybackApi {
public:
    /// Destroys the client.
    virtual ~PlaybackApi() = default;

    PlaybackApi() = default;
    PlaybackApi(const PlaybackApi&) = delete;
    PlaybackApi& operator=(const PlaybackApi&) = delete;
    PlaybackApi(PlaybackApi&&) = delete;
    PlaybackApi& operator=(PlaybackApi&&) = delete;

    /// The engine parameters of the loaded score.
    ///
    /// @return The parameters.
    /// @throws PlaybackApiError if the call fails.
    virtual EngineParameters engineParameters() = 0;

    /// The root tempo map of the loaded score.
    ///
    /// @return The map.
    /// @throws PlaybackApiError if the call fails.
    virtual TempoMapSummary rootTempoMap() = 0;

    /// Where a musical position falls in the sample stream.
    ///
    /// @param tempoMapId  The map, from rootTempoMap().
    /// @param at          The position on that map.
    /// @return The sample at or before the position, as frame and offset.
    /// @throws PlaybackApiError if the call fails.
    virtual FrameOffset framePosition(std::int64_t tempoMapId, const MusicalPosition& at) = 0;

    /// Renders one frame, and returns the URI of the object holding it.
    ///
    /// @param frame  The frame.
    /// @return The object's URI: today a `file://` URI on the server's
    ///         machine; in the target, object storage.
    /// @throws PlaybackApiError if the call fails.
    virtual std::string renderFrame(FrameNumber frame) = 0;

    /// Ends any call in flight, and makes every later call fail at once.
    ///
    /// The one member that may be called from another thread while a call
    /// runs: the session calls it from the main thread when it stops, so that
    /// a stop does not wait for a slow render to finish or time out. The
    /// interrupted call throws PlaybackApiError. Safe to call any number of
    /// times; each call also ends any call that has started since the last.
    /// It may wait while a call is still connecting, for at most the connect
    /// timeout.
    virtual void cancel() noexcept = 0;
};

/// Makes the HTTP implementation of PlaybackApi.
///
/// @param serverUrl  The API's base URL, `/v1` included, as the OpenAPI
///                   document's server URL: `http://host:port/v1`. Only
///                   `http` is accepted; a trailing `/` is ignored.
/// @return The client. It connects per call, and keeps no connection open
///         between calls. One call at a time: it is not thread-safe, and the
///         session uses it from one thread at a time, except for cancel(),
///         which may be called from any thread.
/// @throws PlaybackApiError if `serverUrl` is not an `http://` URL with a
///         host.
std::unique_ptr<PlaybackApi> makeHttpPlaybackApi(const std::string& serverUrl);

} // namespace rainbohz::playback_client
