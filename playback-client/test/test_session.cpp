// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/object_fetch.hpp"
#include "rainbohz/playback_client/playback_session.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rainbohz::playback_client {
namespace {

using namespace std::chrono_literals;
using test::FakePlaybackApi;
using test::frameValue;
using test::MemorySink;
using test::TempDir;

// The scenario every session test uses: frames of 10 samples, ticks of one
// sample, four ticks a beat; the range 0:5 to 2:3 is samples 5 to 11, so
// frame 0 from 5, and frame 1 to 1 -- two slices, 6 samples.
constexpr std::int32_t kRate = 1000;
constexpr std::int32_t kFrameSize = 10;
constexpr std::size_t kFramesPerLane = 2;
const MusicalPosition kStart{.beats = 1, .ticks = 1}; // sample 5
const MusicalPosition kEnd{.beats = 2, .ticks = 3};   // sample 11

/// The samples `loops` loops of the scenario must give, fill j for loop j.
std::vector<float> expectedOutput(std::int64_t loops) {
    std::vector<float> expected;
    for (std::int64_t loop = 0; loop < loops; ++loop) {
        expected.insert(expected.end(), 5, frameValue(loop, 0)); // frame 0, samples 5-9
        expected.insert(expected.end(), 1, frameValue(loop, 1)); // frame 1, sample 0
    }
    return expected;
}

/// A session over the fakes.
class SessionTest : public ::testing::Test {
protected:
    SessionTest()
        : api_{serverDir_.path(), kRate, kFrameSize, kFramesPerLane},
          fetcher_{makeObjectFetcher()} {}

    SessionPlan plan() { return planSession(api_, kStart, kEnd); }

    SessionResult play(std::int64_t loops) {
        const BufferDir buffer{bufferRoot_.path()};
        bufferPath_ = buffer.path();
        return playSession(api_, *fetcher_, sink_, plan(), buffer, loops, interrupt_);
    }

    TempDir serverDir_;
    TempDir bufferRoot_;
    FakePlaybackApi api_;
    std::unique_ptr<ObjectFetcher> fetcher_;
    MemorySink sink_;
    std::atomic<bool> interrupt_{false};
    std::filesystem::path bufferPath_;
};

TEST_F(SessionTest, PlansTheRangeOnTheRootMap) {
    const SessionPlan planned = plan();
    EXPECT_EQ(api_.lastTempoMapId, 7);
    EXPECT_EQ(planned.start.frame, FrameNumber{0});
    EXPECT_EQ(planned.start.offset, 5);
    EXPECT_EQ(planned.end.frame, FrameNumber{1});
    EXPECT_EQ(planned.end.offset, 1);
    ASSERT_EQ(planned.frames.slices.size(), kFramesPerLane);
    EXPECT_EQ(planned.frames.length(), SampleCount{6});
}

TEST_F(SessionTest, RefusesAnythingButMono) {
    api_.channelMode = "stereo";
    EXPECT_THROW(plan(), SessionError);
}

TEST_F(SessionTest, RefusesTicksBeyondTheMapsResolution) {
    EXPECT_THROW(planSession(api_, MusicalPosition{.beats = 0, .ticks = 4}, kEnd), SessionError);
}

TEST_F(SessionTest, RefusesARangeShorterThanOneSample) {
    api_.samplesPerTick = 0; // every position is sample 0
    EXPECT_THROW(plan(), SessionError);
}

TEST_F(SessionTest, OneLoopFillsOneLane) {
    const SessionResult result = play(1);
    ASSERT_TRUE(result.ok()) << result.error.value_or("");
    EXPECT_EQ(result.loopsPlayed, 1);
    EXPECT_EQ(api_.calls(), 2);
    EXPECT_EQ(sink_.samples(), expectedOutput(1));
    EXPECT_TRUE(sink_.started);
    EXPECT_TRUE(sink_.finished);
    EXPECT_TRUE(std::filesystem::exists(bufferPath_ / "lane-a" / "frame-000000.wav"));
    EXPECT_FALSE(std::filesystem::exists(bufferPath_ / "lane-b" / "frame-000000.wav"));
}

TEST_F(SessionTest, EveryLoopIsAFreshFillInAlternateLanes) {
    const SessionResult result = play(3);
    ASSERT_TRUE(result.ok()) << result.error.value_or("");
    EXPECT_EQ(result.loopsPlayed, 3);
    EXPECT_EQ(api_.calls(), 6);
    EXPECT_EQ(sink_.samples(), expectedOutput(3));
}

// Playback much slower than the network: the network thread is always ready
// early, and must wait to refill a lane until playback has left it. Were it
// to overwrite a lane being read, a loop would contain a later fill's values.
TEST_F(SessionTest, FastNetworkNeverOverwritesTheLaneBeingPlayed) {
    sink_.writeDelay = 15ms;
    const SessionResult result = play(6);
    ASSERT_TRUE(result.ok()) << result.error.value_or("");
    EXPECT_EQ(sink_.samples(), expectedOutput(6));
}

// The network much slower than playback: every loop after the first waits
// for its lane, and still plays its own fill.
TEST_F(SessionTest, SlowNetworkMakesPlaybackWaitButStaysCorrect) {
    api_.renderDelay = 20ms;
    const SessionResult result = play(3);
    ASSERT_TRUE(result.ok()) << result.error.value_or("");
    EXPECT_EQ(sink_.samples(), expectedOutput(3));
}

// The first loop streams: playback starts before lane A is complete.
TEST_F(SessionTest, TheFirstLoopStreams) {
    api_.renderDelay = 30ms;
    std::size_t writtenWhenSecondFrameRendered = 0;
    api_.onRender = [&](std::int64_t call) {
        if (call == 1) {
            // Give the feeder time to read frame 0, which is published.
            std::this_thread::sleep_for(50ms);
            writtenWhenSecondFrameRendered = sink_.size();
        }
    };
    const SessionResult result = play(1);
    ASSERT_TRUE(result.ok()) << result.error.value_or("");
    EXPECT_EQ(writtenWhenSecondFrameRendered, 5U);
}

TEST_F(SessionTest, ANetworkErrorStopsTheSession) {
    api_.failAtCall = 3; // the second frame of the second fill
    const SessionResult result = play(4);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->find("scripted failure at call 3"), std::string::npos);
    EXPECT_LT(result.loopsPlayed, 4);
}

