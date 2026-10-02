// Tests for the musical unit types, and the seconds types they convert to.
//
// As with test_units.cpp, much of this is compile-time: a static_assert
// states an operation that must, or must not, compile.

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace {

using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::SampleTime;
using rainbohz::rendering_engine::SecondsDuration;
using rainbohz::rendering_engine::SecondsTime;
using rainbohz::rendering_engine::toSampleDuration;
using rainbohz::rendering_engine::toSampleTime;
using rainbohz::rendering_engine::transform::beats;
using rainbohz::rendering_engine::transform::MusicalDuration;
using rainbohz::rendering_engine::transform::MusicalTime;
using rainbohz::rendering_engine::transform::Tempo;
using rainbohz::rendering_engine::transform::TicksPerBeat;

template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };

template <typename A, typename B>
concept Subtractable = requires(A a, B b) { a - b; };

// No implicit conversion from, or to, a representation.
static_assert(!std::is_convertible_v<std::int64_t, MusicalTime>);
static_assert(!std::is_convertible_v<MusicalTime, std::int64_t>);
static_assert(!std::is_convertible_v<double, SecondsTime>);
static_assert(!std::is_convertible_v<MusicalTime, SampleTime>);
static_assert(!std::is_convertible_v<SecondsTime, SampleTime>);
static_assert(!std::is_convertible_v<MusicalDuration, SecondsDuration>);

// Time and duration algebra, as with the sample types.
static_assert(!Addable<MusicalTime, MusicalTime>);
static_assert(Addable<MusicalTime, MusicalDuration>);
static_assert(Subtractable<MusicalTime, MusicalTime>);
static_assert(std::is_same_v<decltype(MusicalTime{} - MusicalTime{}), MusicalDuration>);
static_assert(!Addable<SecondsTime, SecondsTime>);
static_assert(Addable<SecondsTime, SecondsDuration>);
static_assert(std::is_same_v<decltype(SecondsTime{} - SecondsTime{}), SecondsDuration>);

// Musical and physical quantities never mix by arithmetic.
static_assert(!Addable<MusicalTime, SecondsDuration>);
static_assert(!Addable<SecondsTime, MusicalDuration>);
static_assert(!Addable<SampleTime, SecondsDuration>);

TEST(MusicalUnits, MusicalTimeDifferenceIsADuration) {
    EXPECT_EQ((MusicalTime{3840} - MusicalTime{960}).value(), 2880);
    EXPECT_EQ((MusicalTime{960} + MusicalDuration{480}).value(), 1440);
    EXPECT_LT(MusicalTime{0}, MusicalTime{1});
}

TEST(MusicalUnits, BeatsDivideTicksByTheResolution) {
    EXPECT_DOUBLE_EQ(beats(MusicalDuration{1920}, TicksPerBeat{960}), 2.0);
    EXPECT_DOUBLE_EQ(beats(MusicalDuration{480}, TicksPerBeat{960}), 0.5);
    EXPECT_DOUBLE_EQ(beats(MusicalDuration{-960}, TicksPerBeat{960}), -1.0);
}

TEST(MusicalUnits, TempoOrders) {
    EXPECT_LT(Tempo{60.0}, Tempo{120.0});
    EXPECT_EQ(Tempo{120.0}, Tempo{120.0});
}

TEST(SecondsUnits, SecondsConvertToSamplesAtTheRate) {
    EXPECT_EQ(toSampleTime(SecondsTime{1.5}, SampleRate{48000}), SampleTime{72000.0});
    EXPECT_EQ(
        toSampleDuration(SecondsDuration{-0.25}, SampleRate{48000}), SampleDuration{-12000.0});
    EXPECT_EQ(toSampleTime(SecondsTime{0.0}, SampleRate{44100}), SampleTime{0.0});
}

TEST(SecondsUnits, DurationAlgebra) {
    SecondsDuration d{1.0};
    d += SecondsDuration{0.5};
    EXPECT_EQ(d, SecondsDuration{1.5});
    EXPECT_EQ(SecondsTime{2.0} + SecondsDuration{-0.5}, SecondsTime{1.5});
    EXPECT_EQ(SecondsTime{2.0} - SecondsTime{0.5}, SecondsDuration{1.5});
    EXPECT_DOUBLE_EQ(SecondsDuration{3.0} / SecondsDuration{2.0}, 1.5);
}

} // namespace
