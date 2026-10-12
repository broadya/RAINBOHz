// SPDX-License-Identifier: Apache-2.0

#pragma once

// How the session's channels are laid onto the output device's.
//
// The device runs with its own channel count, which need not be the
// session's. A mono signal goes to every device channel. A left/right pair
// goes to the device's first two channels, with the rest silent; on a
// one-channel device it is mixed down, (L + R) / 2.
//
// Used by the device sink's audio callback, so it is real-time safe; kept
// apart from miniaudio so that it can be tested without a device.

#include "rainbohz/playback_client/units.hpp"

#include <cstddef>
#include <span>

namespace rainbohz::playback_client {

/// Lays interleaved session audio onto the device's interleaved buffer.
///
/// With one source channel, it is copied to every device channel. With more
/// and a one-channel device, they are averaged. Otherwise source channel c
/// goes to device channel c, and device channels beyond the source's are
/// silent; source channels beyond the device's are dropped.
///
/// @param source          Whole sample frames, `sourceChannels` values each.
/// @param sourceChannels  The session's channels; positive.
/// @param[out] device     Receives as many sample frames as `source` has,
///                        `deviceChannels` values each; must be exactly that
///                        long. Every value is written.
/// @param deviceChannels  The device's channels; positive.
/// @note Real-time safe: no lock, allocation, system call or log.
void mixToDevice(std::span<const float> source, ChannelCount sourceChannels,
    std::span<float> device, std::size_t deviceChannels) noexcept;

} // namespace rainbohz::playback_client
