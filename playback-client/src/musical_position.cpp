#include "rainbohz/playback_client/musical_position.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

/// @file
/// @brief Parses and writes musical positions, `beats:ticks`.
///
/// @see musical_position.hpp

namespace rainbohz::playback_client {

namespace {

/// Parses a run of decimal digits as a non-negative INT64.
///
/// @param digits  The digits; must be non-empty and contain digits only.
/// @param whole   The whole text being parsed, for the diagnostic.
/// @return The value.
/// @throws MusicalPositionError if `digits` is empty, holds anything but
///         digits, or is beyond an INT64.
std::int64_t parseDigits(std::string_view digits, std::string_view whole) {
    const auto bad = [&whole](const std::string& why) {
        return MusicalPositionError{
            "'" + std::string{whole} + "' is not a position, beats:ticks or beats: " + why};
    };
    if (digits.empty()) {
        throw bad("a number is missing");
    }
    for (const char character : digits) {
        if (character < '0' || character > '9') {
            throw bad("only digits and one ':' are allowed");
        }
    }
    std::int64_t value = 0;
    const char* const first = digits.data();
    const char* const last = std::next(first, static_cast<std::ptrdiff_t>(digits.size()));
    const auto [end, error] = std::from_chars(first, last, value);
    if (error == std::errc::result_out_of_range) {
        throw bad("a number is too large");
    }
    if (error != std::errc{} || end != last) {
        throw bad("a number is not readable");
    }
    return value;
}

} // namespace

MusicalPosition parseMusicalPosition(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return MusicalPosition{.beats = parseDigits(text, text), .ticks = 0};
    }
    return MusicalPosition{.beats = parseDigits(text.substr(0, colon), text),
        .ticks = parseDigits(text.substr(colon + 1), text)};
}

std::string formatMusicalPosition(const MusicalPosition& position) {
    return std::to_string(position.beats) + ":" + std::to_string(position.ticks);
}

} // namespace rainbohz::playback_client
