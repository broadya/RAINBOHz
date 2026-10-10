// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/spatial_poc/units.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace rainbohz::spatial_poc {
namespace {

// Unit types must be free at run time and must not convert implicitly.
static_assert(std::is_trivially_copyable_v<Radians>);
static_assert(std::is_trivially_copyable_v<Metres>);
static_assert(std::is_trivially_copyable_v<LinearGain>);
static_assert(!std::is_convertible_v<double, Radians>);
static_assert(!std::is_convertible_v<double, Metres>);
static_assert(!std::is_convertible_v<double, LinearGain>);
static_assert(!std::is_convertible_v<Radians, Metres>);
static_assert(!std::is_convertible_v<Metres, LinearGain>);

TEST(Units, ValueRoundTrips) {
    EXPECT_EQ(Radians{1.25}.value(), 1.25);
    EXPECT_EQ(Metres{7.5}.value(), 7.5);
    EXPECT_EQ(LinearGain{-0.5}.value(), -0.5);
}

TEST(Units, DefaultGainIsSilence) {
    EXPECT_EQ(LinearGain{}.value(), 0.0);
}

TEST(Units, GainsMultiplyInSeries) {
    EXPECT_EQ((LinearGain{-0.5} * LinearGain{0.25}).value(), -0.125);
}

TEST(Units, Ordering) {
    EXPECT_LT(Radians{-1.0}, Radians{1.0});
    EXPECT_GT(Metres{2.0}, Metres{1.0});
    EXPECT_EQ(LinearGain{0.5}, LinearGain{0.5});
}

} // namespace
} // namespace rainbohz::spatial_poc
