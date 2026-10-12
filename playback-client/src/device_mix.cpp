// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/device_mix.hpp"

#include <cstddef>
#include <span>

/// @file
/// @brief The session-to-device channel layout.
///
/// @see device_mix.hpp

namespace rainbohz::playback_client {

void mixToDevice(std::span<const float> source, ChannelCount sourceChannels,
    std::span<float> device, std::size_t deviceChannels) noexcept {
    const auto from = static_cast<std::size_t>(sourceChannels.value());
    const std::size_t frames = source.size() / from;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::span<const float> in = source.subspan(frame * from, from);
        const std::span<float> out = device.subspan(frame * deviceChannels, deviceChannels);
        if (from == 1) {
            for (float& value : out) {
                value = in[0];
            }
        } else if (deviceChannels == 1) {
            float sum = 0.0F;
            for (const float value : in) {
                sum += value;
            }
            out[0] = sum / static_cast<float>(from);
        } else {
            for (std::size_t channel = 0; channel < deviceChannels; ++channel) {
                out[channel] = channel < from ? in[channel] : 0.0F;
            }
        }
    }
}

} // namespace rainbohz::playback_client
