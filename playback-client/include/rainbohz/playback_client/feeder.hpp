// SPDX-License-Identifier: Apache-2.0

#pragma once

// The playback thread: reads the lanes in loop order and feeds the output.
//
// Loop j is played from lane laneForLoop(j). The first loop streams: each
// frame is read as soon as the network thread has published it. Every later
// loop waits for its lane to be completely filled, then switches to it,
// which frees the lane just played for the network thread to refill
// (LaneBoard::switchTo()). Each frame is read whole, trimmed to the samples
// the range plays, and written to the sink. After the last loop the feeder
// plays the output out (AudioSink::finish()).
//
// Every wait is timed. The first is start-up latency; any later one means
// the network thread had not kept up, and is logged, with the silence the
// output actually played (AudioSink::underrun()) where the sink is paced.

#include "rainbohz/playback_client/audio_sink.hpp"
#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/frame_plan.hpp"
#include "rainbohz/playback_client/lane_board.hpp"
#include "rainbohz/playback_client/units.hpp"

#include <cstdint>
#include <vector>

namespace rainbohz::playback_client {

/// The feeder's settings.
struct FeederSettings {
    /// How many loops to play; at least 1.
    std::int64_t loops = 1;
    /// The sample rate every frame must have.
    SampleRate sampleRate{1};
    /// The channels every frame must have.
    ChannelCount channels{1};
};

/// One time the feeder had to wait for the network thread.
struct FeederWait {
    /// The loop being played, from 0.
    std::int64_t loop = 0;
    /// The frame's position in the lane, from 0; 0 for a lane switch.
    std::size_t frameIndex = 0;
    /// Wall-clock seconds waited.
    double seconds = 0.0;
};

/// What the feeder did.
struct FeederReport {
    /// Loops handed to the output in full.
    std::int64_t loopsPlayed = 0;
    /// Seconds from the start until the first frame could be read.
    double startupSeconds = 0.0;
    /// Every wait after the first frame that took longer than a millisecond.
    std::vector<FeederWait> waits;
    /// Whether the output was played out to the end (finish() returned).
    bool finished = false;
};

/// Runs the feeder to completion, or until the session stops.
///
/// Every exception is caught and recorded with LaneBoard::fail(), which
/// stops the session; nothing propagates out. None of the references is kept
/// beyond the call.
///
/// @param[in,out] board  The shared lane state; lanes are switched and loops
///                       counted through it.
/// @param plan           The frames of one pass.
/// @param buffer         Where the lanes are.
/// @param[in,out] sink   Where the audio goes; started by the caller, and
///                       finished here after the last loop.
/// @param settings       How many loops, the sample rate and the channels.
/// @return What it did.
/// @note Side effects: reads frame files; writes to the sink, and finishes
///       it after the last loop; logs each loop at info level, waits at
///       debug, and underruns as warnings.
FeederReport runFeeder(LaneBoard& board, const FramePlan& plan, const BufferDir& buffer,
    AudioSink& sink, FeederSettings settings) noexcept;

} // namespace rainbohz::playback_client
