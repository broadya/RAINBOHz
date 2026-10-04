#pragma once

// Unit types for the spatial-audio PoC.
//
// The same convention as rendering-engine's units.hpp (AGENTS.md, C++
// Conventions), but standalone: this PoC depends on nothing in
// rendering-engine. Each type is a class with one member, explicitly
// constructed and read back with value(), with no implicit conversion to or
// from its representation or any other unit type. On integration these fold
// into the engine's units.hpp.
//
// Units follow the rendering model in engine-db/schema/engine-schema.cypher
// ("Units and limits"): azimuth and elevation in radians (AmbiX), distance in
// metres.

#include <compare>

namespace rainbohz::spatial_poc {

/// An angle in radians: an azimuth or an elevation.
///
/// Construction accepts any value. Azimuths and elevations reach this module
/// already wrapped into the Ambisonics ranges (azimuth [−π, π], elevation
/// [−π/2, π/2]); wrapping is done upstream, and checkPosition() rejects an
/// angle outside its range.
class Radians {
public:
    /// An angle of `radians` radians.
    ///
    /// @param radians  The angle; not checked here.
    constexpr explicit Radians(double radians) noexcept : value_{radians} {}

    /// The angle in radians.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Radians, Radians) noexcept = default;

private:
    double value_;
};

/// A distance from the receiver, in metres.
///
/// The model's valid range is [1, 16384] (engine-schema.cypher); 1 m is the
/// reference distance, with zero attenuation. Construction does not check it;
/// checkPosition() does.
class Metres {
public:
    /// A distance of `metres` m.
    ///
    /// @param metres  Distance in metres; valid in [1, 16384].
    constexpr explicit Metres(double metres) noexcept : value_{metres} {}

    /// The distance in metres.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Metres, Metres) noexcept = default;

private:
    double value_;
};

/// A signed linear gain: a multiplier on a sample, unitless.
///
/// 1 is 0 dB. A negative gain is a polarity reversal (the rear lobe of a
/// figure-8, for example); 0 is silence. Unlike the engine's LinearAmplitude
/// it is not limited to [0, 1].
class LinearGain {
public:
    /// Silence.
    constexpr LinearGain() noexcept = default;

    /// A gain of `multiplier`.
    ///
    /// @param multiplier  Signed linear gain; negative reverses polarity.
    constexpr explicit LinearGain(double multiplier) noexcept : value_{multiplier} {}

    /// The multiplier.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(LinearGain, LinearGain) noexcept = default;

private:
    double value_ = 0.0;
};

/// Two gains applied in series, such as a direction gain and a distance gain.
///
/// @param lhs  First gain.
/// @param rhs  Second gain.
/// @return The product of the two multipliers.
constexpr LinearGain operator*(LinearGain lhs, LinearGain rhs) noexcept {
    return LinearGain{lhs.value() * rhs.value()};
}

} // namespace rainbohz::spatial_poc
