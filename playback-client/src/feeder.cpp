// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/feeder.hpp"

#include "rainbohz/playback_client/log.hpp"
#include "rainbohz/playback_client/wav_io.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <vector>

/// @file
/// @brief The playback thread: lanes to the audio output, in loop order.
///
/// @see feeder.hpp

namespace rainbohz::playback_client {

namespace {

/// The monotonic clock that waits and the startup time are measured with.
using Clock = std::chrono::steady_clock;

/// Waits shorter than this are not recorded: they are the cost of a lock and
/// a context switch, not the network thread falling behind.
constexpr double kWaitThresholdSeconds = 0.001;

/// Plays the feeder's loops, without the exception guard.
class Feeder {
public:
    /// A feeder over the objects runFeeder() was given.
    ///
    /// @param board     The shared lane state.
    /// @param plan      The frames of one pass.
    /// @param buffer    Where the lanes are.
    /// @param sink      Where the audio goes.
    /// @param settings  How many loops, and the sample rate.
    Feeder(LaneBoard& board, const FramePlan& plan, const BufferDir& buffer, AudioSink& sink,
        FeederSettings settings)
        : board_{&board}, plan_{&plan}, buffer_{&buffer}, sink_{&sink}, settings_{settings},
          started_{Clock::now()} {}

    /// Plays every loop, then finishes the sink.
    ///
    /// @return What it did.
    /// @throws WavError if a frame cannot be read; std::runtime_error if the
    ///         sink fails.
    FeederReport run() {
        for (std::int64_t loop = 0; loop < settings_.loops; ++loop) {
            if (!playLoop(loop)) {
                return report_;
            }
            report_.loopsPlayed = board_->loopPlayed();
            logLine(LogLevel::Info,
                "loop " + std::to_string(loop + 1) + " of " + std::to_string(settings_.loops) +
                    " handed to output (lane " + std::string{laneName(laneForLoop(loop))} + ")");
        }
        sink_->finish();
        reportUnderrun("the end");
        report_.finished = true;
        return report_;
    }

private:
    /// Plays one loop.
    ///
    /// @param loop  The loop, from 0.
    /// @return True when the whole loop was written; false when the session
    ///         stopped.
    bool playLoop(std::int64_t loop) {
        const Lane lane = laneForLoop(loop);
        if (loop == 0) {
            board_->beginStreaming(lane);
        } else {
            const auto waitStart = Clock::now();
            if (!board_->switchTo(lane)) {
                return false;
            }
            recordWait(loop, 0, waitStart, "lane " + std::string{laneName(lane)} + " to fill");
            reportUnderrun("loop " + std::to_string(loop + 1));
        }

        const auto& slices = plan_->slices;
        for (std::size_t index = 0; index < slices.size(); ++index) {
            const FrameSlice& slice = slices[index];
            if (loop == 0) {
                const auto waitStart = Clock::now();
                if (!board_->waitForFrame(lane, index)) {
                    return false;
                }
                if (index == 0) {
                    report_.startupSeconds =
                        std::chrono::duration<double>(Clock::now() - started_).count();
                    logLine(LogLevel::Info, "first frame ready after " +
                                                formatFixed(report_.startupSeconds, 3) +
                                                " s; playing");
                } else {
                    recordWait(loop, index, waitStart,
                        "frame " + std::to_string(slice.frame.value()) + " (streaming)");
                }
            }
            const std::vector<float> samples = readFrame(
                buffer_->framePath(lane, slice.frame), settings_.sampleRate, plan_->frameSize);
            const auto played =
                std::span<const float>{samples}.subspan(static_cast<std::size_t>(slice.begin),
                    static_cast<std::size_t>(slice.end - slice.begin));
            if (!sink_->write(played)) {
                return false;
            }
        }
        return true;
    }

    /// Records a wait, if it was long enough to matter.
    ///
    /// @param loop        The loop being played.
    /// @param frameIndex  The frame's position in the lane; 0 for a switch.
    /// @param waitStart   When the wait began.
    /// @param what        What was waited for, for the log.
    void recordWait(std::int64_t loop, std::size_t frameIndex, Clock::time_point waitStart,
        const std::string& what) {
        const double seconds = std::chrono::duration<double>(Clock::now() - waitStart).count();
        if (seconds < kWaitThresholdSeconds) {
            return;
        }
        report_.waits.push_back(
            FeederWait{.loop = loop, .frameIndex = frameIndex, .seconds = seconds});
        logLine(LogLevel::Debug, "waited " + formatFixed(seconds * 1000.0, 1) + " ms for " + what);
    }

    /// Logs the silence the output played since the last report, if any.
    ///
    /// The sink counts silence as it plays it, so what is reported at a
    /// loop's start is mostly the gap just before it.
    ///
    /// @param where  Where in the session this is, for the log.
    void reportUnderrun(const std::string& where) {
        const SampleCount underrun = sink_->underrun();
        const SampleCount fresh = underrun - reported_;
        reported_ = underrun;
        if (fresh.value() > 0) {
            logLine(LogLevel::Warn,
                "gap before " + where + ": " + std::to_string(fresh.value()) + " samples (" +
                    formatFixed(fresh.seconds(settings_.sampleRate) * 1000.0, 1) +
                    " ms) of silence; rendering did not keep up");
        }
    }

    /// The lane state shared with the network thread.
    LaneBoard* board_;
    /// The frames of one pass, and the samples played from each.
    const FramePlan* plan_;
    /// The run's buffer directory, where the lanes' frame files are.
    const BufferDir* buffer_;
    /// The audio output every loop is written to.
    AudioSink* sink_;
    /// How many loops to play, and the sample rate every frame must have.
    FeederSettings settings_;
    /// When the feeder was made; the startup time is measured from it.
    Clock::time_point started_;
    /// What the feeder has done so far; run() returns it.
    FeederReport report_;
    /// The sink's underrun total at the last reportUnderrun(), so only new
    /// silence is logged.
    SampleCount reported_;
};

} // namespace

// Only std::bad_alloc, from building an error message, can escape the
// handlers; std::terminate is then the right end.
// NOLINTNEXTLINE(bugprone-exception-escape)
FeederReport runFeeder(LaneBoard& board, const FramePlan& plan, const BufferDir& buffer,
    AudioSink& sink, FeederSettings settings) noexcept {
    try {
        setThreadName("playback");
        return Feeder{board, plan, buffer, sink, settings}.run();
    } catch (const std::exception& error) {
        board.fail(error.what());
    } catch (...) {
        board.fail("the playback thread failed with an unknown exception");
    }
    return FeederReport{};
}

} // namespace rainbohz::playback_client