TEST_F(SessionTest, APlaybackErrorStopsTheSession) {
    api_.shortFrameInFill = 1; // fill 1's frames are a sample short
    const SessionResult result = play(3);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->find("not a frame of"), std::string::npos);
    EXPECT_EQ(result.loopsPlayed, 1);
}

TEST_F(SessionTest, AnInterruptStopsPromptlyEvenMidCall) {
    api_.renderDelay = 200ms;
    std::thread interrupter{[this] {
        std::this_thread::sleep_for(300ms);
        interrupt_.store(true);
    }};
    const auto started = std::chrono::steady_clock::now();
    const SessionResult result = play(10);
    interrupter.join();
    EXPECT_TRUE(result.interrupted);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_LT(result.loopsPlayed, 10);
    // At most the call in flight is waited for.
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1500ms);
}

// A server that never answers: without cancel() the session would wait for
// the call's read timeout, minutes in the HTTP client. Cancelling ends it,
// and the cancelled call is not reported as the session's error.
TEST_F(SessionTest, AnInterruptCancelsAStalledCall) {
    api_.hangAtCall = 1;
    std::thread interrupter{[this] {
        std::this_thread::sleep_for(300ms);
        interrupt_.store(true);
    }};
    const auto started = std::chrono::steady_clock::now();
    const SessionResult result = play(10);
    interrupter.join();
    EXPECT_TRUE(result.interrupted);
    EXPECT_FALSE(result.error.has_value()) << *result.error;
    EXPECT_GT(api_.cancelCount(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1500ms);
}

// When the feeder fails, the network thread's stalled call is cancelled too,
// and the error reported is the feeder's, the cause, not the cancelled call.
TEST_F(SessionTest, AFeederErrorCancelsAStalledCall) {
    constexpr auto kStalledCall = static_cast<std::int64_t>(kFramesPerLane); // fill 1's first call
    api_.hangAtCall = kStalledCall;
    // The output fails, but only once the network thread is in the stalled
    // call, so that there is a call to cancel.
    sink_.beforeWrite = [this] {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (api_.calls() <= kStalledCall && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error{"scripted output failure"};
    };
    const auto started = std::chrono::steady_clock::now();
    const SessionResult result = play(3);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->find("scripted output failure"), std::string::npos) << *result.error;
    EXPECT_EQ(result.error->find("cancelled"), std::string::npos) << *result.error;
    EXPECT_GT(api_.cancelCount(), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1500ms);
}

} // namespace
} // namespace rainbohz::playback_client
