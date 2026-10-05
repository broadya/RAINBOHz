#pragma once

// The position of a SoundSource at one instant: linear interpolation of its
// azimuth, elevation and distance timelines, with the azimuth taking the
// shorter way round (docs/rendering.md section 8.5).
//
// A piece of a spatial timeline runs from one point to the next. Within it
// elevation and distance are straight lines in radians and metres. Azimuth
// is a straight line too, but its change over the piece is the shorter arc
// between the two points, wrap(az₁ − az₀) in (−π, π], so that a source moving
// from 3.0 to −3.0 rad passes behind the listener rather than all the way
// round through the front. A difference of exactly π is ambiguous and is
// taken as +π, anticlockwise. The interpolated value is wrapped back into
// [−π, π].
//
// As in amplitude_maths.hpp, the `raw` functions on plain doubles are what
// the kernel calls, and the unit-typed functions wrap them. Header-only and
// free of allocation, locking and exceptions.

#include "rainbohz/rendering_engine/spatial_gains.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <numbers>

namespace rainbohz::rendering_engine {

/// The path of a source's position over one spatial segment, re-based to the
/// segment's start (docs/rendering.md section 8.5).
///
/// azimuth(τ) = wrap(startAzimuth + azimuthGradient · τ), elevation(τ) =
/// startElevation + elevationGradient · τ, distance(τ) = startDistance +
/// distanceGradient · τ.
struct NominalPosition {
    /// Azimuth at τ = 0, in [−π, π].
    Azimuth startAzimuth;

    /// Rate of change of azimuth along the shorter arc.
    AngleGradient azimuthGradient;

    /// Elevation at τ = 0.
    Elevation startElevation;

    /// Rate of change of elevation.
    AngleGradient elevationGradient;

    /// Distance at τ = 0.
    Metres startDistance;

    /// Rate of change of distance.
    DistanceGradient distanceGradient;
};

namespace raw {

/// Wraps an azimuth that is at most 2π outside [−π, π] back into it.
///
/// One conditional addition or subtraction: an interpolated azimuth is a
/// value in [−π, π] plus a change of at most π in magnitude, so it is never
/// further out than that.
///
/// @param azimuth  Radians, in [−2π, 2π].
/// @return The same direction, in [−π, π].
constexpr double wrapAzimuth(double azimuth) noexcept {
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    if (azimuth > std::numbers::pi) {
        return azimuth - kTwoPi;
    }
    if (azimuth < -std::numbers::pi) {
        return azimuth + kTwoPi;
    }
    return azimuth;
}

/// The change of azimuth along the shorter arc from `from` to `to`.
///
/// @param from  Radians, in [−π, π].
/// @param to    Radians, in [−π, π].
/// @return The difference in (−π, π]: positive anticlockwise. A difference
///         of exactly π either way is +π.
constexpr double azimuthChange(double from, double to) noexcept {
    constexpr double kTwoPi = 2.0 * std::numbers::pi;
    double change = to - from;
    if (change > std::numbers::pi) {
        change -= kTwoPi;
    } else if (change <= -std::numbers::pi) {
        change += kTwoPi;
    }
    return change;
}

} // namespace raw

/// The gradient of the shorter arc from one azimuth to another over `d`.
///
/// @param from  Azimuth at the start of the piece, in [−π, π].
/// @param to    Azimuth at the end of the piece, in [−π, π].
/// @param d     The piece's length. Positive; not checked.
/// @return raw::azimuthChange(from, to) / d.
constexpr AngleGradient azimuthGradient(Azimuth from, Azimuth to, SampleDuration d) noexcept {
    return AngleGradient{raw::azimuthChange(from.value(), to.value()) / d.value()};
}

/// The gradient of a straight line from one elevation to another over `d`.
///
/// @param from  Elevation at the start of the piece.
/// @param to    Elevation at the end of the piece.
/// @param d     The piece's length. Positive; not checked.
/// @return (to − from) / d.
constexpr AngleGradient elevationGradient(Elevation from, Elevation to, SampleDuration d) noexcept {
    return AngleGradient{(to.value() - from.value()) / d.value()};
}

/// The gradient of a straight line from one distance to another over `d`.
///
/// @param from  Distance at the start of the piece.
/// @param to    Distance at the end of the piece.
/// @param d     The piece's length. Positive; not checked.
/// @return (to − from) / d.
constexpr DistanceGradient distanceGradient(Metres from, Metres to, SampleDuration d) noexcept {
    return DistanceGradient{(to.value() - from.value()) / d.value()};
}

/// The azimuth `tau` along a piece.
///
/// @param start     Azimuth at τ = 0, in [−π, π].
/// @param gradient  The piece's gradient along the shorter arc.
/// @param tau       Offset from the start; at most the piece's length.
/// @return wrap(start + gradient · τ), in [−π, π].
constexpr Azimuth azimuthAt(Azimuth start, AngleGradient gradient, SampleDuration tau) noexcept {
    return Azimuth{raw::wrapAzimuth(start.value() + (gradient.value() * tau.value()))};
}

/// The elevation `tau` along a piece.
///
/// @param start     Elevation at τ = 0.
/// @param gradient  The piece's gradient.
/// @param tau       Offset from the start.
/// @return start + gradient · τ.
constexpr Elevation elevationAt(
    Elevation start, AngleGradient gradient, SampleDuration tau) noexcept {
    return Elevation{start.value() + (gradient.value() * tau.value())};
}

/// The distance `tau` along a piece.
///
/// @param start     Distance at τ = 0.
/// @param gradient  The piece's gradient.
/// @param tau       Offset from the start.
/// @return start + gradient · τ.
constexpr Metres distanceAt(Metres start, DistanceGradient gradient, SampleDuration tau) noexcept {
    return Metres{start.value() + (gradient.value() * tau.value())};
}

/// The position `tau` into a spatial segment.
///
/// @param nominal  The segment's path.
/// @param tau      Offset from the segment's start. Not negative.
/// @return The position, with the azimuth wrapped into [−π, π].
constexpr SpatialPosition positionAt(const NominalPosition& nominal, SampleDuration tau) noexcept {
    return SpatialPosition{
        .azimuth = azimuthAt(nominal.startAzimuth, nominal.azimuthGradient, tau),
        .elevation = elevationAt(nominal.startElevation, nominal.elevationGradient, tau),
        .distance = distanceAt(nominal.startDistance, nominal.distanceGradient, tau),
    };
}

} // namespace rainbohz::rendering_engine
