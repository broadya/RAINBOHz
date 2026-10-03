#include "rainbohz/playback_client/frame_plan.hpp"

#include <string>

/// @file
/// @brief Turns a range of the stream into frames and the samples of each.
///
/// @see frame_plan.hpp

namespace rainbohz::playback_client {

namespace {

/// Checks that an offset lies within a frame.
///
/// @param where   Which end of the range it is, for the diagnostic.
/// @param point   The frame and offset.
/// @param size    Samples per frame.
/// @throws FramePlanError if the offset is negative or not below `size`.
void checkOffset(const char* where, FrameOffset point, FrameSize size) {
    if (point.offset < 0 || point.offset >= size.value()) {
        throw FramePlanError{std::string{where} + " offset " + std::to_string(point.offset) +
                             " is outside a frame of " + std::to_string(size.value()) + " samples"};
    }
}

} // namespace

SampleCount FramePlan::length() const noexcept {
    SampleCount total;
    for (const FrameSlice& slice : slices) {
        total += slice.length();
    }
    return total;
}

FramePlan planFrames(FrameOffset start, FrameOffset end, FrameSize size) {
    if (size.value() <= 0) {
        throw FramePlanError{
            "frame size " + std::to_string(size.value()) + " is not a positive number of samples"};
    }
    checkOffset("start", start, size);
    checkOffset("end", end, size);

    const SampleIndex first = SampleIndex::inFrame(start.frame, start.offset, size);
    const SampleIndex last = SampleIndex::inFrame(end.frame, end.offset, size);
    if (last <= first) {
        throw FramePlanError{"the range is empty: its end, sample " + std::to_string(last.value()) +
                             ", is not after its start, sample " + std::to_string(first.value())};
    }

    // An end on a frame boundary needs nothing from that frame: the range
    // stops at the end of the one before.
    const std::int32_t lastFrame = end.offset == 0 ? end.frame.value() - 1 : end.frame.value();

    FramePlan plan{.frameSize = size, .slices = {}};
    for (std::int32_t frame = start.frame.value(); frame <= lastFrame; ++frame) {
        const std::int32_t begin = frame == start.frame.value() ? start.offset : 0;
        const std::int32_t stop = frame == end.frame.value() ? end.offset : size.value();
        plan.slices.push_back(FrameSlice{.frame = FrameNumber{frame}, .begin = begin, .end = stop});
    }
    return plan;
}

} // namespace rainbohz::playback_client
