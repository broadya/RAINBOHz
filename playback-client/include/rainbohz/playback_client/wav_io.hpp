// SPDX-License-Identifier: Apache-2.0

#pragma once

// Reading the frames rendering-engine writes, and writing --output-wav.
//
// Frames are 32-bit float WAV files (rendering-engine/docs/rendering.md §5).
// This version plays mono only: a frame must be one channel, at the session's
// sample rate, and exactly one frame long; anything else means the buffer
// does not hold what the session asked for, and is an error rather than
// something to convert. dr_wav does the container work, behind this header.

#include "rainbohz/playback_client/units.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace rainbohz::playback_client {

/// Thrown when a WAV file cannot be read or written, or is not what was
/// expected.
class WavError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Reads one rendered frame.
///
/// @param file  The frame's WAV file.
/// @param rate  The sample rate it must have.
/// @param size  The number of samples it must hold.
/// @return The frame's samples, `size` of them.
/// @throws WavError if the file cannot be opened or read, or is not mono, at
///         `rate`, and `size` samples long.
std::vector<float> readFrame(const std::filesystem::path& file, SampleRate rate, FrameSize size);

/// Writes a mono 32-bit float WAV file incrementally.
class WavWriter {
public:
    /// Opens `file` for writing, replacing it.
    ///
    /// @param file  The file.
    /// @param rate  Its sample rate.
    /// @throws WavError if the file cannot be opened.
    WavWriter(const std::filesystem::path& file, SampleRate rate);

    /// Finishes the file, if close() was not called; errors are ignored.
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    WavWriter(WavWriter&&) = delete;
    WavWriter& operator=(WavWriter&&) = delete;

    /// Appends samples.
    ///
    /// @param samples  Mono samples, in order.
    /// @throws WavError if they cannot all be written.
    void write(std::span<const float> samples);

    /// Finishes the file: writes its header's lengths and closes it.
    ///
    /// @throws WavError if it cannot be finished.
    void close();

private:
    struct Impl;
    /// dr_wav's state for the open file, kept out of this header.
    std::unique_ptr<Impl> impl_;
};

} // namespace rainbohz::playback_client
