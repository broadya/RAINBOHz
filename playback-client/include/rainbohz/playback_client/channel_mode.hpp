// SPDX-License-Identifier: Apache-2.0

#pragma once

// The channel modes the client plays, and the channels each one has.
//
// The score decides the mode (EngineConfig.channelMode, set by hzlc
// --channel-mode), and the server renders it into every frame; the client
// plays what it is given. It plays the modes whose output is a mono signal or
// a left/right pair: mono, the two stereo decodes, and classic Blumlein. The
// rest -- raw mid-side, rotated Blumlein, first-order Ambisonics -- need a
// decode or a layout the client does not have, and are refused.
//
// Names are the server's, GET /engine-config (rendering-engine/docs/server.md
// §3.5).

#include "rainbohz/playback_client/units.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace rainbohz::playback_client {

/// The channel modes this version plays, by the server's names.
inline constexpr std::array<std::string_view, 4> kPlayableChannelModes{
    "mono", "stereo-from-xy", "stereo-from-mid-side", "blumlein-classic"};

/// The channels a playable mode's frames have.
///
/// @param mode  A channel mode, by the server's name.
/// @return 1 for `mono`, 2 for the three left/right modes; empty for any
///         other mode, which this version does not play.
std::optional<ChannelCount> playableChannels(std::string_view mode) noexcept;

/// The playable modes, for messages and --help.
///
/// @return kPlayableChannelModes, comma-separated.
std::string playableChannelModesText();

} // namespace rainbohz::playback_client
