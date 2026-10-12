// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/audio_sink.hpp"

#include "rainbohz/playback_client/wav_io.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

/// @file
/// @brief The WAV file sink, for --output-wav.
///
/// The device sink is in device_sink.cpp, the only file that includes
/// miniaudio.
///
/// @see audio_sink.hpp

namespace rainbohz::playback_client {

namespace {

/// Writes what would be played to a WAV file, unpaced.
class WavFileSink final : public AudioSink {
public:
    /// A sink writing `file` at `rate`, with `channels` channels.
    ///
    /// @param file      The file.
    /// @param rate      Its sample rate.
    /// @param channels  Its channels.
    WavFileSink(std::filesystem::path file, SampleRate rate, ChannelCount channels)
        : file_{std::move(file)}, rate_{rate}, channels_{channels} {}

    void start() override { writer_.emplace(file_, rate_, channels_); }

    bool write(std::span<const float> samples) override {
        if (aborted_.load()) {
            return false;
        }
        started().write(samples);
        return true;
    }

    void finish() override { started().close(); }

    void abort() noexcept override { aborted_.store(true); }

    SampleCount underrun() const noexcept override { return SampleCount{0}; }

private:
    /// The open writer.
    ///
    /// @return The writer start() opened.
    /// @throws std::logic_error if start() has not been called.
    WavWriter& started() {
        if (!writer_.has_value()) {
            throw std::logic_error{"the WAV sink was written to before it was started"};
        }
        return *writer_;
    }

    /// The WAV file to write.
    std::filesystem::path file_;
    /// The sample rate of the samples written, and of the file.
    SampleRate rate_;
    /// The channels of the samples written, and of the file.
    ChannelCount channels_;
    /// The open writer; empty until start().
    std::optional<WavWriter> writer_;
    /// Set by abort(): every later write() returns false.
    std::atomic<bool> aborted_{false};
};

} // namespace

std::unique_ptr<AudioSink> makeWavFileSink(
    const std::filesystem::path& file, SampleRate rate, ChannelCount channels) {
    return std::make_unique<WavFileSink>(file, rate, channels);
}

} // namespace rainbohz::playback_client
