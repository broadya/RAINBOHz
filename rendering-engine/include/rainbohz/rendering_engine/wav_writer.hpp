// SPDX-License-Identifier: Apache-2.0

#pragma once

// Writes a buffer of samples as a 32-bit float WAV file.
//
// A thin wrapper over dr_wav (ADR 0009), which is confined to wav_writer.cpp:
// no dr_wav type appears here, the same rule Ladybug, CLI11 and yaml-cpp are
// held to. Channels and sample rate are parameters of the one call, so that
// multi-channel output, when it arrives, is a change of argument rather than
// of writer (docs/rendering.md section 5).

#include "rainbohz/rendering_engine/sample_value.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace rainbohz::rendering_engine {

/// Thrown when a WAV file cannot be written. The file has been removed, so
/// that a partly written file is never mistaken for a rendered frame.
class WavWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Writes `interleaved` as a RIFF/WAVE file of IEEE float samples, 32 bits
/// per sample.
///
/// The container is dr_wav's: a 16-byte `fmt ` chunk with format tag 3, and
/// a `data` chunk. dr_wav writes no `fact` chunk for float data, which the
/// WAVE specification asks for and which readers in practice do not require;
/// test_wav_writer pins exactly what is written.
///
/// @param file         Where to write. An existing file is overwritten
///                     without comment: the same frame from the same database
///                     is the same audio.
/// @param rate         Sample rate recorded in the header. Positive.
/// @param channels     Number of channels, 1 or more. The samples are
///                     interleaved by channel: all channels of frame 0, then
///                     all channels of frame 1, and so on.
/// @param interleaved  The samples. Its size must be a multiple of
///                     `channels`; it may be empty, which writes a valid file
///                     of no frames.
/// @throws WavWriteError  If the file cannot be created or written in full,
///         or if `channels` is less than 1 or does not divide the sample
///         count. Any partly written file has been removed.
/// @note Side effect: creates or replaces `file` on disk.
void writeFloatWav(const std::filesystem::path& file, SampleRate rate, std::int32_t channels,
    std::span<const SampleValue> interleaved);

} // namespace rainbohz::rendering_engine
