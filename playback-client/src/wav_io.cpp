// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/wav_io.hpp"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief The dr_wav-backed frame reader and WAV writer.
///
/// This is the only translation unit that includes dr_wav.h, and the one
/// that carries its implementation. miniaudio carries its own, renamed copy
/// of dr_wav, so the two do not collide.
///
/// @see wav_io.hpp

namespace rainbohz::playback_client {

std::vector<float> readFrame(
    const std::filesystem::path& file, SampleRate rate, ChannelCount channels, FrameSize size) {
    drwav wav{};
    if (drwav_init_file(&wav, file.string().c_str(), nullptr) == DRWAV_FALSE) {
        throw WavError{"cannot open " + file.string() + " as a WAV file"};
    }
    const auto close = std::unique_ptr<drwav, decltype(&drwav_uninit)>{&wav, &drwav_uninit};

    if (std::cmp_not_equal(wav.channels, channels.value())) {
        throw WavError{file.string() + " has " + std::to_string(wav.channels) +
                       " channel(s), not the score's " + std::to_string(channels.value())};
    }
    if (std::cmp_not_equal(wav.sampleRate, rate.value())) {
        throw WavError{file.string() + " is at " + std::to_string(wav.sampleRate) +
                       " Hz, not the score's " + std::to_string(rate.value()) + " Hz"};
    }
    if (std::cmp_not_equal(wav.totalPCMFrameCount, size.value())) {
        throw WavError{file.string() + " holds " + std::to_string(wav.totalPCMFrameCount) +
                       " samples, not a frame of " + std::to_string(size.value())};
    }

    std::vector<float> samples(
        static_cast<std::size_t>(size.value()) * static_cast<std::size_t>(channels.value()));
    const drwav_uint64 read =
        drwav_read_pcm_frames_f32(&wav, static_cast<drwav_uint64>(size.value()), samples.data());
    if (std::cmp_not_equal(read, size.value())) {
        throw WavError{"read only " + std::to_string(read) + " of " + std::to_string(size.value()) +
                       " sample frames from " + file.string()};
    }
    return samples;
}

/// The open dr_wav writer.
struct WavWriter::Impl {
    /// dr_wav's state for the open file.
    drwav wav{};
    /// The file, for diagnostics.
    std::filesystem::path file;
    /// Values per sample frame.
    std::size_t channels = 1;
    /// Whether `wav` is open.
    bool open = false;
};

WavWriter::WavWriter(const std::filesystem::path& file, SampleRate rate, ChannelCount channels)
    : impl_{std::make_unique<Impl>()} {
    impl_->file = file;
    impl_->channels = static_cast<std::size_t>(channels.value());
    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = static_cast<drwav_uint32>(channels.value());
    format.sampleRate = static_cast<drwav_uint32>(rate.value());
    format.bitsPerSample = 32;
    if (drwav_init_file_write(&impl_->wav, file.string().c_str(), &format, nullptr) ==
        DRWAV_FALSE) {
        throw WavError{"cannot open " + file.string() + " for writing"};
    }
    impl_->open = true;
}

WavWriter::~WavWriter() {
    if (impl_ && impl_->open) {
        drwav_uninit(&impl_->wav);
    }
}

void WavWriter::write(std::span<const float> samples) {
    if (samples.size() % impl_->channels != 0) {
        throw WavError{std::to_string(samples.size()) + " values are not whole sample frames of " +
                       std::to_string(impl_->channels) + " channel(s), for " +
                       impl_->file.string()};
    }
    const std::size_t frames = samples.size() / impl_->channels;
    const drwav_uint64 written = drwav_write_pcm_frames(&impl_->wav, frames, samples.data());
    if (written != frames) {
        throw WavError{"wrote only " + std::to_string(written) + " of " + std::to_string(frames) +
                       " sample frames to " + impl_->file.string()};
    }
}

void WavWriter::close() {
    if (!impl_->open) {
        return;
    }
    impl_->open = false;
    if (drwav_uninit(&impl_->wav) != DRWAV_SUCCESS) {
        throw WavError{"cannot finish " + impl_->file.string()};
    }
}

} // namespace rainbohz::playback_client
