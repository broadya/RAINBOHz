// SPDX-License-Identifier: Apache-2.0

// Tests for the squash rule (docs/transform.md section 6.3, ADR 0014).
//
// Each case states a sequence of computed times in semantic order and the
// placements the rule must produce. Times are plain numbers of samples; the
// rule does not care about the unit.

#include "rainbohz/rendering_engine/transform/squash.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <vector>

namespace {

using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::transform::Placement;
using rainbohz::rendering_engine::transform::squash;

/// Runs squash over plain numbers.
///
/// @param times  Computed times, in semantic order.
/// @return The placements.
std::vector<Placement> squashed(std::initializer_list<double> times) {
    std::vector<SampleTime> input;
    input.reserve(times.size());
    for (const double t : times) {
        input.emplace_back(t);
    }
    return squash(input);
}

/// A placement for an expectation.
///
/// @param index  Input index.
/// @param time   Placed time.
/// @return The placement.
Placement at(std::size_t index, double time) {
    return Placement{.index = index, .time = SampleTime{time}};
}

/// Checks the invariants every result must have: indices and times strictly
/// increase, no time is negative, and the last input point is present.
///
/// @param placed  A result.
/// @param n       The input's size.
void expectWellFormed(const std::vector<Placement>& placed, std::size_t n) {
    ASSERT_FALSE(placed.empty());
    EXPECT_GE(placed.front().time, SampleTime{0.0});
    for (std::size_t i = 1; i < placed.size(); ++i) {
        EXPECT_LT(placed[i - 1].index, placed[i].index);
        EXPECT_LT(placed[i - 1].time, placed[i].time);
    }
    EXPECT_EQ(placed.back().index, n - 1);
}

TEST(Squash, PointsInOrderStayWhereTheyAre) {
    EXPECT_EQ(squashed({0.0, 10.0, 20.0}), (std::vector{at(0, 0.0), at(1, 10.0), at(2, 20.0)}));
    EXPECT_EQ(squashed({5.5, 6.0}), (std::vector{at(0, 5.5), at(1, 6.0)}));
}

TEST(Squash, EmptyInputGivesNothing) {
    EXPECT_TRUE(squashed({}).empty());
}

TEST(Squash, AFirstPointBeforeZeroIsPlacedAtZero) {
    EXPECT_EQ(squashed({-5.0, 10.0}), (std::vector{at(0, 0.0), at(1, 10.0)}));
    EXPECT_EQ(squashed({0.0, 10.0}), (std::vector{at(0, 0.0), at(1, 10.0)}));
}

TEST(Squash, ALaterPointReplacesTheTailItCollidesWith) {
    // 50 lands before the tail at 100: it takes the tail's time and place.
    EXPECT_EQ(
        squashed({0.0, 100.0, 50.0, 200.0}), (std::vector{at(0, 0.0), at(2, 100.0), at(3, 200.0)}));
}

TEST(Squash, EqualTimesCollapseAndTheLaterStateWins) {
    EXPECT_EQ(
        squashed({0.0, 10.0, 10.0, 20.0}), (std::vector{at(0, 0.0), at(2, 10.0), at(3, 20.0)}));
}

TEST(Squash, ADecreasingRunCollapsesToItsLastMember) {
    // 60 then 40 keep stepping backwards; 40 replaces the tail, 60 is dropped.
    EXPECT_EQ(squashed({0.0, 100.0, 60.0, 40.0, 200.0}),
        (std::vector{at(0, 0.0), at(3, 100.0), at(4, 200.0)}));
}

TEST(Squash, PointsAfterACollisionAreCompressedInProportion) {
    // 100 collides with the tail at 200 and takes its place. 150 sat half way
    // from 100 to the next in-order point, 300; it is placed half way from
    // 200 to 300.
    EXPECT_EQ(squashed({0.0, 200.0, 100.0, 150.0, 300.0}),
        (std::vector{at(0, 0.0), at(2, 200.0), at(3, 225.0), at(4, 300.0)}));
}

TEST(Squash, ACollisionInsideACompressedStretchFollowsTheSameRule) {
    // As above, with 120 after 150: it maps to 210, behind 225, so it
    // replaces 150 at 225.
    EXPECT_EQ(squashed({0.0, 200.0, 100.0, 150.0, 120.0, 300.0}),
        (std::vector{at(0, 0.0), at(2, 200.0), at(4, 225.0), at(5, 300.0)}));
}

TEST(Squash, WithNoLaterPointEverythingCollapsesOntoTheTail) {
    // After 50 collides with the tail at 100, 80 is still behind it: the
    // envelope ends at 100 with the last state.
    EXPECT_EQ(squashed({0.0, 100.0, 50.0, 80.0}), (std::vector{at(0, 0.0), at(3, 100.0)}));
}

TEST(Squash, EarlierPlacedPointsAreNeverTouched) {
    // 5 computes earlier than every placed point but the first; only the
    // tail yields.
    EXPECT_EQ(squashed({0.0, 10.0, 20.0, 30.0, 5.0, 40.0}),
        (std::vector{at(0, 0.0), at(1, 10.0), at(2, 20.0), at(4, 30.0), at(5, 40.0)}));
}

TEST(Squash, TheFloorBehavesLikeATail) {
    // -20 is between -50 and 100 at a third of the way; it keeps that
    // proportion between 0 and 100.
    EXPECT_EQ(
        squashed({-50.0, -20.0, 100.0}), (std::vector{at(0, 0.0), at(1, 20.0), at(2, 100.0)}));
    // A decreasing run at the floor collapses to its last member at 0.
    EXPECT_EQ(squashed({-10.0, -20.0, 5.0}), (std::vector{at(1, 0.0), at(2, 5.0)}));
}

TEST(Squash, AllPointsBeforeZeroLeaveOnePointAtZero) {
    EXPECT_EQ(squashed({-30.0, -20.0, -10.0}), (std::vector{at(2, 0.0)}));
}

TEST(Squash, ResultsAreAlwaysWellFormed) {
    const std::vector<std::vector<double>> cases{
        {0.0, 10.0, 20.0},
        {-5.0, 10.0},
        {0.0, 100.0, 50.0, 200.0},
        {0.0, 200.0, 100.0, 150.0, 120.0, 300.0},
        {0.0, 100.0, 50.0, 80.0},
        {-30.0, -20.0, -10.0},
        {3.0, 1.0, 4.0, 1.0, 5.0, 9.0, 2.0, 6.0, 5.0, 3.0, 5.0},
        {100.0, 90.0, 80.0, 70.0, 60.0},
        {0.0, 0.0, 0.0, 1.0, 1.0},
    };
    for (const std::vector<double>& times : cases) {
        std::vector<SampleTime> input;
        input.reserve(times.size());
        for (const double t : times) {
            input.emplace_back(t);
        }
        expectWellFormed(squash(input), input.size());
    }
}

} // namespace
