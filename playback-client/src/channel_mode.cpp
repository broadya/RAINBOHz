// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/channel_mode.hpp"

#include <optional>
#include <string>
#include <string_view>

/// @file
/// @brief The playable channel modes.
///
/// @see channel_mode.hpp

namespace rainbohz::playback_client {

std::optional<ChannelCount> playableChannels(std::string_view mode) noexcept {
    if (mode == "mono") {
        return ChannelCount{1};
    }
    if (mode == "stereo-from-xy" || mode == "stereo-from-mid-side" || mode == "blumlein-classic") {
        return ChannelCount{2};
    }
    return std::nullopt;
}

std::string playableChannelModesText() {
    std::string text;
    for (const std::string_view mode : kPlayableChannelModes) {
        if (!text.empty()) {
            text += ", ";
        }
        text += mode;
    }
    return text;
}

} // namespace rainbohz::playback_client
