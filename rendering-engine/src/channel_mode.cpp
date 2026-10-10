// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/channel_mode.hpp"

#include <array>
#include <stdexcept>
#include <string>

/// @file
/// @brief The channel modes' channel counts, labels and names.
///
/// @see channel_mode.hpp

namespace rainbohz::rendering_engine {

namespace {

// Channel names, in output order, shared between the modes that have them.

/// Mono.
constexpr std::array<std::string_view, 1> kMonoLabels{"M"};
/// Stereo.
constexpr std::array<std::string_view, 2> kLeftRightLabels{"L", "R"};
/// Mid/side, omni or figure-8.
constexpr std::array<std::string_view, 2> kMidSideLabels{"M", "S"};
/// Tetrahedral capsules: front-left-up, front-right-down, back-left-down, back-right-up.
constexpr std::array<std::string_view, 4> kAFormatLabels{"FLU", "FRD", "BLD", "BRU"};
/// AmbiX ACN order.
constexpr std::array<std::string_view, 4> kBFormatLabels{"W", "Y", "Z", "X"};

} // namespace

ChannelMode channelModeFromInt(std::int64_t value) {
    if (value < static_cast<std::int64_t>(ChannelMode::Mono) ||
        value > static_cast<std::int64_t>(ChannelMode::FoaBFormat)) {
        throw std::invalid_argument("channel mode must be 0 to 7, got " + std::to_string(value));
    }
    return static_cast<ChannelMode>(value);
}

std::size_t channelCount(ChannelMode mode) noexcept {
    return channelLabels(mode).size();
}

std::span<const std::string_view> channelLabels(ChannelMode mode) noexcept {
    switch (mode) {
    case ChannelMode::Mono:
        return kMonoLabels;
    case ChannelMode::StereoFromMidSide:
    case ChannelMode::StereoFromXY:
    case ChannelMode::BlumleinClassic:
        return kLeftRightLabels;
    case ChannelMode::MidSide:
    case ChannelMode::BlumleinRotated:
        return kMidSideLabels;
    case ChannelMode::FoaAFormat:
        return kAFormatLabels;
    case ChannelMode::FoaBFormat:
        return kBFormatLabels;
    }
    return kMonoLabels; // unreachable for a defined mode
}

std::string_view channelModeName(ChannelMode mode) noexcept {
    switch (mode) {
    case ChannelMode::Mono:
        return "mono";
    case ChannelMode::StereoFromMidSide:
        return "stereo-from-mid-side";
    case ChannelMode::StereoFromXY:
        return "stereo-from-xy";
    case ChannelMode::MidSide:
        return "mid-side";
    case ChannelMode::BlumleinClassic:
        return "blumlein-classic";
    case ChannelMode::BlumleinRotated:
        return "blumlein-rotated";
    case ChannelMode::FoaAFormat:
        return "foa-a-format";
    case ChannelMode::FoaBFormat:
        return "foa-b-format";
    }
    return "unknown"; // unreachable for a defined mode
}

} // namespace rainbohz::rendering_engine
