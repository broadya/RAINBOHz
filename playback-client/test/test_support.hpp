#pragma once

// Fakes for driving playback-client's threads without a server or a device.
//
// FakePlaybackApi renders by writing real WAV files, so the whole path --
// fetch, rename, publish, read, trim -- runs as it does against a server.
// Every sample of a frame it writes is fill × 100 + frame number, so the
// output of a session says, sample by sample, which fill of which frame was
// played: a lane overwritten while it was being read, or a loop played from
// the wrong fill, shows up as a wrong value.

#include "rainbohz/playback_client/audio_sink.hpp"
#include "rainbohz/playback_client/playback_api.hpp"
#include "rainbohz/playback_client/wav_io.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace rainbohz::playback_client::test {

/// A temporary directory, removed with everything in it on destruction.
class TempDir {
public:
    TempDir() {
        std::mt19937_64 random{std::random_device{}()};
        path_ = std::filesystem::temp_directory_path() /
                ("playback-client-test-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// The value every sample of `frame` from `fill` holds.
inline float frameValue(std::int64_t fill, std::int32_t frame) {
    return static_cast<float>((fill * 100) + frame);
}

/// A scripted PlaybackApi.
class FakePlaybackApi final : public PlaybackApi {
public:
    /// An API whose frames are written beneath `outDir`.
    FakePlaybackApi(std::filesystem::path outDir, std::int32_t sampleRate, std::int32_t frameSize,
        std::size_t framesPerLane)
        : outDir_{std::move(outDir)}, sampleRate_{sampleRate}, frameSize_{frameSize},
          framesPerLane_{framesPerLane} {}

    EngineParameters engineParameters() override {
        return EngineParameters{.sampleRate = SampleRate{sampleRate_},
            .frameSize = FrameSize{frameSize_},
            .channelMode = channelMode};
    }

    TempoMapSummary rootTempoMap() override {
        return TempoMapSummary{.id = 7, .name = "Main", .ticksPerBeat = ticksPerBeat};
    }

    /// Positions map linearly: one tick is `samplesPerTick` samples.
    FrameOffset framePosition(std::int64_t tempoMapId, const MusicalPosition& at) override {
        lastTempoMapId = tempoMapId;
        const std::int64_t ticks = (at.beats * ticksPerBeat.value_or(1)) + at.ticks;
        const std::int64_t sample = ticks * samplesPerTick;
        return FrameOffset{.frame = FrameNumber{static_cast<std::int32_t>(sample / frameSize_)},
            .offset = static_cast<std::int32_t>(sample % frameSize_)};
    }

    std::string renderFrame(FrameNumber frame) override {
        const std::int64_t call = calls_.fetch_add(1);
        if (onRender) {
            onRender(call);
        }
        if (renderDelay.count() > 0) {
            std::this_thread::sleep_for(renderDelay);
        }
        if (failAtCall.has_value() && call == *failAtCall) {
            throw PlaybackApiError{"scripted failure at call " + std::to_string(call), 500};
        }
        const auto fill =
            static_cast<std::int64_t>(static_cast<std::size_t>(call) / framesPerLane_);
        const std::int32_t length =
            shortFrameInFill.has_value() && fill == *shortFrameInFill ? frameSize_ - 1 : frameSize_;
        // The server writes the same file name for a frame every time, as
        // rendering-engine does.
        const std::filesystem::path file =
            outDir_ / ("frame-" + std::to_string(frame.value()) + ".wav");
        {
            WavWriter writer{file, SampleRate{sampleRate_}};
            const std::vector<float> samples(
                static_cast<std::size_t>(length), frameValue(fill, frame.value()));
            writer.write(samples);
            writer.close();
        }
        return "file://" + file.string();
    }

    std::int64_t calls() const { return calls_.load(); }

    std::string channelMode = "mono";
    std::optional<std::int32_t> ticksPerBeat = 4;
    std::int64_t samplesPerTick = 1;
    std::chrono::milliseconds renderDelay{0};
    std::optional<std::int64_t> failAtCall;
    std::optional<std::int64_t> shortFrameInFill;
    std::function<void(std::int64_t)> onRender;
    std::int64_t lastTempoMapId = -1;

private:
    std::filesystem::path outDir_;
    std::int32_t sampleRate_;
    std::int32_t frameSize_;
    std::size_t framesPerLane_;
    std::atomic<std::int64_t> calls_{0};
};

/// An AudioSink that keeps everything written, optionally slowly.
class MemorySink final : public AudioSink {
public:
    void start() override { started = true; }

    bool write(std::span<const float> samples) override {
        if (aborted_.load()) {
            return false;
        }
        if (writeDelay.count() > 0) {
            std::this_thread::sleep_for(writeDelay);
        }
        const std::scoped_lock lock{mutex_};
        samples_.insert(samples_.end(), samples.begin(), samples.end());
        return true;
    }

    void finish() override { finished = true; }

    void abort() noexcept override { aborted_.store(true); }

    SampleCount underrun() const noexcept override { return SampleCount{0}; }

    /// Everything written so far.
    std::vector<float> samples() const {
        const std::scoped_lock lock{mutex_};
        return samples_;
    }

    /// How many samples have been written so far.
    std::size_t size() const {
        const std::scoped_lock lock{mutex_};
        return samples_.size();
    }

    std::chrono::milliseconds writeDelay{0};
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

private:
    mutable std::mutex mutex_;
    std::vector<float> samples_;
    std::atomic<bool> aborted_{false};
};

} // namespace rainbohz::playback_client::test
