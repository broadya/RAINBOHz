// SPDX-License-Identifier: Apache-2.0

#pragma once

// The engine's channel modes: which virtual microphones are rendered, and to
// which output channels. Mirrors EngineConfig.channelMode in
// engine-db/schema/engine-schema.cypher, whose INT8 values are the
// enumerators' values.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rainbohz::spatial_poc {

/// The most output channels any mode has (the FOA modes have four).
inline constexpr std::size_t kMaxChannels = 4;

/// A combination of virtual microphones and output channels.
///
/// The microphones are perfect, coincident and first order (see the
/// spatial-audio README). Modes 3 to 6 output the raw microphone signals;
/// modes 1 and 7 output a linear decode of them.
enum class ChannelMode : std::int8_t {
    /// One omni. Channel M.
    Mono = 0,
    /// Omni M and figure-8 S facing left, decoded to L = (M + S)/2,
    /// R = (M − S)/2.
    StereoFromMidSide = 1,
    /// Cardioids at +45° (L) and −45° (R), output directly.
    StereoFromXY = 2,
    /// Omni M and figure-8 S facing left, undecoded. Channels M, S.
    MidSide = 3,
    /// Figure-8s at +45° (L) and −45° (R), raw. Channels L, R.
    BlumleinClassic = 4,
    /// Figure-8s at 0° (M) and +90° (S), raw: Blumlein rotated by 45°.
    /// Channels M, S.
    BlumleinRotated = 5,
    /// Four cardioids on a tetrahedron, raw. Channels FLU, FRD, BLD, BRU.
    FoaAFormat = 6,
    /// First-order AmbiX B-format: ACN order W, Y, Z, X, SN3D normalisation.
    FoaBFormat = 7,
};

/// Converts EngineConfig.channelMode, as stored in engine-db, to a mode.
///
/// This is the boundary at which a database value is checked: everything past
/// it may assume a valid mode.
///
/// @param value  The stored INT8, 0 to 7.
/// @return The mode whose value is `value`.
/// @throws std::invalid_argument if `value` is not one of the defined modes.
ChannelMode channelModeFromInt(std::int8_t value);

/// The number of output channels of a mode, 1 to kMaxChannels.
///
/// @param mode  A defined mode.
/// @return Its output channel count.
std::size_t channelCount(ChannelMode mode) noexcept;

/// The names of a mode's output channels, in output order.
///
/// @param mode  A defined mode.
/// @return One name per output channel, such as "W", "Y", "Z", "X". The views
///         refer to static storage and stay valid for the program's lifetime.
std::span<const std::string_view> channelLabels(ChannelMode mode) noexcept;

} // namespace rainbohz::spatial_poc
