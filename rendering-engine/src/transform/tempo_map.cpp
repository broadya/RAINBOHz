// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/tempo_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

/// @file
/// @brief Validation of a TempoMap and its conversion of ticks to seconds.
///
/// Section numbers in comments refer to docs/transform.md.
///
/// @see tempo_map.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

/// The schema's valid tempo range, BPM (engine-schema.cypher, "tempo").
constexpr double kMinTempo = 1.0;
/// The schema's highest valid tempo, BPM.
constexpr double kMaxTempo = 1000.0;

/// Formats a double for an error message, with enough digits to identify it.
///
/// @param value  Any value, including non-finite ones.
/// @return The value in the shortest form that round-trips.
std::string formatNumber(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    return out.str();
}

/// Throws a TempoMapError about the point at `index`.
///
/// @param index   Position of the point in the list, from 0.
/// @param reason  What is wrong with it.
/// @throws TempoMapError  Always.
[[noreturn]] void failAtPoint(std::size_t index, const std::string& reason) {
    throw TempoMapError{"TempoMapPoint " + std::to_string(index) + ": " + reason};
}

} // namespace

SecondsDuration secondsThrough(Tempo from, Tempo to, MusicalDuration length,
    MusicalDuration elapsed, TicksPerBeat resolution, TempoRamp ramp) noexcept {
    constexpr double kSecondsPerMinute = 60.0;
    if (from == to) {
        return SecondsDuration{kSecondsPerMinute * beats(elapsed, resolution) / from.value()};
    }
    const double fraction =
        static_cast<double>(elapsed.value()) / static_cast<double>(length.value());
    const double start = from.value();
    const double end = to.value();
    if (ramp == TempoRamp::PhysicalTime) {
        // The tempo is linear in time, so over the elapsed part it averages
        // (start + T) / 2, where T² is linear in beats. This form has no
        // cancellation as the ramp flattens.
        const double tempoAt =
            std::sqrt((start * start) + (((end * end) - (start * start)) * fraction));
        return SecondsDuration{
            2.0 * kSecondsPerMinute * beats(elapsed, resolution) / (start + tempoAt)};
    }
    const double tempoAt = start + ((end - start) * fraction);
    return SecondsDuration{
        kSecondsPerMinute * beats(length, resolution) * std::log(tempoAt / start) / (end - start)};
}

TempoMap::TempoMap(TicksPerBeat resolution, std::vector<TempoMapPoint> points, TempoRamp ramp)
    : resolution_{resolution}, ramp_{ramp}, points_{std::move(points)} {
    if (resolution_.value() <= 0) {
        throw TempoMapError{
            "ticks_per_beat is " + std::to_string(resolution_.value()) + "; it must be positive"};
    }
    if (points_.empty()) {
        throw TempoMapError{"the TempoMap has no TempoMapPoint; at least one is required"};
    }
    if (points_.front().position != MusicalTime{0}) {
        failAtPoint(0, "position is " + std::to_string(points_.front().position.value()) +
                           "; the first point must be at position 0");
    }
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const TempoMapPoint& point = points_[i];
        const double tempo = point.tempo.value();
        if (!std::isfinite(tempo) || tempo < kMinTempo || tempo > kMaxTempo) {
            failAtPoint(i, "start_tempo is " + formatNumber(tempo) + "; it must be in [" +
                               formatNumber(kMinTempo) + ", " + formatNumber(kMaxTempo) + "]");
        }
        if (i > 0 && point.position <= points_[i - 1].position) {
            failAtPoint(i, "position is " + std::to_string(point.position.value()) +
                               ", not after the previous point's " +
                               std::to_string(points_[i - 1].position.value()) +
                               "; positions must strictly increase");
        }
    }
    if (!points_.back().hold) {
        failAtPoint(points_.size() - 1,
            "hold is false on the last point; the last point must hold, since there "
            "is no next tempo to ramp to");
    }

    // Cumulative seconds at each point (§6.1): the sum of every complete
    // segment before it.
    secondsAt_.reserve(points_.size());
    SecondsTime at;
    secondsAt_.push_back(at);
    for (std::size_t i = 1; i < points_.size(); ++i) {
        const TempoMapPoint& start = points_[i - 1];
        const MusicalDuration length = points_[i].position - start.position;
        const Tempo end = start.hold ? start.tempo : points_[i].tempo;
        at += secondsThrough(start.tempo, end, length, length, resolution_, ramp_);
        secondsAt_.push_back(at);
    }
}

TempoMap TempoMap::fromRecords(std::optional<std::int64_t> ticksPerBeat,
    std::span<const TempoMapPointRecord> records, TempoRamp ramp) {
    if (!ticksPerBeat.has_value()) {
        throw TempoMapError{"ticks_per_beat is NULL"};
    }
    if (*ticksPerBeat <= 0 || *ticksPerBeat > std::numeric_limits<std::int32_t>::max()) {
        throw TempoMapError{
            "ticks_per_beat is " + std::to_string(*ticksPerBeat) + "; it must be positive"};
    }
    std::vector<TempoMapPoint> points;
    points.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const TempoMapPointRecord& record = records[i];
        if (!record.position.has_value()) {
            failAtPoint(i, "position is NULL");
        }
        if (!record.startTempo.has_value()) {
            failAtPoint(i, "start_tempo is NULL");
        }
        if (!record.hold.has_value()) {
            failAtPoint(i, "hold is NULL");
        }
        points.push_back(TempoMapPoint{
            .position = MusicalTime{*record.position},
            .tempo = Tempo{*record.startTempo},
            .hold = *record.hold,
        });
    }
    return TempoMap{
        TicksPerBeat{static_cast<std::int32_t>(*ticksPerBeat)}, std::move(points), ramp};
}

SecondsTime TempoMap::toSeconds(MusicalTime at) const {
    if (at < MusicalTime{0}) {
        throw std::invalid_argument{
            "a musical position cannot be negative: " + std::to_string(at.value())};
    }
    // The segment containing `at` is opened by the last point at or before
    // it. The first point is at 0, so there always is one.
    const auto after = std::ranges::upper_bound(
        points_, at, {}, [](const TempoMapPoint& point) { return point.position; });
    const auto index = static_cast<std::size_t>(std::distance(points_.begin(), after)) - 1;
    const TempoMapPoint& start = points_[index];
    const bool last = index + 1 == points_.size();
    const MusicalDuration elapsed = at - start.position;
    const MusicalDuration length = last ? elapsed : points_[index + 1].position - start.position;
    const Tempo end = (start.hold || last) ? start.tempo : points_[index + 1].tempo;
    return secondsAt_[index] +
           secondsThrough(start.tempo, end, length, elapsed, resolution_, ramp_);
}

} // namespace rainbohz::rendering_engine::transform
