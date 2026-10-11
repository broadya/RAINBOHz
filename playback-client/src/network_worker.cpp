// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/network_worker.hpp"

#include "rainbohz/playback_client/log.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

/// @file
/// @brief The network thread: fills the lanes, one REST call at a time.
///
/// @see network_worker.hpp

namespace rainbohz::playback_client {

namespace {

using Clock = std::chrono::steady_clock;

/// Seconds since `start`.
///
/// @param start  A time taken earlier from Clock.
/// @return The wall-clock seconds elapsed.
double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// Writes the summary line of a completed fill.
///
/// @param timing  The fill's timing.
/// @param fills   How many fills the session makes.
/// @param audio   Seconds of audio in one pass.
/// @note Side effect: one info log line.
void logFill(const FillTiming& timing, std::int64_t fills, double audio) {
    const auto& renders = timing.renderSeconds;
    const double mean =
        std::accumulate(renders.begin(), renders.end(), 0.0) / static_cast<double>(renders.size());
    const double max = *std::ranges::max_element(renders);
    const double fetch =
        std::accumulate(timing.fetchSeconds.begin(), timing.fetchSeconds.end(), 0.0);
    logLine(LogLevel::Info,
        "lane " + std::string{laneName(timing.lane)} + " filled (fill " +
            std::to_string(timing.fill + 1) + " of " + std::to_string(fills) +
            "): " + std::to_string(renders.size()) + " frame(s) in " +
            formatFixed(timing.totalSeconds, 3) + " s; render mean " +
            formatFixed(mean * 1000.0, 1) + " ms, max " + formatFixed(max * 1000.0, 1) +
            " ms; fetch total " + formatFixed(fetch * 1000.0, 1) + " ms; real-time factor " +
            formatFixed(timing.realTimeFactor, 3) + " (" + formatFixed(audio, 3) + " s of audio)");
}

/// What the network thread works with, as runNetwork() was given it.
struct NetworkContext {
    /// The lane state shared with the feeder.
    LaneBoard* board = nullptr;
    /// The server's Playback API, which renders each frame.
    PlaybackApi* api = nullptr;
    /// Copies each rendered frame from its URI into the buffer.
    ObjectFetcher* fetcher = nullptr;
    /// The frames of one pass; every fill renders all of them.
    const FramePlan* plan = nullptr;
    /// The run's buffer directory, where the lanes' frame files go.
    const BufferDir* buffer = nullptr;
    /// How many fills to make, and the sample rate for the real-time factor.
    NetworkSettings settings;
};

/// Makes one fill: every frame of the plan, into one lane.
///
/// @param job   The work; every pointer non-null.
/// @param fill  The fill's number, from 0.
/// @param[out] timing  Receives the fill's timing, frame by frame.
/// @return True when the fill completed; false when the session stopped.
/// @throws PlaybackApiError, ObjectFetchError or std::filesystem_error when a
///         call, fetch or rename fails.
bool fillLane(const NetworkContext& job, std::int64_t fill, FillTiming& timing) {
    const Lane lane = laneForLoop(fill);
    timing.fill = fill;
    timing.lane = lane;
    if (!job.board->beginFill(lane)) {
        return false;
    }
    logLine(LogLevel::Debug,
        "filling lane " + std::string{laneName(lane)} + " (fill " + std::to_string(fill + 1) + ")");

    const auto started = Clock::now();
    for (const FrameSlice& slice : job.plan->slices) {
        if (job.board->stopped()) {
            return false;
        }
        const auto renderStart = Clock::now();
        const std::string uri = job.api->renderFrame(slice.frame);
        const double render = secondsSince(renderStart);

        const auto fetchStart = Clock::now();
        const std::filesystem::path part = job.buffer->partPath(lane, slice.frame);
        job.fetcher->fetch(uri, part);
        std::filesystem::rename(part, job.buffer->framePath(lane, slice.frame));
        const double fetch = secondsSince(fetchStart);

        job.board->frameWritten(lane);
        timing.renderSeconds.push_back(render);
        timing.fetchSeconds.push_back(fetch);
        logLine(LogLevel::Debug, "frame " + std::to_string(slice.frame.value()) + " -> lane " +
                                     std::string{laneName(lane)} + ": render " +
                                     formatFixed(render * 1000.0, 1) + " ms, fetch " +
                                     formatFixed(fetch * 1000.0, 1) + " ms");
    }
    timing.totalSeconds = secondsSince(started);
    const double audio = job.plan->length().seconds(job.settings.sampleRate);
    timing.realTimeFactor = timing.totalSeconds / audio;
    logFill(timing, job.settings.fills, audio);
    return true;
}

} // namespace

// Only std::bad_alloc, from building an error message, can escape the
// handlers; std::terminate is then the right end.
// NOLINTNEXTLINE(bugprone-exception-escape)
std::vector<FillTiming> runNetwork(LaneBoard& board, PlaybackApi& api, ObjectFetcher& fetcher,
    const FramePlan& plan, const BufferDir& buffer, NetworkSettings settings) noexcept {
    const NetworkContext job{.board = &board,
        .api = &api,
        .fetcher = &fetcher,
        .plan = &plan,
        .buffer = &buffer,
        .settings = settings};
    std::vector<FillTiming> timings;
    try {
        setThreadName("network");
        for (std::int64_t fill = 0; fill < job.settings.fills; ++fill) {
            FillTiming timing;
            if (!fillLane(job, fill, timing)) {
                break;
            }
            timings.push_back(std::move(timing));
        }
    } catch (const std::exception& error) {
        board.fail(error.what());
    } catch (...) {
        board.fail("the network thread failed with an unknown exception");
    }
    return timings;
}

} // namespace rainbohz::playback_client
