#include "rainbohz/rendering_engine/wav_writer.hpp"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

/// @file
/// @brief The dr_wav-backed implementation of writeFloatWav().
///
/// This is the only translation unit that includes dr_wav.h, and the one that
/// carries its implementation (DR_WAV_IMPLEMENTATION). The header is included
/// as a SYSTEM header (cmake/Dependencies.cmake), so the project's warning
/// set does not fire on it.
///
/// @see wav_writer.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Removes a file that was being written, ignoring any error.
///
/// @param file  The file. Need not exist.
void removeQuietly(const std::filesystem::path& file) noexcept {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

} // namespace

void writeFloatWav(const std::filesystem::path& file, SampleRate rate, std::int32_t channels,
    std::span<const SampleValue> interleaved) {
    if (channels < 1) {
        throw WavWriteError{"cannot write " + file.string() + ": channel count " +
                            std::to_string(channels) + " is not positive"};
    }
    if (interleaved.size() % static_cast<std::size_t>(channels) != 0) {
        throw WavWriteError{"cannot write " + file.string() + ": " +
                            std::to_string(interleaved.size()) + " samples do not divide into " +
                            std::to_string(channels) + " channel(s)"};
    }
    const drwav_uint64 frames = interleaved.size() / static_cast<std::size_t>(channels);

    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = static_cast<drwav_uint32>(channels);
    format.sampleRate = static_cast<drwav_uint32>(rate.value());
    format.bitsPerSample = 32;

    drwav wav{};
    const std::string path = file.string();
    if (drwav_init_file_write_sequential_pcm_frames(&wav, path.c_str(), &format, frames, nullptr) ==
        DRWAV_FALSE) {
        removeQuietly(file);
        throw WavWriteError{"cannot create " + path};
    }

    const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, interleaved.data());
    const drwav_result closed = drwav_uninit(&wav);
    if (written != frames || closed != DRWAV_SUCCESS) {
        removeQuietly(file);
        throw WavWriteError{"cannot write " + path + ": wrote " + std::to_string(written) + " of " +
                            std::to_string(frames) + " frame(s)"};
    }
}

} // namespace rainbohz::rendering_engine
