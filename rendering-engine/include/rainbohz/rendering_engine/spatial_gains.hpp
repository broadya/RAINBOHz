#pragma once

// Per-channel gains of a point source, for each channel mode
// (docs/rendering.md section 8.5).
//
// Ported from the spatial-audio proof of concept
// (proof-of-concept/spatial-audio/cpp), whose Python oracle's golden CSV the
// tests check against. Microphones are perfect (gain depends on direction
// only, not frequency) and coincident (every microphone sees the same
// direction; distance never changes the angle). Patterns are first order:
// a + (1 − a)·cos θ, where θ is the angle between the microphone's axis and
// the source.
//
// Direction and distance are separate functions because distance will later
// be treated together with propagation delay and Doppler; positionGains()
// combines them for the present, delay-free model.
//
// Coordinates are AmbiX: x to the front, y to the left, z up; azimuth
// anticlockwise from the front, elevation up from the horizontal plane.
//
// The typed functions wrap raw:: twins on doubles, which the render kernel
// calls, so that the kernel and the reference share one body (as
// amplitude_maths does).

#include "rainbohz/rendering_engine/channel_mode.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace rainbohz::rendering_engine {

/// The position of a source relative to the receiver at the origin.
struct SpatialPosition {
    /// Anticlockwise from the front, in [−π, π]: π/2 is left, −π/2 right.
    Azimuth azimuth;
    /// Up from the horizontal plane, in [−π/2, π/2]: π/2 is straight up.
    Elevation elevation;
    /// From the receiver, in [1, 16384] m.
    Metres distance;
};

/// A unit vector in AmbiX axes: x front, y left, z up.
struct Direction {
    /// Component towards the front.
    double x = 0.0;
    /// Component towards the left.
    double y = 0.0;
    /// Component upwards.
    double z = 0.0;
};

/// The signed gain of each output channel of a mode, in output order.
///
/// Fixed capacity, so computing one never allocates. Entries from `count` on
/// are zero.
struct ChannelGains {
    /// One gain per output channel; only the first `count` are meaningful.
    std::array<LinearGain, kMaxChannels> gains{};
    /// Number of output channels, as channelCount() gives for the mode.
    std::size_t count = 0;

    /// The meaningful gains.
    ///
    /// @return A view of the first `count` gains; valid while this object is.
    std::span<const LinearGain> channels() const noexcept { return {gains.data(), count}; }
};

/// The nearest distance in the rendering model, metres (engine-schema.cypher).
inline constexpr double kMinDistanceMetres = 1.0;

/// The furthest distance in the rendering model, metres (engine-schema.cypher).
inline constexpr double kMaxDistanceMetres = 16384.0;

namespace raw {

/// One gain per channel, as plain doubles; entries past the mode's channel
/// count are zero.
using Gains = std::array<double, kMaxChannels>;

/// directionGains() on raw values: radians in, multipliers out.
///
/// @param mode       A defined mode.
/// @param azimuth    Source azimuth, radians, in [−π, π].
/// @param elevation  Source elevation, radians, in [−π/2, π/2].
/// @return One signed gain per output channel of `mode`, then zeros.
Gains directionGains(ChannelMode mode, double azimuth, double elevation) noexcept;

/// positionGains() on raw values: direction gain times 1/distance.
///
/// @param mode       A defined mode.
/// @param azimuth    Source azimuth, radians, in [−π, π].
/// @param elevation  Source elevation, radians, in [−π/2, π/2].
/// @param distance   Source distance, metres, at least 1.
/// @return One signed gain per output channel of `mode`, then zeros.
Gains positionGains(ChannelMode mode, double azimuth, double elevation, double distance) noexcept;

} // namespace raw

/// The unit vector pointing from the receiver towards a direction.
///
/// @param azimuth    Anticlockwise from the front, in [−π, π].
/// @param elevation  Up from the horizontal plane, in [−π/2, π/2].
/// @return (cos el·cos az, cos el·sin az, sin el).
Direction unitVector(Azimuth azimuth, Elevation elevation) noexcept;

/// Each output channel's gain for a source in a direction, ignoring distance.
///
/// The microphones' pattern gains, decoded where the mode has a decode.
/// Real-time safe: no allocation, locking or exceptions.
///
/// @param mode       A defined mode.
/// @param azimuth    Source azimuth, in [−π, π].
/// @param elevation  Source elevation, in [−π/2, π/2].
/// @return One signed gain per output channel of `mode`.
/// @pre The angles are within their ranges: wrapping is done upstream, before
///      this is called. checkPosition() enforces this where positions enter.
ChannelGains directionGains(ChannelMode mode, Azimuth azimuth, Elevation elevation) noexcept;

/// The gain of a point source due to its distance alone.
///
/// Free-field inverse-distance law referenced to 1 m: 0 dB at 1 m, −6.02 dB
/// per doubling of distance.
///
/// @param distance  From the receiver.
/// @return 1 / distance, linear.
/// @pre `distance` is at least 1 m (checkPosition() enforces this).
LinearGain distanceGain(Metres distance) noexcept;

/// Each output channel's gain for a source at a position: direction gain
/// times distance gain.
///
/// @param mode      A defined mode.
/// @param position  Where the source is.
/// @return One signed gain per output channel of `mode`.
/// @pre `position` passes checkPosition().
ChannelGains positionGains(ChannelMode mode, const SpatialPosition& position) noexcept;

/// Checks that a position is one the model accepts.
///
/// Kept apart from the gain functions so that they stay noexcept on the
/// render path; call it where a position enters the system. It does not wrap:
/// angles must already be in the Ambisonics ranges.
///
/// @param position  The position to check.
/// @throws std::invalid_argument if the azimuth is outside [−π, π], the
///         elevation outside [−π/2, π/2], or the distance outside
///         [1, 16384] m. NaN fails every range.
void checkPosition(const SpatialPosition& position);

} // namespace rainbohz::rendering_engine
