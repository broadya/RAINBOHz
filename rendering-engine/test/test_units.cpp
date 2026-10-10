// SPDX-License-Identifier: Apache-2.0

// Tests for the unit types.
//
// Half of what these types are for is what they refuse to do, so much of this
// file is compile-time: each static_assert states an operation that must, or
// must not, compile.

#include "rainbohz/rendering_engine/units.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace {

using rainbohz::rendering_engine::AngularFrequency;
using rainbohz::rendering_engine::frameContaining;
using rainbohz::rendering_engine::FrameNumber;
using rainbohz::rendering_engine::FrameSize;
using rainbohz::rendering_engine::frameStart;
using rainbohz::rendering_engine::FrequencyGradient;
using rainbohz::rendering_engine::lastFrameBefore;
using rainbohz::rendering_engine::Phase;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::SampleTime;

template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };

template <typename A, typename B>
concept Multipliable = requires(A a, B b) { a * b; };

template <typename A, typename B>
concept Subtractable = requires(A a, B b) { a - b; };

// No implicit conversion from, or to, a representation.
static_assert(!std::is_convertible_v<double, SampleTime>);
static_assert(!std::is_convertible_v<double, SampleDuration>);
static_assert(!std::is_convertible_v<double, Phase>);
static_assert(!std::is_convertible_v<double, AngularFrequency>);
static_assert(!std::is_convertible_v<std::int32_t, FrameNumber>);
static_assert(!std::is_convertible_v<SampleTime, double>);
static_assert(!std::is_convertible_v<Phase, double>);

// No conversion between unit types that share a representation.
static_assert(!std::is_convertible_v<FrameSize, FrameNumber>);
static_assert(!std::is_convertible_v<FrameNumber, FrameSize>);
static_assert(!std::is_constructible_v<FrameNumber, FrameSize>);
static_assert(!std::is_convertible_v<SampleTime, SampleDuration>);
static_assert(!std::is_convertible_v<SampleDuration, SampleTime>);
static_assert(!std::is_convertible_v<Phase, SampleDuration>);

// Absolute and relative time, as with std::chrono.
static_assert(std::same_as<decltype(SampleTime{} - SampleTime{}), SampleDuration>);
static_assert(std::same_as<decltype(SampleTime{} + SampleDuration{}), SampleTime>);
static_assert(!Addable<SampleTime, SampleTime>);

// Cross-unit products give the physically correct type.
static_assert(std::same_as<decltype(AngularFrequency{} * SampleDuration{}), Phase>);
static_assert(std::same_as<decltype(FrequencyGradient{} * SampleDuration{}), AngularFrequency>);
static_assert(std::same_as<decltype(AngularFrequency{} / SampleDuration{1.0}), FrequencyGradient>);
static_assert(std::same_as<decltype(SampleDuration{} / SampleDuration{1.0}), double>);

// And nothing else does.
static_assert(!Multipliable<AngularFrequency, SampleTime>);
static_assert(!Multipliable<AngularFrequency, AngularFrequency>);
static_assert(!Addable<Phase, AngularFrequency>);
static_assert(!Addable<SampleDuration, Phase>);
static_assert(!Addable<FrameNumber, FrameSize>);
static_assert(!Subtractable<FrameNumber, FrameSize>);

// Zero cost: the same size as the representation, and trivially copyable.
static_assert(sizeof(SampleTime) == sizeof(double));
static_assert(sizeof(FrameNumber) == sizeof(std::int32_t));
static_assert(std::is_trivially_copyable_v<SampleTime>);
static_assert(std::is_trivially_copyable_v<Phase>);
static_assert(std::is_trivially_copyable_v<FrameNumber>);

TEST(Units, TimeArithmetic) {
    const SampleTime start{100.5};
    const SampleTime end{250.0};
    EXPECT_EQ((end - start).value(), 149.5);
    EXPECT_EQ((start + SampleDuration{10.0}).value(), 110.5);
    EXPECT_EQ((end - SampleDuration{50.0}).value(), 200.0);
    EXPECT_LT(start, end);
}

TEST(Units, PhaseFromFrequencyAndDuration) {
    const Phase phase = AngularFrequency{0.25} * SampleDuration{8.0};
    EXPECT_EQ(phase.value(), 2.0);
}

TEST(Units, GradientFromFrequencyChangeOverDuration) {
    const FrequencyGradient gradient = AngularFrequency{0.1} / SampleDuration{1000.0};
    EXPECT_DOUBLE_EQ(gradient.value(), 1e-4);
    EXPECT_DOUBLE_EQ((gradient * SampleDuration{500.0}).value(), 0.05);
}

TEST(Units, OneSecondFrameSize) {
    EXPECT_EQ(FrameSize::oneSecond(SampleRate{48000}).value(), 48000);
}

TEST(Units, FrameContainingUsesHalfOpenFrames) {
    const FrameSize size{1000};
    EXPECT_EQ(frameContaining(SampleTime{0.0}, size)->value(), 0);
    EXPECT_EQ(frameContaining(SampleTime{999.999}, size)->value(), 0);
    EXPECT_EQ(frameContaining(SampleTime{1000.0}, size)->value(), 1);
}

TEST(Units, LastFrameBeforeExcludesTheEnd) {
    const FrameSize size{1000};
    // A range ending exactly on a frame boundary does not reach that frame.
    EXPECT_EQ(lastFrameBefore(SampleTime{2000.0}, size)->value(), 1);
    EXPECT_EQ(lastFrameBefore(SampleTime{2000.5}, size)->value(), 2);
    EXPECT_EQ(lastFrameBefore(SampleTime{0.5}, size)->value(), 0);
}

TEST(Units, FrameNumbersOutsideInt32AreRefused) {
    const FrameSize size{1};
    const double beyond = static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 1.0;
    EXPECT_FALSE(frameContaining(SampleTime{beyond}, size).has_value());
    EXPECT_FALSE(frameContaining(SampleTime{std::numeric_limits<double>::infinity()}, size));
    EXPECT_FALSE(lastFrameBefore(SampleTime{std::numeric_limits<double>::quiet_NaN()}, size));
}

TEST(Units, FrameStart) {
    EXPECT_EQ(frameStart(FrameNumber{3}, FrameSize{48000}).value(), 144000.0);
}

} // namespace
