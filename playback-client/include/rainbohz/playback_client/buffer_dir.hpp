// SPDX-License-Identifier: Apache-2.0

#pragma once

// The client's audio buffer on disk: one directory per run, two lanes in it.
//
//   <root>/rainbohz-playback-XXXXXX/
//       lane-a/frame-000003.wav
//       lane-b/frame-000003.wav
//
// Each run makes its own uniquely named directory beneath the root, so two
// clients given the same root never share files, and a run never reads a
// file left by an earlier one. Removing the buffer removes that directory
// only, never the root the user gave. A frame is written first under a
// `.part` name and renamed into place once complete, so a `.wav` file in a
// lane is always whole. The lanes are named for the frames' file names
// rendering-engine writes (`frame-NNNNNN.wav`).

#include "rainbohz/playback_client/units.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace rainbohz::playback_client {

/// Thrown when the buffer directory cannot be made or removed.
class BufferDirError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One of the two lanes of the buffer.
enum class Lane : std::uint8_t { A, B };

/// The other lane.
///
/// @param lane  A lane.
/// @return B for A, A for B.
constexpr Lane otherLane(Lane lane) noexcept {
    return lane == Lane::A ? Lane::B : Lane::A;
}

/// The lane loop `loop` of a session is played from, and fill `loop` written
/// to: A, B, A, B, ...
///
/// @param loop  The loop number, from 0.
/// @return A for even loops, B for odd.
constexpr Lane laneForLoop(std::int64_t loop) noexcept {
    return loop % 2 == 0 ? Lane::A : Lane::B;
}

/// The lane's name, `a` or `b`.
///
/// @param lane  The lane.
/// @return A string literal.
std::string_view laneName(Lane lane) noexcept;

/// A run's buffer directory.
class BufferDir {
public:
    /// Makes a new run directory, with both lane directories, beneath `root`.
    ///
    /// @param root  An existing directory, e.g. the system's temporary
    ///              directory.
    /// @throws BufferDirError if `root` is not a directory or the run
    ///         directory cannot be made.
    /// @note Side effect: creates the run directory and its lanes.
    explicit BufferDir(const std::filesystem::path& root);

    /// The run directory.
    ///
    /// @return `<root>/rainbohz-playback-XXXXXX`. Valid while this object is.
    const std::filesystem::path& path() const noexcept { return path_; }

    /// The directory of a lane.
    ///
    /// @param lane  The lane.
    /// @return `<run>/lane-a` or `<run>/lane-b`.
    std::filesystem::path laneDir(Lane lane) const;

    /// Where a frame lives in a lane once complete.
    ///
    /// @param lane   The lane.
    /// @param frame  The frame.
    /// @return `<lane>/frame-NNNNNN.wav`.
    std::filesystem::path framePath(Lane lane, FrameNumber frame) const;

    /// Where a frame is written before it is complete.
    ///
    /// @param lane   The lane.
    /// @param frame  The frame.
    /// @return framePath() with `.part` appended.
    std::filesystem::path partPath(Lane lane, FrameNumber frame) const;

    /// Removes the run directory and everything in it.
    ///
    /// @throws BufferDirError if it cannot be removed.
    /// @note Side effect: deletes files. The root is left alone.
    void remove() const;

private:
    /// The run directory, `<root>/rainbohz-playback-XXXXXX`.
    std::filesystem::path path_;
};

} // namespace rainbohz::playback_client
