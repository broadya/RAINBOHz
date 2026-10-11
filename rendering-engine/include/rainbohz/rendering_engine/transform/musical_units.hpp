// SPDX-License-Identifier: Apache-2.0

#pragma once

// Unit types for musical time.
//
// Musical time is measured in ticks on a TempoMap (engine-schema.cypher, "6.
// Timing"; ADR 0012). Where a tick falls in seconds depends on the tempo, so a
// musical quantity must never be mistaken for a physical one. These types make
// that a compile error, in the same way units.hpp keeps samples apart from
// frame numbers (AGENTS.md, C++ Conventions).
//
// They exist only inside the transform stage. The render path never sees
// musical time (architecture/overview.md section 8; docs/design.md section 7),
// and the compiler helps keep it that way: nothing outside
// rainbohz::rendering_engine::transform includes this header.

#include <compare>
#include <cstdint>

namespace rainbohz::rendering_engine::transform {

/// A length of musical time, in ticks of one TempoMap.
///
/// Discrete: ticks are the finest subdivision a TempoMap offers. May be
/// negative as the difference of two times.
class MusicalDuration {
public:
    /// A zero duration.
    constexpr MusicalDuration() noexcept = default;

    /// A duration of `ticks` ticks.
    ///
    /// @param ticks  Length in ticks; any value.
    constexpr explicit MusicalDuration(std::int64_t ticks) noexcept : value_{ticks} {}

    /// The duration in ticks.
    ///
    /// @return Ticks; may be negative.
    constexpr std::int64_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(MusicalDuration, MusicalDuration) noexcept = default;

    /// The sum of the two operands.
    friend constexpr MusicalDuration operator+(MusicalDuration a, MusicalDuration b) noexcept {
        return MusicalDuration{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr MusicalDuration operator-(MusicalDuration a, MusicalDuration b) noexcept {
        return MusicalDuration{a.value_ - b.value_};
    }

private:
    std::int64_t value_ = 0; ///< Length in ticks; may be negative.
};

/// A position in musical time: ticks from the start of one TempoMap.
///
/// A TempoMapPoint's `position` and a POINT_AT's `sync_point` are both
/// MusicalTime. Modelled on SampleTime: subtracting two gives a
/// MusicalDuration, and adding two does not compile.
class MusicalTime {
public:
    /// The start of the tempo map, tick 0.
    constexpr MusicalTime() noexcept = default;

    /// The point `ticks` ticks after the start of the tempo map.
    ///
    /// @param ticks  Position in ticks; any value. Validation is the caller's
    ///               concern.
    constexpr explicit MusicalTime(std::int64_t ticks) noexcept : value_{ticks} {}

    /// The position in ticks from the start of the tempo map.
    ///
    /// @return Ticks from tick 0.
    constexpr std::int64_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(MusicalTime, MusicalTime) noexcept = default;

    /// The duration from `b` to `a`.
    friend constexpr MusicalDuration operator-(MusicalTime a, MusicalTime b) noexcept {
        return MusicalDuration{a.value_ - b.value_};
    }
    /// The sum of the two operands.
    friend constexpr MusicalTime operator+(MusicalTime t, MusicalDuration d) noexcept {
        return MusicalTime{t.value_ + d.value()};
    }
    /// The sum of the two operands.
    friend constexpr MusicalTime operator+(MusicalDuration d, MusicalTime t) noexcept {
        return MusicalTime{d.value() + t.value_};
    }

private:
    std::int64_t value_ = 0; ///< Ticks from the start of the tempo map.
};

/// The resolution of a TempoMap: how many ticks make one beat.
///
/// TempoMap.ticks_per_beat. Positive; at 60 BPM, 1000 ticks per beat makes a
/// tick a millisecond and 48000 makes it a sample (ADR 0012).
class TicksPerBeat {
public:
    /// A resolution of `ticks` ticks per beat.
    ///
    /// @param ticks  Ticks per beat. Not checked here; a usable value is
    ///               positive.
    constexpr explicit TicksPerBeat(std::int32_t ticks) noexcept : value_{ticks} {}

    /// The ticks per beat.
    ///
    /// @return The value given to the constructor, unchecked.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(TicksPerBeat, TicksPerBeat) noexcept = default;

private:
    std::int32_t value_; ///< Ticks per beat; not checked to be positive.
};

/// A tempo, in beats per minute.
///
/// TempoMapPoint.start_tempo. The schema's valid range is [1, 1000]; nothing
/// here enforces it.
class Tempo {
public:
    /// A tempo of `beatsPerMinute` BPM.
    ///
    /// @param beatsPerMinute  Any value.
    constexpr explicit Tempo(double beatsPerMinute) noexcept : value_{beatsPerMinute} {}

    /// The tempo in beats per minute.
    ///
    /// @return Beats per minute, unchecked.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Tempo, Tempo) noexcept = default;

private:
    double value_; ///< Beats per minute; not checked against the schema's range.
};

/// A length of musical time in beats, which is what a tempo divides.
///
/// Unitless once the resolution is applied: a double, because a duration in
/// ticks need not be a whole number of beats.
///
/// @param d           Length in ticks.
/// @param resolution  Ticks per beat of the map `d` was measured on. Must be
///                    positive.
/// @return `d / resolution`.
constexpr double beats(MusicalDuration d, TicksPerBeat resolution) noexcept {
    return static_cast<double>(d.value()) / static_cast<double>(resolution.value());
}

} // namespace rainbohz::rendering_engine::transform
