// SPDX-License-Identifier: Apache-2.0

#pragma once

// The state the network and playback threads share: which lane holds what,
// which lane is playing, and whether the session is stopping.
//
// The buffer has two lanes (buffer_dir.hpp). The network thread fills them in
// turn, A, B, A, ..., one fill per loop; the playback thread (the feeder)
// plays them in the same order. Every rule that keeps the two from treading
// on each other is enforced here, under one mutex, so that each transition
// is one atomic step. The rules, and the race each one closes, are in
// docs/design.md §4:
//
// - A lane is Free, Filling or Ready. The network thread may write only a
//   Free lane (beginFill() waits for one).
// - A lane becomes Ready in the same step that publishes its last frame, so
//   the feeder can never free a lane before it is marked Ready, only for the
//   late mark to make it look full again.
// - A lane becomes Free only when the feeder switches away from it to the
//   other lane, and the feeder switches only after it has read every file of
//   the lane it leaves. So a lane being read is never Free, and a Free lane
//   is never read.
// - The first loop streams: the feeder may read frame i of lane A as soon as
//   the network thread has published it (waitForFrame()). Every later loop is
//   all-or-nothing: the feeder switches to a lane only once it is Ready
//   (switchTo()).
// - Stopping wakes every wait. After requestStop() or fail(), every wait
//   returns false at once, and stays false.
//
// Thread-safe throughout. Every member function may block on the mutex, and
// the waits block until their condition holds; none may be called from the
// audio callback.

#include "rainbohz/playback_client/buffer_dir.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace rainbohz::playback_client {

/// How full a lane is.
enum class FillState : std::uint8_t {
    /// Holds nothing the feeder will read; the network thread may write it.
    Free,
    /// Being written. framesReady() of its frames are complete.
    Filling,
    /// Holds a complete pass over the range, not yet released by the feeder.
    Ready,
};

/// The lanes' shared state.
class LaneBoard {
public:
    /// A board for lanes of `framesPerLane` frames, both Free.
    ///
    /// @param framesPerLane  Frames in one pass over the range; positive.
    explicit LaneBoard(std::size_t framesPerLane);

    // -- The network thread ------------------------------------------------

    /// Waits until `lane` is Free, then marks it Filling with no frames.
    ///
    /// @param lane  The lane to fill next.
    /// @return True when the lane is now Filling; false when the session
    ///         stopped first, in which case nothing changed.
    bool beginFill(Lane lane);

    /// Publishes the next frame of a Filling lane, which the caller has
    /// renamed into place.
    ///
    /// Publishing the lane's last frame also marks it Ready, in the same step.
    ///
    /// @param lane  The lane being filled.
    /// @pre `lane` is Filling.
    /// @post framesReady(lane) is one more; Ready if it reached the lane size.
    void frameWritten(Lane lane);

    // -- The feeder ---------------------------------------------------------

    /// Starts the first loop: marks `lane` as playing while it streams.
    ///
    /// @param lane  The lane of the first loop, A.
    /// @pre Nothing is playing yet.
    void beginStreaming(Lane lane);

    /// Waits until frame `index` of a streaming lane is complete.
    ///
    /// @param lane   The lane, as given to beginStreaming().
    /// @param index  The frame's position in the lane, from 0.
    /// @return True when the frame can be read; false when the session
    ///         stopped first.
    bool waitForFrame(Lane lane, std::size_t index);

    /// Waits until `lane` is Ready, then makes it the playing lane and frees
    /// the lane that was playing.
    ///
    /// @param lane  The lane of the next loop.
    /// @return True when `lane` is now playing; false when the session
    ///         stopped first, in which case nothing changed.
    /// @pre The caller has read every file of the lane it is leaving.
    bool switchTo(Lane lane);

    /// Records that one loop has been handed to the audio output.
    ///
    /// @return The number of loops played, this one included.
    std::int64_t loopPlayed();

    // -- Control ------------------------------------------------------------

    /// Stops the session: every wait, now and later, returns false.
    void requestStop();

    /// Stops the session because of an error, keeping the first message.
    ///
    /// @param message  What went wrong. Ignored when an error is already
    ///                 recorded, so the root cause is the one reported.
    void fail(const std::string& message);

    /// Whether the session is stopping.
    ///
    /// @return True once requestStop() or fail() has been called.
    bool stopped() const;

    /// The first error, if any.
    ///
    /// @return The message first given to fail(); empty if none was.
    std::optional<std::string> error() const;

    // -- Observation, for logs and tests --------------------------------------

    /// A lane's fill state.
    ///
    /// @param lane  The lane.
    /// @return Its state now.
    FillState state(Lane lane) const;

    /// How many frames of a lane are complete.
    ///
    /// @param lane  The lane.
    /// @return Frames published in its current fill; the lane size once
    ///         Ready; 0 when Free.
    std::size_t framesReady(Lane lane) const;

    /// The lane being played, if any.
    ///
    /// @return The lane last given to beginStreaming() or switchTo(); empty
    ///         before either.
    std::optional<Lane> playing() const;

    /// The number of loops played.
    ///
    /// @return The count recorded by loopPlayed().
    std::int64_t loopsPlayed() const;

    /// Frames in one lane.
    ///
    /// @return The size given to the constructor.
    std::size_t framesPerLane() const noexcept { return framesPerLane_; }

private:
    /// One lane's state.
    struct LaneState {
        /// Whether the lane is Free, Filling or Ready.
        FillState fill = FillState::Free;
        /// Frames of the current fill published so far; framesPerLane_ once Ready.
        std::size_t framesReady = 0;
    };

    /// The state of a lane. The caller holds mutex_.
    ///
    /// @param lane  The lane.
    /// @return Its state, to read or change.
    LaneState& at(Lane lane) noexcept { return lane == Lane::A ? a_ : b_; }

    /// The state of a lane. The caller holds mutex_.
    ///
    /// @param lane  The lane.
    /// @return Its state.
    const LaneState& at(Lane lane) const noexcept { return lane == Lane::A ? a_ : b_; }

    /// Frames in one pass over the range: a lane's size.
    const std::size_t framesPerLane_;
    /// Guards every member below; each transition holds it throughout.
    mutable std::mutex mutex_;
    /// Notified on every transition; every wait re-checks its condition.
    std::condition_variable changed_;
    /// The state of lane A.
    LaneState a_;
    /// The state of lane B.
    LaneState b_;
    /// The lane the feeder is reading; empty before beginStreaming().
    std::optional<Lane> playing_;
    /// Loops handed to the audio output so far (loopPlayed()).
    std::int64_t loopsPlayed_ = 0;
    /// Set by requestStop() or fail(); once set, every wait returns false.
    bool stop_ = false;
    /// The first message given to fail(); empty if none was.
    std::optional<std::string> error_;
};

} // namespace rainbohz::playback_client
