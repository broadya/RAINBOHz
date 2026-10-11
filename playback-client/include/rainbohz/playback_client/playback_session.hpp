// SPDX-License-Identifier: Apache-2.0

#pragma once

// A playback session: plan the range with the server, then run the two
// threads until every loop is played, the session fails, or it is
// interrupted.
//
// Split in two so that each half can be tested on its own: planSession()
// makes the REST calls that fix the range (docs/design.md §3), and
// playSession() runs the lanes (§4).

#include "rainbohz/playback_client/audio_sink.hpp"
#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/frame_plan.hpp"
#include "rainbohz/playback_client/musical_position.hpp"
#include "rainbohz/playback_client/object_fetch.hpp"
#include "rainbohz/playback_client/playback_api.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace rainbohz::playback_client {

/// Thrown when the range cannot be played with the loaded score.
class SessionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Everything the server said that fixes what a session plays.
struct SessionPlan {
    /// The engine parameters.
    EngineParameters engine;
    /// The root tempo map, on which the positions are read.
    TempoMapSummary tempoMap;
    /// The range's first sample.
    FrameOffset start;
    /// One past the range's last sample.
    FrameOffset end;
    /// The frames of one pass.
    FramePlan frames;
};

/// Asks the server where the range [start, end) falls, and plans its frames.
///
/// Calls GET /engine-config, GET /tempo-maps/root, and GET
/// /tempo-maps/{id}/frame-position for each end, in that order.
///
/// @param api    The REST calls.
/// @param start  The range's start on the root map.
/// @param end    The range's end on the root map; after `start`.
/// @return The plan.
/// @throws PlaybackApiError if a call fails.
/// @throws SessionError if the channel mode is not mono, a position's ticks
///         are not below the map's ticks_per_beat, or the range is empty once
///         converted to samples.
/// @note Side effect: logs the plan at info level.
SessionPlan planSession(PlaybackApi& api, const MusicalPosition& start, const MusicalPosition& end);

/// How a session ended.
struct SessionResult {
    /// Loops handed to the output in full.
    std::int64_t loopsPlayed = 0;
    /// The first error, if the session failed.
    std::optional<std::string> error;
    /// Whether it was interrupted before the end.
    bool interrupted = false;

    /// Whether every loop was played.
    ///
    /// @return True when there was neither an error nor an interrupt.
    bool ok() const noexcept { return !error.has_value() && !interrupted; }
};

/// Plays `loops` loops of a planned range.
///
/// Starts the sink, runs the network thread and the feeder, and waits for
/// both. Polls `interrupt` about every 50 ms meanwhile: once it is true, the
/// session stops, the sink is aborted, and any REST call in flight is ended
/// with PlaybackApi::cancel() before its thread is joined. The same happens
/// when either thread fails.
///
/// @param api        The REST calls; used by the network thread, and
///                   cancelled from this one when the session stops.
/// @param fetcher    Fetches the rendered frames.
/// @param sink       The audio output, not yet started.
/// @param plan       What to play, from planSession().
/// @param buffer     Where the lanes are.
/// @param loops      How many loops; at least 1.
/// @param interrupt  Set from outside, e.g. a SIGINT handler, to stop.
/// @return How it ended. Errors in either thread are reported here, not
///         thrown.
/// @throws std::runtime_error if the sink cannot be started.
/// @note Side effects: REST calls, files in the buffer, audio, log lines.
SessionResult playSession(PlaybackApi& api, ObjectFetcher& fetcher, AudioSink& sink,
    const SessionPlan& plan, const BufferDir& buffer, std::int64_t loops,
    const std::atomic<bool>& interrupt);

} // namespace rainbohz::playback_client
