// SPDX-License-Identifier: Apache-2.0

#pragma once

// Where the feeder sends the audio it plays.
//
// Two sinks: the operating system's default output device, which plays in
// real time, and a WAV file (--output-wav), which takes the samples as fast
// as they come, so that a test can check exactly what would have been played.
// The file sink leaves out the gaps a real device would fill with silence.
//
// write() and finish() are called by the feeder thread only, so that the
// session's main thread stays free to notice Ctrl-C while the output plays
// out; start() and abort() by the main thread. Neither sink is called from
// the audio callback; the device sink's callback is internal to it.

#include "rainbohz/playback_client/units.hpp"

#include <filesystem>
#include <memory>
#include <span>

namespace rainbohz::playback_client {

/// An audio output for mono samples at the session's sample rate.
class AudioSink {
public:
    /// Stops the output if it is still running.
    virtual ~AudioSink() = default;

    AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;
    AudioSink(AudioSink&&) = delete;
    AudioSink& operator=(AudioSink&&) = delete;

    /// Starts the output. Until the first write() it plays silence.
    ///
    /// @throws std::runtime_error if the output cannot be started.
    virtual void start() = 0;

    /// Hands samples to the output, blocking until it has taken them all.
    ///
    /// @param samples  Mono samples, in playing order.
    /// @return True when every sample was taken; false when abort() stopped
    ///         the write part-way.
    /// @throws std::runtime_error if the output fails.
    virtual bool write(std::span<const float> samples) = 0;

    /// Plays out everything written, then stops the output.
    ///
    /// @throws std::runtime_error if the output cannot be finished.
    /// @note Blocks for as long as the written audio takes to play.
    virtual void finish() = 0;

    /// Stops the output as soon as possible, discarding what is unplayed.
    /// A write() in progress returns false. Safe from any thread.
    virtual void abort() noexcept = 0;

    /// How many samples of silence the output has played because nothing had
    /// been written in time.
    ///
    /// @return The silence played between the first write() and finish(), in
    ///         samples at the session's rate. Always zero for a sink that is
    ///         not paced in real time.
    virtual SampleCount underrun() const noexcept = 0;
};

/// The operating system's default output device, through miniaudio.
///
/// The mono signal is copied to every channel of the device. If the device
/// does not run at `rate`, miniaudio resamples, and the sink logs that it
/// does.
///
/// @param rate  The rate of the samples written.
/// @return The sink, not yet started.
/// @throws std::runtime_error if there is no usable output device.
std::unique_ptr<AudioSink> makeDeviceSink(SampleRate rate);

/// A mono 32-bit float WAV file, written as fast as samples arrive.
///
/// @param file  The file, replaced if it exists.
/// @param rate  Its sample rate.
/// @return The sink. The file is opened by start() and complete after
///         finish(); abort() leaves it truncated but valid.
std::unique_ptr<AudioSink> makeWavFileSink(const std::filesystem::path& file, SampleRate rate);

} // namespace rainbohz::playback_client
