#pragma once

// Which frames a range of the score needs, and which samples of each.
//
// The range is half-open, [start, end), as the server's frame-position call
// and a frame are (rendering-engine/docs/server.md §3.4). Each end is given
// as the server reports it: a frame and an offset within it. The first frame
// is played from the start's offset, the last up to the end's offset, and
// every frame between them whole. A range that ends exactly on a frame
// boundary (end offset 0) does not need that frame at all.

#include "rainbohz/playback_client/units.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rainbohz::playback_client {

/// Thrown when a range cannot be planned.
class FramePlanError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A sample of the stream, as the server locates it: a frame, and the
/// sample's index within that frame.
struct FrameOffset {
    /// The frame holding the sample.
    FrameNumber frame{0};
    /// The sample's index within the frame, from 0 to below the frame size.
    std::int32_t offset = 0;
};

/// The part of one frame a range plays.
struct FrameSlice {
    /// The frame.
    FrameNumber frame{0};
    /// The first sample played, as an index within the frame.
    std::int32_t begin = 0;
    /// One past the last sample played, as an index within the frame; above
    /// `begin` and at most the frame size.
    std::int32_t end = 0;

    /// The number of samples played from this frame.
    ///
    /// @return end − begin.
    constexpr SampleCount length() const noexcept { return SampleCount{end - begin}; }
};

/// The frames of a range, in order, and the samples played from each.
struct FramePlan {
    /// Samples per frame.
    FrameSize frameSize{1};
    /// One slice per frame needed, in increasing frame order, with no gaps.
    std::vector<FrameSlice> slices;

    /// The samples one pass over the range plays.
    ///
    /// @return The sum of the slices' lengths.
    SampleCount length() const noexcept;
};

/// Plans the frames of the range [start, end).
///
/// @param start  The range's first sample.
/// @param end    One past the range's last sample.
/// @param size   Samples per frame; positive.
/// @return The plan, with at least one slice.
/// @throws FramePlanError if `size` is not positive, an offset is negative or
///         not below `size`, or `end` is not after `start` (an empty or
///         reversed range, which a range shorter than one sample also gives).
FramePlan planFrames(FrameOffset start, FrameOffset end, FrameSize size);

} // namespace rainbohz::playback_client
