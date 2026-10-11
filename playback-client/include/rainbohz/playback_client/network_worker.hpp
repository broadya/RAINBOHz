// SPDX-License-Identifier: Apache-2.0

#pragma once

// The network thread: renders and fetches every frame of every fill.
//
// Fill k is written to lane laneForLoop(k), so A, B, A, ...; there is one fill
// per loop, so every loop is freshly rendered. A fill waits for its lane to be
// Free (LaneBoard::beginFill()), then makes one REST call at a time, frame by
// frame in order: render the frame, fetch its object into the lane under a
// `.part` name, rename it into place, publish it. Only one call is ever in
// flight. Each fill's calls are timed, and the fill's summary logged; see
// docs/design.md §6 for what the numbers mean.

#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/frame_plan.hpp"
#include "rainbohz/playback_client/lane_board.hpp"
#include "rainbohz/playback_client/object_fetch.hpp"
#include "rainbohz/playback_client/playback_api.hpp"
#include "rainbohz/playback_client/units.hpp"

#include <cstdint>
#include <vector>

namespace rainbohz::playback_client {

/// The network thread's settings.
struct NetworkSettings {
    /// How many fills to make: the number of loops.
    std::int64_t fills = 1;
    /// The sample rate, for the real-time factor.
    SampleRate sampleRate{1};
};

/// The timing of one fill.
struct FillTiming {
    /// The fill's number, from 0.
    std::int64_t fill = 0;
    /// The lane it was written to.
    Lane lane = Lane::A;
    /// Wall-clock seconds of each frame's render call, in frame order.
    std::vector<double> renderSeconds;
    /// Wall-clock seconds of each frame's fetch, in frame order.
    std::vector<double> fetchSeconds;
    /// Wall-clock seconds from the first call to the last frame published.
    double totalSeconds = 0.0;
    /// totalSeconds ÷ the pass's audio duration. Below 1, filling keeps up
    /// with playing.
    double realTimeFactor = 0.0;
};

/// Runs the network thread's work to completion, or until the session stops.
///
/// Every exception is caught and recorded with LaneBoard::fail(), which
/// stops the session; nothing propagates out. An exception once the session
/// has already stopped is not recorded: it is the consequence of the stop,
/// usually a call ended by PlaybackApi::cancel(), not its cause. None of the
/// references is kept beyond the call.
///
/// @param[in,out] board  The shared lane state; lanes are filled and frames
///                       published through it.
/// @param api            The REST calls; used from this thread only while
///                       the call runs.
/// @param fetcher        Fetches each rendered frame's object.
/// @param plan           The frames of one pass.
/// @param buffer         Where the lanes are.
/// @param settings       How many fills, and the sample rate.
/// @return The timing of each fill completed, in order.
/// @note Side effects: REST calls; writes frame files into the lanes; logs
///       each fill's summary at info level, and each frame's timing at debug.
std::vector<FillTiming> runNetwork(LaneBoard& board, PlaybackApi& api, ObjectFetcher& fetcher,
    const FramePlan& plan, const BufferDir& buffer, NetworkSettings settings) noexcept;

} // namespace rainbohz::playback_client
