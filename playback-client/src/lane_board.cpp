#include "rainbohz/playback_client/lane_board.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

/// @file
/// @brief The lanes' shared state and its transitions.
///
/// Every transition notifies every waiter: there are at most two, and each
/// re-checks its own condition, so a broadcast is simpler than tracking who
/// waits on what and costs nothing that matters.
///
/// @see lane_board.hpp

namespace rainbohz::playback_client {

LaneBoard::LaneBoard(std::size_t framesPerLane) : framesPerLane_{framesPerLane} {
    assert(framesPerLane > 0);
}

bool LaneBoard::beginFill(Lane lane) {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [&] { return stop_ || at(lane).fill == FillState::Free; });
    if (stop_) {
        return false;
    }
    at(lane) = LaneState{.fill = FillState::Filling, .framesReady = 0};
    changed_.notify_all();
    return true;
}

void LaneBoard::frameWritten(Lane lane) {
    const std::scoped_lock lock{mutex_};
    LaneState& state = at(lane);
    assert(state.fill == FillState::Filling);
    assert(state.framesReady < framesPerLane_);
    ++state.framesReady;
    if (state.framesReady == framesPerLane_) {
        state.fill = FillState::Ready;
    }
    changed_.notify_all();
}

void LaneBoard::beginStreaming(Lane lane) {
    const std::scoped_lock lock{mutex_};
    assert(!playing_.has_value());
    playing_ = lane;
    changed_.notify_all();
}

bool LaneBoard::waitForFrame(Lane lane, std::size_t index) {
    std::unique_lock lock{mutex_};
    // A Free lane has nothing in it yet, whatever its count says: the
    // network thread has not begun this fill.
    changed_.wait(lock, [&] {
        return stop_ || (at(lane).fill != FillState::Free && at(lane).framesReady > index);
    });
    return !stop_;
}

bool LaneBoard::switchTo(Lane lane) {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [&] { return stop_ || at(lane).fill == FillState::Ready; });
    if (stop_) {
        return false;
    }
    if (playing_.has_value()) {
        // The lane being left was fully read, so it was Ready: the feeder
        // reads a frame only once it is published, and publishing the last
        // frame made it Ready in the same step.
        LaneState& leaving = at(playing_.value());
        assert(leaving.fill == FillState::Ready);
        leaving = LaneState{};
    }
    playing_ = lane;
    changed_.notify_all();
    return true;
}

std::int64_t LaneBoard::loopPlayed() {
    const std::scoped_lock lock{mutex_};
    ++loopsPlayed_;
    changed_.notify_all();
    return loopsPlayed_;
}

void LaneBoard::requestStop() {
    const std::scoped_lock lock{mutex_};
    stop_ = true;
    changed_.notify_all();
}

void LaneBoard::fail(const std::string& message) {
    const std::scoped_lock lock{mutex_};
    if (!error_.has_value()) {
        error_ = message;
    }
    stop_ = true;
    changed_.notify_all();
}

bool LaneBoard::stopped() const {
    const std::scoped_lock lock{mutex_};
    return stop_;
}

std::optional<std::string> LaneBoard::error() const {
    const std::scoped_lock lock{mutex_};
    return error_;
}

FillState LaneBoard::state(Lane lane) const {
    const std::scoped_lock lock{mutex_};
    return at(lane).fill;
}

std::size_t LaneBoard::framesReady(Lane lane) const {
    const std::scoped_lock lock{mutex_};
    return at(lane).framesReady;
}

std::optional<Lane> LaneBoard::playing() const {
    const std::scoped_lock lock{mutex_};
    return playing_;
}

std::int64_t LaneBoard::loopsPlayed() const {
    const std::scoped_lock lock{mutex_};
    return loopsPlayed_;
}

} // namespace rainbohz::playback_client
