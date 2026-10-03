#pragma once

// A position in musical time, as the command line takes it.
//
// Written `beats:ticks`, as HzL and the REST API write positions
// (rendering-engine/api/openapi.yaml, "Musical positions"): two non-negative
// integers, or a bare integer of beats. The ticks must be below the tempo
// map's ticks_per_beat, which the client learns only from the server; until
// then a position is checked for form alone.

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rainbohz::playback_client {

/// Thrown when text is not a musical position.
class MusicalPositionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A position on a tempo map: whole beats and the ticks after them.
struct MusicalPosition {
    /// Whole beats from the map's origin.
    std::int64_t beats = 0;
    /// Ticks after `beats`. Meaningful only below the map's ticks_per_beat.
    std::int64_t ticks = 0;

    /// Orders positions by beats, then ticks.
    ///
    /// This is the musical order whenever both positions' ticks are below the
    /// map's resolution, which validTicks() checks.
    friend constexpr auto operator<=>(
        const MusicalPosition&, const MusicalPosition&) noexcept = default;
};

/// Parses `beats:ticks`, or bare beats, into a position.
///
/// The grammar is the server's, `^[0-9]+(:[0-9]+)?$`: digits only, no sign,
/// no whitespace.
///
/// @param text  The text to parse.
/// @return The position; bare beats give zero ticks.
/// @throws MusicalPositionError if `text` does not fit the grammar, or a
///         number is beyond an INT64.
MusicalPosition parseMusicalPosition(std::string_view text);

/// Writes a position in full, `beats:ticks`, as the server expects it.
///
/// @param position  The position.
/// @return The text, e.g. `3:480`.
std::string formatMusicalPosition(const MusicalPosition& position);

/// Whether a position's ticks are below a map's resolution.
///
/// @param position      The position.
/// @param ticksPerBeat  The map's ticks per beat; positive.
/// @return True when `position.ticks` < `ticksPerBeat`.
constexpr bool validTicks(const MusicalPosition& position, std::int64_t ticksPerBeat) noexcept {
    return position.ticks < ticksPerBeat;
}

} // namespace rainbohz::playback_client
