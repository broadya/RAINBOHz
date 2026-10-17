// SPDX-License-Identifier: Apache-2.0

#pragma once

// The tempo map: what defines musical time, and how it converts to seconds.
//
// A TempoMap is a NEXT_POINT list of TempoMapPoints. Each point sets the tempo
// at the start of the segment it opens; `hold` says whether that tempo holds
// until the next point or ramps to the next point's tempo. The design, with
// the rules a valid map obeys and the segment formulas, is docs/transform.md
// section 6.1 and ADR 0013. In outline:
//
//   - positions are ticks of this map, strictly increasing, the first at 0;
//   - the last point holds, so tempo continues indefinitely after it, and a
//     one-point map is "this tempo forever";
//   - a ramp has one of two shapes, chosen for the whole Score by
//     Score.tempo_ramp_physical_time: linear in physical time (the default,
//     as in REAPER) or linear in musical position. Both give a closed form
//     for the elapsed seconds (secondsThrough).
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

/// The shape of every tempo ramp in a Score (@see ADR 0013).
enum class TempoRamp : std::uint8_t {
    /// The tempo changes at a constant rate per second, dT/dt constant. The
    /// default, used when Score.tempo_ramp_physical_time is NULL or true.
    PhysicalTime,

    /// The tempo changes at a constant rate per beat, dT/db constant. Used
    /// when Score.tempo_ramp_physical_time is false.
    MusicalPosition,
};

/// The ramp shape that Score.tempo_ramp_physical_time selects.
///
/// @param tempoRampPhysicalTime  The property as read; nullopt for NULL.
/// @return PhysicalTime for NULL or true; MusicalPosition for false.
constexpr TempoRamp tempoRampFromScore(std::optional<bool> tempoRampPhysicalTime) noexcept {
    return tempoRampPhysicalTime.value_or(true) ? TempoRamp::PhysicalTime
                                                : TempoRamp::MusicalPosition;
}

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
    /// to the next point's tempo, in the map's TempoRamp shape. Always true
    /// on the last point.
    bool hold = true;
};

/// The seconds elapsed from the start of a segment to a point inside it.
///
/// The tempo goes from `from` at the segment's start to `to` at its end, in
/// the shape `ramp`. With e = elapsed / length, the fraction of the segment's
/// beats covered:
///
///   linear in physical time:   T² = from² + (to² − from²) · e, and the
///                              seconds are 120 · elapsed_beats / (from + T),
///                              the beats over their mean tempo;
///   linear in musical position: T = from + (to − from) · e, and the seconds
///                              are 60 · length_beats · ln(T / from) /
///                              (to − from), the integral of 60 / T.
///
/// When from = to both reduce to 60 · elapsed_beats / from, which is computed
/// directly so that a held tempo gives the same result in either shape.
/// `length` is not used in that case, so a held or final segment may pass any
/// value for it.
///
/// @param from        Tempo at the segment's start, BPM. Positive.
/// @param to          Tempo at the segment's end, BPM. Positive.
/// @param length      The segment's full length in ticks. Positive when
///                    `from ≠ to`.
/// @param elapsed     Ticks from the segment's start to the point asked
///                    about, in [0, length] for a ramp; any non-negative value
///                    for a held tempo.
/// @param resolution  Ticks per beat of the map. Positive.
/// @param ramp        The shape of the ramp.
/// @return The elapsed seconds.
SecondsDuration secondsThrough(Tempo from, Tempo to, MusicalDuration length,
    MusicalDuration elapsed, TicksPerBeat resolution, TempoRamp ramp) noexcept;

/// The seconds elapsed from the start of a segment to a point inside it, for
/// a segment whose bounds need not fall on whole ticks.
///
/// This is the form every conversion runs on: the unit-typed overload casts
/// its ticks and calls it, and so does a resolved synchronised map
/// (polytempo.hpp), whose change points fall at fractional ticks. The
/// arithmetic is the same expression by expression, so a map that is not
/// synchronised converts to the same bits through either.
///
/// @param from          Tempo at the segment's start, BPM. Positive.
/// @param to            Tempo at the segment's end, BPM. Positive.
/// @param lengthTicks   The segment's full length in ticks. Positive and
///                      finite when `from ≠ to`; unused otherwise.
/// @param elapsedTicks  Ticks from the segment's start to the point asked
///                      about, in [0, lengthTicks] for a ramp; any
///                      non-negative value for a held tempo.
/// @param resolution    Ticks per beat of the map. Positive.
/// @param ramp          The shape of the ramp.
/// @return The elapsed seconds.
SecondsDuration secondsThrough(Tempo from, Tempo to, double lengthTicks, double elapsedTicks,
    TicksPerBeat resolution, TempoRamp ramp) noexcept;

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
    /// @param ramp        The shape of every ramp, from the Score.
    /// @throws TempoMapError  If any rule is broken. The message says which,
    ///         and names the point by its position in the list.
    TempoMap(TicksPerBeat resolution, std::vector<TempoMapPoint> points, TempoRamp ramp);

    /// Validates records as read from engine-db and builds the map.
    ///
    /// @param ticksPerBeat  TempoMap.ticks_per_beat; nullopt for NULL.
    /// @param records       The map's TempoMapPoints in NEXT_POINT order.
    /// @param ramp          The shape of every ramp, from the Score (see
    ///                      tempoRampFromScore()).
    /// @return The map.
    /// @throws TempoMapError  If `ticksPerBeat` is NULL or not positive; a
    ///         point's position, tempo or hold is NULL; a tempo is not
    ///         finite; or the rules of the constructor are broken.
    static TempoMap fromRecords(std::optional<std::int64_t> ticksPerBeat,
        std::span<const TempoMapPointRecord> records, TempoRamp ramp);

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

    /// The shape of the map's ramps.
    ///
    /// @return The shape given to the constructor.
    TempoRamp ramp() const noexcept { return ramp_; }

    /// The map's points, in order.
    ///
    /// @return The validated points; never empty. Valid while this map is.
    const std::vector<TempoMapPoint>& points() const noexcept { return points_; }

private:
    /// Ticks per beat, as given to the constructor.
    TicksPerBeat resolution_;

    /// The shape of every ramp, as given to the constructor.
    TempoRamp ramp_;

    /// The validated points, in list order; never empty.
    std::vector<TempoMapPoint> points_;

    /// Seconds from the start of the map at each point's position.
    std::vector<SecondsTime> secondsAt_;
};

} // namespace rainbohz::rendering_engine::transform
