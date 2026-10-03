#include "rainbohz/playback_client/lane_board.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace rainbohz::playback_client {
namespace {

using namespace std::chrono_literals;

/// Whether a future is still pending after a short wait: the call behind it
/// is blocked.
template <typename T>
bool blocked(std::future<T>& future) {
    return future.wait_for(50ms) == std::future_status::timeout;
}

TEST(LaneBoard, StartsWithBothLanesFreeAndNothingPlaying) {
    const LaneBoard board{3};
    EXPECT_EQ(board.state(Lane::A), FillState::Free);
    EXPECT_EQ(board.state(Lane::B), FillState::Free);
    EXPECT_FALSE(board.playing().has_value());
    EXPECT_FALSE(board.stopped());
}

TEST(LaneBoard, TheLastFrameMakesALaneReadyInTheSameStep) {
    LaneBoard board{2};
    ASSERT_TRUE(board.beginFill(Lane::A));
    EXPECT_EQ(board.state(Lane::A), FillState::Filling);
    board.frameWritten(Lane::A);
    EXPECT_EQ(board.state(Lane::A), FillState::Filling);
    EXPECT_EQ(board.framesReady(Lane::A), 1U);
    board.frameWritten(Lane::A);
    EXPECT_EQ(board.state(Lane::A), FillState::Ready);
}

TEST(LaneBoard, StreamingWaitsForEachFrame) {
    LaneBoard board{2};
    board.beginStreaming(Lane::A);
    auto frame0 = std::async(std::launch::async, [&] { return board.waitForFrame(Lane::A, 0); });
    // Lane A is Free: the network thread has not begun, so there is nothing.
    EXPECT_TRUE(blocked(frame0));
    ASSERT_TRUE(board.beginFill(Lane::A));
    EXPECT_TRUE(blocked(frame0));
    board.frameWritten(Lane::A);
    EXPECT_TRUE(frame0.get());

    auto frame1 = std::async(std::launch::async, [&] { return board.waitForFrame(Lane::A, 1); });
    EXPECT_TRUE(blocked(frame1));
    board.frameWritten(Lane::A);
    EXPECT_TRUE(frame1.get());
}

TEST(LaneBoard, ASwitchWaitsForAFullLaneAndFreesTheOneLeft) {
    LaneBoard board{1};
    ASSERT_TRUE(board.beginFill(Lane::A));
    board.beginStreaming(Lane::A);
    board.frameWritten(Lane::A);

    auto toB = std::async(std::launch::async, [&] { return board.switchTo(Lane::B); });
    EXPECT_TRUE(blocked(toB));
    ASSERT_TRUE(board.beginFill(Lane::B));
    EXPECT_TRUE(blocked(toB)); // Filling is not enough
    board.frameWritten(Lane::B);
    EXPECT_TRUE(toB.get());
    EXPECT_EQ(board.playing(), Lane::B);
    EXPECT_EQ(board.state(Lane::A), FillState::Free);
    EXPECT_EQ(board.state(Lane::B), FillState::Ready);
}

TEST(LaneBoard, TheNetworkCannotRefillTheLaneBeingPlayed) {
    LaneBoard board{1};
    ASSERT_TRUE(board.beginFill(Lane::A));
    board.beginStreaming(Lane::A);
    board.frameWritten(Lane::A);
    ASSERT_TRUE(board.beginFill(Lane::B));
    board.frameWritten(Lane::B);

    // Fill 2 goes to lane A, which is still playing: it must wait for the
    // feeder to move to B.
    auto refillA = std::async(std::launch::async, [&] { return board.beginFill(Lane::A); });
    EXPECT_TRUE(blocked(refillA));
    ASSERT_TRUE(board.switchTo(Lane::B));
    EXPECT_TRUE(refillA.get());
    EXPECT_EQ(board.state(Lane::A), FillState::Filling);
}

TEST(LaneBoard, StopWakesEveryWait) {
    LaneBoard board{1};
    board.beginStreaming(Lane::A);
    ASSERT_TRUE(board.beginFill(Lane::B));
    auto frame = std::async(std::launch::async, [&] { return board.waitForFrame(Lane::A, 0); });
    auto toB = std::async(std::launch::async, [&] { return board.switchTo(Lane::B); });
    auto fillB = std::async(std::launch::async, [&] { return board.beginFill(Lane::B); });
    EXPECT_TRUE(blocked(frame));
    board.requestStop();
    EXPECT_FALSE(frame.get());
    EXPECT_FALSE(toB.get());
    EXPECT_FALSE(fillB.get());
    EXPECT_FALSE(board.beginFill(Lane::A));
    EXPECT_FALSE(board.error().has_value());
}

TEST(LaneBoard, KeepsTheFirstError) {
    LaneBoard board{1};
    board.fail("first");
    board.fail("second");
    EXPECT_TRUE(board.stopped());
    EXPECT_EQ(board.error(), "first");
}

TEST(LaneBoard, CountsLoops) {
    LaneBoard board{1};
    EXPECT_EQ(board.loopPlayed(), 1);
    EXPECT_EQ(board.loopPlayed(), 2);
    EXPECT_EQ(board.loopsPlayed(), 2);
}

} // namespace
} // namespace rainbohz::playback_client
