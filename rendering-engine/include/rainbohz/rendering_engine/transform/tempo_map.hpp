// SPDX-License-Identifier: Apache-2.0

#pragma once

// The tempo map: what defines musical time, and how it converts to seconds.
//
// A TempoMap is a NEXT_POINT list of TempoMapPoints. Each point sets the tempo
// at the start of the segment it opens; `hold` says whether that tempo holds
// until the next point or ramps linearly to the next point's tempo. The
// design, with the rules a valid map obeys and the segment formulas, is
// docs/transform.md section 6.1 and ADR 0013. In outline:
//
//   - positions are ticks of this map, strictly increasing, the first at 0;
//   - the last point holds, so tempo continues indefinitely after it, and a
//     one-point map is "this tempo forever";
//   - a ramp is linear in musical position, the DAW convention, which gives a
//     closed form for the elapsed seconds (secondsThrough).
//
// The class is built once per map from validated points and answers
// toSeconds() in O(log n) from cumulative seconds kept at each point.

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// Thrown when a TempoMap is not valid under the rules of docs/transform.md
/// section 6.1. The message names the point by its position in the list,
/// counting from 0, where one is involved.
class TempoMapError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A TempoMapPoint exactly as read from engine-db, before validation. Every
/// property is nullable in the schema, so every one is optional here.
struct TempoMapPointRecord {
    /// TempoMapPoint.id.
    std::int64_t id = 0;

    /// TempoMapPoint.position, ticks.
    std::optional<std::int64_t> position;

    /// TempoMapPoint.start_tempo, BPM.
    std::optional<double> startTempo;

    /// TempoMapPoint.hold.
    std::optional<bool> hold;
};

/// A validated point of a tempo map.
struct TempoMapPoint {
    /// Where the segment this point opens starts.
    MusicalTime position;

    /// The tempo at `position`.
    Tempo tempo{0.0};

    /// True when the tempo holds until the next point; false when it ramps
    /// linearly to the next point's tempo. Always true on the last point.
    bool hold = true;
};

/// The seconds elapsed from the start of a segment to a point inside it.
///
/// The tempo ramps linearly in musical position from `from` at the segment's
/// start to `to` at its end, so at `elapsed` ticks it is
/// `T = from + (to − from) · elapsed / length`, and the elapsed seconds are
/// the integral of 60 / T over the beats covered:
///
///   60 · length_beats · ln(T / from) / (to − from)   when from ≠ to
///   60 · elapsed_beats / from                        when from = to
///
/// The second form is a held tempo, and also the limit of the first as the
/// ramp flattens. `length` is not used in the held case, so a held or final
/// segment may pass any value for it.
///
/// @param from        Tempo at the segment's start, BPM. Positive.
/// @param to          Tempo at the segment's end, BPM. Positive.
/// @param length      The segment's full length in ticks. Positive when
///                    `from ≠ to`.
/// @param elapsed     Ticks from the segment's start to the point asked
///                    about, in [0, length] for a ramp; any non-negative value
///                    for a held tempo.
/// @param resolution  Ticks per beat of the map. Positive.
/// @return The elapsed seconds.
SecondsDuration secondsThrough(Tempo from, Tempo to, MusicalDuration length,
    MusicalDuration elapsed, TicksPerBeat resolution) noexcept;

/// A validated tempo map that converts musical time to seconds.
class TempoMap {
public:
    /// Builds a map from validated points.
    ///
    /// @param resolution  Ticks per beat. Must be positive.
    /// @param points      The map's points in list order. Must satisfy the
    ///                    rules of docs/transform.md section 6.1: at least
    ///                    one; the first at position 0; positions strictly
    ///                    increasing; tempos in [1, 1000]; the last holding.
    /// @throws TempoMapError  If any rule is broken. The message says which,
    ///         and names the point by its position in the list.
    TempoMap(TicksPerBeat resolution, std::vector<TempoMapPoint> points);

    /// Validates records as read from engine-db and builds the map.
    ///
    /// @param ticksPerBeat  TempoMap.ticks_per_beat; nullopt for NULL.
    /// @param records       The map's TempoMapPoints in NEXT_POINT order.
    /// @return The map.
    /// @throws TempoMapError  If `ticksPerBeat` is NULL or not positive; a
    ///         point's position, tempo or hold is NULL; a tempo is not
    ///         finite; or the rules of the constructor are broken.
    static TempoMap fromRecords(
        std::optional<std::int64_t> ticksPerBeat, std::span<const TempoMapPointRecord> records);

    /// The seconds from the start of the map to a musical position.
    ///
    /// Finds the segment containing `at` by binary search and adds
    /// secondsThrough() for the part of it before `at` to the seconds
    /// accumulated at the segment's start. After the last point the last
    /// tempo holds.
    ///
    /// @param at  A position on this map. Must not be before tick 0.
    /// @return The position in seconds.
    /// @throws std::invalid_argument  If `at` is negative.
    SecondsTime toSeconds(MusicalTime at) const;

    /// The map's resolution.
    ///
    /// @return Ticks per beat, as given to the constructor.
    TicksPerBeat ticksPerBeat() const noexcept { return resolution_; }

    /// The map's points, in order.
    ///
    /// @return The validated points; never empty. Valid while this map is.
    const std::vector<TempoMapPoint>& points() const noexcept { return points_; }

private:
    /// Ticks per beat, as given to the constructor.
    TicksPerBeat resolution_;

    /// The validated points, in list order; never empty.
    std::vector<TempoMapPoint> points_;

    /// Seconds from the start of the map at each point's position.
    std::vector<SecondsTime> secondsAt_;
};

} // namespace rainbohz::rendering_engine::transform
