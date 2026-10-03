#include "rainbohz/playback_client/frame_plan.hpp"
#include "rainbohz/playback_client/musical_position.hpp"
#include "rainbohz/playback_client/options.hpp"
#include "rainbohz/playback_client/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace rainbohz::playback_client {
namespace {

// --- Musical positions -------------------------------------------------------

TEST(MusicalPosition, ParsesBeatsAndTicks) {
    const MusicalPosition position = parseMusicalPosition("12:480");
    EXPECT_EQ(position.beats, 12);
    EXPECT_EQ(position.ticks, 480);
}

TEST(MusicalPosition, ParsesBareBeats) {
    const MusicalPosition position = parseMusicalPosition("3");
    EXPECT_EQ(position.beats, 3);
    EXPECT_EQ(position.ticks, 0);
}

TEST(MusicalPosition, RefusesWhatTheServerRefuses) {
    for (const char* text : {"", ":", "1:", ":5", "-1", "+1", "1.5", "1:2:3", " 1", "a", "1:x",
             "99999999999999999999"}) {
        EXPECT_THROW(parseMusicalPosition(text), MusicalPositionError) << text;
    }
}

TEST(MusicalPosition, OrdersByBeatsThenTicks) {
    EXPECT_LT(parseMusicalPosition("1:959"), parseMusicalPosition("2:0"));
    EXPECT_LT(parseMusicalPosition("2:0"), parseMusicalPosition("2:1"));
    EXPECT_EQ(parseMusicalPosition("2"), parseMusicalPosition("2:0"));
}

TEST(MusicalPosition, FormatsInFull) {
    EXPECT_EQ(formatMusicalPosition(parseMusicalPosition("4")), "4:0");
    EXPECT_EQ(formatMusicalPosition(MusicalPosition{.beats = 1, .ticks = 2}), "1:2");
}

TEST(MusicalPosition, ChecksTicksAgainstResolution) {
    EXPECT_TRUE(validTicks(MusicalPosition{.beats = 0, .ticks = 959}, 960));
    EXPECT_FALSE(validTicks(MusicalPosition{.beats = 0, .ticks = 960}, 960));
}

// --- Frame plan --------------------------------------------------------------

TEST(FramePlan, TrimsTheFirstAndLastFrames) {
    const FramePlan plan = planFrames(FrameOffset{.frame = FrameNumber{1}, .offset = 30},
        FrameOffset{.frame = FrameNumber{3}, .offset = 20}, FrameSize{100});
    ASSERT_EQ(plan.slices.size(), 3U);
    EXPECT_EQ(plan.slices[0].frame, FrameNumber{1});
    EXPECT_EQ(plan.slices[0].begin, 30);
    EXPECT_EQ(plan.slices[0].end, 100);
    EXPECT_EQ(plan.slices[1].begin, 0);
    EXPECT_EQ(plan.slices[1].end, 100);
    EXPECT_EQ(plan.slices[2].frame, FrameNumber{3});
    EXPECT_EQ(plan.slices[2].begin, 0);
    EXPECT_EQ(plan.slices[2].end, 20);
    EXPECT_EQ(plan.length(), SampleCount{70 + 100 + 20});
}

TEST(FramePlan, AnEndOnAFrameBoundaryNeedsNothingFromThatFrame) {
    const FramePlan plan = planFrames(FrameOffset{.frame = FrameNumber{0}, .offset = 0},
        FrameOffset{.frame = FrameNumber{2}, .offset = 0}, FrameSize{100});
    ASSERT_EQ(plan.slices.size(), 2U);
    EXPECT_EQ(plan.slices[1].frame, FrameNumber{1});
    EXPECT_EQ(plan.slices[1].end, 100);
    EXPECT_EQ(plan.length(), SampleCount{200});
}

TEST(FramePlan, ARangeWithinOneFrame) {
    const FramePlan plan = planFrames(FrameOffset{.frame = FrameNumber{5}, .offset = 10},
        FrameOffset{.frame = FrameNumber{5}, .offset = 11}, FrameSize{100});
    ASSERT_EQ(plan.slices.size(), 1U);
    EXPECT_EQ(plan.slices[0].begin, 10);
    EXPECT_EQ(plan.slices[0].end, 11);
}

TEST(FramePlan, RefusesEmptyAndReversedRanges) {
    const FrameOffset at{.frame = FrameNumber{2}, .offset = 5};
    EXPECT_THROW(planFrames(at, at, FrameSize{100}), FramePlanError);
    EXPECT_THROW(planFrames(at, FrameOffset{.frame = FrameNumber{1}, .offset = 99}, FrameSize{100}),
        FramePlanError);
}

TEST(FramePlan, RefusesOffsetsOutsideAFrame) {
    const FrameOffset start{.frame = FrameNumber{0}, .offset = 0};
    EXPECT_THROW(
        planFrames(start, FrameOffset{.frame = FrameNumber{1}, .offset = 100}, FrameSize{100}),
        FramePlanError);
    EXPECT_THROW(planFrames(FrameOffset{.frame = FrameNumber{0}, .offset = -1},
                     FrameOffset{.frame = FrameNumber{1}, .offset = 0}, FrameSize{100}),
        FramePlanError);
    EXPECT_THROW(planFrames(start, start, FrameSize{0}), FramePlanError);
}

// --- Ring buffer -------------------------------------------------------------

TEST(SampleRing, PushesUntilFullAndPopsInOrder) {
    SampleRing ring{4};
    const std::array<float, 6> in{1, 2, 3, 4, 5, 6};
    EXPECT_EQ(ring.push(in), 4U);
    EXPECT_EQ(ring.size(), 4U);
    std::array<float, 3> out{};
    EXPECT_EQ(ring.pop(out), 3U);
    EXPECT_EQ(out, (std::array<float, 3>{1, 2, 3}));
    EXPECT_EQ(ring.push(std::span<const float>{in}.subspan(4)), 2U);
    std::array<float, 8> rest{};
    EXPECT_EQ(ring.pop(rest), 3U);
    EXPECT_EQ(rest[0], 4);
    EXPECT_EQ(rest[1], 5);
    EXPECT_EQ(rest[2], 6);
    EXPECT_EQ(ring.size(), 0U);
}

TEST(SampleRing, WrapsManyTimes) {
    SampleRing ring{3};
    float next = 0;
    float expected = 0;
    for (int round = 0; round < 1000; ++round) {
        const std::array<float, 2> in{next, next + 1};
        ASSERT_EQ(ring.push(in), 2U);
        next += 2;
        std::array<float, 2> out{};
        ASSERT_EQ(ring.pop(out), 2U);
        ASSERT_EQ(out[0], expected);
        ASSERT_EQ(out[1], expected + 1);
        expected += 2;
    }
}

// --- Command line ------------------------------------------------------------

ParseResult parse(std::vector<const char*> args) {
    args.insert(args.begin(), "playback-client");
    return parseCommandLine(static_cast<int>(args.size()), args.data());
}

TEST(Options, TakesTheRangeAndDefaultsTheRest) {
    const ParseResult result = parse({"--start", "1:240", "--end", "4"});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->start, (MusicalPosition{.beats = 1, .ticks = 240}));
    EXPECT_EQ(result.options->end, (MusicalPosition{.beats = 4, .ticks = 0}));
    EXPECT_EQ(result.options->serverUrl, kDefaultServerUrl);
    EXPECT_EQ(result.options->loops, 1);
    EXPECT_FALSE(result.options->keepBuffer);
    EXPECT_FALSE(result.options->debug);
    EXPECT_FALSE(result.options->outputWav.has_value());
    EXPECT_EQ(result.options->bufferRoot, std::filesystem::temp_directory_path());
}

