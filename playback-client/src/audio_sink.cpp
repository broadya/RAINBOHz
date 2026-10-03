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
    /// A sink writing `file` at `rate`.
    ///
    /// @param file  The file.
    /// @param rate  Its sample rate.
    WavFileSink(std::filesystem::path file, SampleRate rate)
        : file_{std::move(file)}, rate_{rate} {}

    void start() override { writer_.emplace(file_, rate_); }

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

    std::filesystem::path file_;
    SampleRate rate_;
    std::optional<WavWriter> writer_;
    std::atomic<bool> aborted_{false};
};

} // namespace

std::unique_ptr<AudioSink> makeWavFileSink(const std::filesystem::path& file, SampleRate rate) {
    return std::make_unique<WavFileSink>(file, rate);
}

} // namespace rainbohz::playback_client
