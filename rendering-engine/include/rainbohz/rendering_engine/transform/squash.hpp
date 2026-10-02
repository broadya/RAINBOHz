#pragma once

// Squash: placing an envelope's points in physical time when their computed
// times are out of order.
//
// Points along a musical timeline are in non-decreasing musical order, but
// each carries a physical offset, so once they are converted to physical time
// a later point can land earlier than one before it (a percussive attack of
// fixed length at a tempo where hits overlap), or before time zero. The user's
// order is the intended order of states and is always kept; time yields. The
// rule is docs/transform.md section 6.3 and ADR 0014. In outline, walking the
// points in order with a tail at time T, initially a floor at 0:
//
//   1. a point later than T is placed at its own time;
//   2. otherwise it collides: the run of points that keep stepping backwards
//      collapses to its last member, which replaces the tail's state at the
//      tail's time, and the earlier members are discarded;
//   3. the points after that run, up to the next point later than T, are
//      compressed in proportion into the room left between T and that point.
//
// Earlier placed points are never touched, so this is one forward pass.

#include "rainbohz/rendering_engine/units.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// Where one input point ended up: its position in the input, and its
/// physical time.
struct Placement {
    /// Index of the point in the input sequence.
    std::size_t index = 0;

    /// The time it was placed at.
    SampleTime time;

    friend bool operator==(const Placement&, const Placement&) = default;
};

/// Places points whose computed physical times may be out of order.
///
/// @param times  The points' computed physical times, in the points' semantic
///               (list) order. Every value must be finite.
/// @return A subsequence of the input indices, in increasing order, each with
///         its placed time. Times strictly increase and are never negative.
///         The last input point is always present; the first is present
///         unless a later point collided with it. Empty only for empty input.
/// @pre Every time is finite. Not checked.
std::vector<Placement> squash(std::span<const SampleTime> times);

} // namespace rainbohz::rendering_engine::transform