TEST(Options, TakesEveryOption) {
    const auto root = std::filesystem::temp_directory_path().string();
    const ParseResult result =
        parse({"--server", "http://h:1/v1", "--start", "0", "--end", "1", "--loops", "3",
            "--buffer-dir", root.c_str(), "--keep-buffer", "--output-wav", "o.wav", "--debug"});
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->serverUrl, "http://h:1/v1");
    EXPECT_EQ(result.options->loops, 3);
    EXPECT_TRUE(result.options->keepBuffer);
    EXPECT_TRUE(result.options->debug);
    EXPECT_EQ(result.options->outputWav, std::filesystem::path{"o.wav"});
}

TEST(Options, RefusesAnEndNotAfterTheStart) {
    EXPECT_EQ(parse({"--start", "2:0", "--end", "2"}).exitCode, 2);
    EXPECT_EQ(parse({"--start", "3", "--end", "2:959"}).exitCode, 2);
}

TEST(Options, RefusesBadValues) {
    EXPECT_EQ(parse({"--start", "x", "--end", "2"}).exitCode, 2);
    EXPECT_EQ(parse({"--start", "0", "--end", "2", "--loops", "0"}).exitCode, 2);
    EXPECT_EQ(parse({"--start", "0"}).exitCode, 2);
    EXPECT_EQ(parse({"--start", "0", "--end", "1", "--buffer-dir", "/no/such/dir"}).exitCode, 2);
}

TEST(Options, HelpExitsZero) {
    const ParseResult result = parse({"--help"});
    EXPECT_FALSE(result.options.has_value());
    EXPECT_EQ(result.exitCode, 0);
}

} // namespace
} // namespace rainbohz::playback_client
