// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/rendering.hpp"

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/frame_plan.hpp"
#include "rainbohz/rendering_engine/parallel.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Renders requested frames to WAV files: the gather, validation, the
/// per-frame loop, summation and the report.
///
/// Implements docs/rendering.md sections 6.1, 6.3, 8.5 and 9. The file is
/// organised as:
///
///   - validation of one partial's gathered data into a PartialData, and of
///     one source's into a SourceData;
///   - the run: per frame, gather, group by source and then by partial set,
///     plan and render each set to mono (frame_plan.hpp), sum each source's
///     sets, spread the sum over the channels by the source's position, write.
///
/// The gather (frame_gather.hpp), which the engine-db backend implements, is
/// the only layer that touches engine-db, and it finishes before the frame is
/// prepared. Planning and rendering the sets is the only
/// step that runs on several threads (ADR 0026); everything else, the sums,
/// the files and the report included, runs on the calling thread in a fixed
/// order, so the output does not depend on the thread count.
///
/// @see rendering.hpp

namespace rainbohz::rendering_engine {

namespace {

/// The value of a `*_curve_type` for a linear curve: the only one this
/// version renders, for amplitude and for spatial position.
constexpr std::int64_t kLinearCurveType = 0;

// ---------------------------------------------------------------------------
// Validation (§9)
// ---------------------------------------------------------------------------

/// Formats a double for an error message, with enough digits to identify it.
///
/// @param value  Any value, including non-finite ones.
/// @return e.g. "48000", "0.1", "-inf", "nan".
std::string formatNumber(double value) {
    std::ostringstream out;
    out.precision(17);
    out << value;
    return out.str();
}

/// Reads a required, finite value.
///
/// @param value  The property as read.
/// @param where  What holds it, e.g. "segment 2".
/// @param name   Property name.
/// @return The value.
/// @throws PartialRenderError  If `value` is NULL or not finite.
double requireFinite(
    const std::optional<double>& value, const std::string& where, std::string_view name) {
    if (!value.has_value()) {
        throw PartialRenderError{where + ": " + std::string{name} + " is NULL"};
    }
    if (!std::isfinite(*value)) {
        throw PartialRenderError{where + ": " + std::string{name} + " is " + formatNumber(*value) +
                                 ", which is not finite"};
    }
    return *value;
}

/// Checks an entry's segments and converts them to typed values (§9).
///
/// @param entry  What the gather produced for one FrameEntry, walked.
/// @return The segments, contiguous, in list order.
/// @throws PartialRenderError  If a value is NULL or not finite, a segment is
///         empty or not contiguous with the one before it, a segment has no
///         nominal frequency, a linear component has no gradient, or a
///         segment has a curve component.
std::vector<Segment> validateSegments(const GatheredEntry& entry) {
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < entry.segments.size(); ++i) {
        const GatheredSegment& record = entry.segments[i];
        const std::string where =
            "segment " + std::to_string(i) + " (id " + std::to_string(record.id) + ")";
        const SampleTime start{requireFinite(record.startTime, where, "start_time")};
        const SampleTime end{requireFinite(record.endTime, where, "end_time")};
        const Phase startPhase{requireFinite(record.startPhase, where, "start_phase")};
        if (!record.startFrequency.has_value()) {
            throw PartialRenderError{where + ": it has no PartialSegmentNominalFrequency, or its "
                                             "start_nominal_frequency is NULL"};
        }
        const AngularFrequency startFrequency{
            requireFinite(record.startFrequency, where, "start_nominal_frequency")};
        if (!(end > start)) {
            throw PartialRenderError{where + ": end_time " + formatNumber(end.value()) +
                                     " is not after start_time " + formatNumber(start.value())};
        }
        if (i > 0 && start != segments.back().endTime) {
            throw PartialRenderError{where + ": start_time " + formatNumber(start.value()) +
                                     " is not the previous segment's end_time " +
                                     formatNumber(segments.back().endTime.value()) +
                                     "; the segments are not contiguous"};
        }
        if (record.curveComponent.value_or(false)) {
            throw PartialRenderError{where + ": curve_component is true; curves are not "
                                             "implemented in this version"};
        }
        std::optional<FrequencyGradient> gradient;
        if (record.linearComponent.value_or(false)) {
            gradient =
                FrequencyGradient{requireFinite(record.linearGradient, where, "linear_gradient")};
        }
        segments.push_back(Segment{
            .startTime = start,
            .endTime = end,
            .nominal =
                NominalFrequency{.startFrequency = startFrequency, .linearGradient = gradient},
            .startPhase = startPhase,
        });
    }
    return segments;
}

/// Checks that the entry segment is where the frame's rendering starts, and
/// finds that instant (§9).
///
/// The entry segment covers `max(b₀, fs)`: either it covers `fs`, or it
/// starts after `fs` and is the head of its list, in which case `b₀` is its
/// start.
///
/// @param entry     The gathered entry, for whether its entry segment has a
///                  predecessor.
/// @param segments  The validated segments; the first is the entry segment.
/// @param fs        The frame's start.
/// @param fe        The frame's end.
/// @return The frame's entry time `t* = max(b₀, fs)`.
/// @throws PartialRenderError  If the entry segment does not cover `t*`,
///         which means the FrameEntry is stale or on the wrong frame.
SampleTime validateEntryPosition(
    const GatheredEntry& entry, std::span<const Segment> segments, SampleTime fs, SampleTime fe) {
    const Segment& entrySegment = segments.front();
    if (entrySegment.startTime > fs && entry.entrySegmentHasPredecessor) {
        throw PartialRenderError{"its entry segment starts at " +
                                 formatNumber(entrySegment.startTime.value()) +
                                 ", after the frame's start " + formatNumber(fs.value()) +
                                 ", but is not the first segment; the FrameEntry is stale"};
    }
    if (entrySegment.startTime <= fs && !(fs < entrySegment.endTime)) {
        throw PartialRenderError{
            "its entry segment [" + formatNumber(entrySegment.startTime.value()) + ", " +
            formatNumber(entrySegment.endTime.value()) + ") does not cover the frame's start " +
            formatNumber(fs.value()) + "; the FrameEntry is stale"};
    }
    if (!(entrySegment.startTime < fe)) {
        throw PartialRenderError{"its entry segment starts at " +
                                 formatNumber(entrySegment.startTime.value()) +
                                 ", at or after the frame's end " + formatNumber(fe.value()) +
                                 "; the FrameEntry is on the wrong frame"};
    }
    return std::max(entrySegment.startTime, fs);
}

/// Checks an entry's amplitude points and converts them to typed values (§9).
///
/// @param entry      What the gather produced for one FrameEntry, walked.
/// @param entryTime  The frame's entry time, from validateEntryPosition().
/// @return The points, strictly increasing, the first at or before
///         `entryTime` and the second after it.
/// @throws PartialRenderError  If a value is NULL or not finite, times do not
///         strictly increase, a point but the last has a curve other than
///         linear, or the entry point's piece does not cover `entryTime`.
std::vector<AmplitudePoint> validateAmplitude(const GatheredEntry& entry, SampleTime entryTime) {
    std::vector<AmplitudePoint> amplitude;
    for (std::size_t i = 0; i < entry.points.size(); ++i) {
        const GatheredPoint& record = entry.points[i];
        const std::string where =
            "amplitude point " + std::to_string(i) + " (id " + std::to_string(record.id) + ")";
        const SampleTime time{requireFinite(record.time, where, "time")};
        const Decibels value{requireFinite(record.amplitude, where, "amplitude")};
        if (i > 0 && !(time > amplitude.back().time)) {
            throw PartialRenderError{where + ": time " + formatNumber(time.value()) +
                                     " is not later than the previous point's time " +
                                     formatNumber(amplitude.back().time.value())};
        }
        if (i + 1 < entry.points.size() && record.curveType != kLinearCurveType) {
            throw PartialRenderError{
                where + ": amplitude_curve_type is " +
                (record.curveType.has_value() ? std::to_string(*record.curveType)
                                              : std::string{"NULL"}) +
                "; only " + std::to_string(kLinearCurveType) +
                " (linear) is supported in this version"};
        }
        amplitude.push_back(AmplitudePoint{.time = time, .amplitude = value});
    }
    if (amplitude.front().time > entryTime) {
        throw PartialRenderError{"its entry amplitude point at " +
                                 formatNumber(amplitude.front().time.value()) +
                                 " is after the frame's entry time " +
                                 formatNumber(entryTime.value()) + "; the FrameEntry is stale"};
    }
    if (amplitude.size() < 2) {
        throw PartialRenderError{"its entry amplitude point is the last of its timeline, so no "
                                 "amplitude piece covers the frame; the FrameEntry is stale or "
                                 "the timeline is shorter than the lifeline"};
    }
    if (!(amplitude[1].time > entryTime)) {
        throw PartialRenderError{"its entry amplitude point's piece ends at " +
                                 formatNumber(amplitude[1].time.value()) +
                                 ", at or before the frame's entry time " +
                                 formatNumber(entryTime.value()) + "; the FrameEntry is stale"};
    }
    return amplitude;
}

/// Checks that where the frame contains the end of either list, it contains
/// the end of both, at the same time (§9, lifeline equality).
///
/// The walk follows a node only while it starts before the frame's end, so a
/// list is known to end only where its last node starts inside the frame's
/// reach and has no successor. A last node starting at or after `fe` says
/// nothing about where the list ends.
///
/// @param entry      The gathered entry, for the walks' successor flags.
/// @param segments   The validated segments.
/// @param amplitude  The validated amplitude points.
/// @param fe         The frame's end.
/// @throws PartialRenderError  If one list is known to end inside the frame
///         and the other does not end there too.
void validateLifelineEnds(const GatheredEntry& entry, std::span<const Segment> segments,
    std::span<const AmplitudePoint> amplitude, SampleTime fe) {
    const bool segmentsEnded =
        segments.back().startTime < fe && !entry.segments.back().hasSuccessor;
    const bool pointsEnded = amplitude.back().time < fe && !entry.points.back().hasSuccessor;
    const SampleTime segmentsEnd = segments.back().endTime;
    const SampleTime pointsEnd = amplitude.back().time;
    if (segmentsEnded && pointsEnded && segmentsEnd != pointsEnd) {
        throw PartialRenderError{"its segments end at " + formatNumber(segmentsEnd.value()) +
                                 " but its amplitude timeline ends at " +
                                 formatNumber(pointsEnd.value()) + "; the two must end together"};
    }
    if (segmentsEnded && !pointsEnded && segmentsEnd < fe) {
        throw PartialRenderError{"its segments end at " + formatNumber(segmentsEnd.value()) +
                                 ", inside the frame, but its amplitude timeline continues past "
                                 "that; the two must end together"};
    }
    if (pointsEnded && !segmentsEnded && pointsEnd < fe) {
        throw PartialRenderError{"its amplitude timeline ends at " +
                                 formatNumber(pointsEnd.value()) +
                                 ", inside the frame, but its segments continue past that; the "
                                 "two must end together"};
    }
}

/// Checks an entry's compensation segments and converts them to typed values
/// (§9).
///
/// They must cover the same span as the integrable segments: the entry
/// compensation segment covers the frame's entry time, they are contiguous,
/// and where the frame contains the end of either list it contains the end
/// of both, at the same time.
///
/// @param entry      What the gather produced for one FrameEntry, walked.
/// @param segments   The validated integrable segments.
/// @param entryTime  The frame's entry time, from validateEntryPosition().
/// @param fe         The frame's end.
/// @return The compensation segments, contiguous, in list order.
/// @throws PartialRenderError  If a value is NULL or not finite, a segment is
///         empty or not contiguous with the one before it, the entry segment
///         does not cover `entryTime`, or the list ends at a different time
///         from the integrable segments.
std::vector<CompensationSegment> validateCompensation(const GatheredEntry& entry,
    std::span<const Segment> segments, SampleTime entryTime, SampleTime fe) {
    std::vector<CompensationSegment> compensation;
    for (std::size_t i = 0; i < entry.compensation.size(); ++i) {
        const GatheredCompensation& record = entry.compensation[i];
        const std::string where = "phase compensation segment " + std::to_string(i) + " (id " +
                                  std::to_string(record.id) + ")";
        const SampleTime start{requireFinite(record.startTime, where, "start_time")};
        const SampleTime end{requireFinite(record.endTime, where, "end_time")};
        const Phase startDelta{requireFinite(record.startDelta, where, "start_phase_delta")};
        const Phase endDelta{requireFinite(record.endDelta, where, "end_phase_delta")};
        if (!(end > start)) {
            throw PartialRenderError{where + ": end_time " + formatNumber(end.value()) +
                                     " is not after start_time " + formatNumber(start.value())};
        }
        if (i > 0 && start != compensation.back().endTime) {
            throw PartialRenderError{where + ": start_time " + formatNumber(start.value()) +
                                     " is not the previous segment's end_time " +
                                     formatNumber(compensation.back().endTime.value()) +
                                     "; the segments are not contiguous"};
        }
        compensation.push_back(CompensationSegment{
            .startTime = start, .endTime = end, .startDelta = startDelta, .endDelta = endDelta});
    }

    const CompensationSegment& first = compensation.front();
    if (!(first.startTime <= entryTime && entryTime < first.endTime)) {
        throw PartialRenderError{"its entry phase compensation segment [" +
                                 formatNumber(first.startTime.value()) + ", " +
                                 formatNumber(first.endTime.value()) +
                                 ") does not cover the frame's entry time " +
                                 formatNumber(entryTime.value()) + "; the FrameEntry is stale"};
    }

    // As validateLifelineEnds(): a list is known to end only where its last
    // node starts inside the frame's reach and has no successor.
    const bool segmentsEnded =
        segments.back().startTime < fe && !entry.segments.back().hasSuccessor;
    const bool compensationEnded =
        compensation.back().startTime < fe && !entry.compensation.back().hasSuccessor;
    const SampleTime segmentsEnd = segments.back().endTime;
    const SampleTime compensationEnd = compensation.back().endTime;
    if ((segmentsEnded && compensationEnded && segmentsEnd != compensationEnd) ||
        (compensationEnded && !segmentsEnded && compensationEnd < fe) ||
        (segmentsEnded && !compensationEnded && segmentsEnd < fe)) {
        throw PartialRenderError{"its phase compensation segments end at " +
                                 (compensationEnded ? formatNumber(compensationEnd.value())
                                                    : std::string{"or after the frame's end"}) +
                                 " but its integrable segments end at " +
                                 (segmentsEnded ? formatNumber(segmentsEnd.value())
                                                : std::string{"or after the frame's end"}) +
                                 "; the two must end together"};
    }
    return compensation;
}

/// Checks one entry's gathered data and converts it to typed values (§9).
///
/// @param entry  What the gather produced for one FrameEntry, walked.
/// @param frame  The frame.
/// @param size   Frame size.
/// @return The partial's validated data for the frame.
/// @throws PartialRenderError  On the first check that fails, with the
///         reason.
PartialData validateEntry(const GatheredEntry& entry, FrameNumber frame, FrameSize size) {
    if (entry.failure.has_value()) {
        throw PartialRenderError{*entry.failure};
    }
    if (!entry.partialId.has_value()) {
        throw PartialRenderError{"its FrameEntry leads to no Partial"};
    }
    if (entry.partialSetIds.size() != 1) {
        throw PartialRenderError{"it is aggregated by " +
                                 std::to_string(entry.partialSetIds.size()) +
                                 " PartialSets; exactly one is required"};
    }
    if (entry.soundSourceIds.size() != 1) {
        throw PartialRenderError{"its PartialSet is aggregated by " +
                                 std::to_string(entry.soundSourceIds.size()) +
                                 " SoundSources; exactly one is required"};
    }

    const SampleTime fs = frameStart(frame, size);
    const SampleTime fe = fs + SampleDuration{static_cast<double>(size.value())};

    PartialData data{
        .partialId = *entry.partialId,
        .partialName = entry.partialName,
        .partialSetId = entry.partialSetIds.front(),
        .soundSourceId = entry.soundSourceIds.front(),
        .segments = validateSegments(entry),
        .amplitude = {},
        .compensation = {},
    };
    const SampleTime entryTime = validateEntryPosition(entry, data.segments, fs, fe);
    data.amplitude = validateAmplitude(entry, entryTime);
    validateLifelineEnds(entry, data.segments, data.amplitude, fe);
    data.compensation = validateCompensation(entry, data.segments, entryTime, fe);
    return data;
}

/// Checks one of a source's walked spatial timelines (§9).
///
/// @param kind     Which timeline.
/// @param records  The walk: the earliest entry point and its successors.
/// @return The values in typed form: one (time, value) pair per point, the
///         value as read.
/// @throws PartialRenderError  If a time or value is NULL, not finite or out
///         of range, the times do not strictly increase, or a point but the
///         last walked has a curve other than linear.
std::vector<std::pair<SampleTime, double>> validateSpatialPoints(
    SpatialKind kind, std::span<const GatheredSpatialPoint> records) {
    const std::string timeline{spatialTimelineName(kind)};
    std::vector<std::pair<SampleTime, double>> points;
    points.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const GatheredSpatialPoint& record = records[i];
        const std::string where = timeline + " point (id " + std::to_string(record.id) + ")";
        const SampleTime time{requireFinite(record.time, where, "time")};
        const double value = requireFinite(record.value, where, spatialValueName(kind));
        if (const std::optional<std::string> problem = spatialValueProblem(kind, value)) {
            throw PartialRenderError{where + ": " + *problem};
        }
        if (i > 0 && !(time > points.back().first)) {
            throw PartialRenderError{where + ": time " + formatNumber(time.value()) +
                                     " is not later than the previous point's time " +
                                     formatNumber(points.back().first.value())};
        }
        if (i + 1 < records.size() && record.curveType != kLinearCurveType) {
            throw PartialRenderError{
                where + ": " + std::string{spatialValueName(kind)} + "_curve_type is " +
                (record.curveType.has_value() ? std::to_string(*record.curveType)
                                              : std::string{"NULL"}) +
                "; only " + std::to_string(kLinearCurveType) +
                " (linear) is supported in this version"};
        }
        points.emplace_back(time, value);
    }
    return points;
}

/// The times of a list of validated spatial points.
///
/// @tparam Point  AzimuthPoint, ElevationPoint or DistancePoint.
/// @param points  The points.
/// @return Their times, in order.
template <typename Point>
std::vector<SampleTime> timesOf(const std::vector<Point>& points) {
    std::vector<SampleTime> times;
    times.reserve(points.size());
    for (const Point& point : points) {
        times.push_back(point.time);
    }
    return times;
}

/// Checks that a source's entry point is where the frame's rendering of it
/// starts (§9): the last point at or before the frame's start, or, when the
/// timeline starts inside the frame, its first point.
///
/// @param kind    Which timeline.
/// @param walk    The walk from the entry point; its first is the entry.
/// @param times   The walk's validated times, index for index.
/// @param fs      The frame's start.
/// @throws PartialRenderError  If the entry is after the frame's start but
///         not the first point of its timeline, or at or before it but not
///         the last such point. Either means the FrameEntry is stale.
void validateSourceEntryPoint(SpatialKind kind, std::span<const GatheredSpatialPoint> walk,
    std::span<const SampleTime> times, SampleTime fs) {
    const std::string timeline{spatialTimelineName(kind)};
    if (times.front() > fs && walk.front().hasPredecessor) {
        throw PartialRenderError{"its " + timeline + " entry point at " +
                                 formatNumber(times.front().value()) +
                                 " is after the frame's start " + formatNumber(fs.value()) +
                                 " but is not the first point; the FrameEntry is stale"};
    }
    if (times.front() <= fs && times.size() > 1 && !(times[1] > fs)) {
        throw PartialRenderError{"its " + timeline + " entry point's piece ends at " +
                                 formatNumber(times[1].value()) +
                                 ", at or before the frame's start " + formatNumber(fs.value()) +
                                 "; the FrameEntry is stale"};
    }
}

/// Checks a source's gathered spatial timelines and converts them to typed
/// values (§9).
///
/// @param source  What the gather produced for the source, walked.
/// @param fs      The frame's start.
/// @return The source's validated timelines for the frame.
/// @throws PartialRenderError  If the gather recorded a failure, as
///         validateSpatialPoints(), or as validateSourceEntryPoint(). The
///         message does not name the source.
SourceData validateSource(const GatheredSource& source, SampleTime fs) {
    if (source.failure.has_value()) {
        throw PartialRenderError{*source.failure};
    }
    SourceData data{
        .soundSourceId = source.soundSourceId, .azimuth = {}, .elevation = {}, .distance = {}};
    for (const auto& [time, value] :
        validateSpatialPoints(SpatialKind::Azimuth, source.points[0])) {
        data.azimuth.push_back(AzimuthPoint{.time = time, .azimuth = Azimuth{value}});
    }
    for (const auto& [time, value] :
        validateSpatialPoints(SpatialKind::Elevation, source.points[1])) {
        data.elevation.push_back(ElevationPoint{.time = time, .elevation = Elevation{value}});
    }
    for (const auto& [time, value] :
        validateSpatialPoints(SpatialKind::Distance, source.points[2])) {
        data.distance.push_back(DistancePoint{.time = time, .distance = Metres{value}});
    }
    validateSourceEntryPoint(SpatialKind::Azimuth, source.points[0], timesOf(data.azimuth), fs);
    validateSourceEntryPoint(SpatialKind::Elevation, source.points[1], timesOf(data.elevation), fs);
    validateSourceEntryPoint(SpatialKind::Distance, source.points[2], timesOf(data.distance), fs);
    return data;
}

/// Checks a partial against its source's spatial timelines (§9).
///
/// The source's entry points are shared and were checked once, against the
/// frame's start (validateSourceEntryPoint()). What is left is per partial:
/// each timeline must start at or before the partial's entry time
/// `t* = max(b₀, fs)`. After its last point a timeline's value holds, so
/// there is no rule for its end (author, 2026-10-05).
///
/// @param entry      The partial's gathered entry, for its entry point ids.
/// @param data       The partial's validated data.
/// @param source     The source's gathered walk, for the entry point ids.
/// @param validated  The source's validated timelines, index for index with
///                   the walk.
/// @param fs         The frame's start.
/// @throws PartialRenderError  If the partial's entry is not the source's
///         (which gatherSources() rules out), or a timeline starts after
///         `t*`.
void validateSourceEntry(const GatheredEntry& entry, const PartialData& data,
    const GatheredSource& source, const SourceData& validated, SampleTime fs) {
    const SampleTime entryTime = std::max(data.segments.front().startTime, fs);
    const std::array<std::vector<SampleTime>, 3> times{
        timesOf(validated.azimuth), timesOf(validated.elevation), timesOf(validated.distance)};
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        const std::string timeline{spatialTimelineName(kSpatialKinds.at(k))};
        const std::vector<GatheredSpatialPoint>& walk = source.points.at(k);
        const std::vector<SampleTime>& t = times.at(k);
        if (entry.spatial.at(k).front().id != walk.front().id) {
            throw PartialRenderError{
                "its " + timeline + " entry point is not its source's; the FrameEntry is stale"};
        }
        if (t.front() > entryTime) {
            throw PartialRenderError{
                "its source's " + timeline + " starts at " + formatNumber(t.front().value()) +
                ", after the partial's entry time " + formatNumber(entryTime.value()) +
                "; the source's position must be defined from the start of every partial of it"};
        }
    }
}

// ---------------------------------------------------------------------------
// The run (§6.1)
// ---------------------------------------------------------------------------

/// The configuration a run needs, read once.
struct Config {
    SampleRate sampleRate{0}; ///< EngineConfig.sampleRate.
    /// EngineConfig.frameSize, or one second at sampleRate when it is NULL.
    FrameSize frameSize{0};
    /// EngineConfig.channelMode: the output channels each frame's file has.
    ChannelMode channelMode = ChannelMode::Mono;
};

/// Reads and checks the EngineConfig.
///
/// @param[in,out] source  engine-db, as rendering reads it.
/// @return The sample rate, frame size and channel mode.
/// @throws RenderingError  If there is not exactly one EngineConfig, its
///         sampleRate is NULL or not positive, its frame size is unusable,
///         or its channelMode is NULL or not 0 to 7.
/// @throws EngineDbError  If the query fails.
Config readConfig(FrameSource& source) {
    const std::vector<EngineConfigRecord> rows = source.readEngineConfigs();
    if (rows.size() != 1) {
        throw RenderingError{
            "expected exactly one EngineConfig, found " + std::to_string(rows.size())};
    }
    const std::optional<std::int64_t> frameSize = rows.front().frameSize;
    const std::optional<std::int64_t> sampleRate = rows.front().sampleRate;
    const std::optional<std::int64_t> channelMode = rows.front().channelMode;

    if (!sampleRate.has_value() || *sampleRate <= 0 ||
        *sampleRate > std::numeric_limits<std::int32_t>::max()) {
        throw RenderingError{"EngineConfig.sampleRate is " +
                             (sampleRate.has_value() ? std::to_string(*sampleRate) : "NULL") +
                             "; a positive sample rate is needed to render"};
    }
    if (!channelMode.has_value()) {
        throw RenderingError{"EngineConfig.channelMode is NULL; it must be 0 to 7"};
    }
    Config config{.sampleRate = SampleRate{static_cast<std::int32_t>(*sampleRate)},
        .frameSize = FrameSize{0},
        .channelMode = ChannelMode::Mono};
    try {
        config.channelMode = channelModeFromInt(*channelMode);
    } catch (const std::invalid_argument& e) {
        throw RenderingError{std::string{"EngineConfig."} + e.what()};
    }
    try {
        config.frameSize = resolveFrameSize(frameSize, sampleRate);
    } catch (const PrecomputationError& e) {
        throw RenderingError{e.what()};
    }
    return config;
}

/// Names a partial for a message.
///
/// @param data  The entry, for its name and id.
/// @return e.g. "Partial 0 (id 0)", or "(unnamed) (id 3)".
std::string describe(const GatheredEntry& data) {
    return data.partialName.value_or("(unnamed)") + " (id " +
           (data.partialId.has_value() ? std::to_string(*data.partialId) : "?") + ")";
}

/// Adds `from` into `into`, sample by sample.
///
/// @param[in,out] into  The frame buffer.
/// @param from          A set's buffer of the same length.
void addInto(std::span<SampleValue> into, std::span<const SampleValue> from) noexcept {
    for (std::size_t i = 0; i < into.size(); ++i) {
        into[i] += from[i];
    }
}

/// The largest absolute value in a buffer.
///
/// @param samples  The buffer.
/// @return The peak; 0 for an empty or silent buffer.
SampleValue peakOf(std::span<const SampleValue> samples) noexcept {
    SampleValue peak = 0;
    for (const SampleValue sample : samples) {
        peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

/// Removes a file, ignoring any error.
///
/// @param file  The file. Need not exist.
void removeQuietly(const std::filesystem::path& file) noexcept {
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}

/// The validated inputs of one frame, grouped for rendering: by SoundSource,
/// then by partial set, both in id order.
struct ValidatedFrame {
    /// Per source, its partials grouped by set.
    std::map<std::int64_t, std::map<std::int64_t, std::vector<PartialData>>> bySource;

    /// Per source, its validated spatial timelines.
    std::map<std::int64_t, SourceData> sources;
};

/// Validates every entry of a frame and the sources they lead to (§9),
/// recording failures.
///
/// A source whose timelines fail validation fails every partial of it, with
/// the source named in the reason.
///
/// @param gathered       The gathered entries and sources.
/// @param frame          The frame.
/// @param size           Frame size.
/// @param[in,out] known  Partials already reported as failed, by id; grows.
/// @param[in,out] outcome  Gains a newFailures entry for each partial failing
///                         for the first time, and its error when any partial
///                         failed.
/// @return The validated frame; empty when the frame failed.
ValidatedFrame validateFrame(const GatheredFrame& gathered, FrameNumber frame, FrameSize size,
    std::unordered_set<std::int64_t>& known, FrameOutcome& outcome) {
    const SampleTime fs = frameStart(frame, size);

    // Each source once, however many partials it has.
    ValidatedFrame result;
    std::map<std::int64_t, std::string> sourceFailures;
    for (const auto& [sourceId, source] : gathered.sources) {
        try {
            result.sources.emplace(sourceId, validateSource(source, fs));
        } catch (const PartialRenderError& e) {
            sourceFailures.emplace(sourceId, "its " + source.description + ": " + e.what());
        }
    }

    std::unordered_set<std::int64_t> seenPartials;
    std::vector<std::string> failed;
    for (const GatheredEntry& entry : gathered.entries) {
        try {
            if (entry.partialId.has_value() && !seenPartials.insert(*entry.partialId).second) {
                throw PartialRenderError{"it has more than one FrameEntry for this frame"};
            }
            PartialData data = validateEntry(entry, frame, size);
            if (const auto failure = sourceFailures.find(data.soundSourceId);
                failure != sourceFailures.end()) {
                throw PartialRenderError{failure->second};
            }
            const GatheredSource& source = gathered.sources.at(data.soundSourceId);
            try {
                validateSourceEntry(entry, data, source, result.sources.at(data.soundSourceId), fs);
            } catch (const PartialRenderError& e) {
                throw PartialRenderError{"its " + source.description + ": " + e.what()};
            }
            result.bySource[data.soundSourceId][data.partialSetId].push_back(std::move(data));
        } catch (const PartialRenderError& e) {
            failed.push_back(describe(entry));
            if (!entry.partialId.has_value() || known.insert(*entry.partialId).second) {
                outcome.newFailures.push_back(PartialFailure{
                    .partialId = entry.partialId.value_or(-1),
                    .partialName = entry.partialName,
                    .frame = frame,
                    .reason = e.what(),
                });
            }
        }
    }
    if (!failed.empty()) {
        std::string error = "not rendered: ";
        for (std::size_t i = 0; i < failed.size(); ++i) {
            error += (i == 0 ? "" : ", ") + failed[i];
        }
        outcome.error = error + " failed validation";
        result = {};
    }
    return result;
}

/// One partial set's place in a frame's render, in rendering order: sources
/// by id, then each source's sets by id. Built on the calling thread before
/// any worker starts, and only read while they run.
struct SetTask {
    /// SoundSource.id of the set's source.
    std::int64_t sourceId = 0;
    /// PartialSet.id.
    std::int64_t setId = 0;
    /// The set's partials, in the validated frame.
    std::span<const PartialData> partials;
    /// Whether this is its source's first set: the source's sum starts here.
    bool firstOfSource = false;
    /// Whether this is its source's last set: the source is spatialised after it.
    bool lastOfSource = false;
};

/// What rendering one partial set produced. Written only by the thread that
/// rendered the set, and read by the calling thread after forEachIndex()
/// has returned.
struct SetResult {
    /// The set's mono samples, one frame long.
    std::vector<SampleValue> samples;
    /// renderPartialSet()'s capped phases.
    std::vector<std::pair<std::int64_t, std::int64_t>> capped;
    /// What planning or rendering the set threw, if anything.
    std::exception_ptr error;
};

/// Scratch buffers reused between sets, sources and frames, so that a frame
/// allocates no sample buffers once the first has been rendered.
struct Buffers {
    /// The frame, interleaved by channel.
    std::vector<SampleValue> frame;
    /// One source's mono sum.
    std::vector<SampleValue> source;
    /// One slot per set of the wave in flight.
    std::vector<SetResult> wave;
};

/// The most memory one wave's set buffers may take.
constexpr std::size_t kWaveBudgetBytes = std::size_t{64} * 1024 * 1024;

/// How many sets to render at once: four per thread, so that a slow set
/// leaves the others work to claim, fewer if the frame is so long that four
/// per thread would pass kWaveBudgetBytes, and never fewer than one per
/// thread.
///
/// @param threads    Threads rendering. Positive.
/// @param frameSize  Samples per frame. Positive.
/// @return The wave size, at least `threads`.
std::size_t waveSize(std::size_t threads, std::size_t frameSize) noexcept {
    const std::size_t byBudget = kWaveBudgetBytes / (frameSize * sizeof(SampleValue));
    return std::clamp(byBudget, threads, 4 * threads);
}

/// Lists a frame's partial sets in rendering order (§6.4).
///
/// @param validated  The frame's validated partials.
/// @return One task per set: sources by id, then sets by id.
std::vector<SetTask> listSetTasks(const ValidatedFrame& validated) {
    std::vector<SetTask> tasks;
    for (const auto& [sourceId, sets] : validated.bySource) {
        const std::size_t first = tasks.size();
        for (const auto& [setId, partials] : sets) {
            tasks.push_back(SetTask{.sourceId = sourceId,
                .setId = setId,
                .partials = partials,
                .firstOfSource = tasks.size() == first,
                .lastOfSource = false});
        }
        if (tasks.size() > first) {
            tasks.back().lastOfSource = true;
        }
    }
    return tasks;
}

/// Plans and renders one partial set into its slot: the work that runs on
/// worker threads (ADR 0026).
///
/// Reads only `task`, the partials it points to and the arguments, none of
/// which anything writes while workers run; writes only `result`. Never
/// touches engine-db, a file or the frame's outcome.
///
/// @param task     The set.
/// @param frame    The frame.
/// @param size     Frame size.
/// @param rollOff  Band edges for the run.
/// @param kernel   Which kernel renders the set's chunks.
/// @param[in,out] result  The task's slot: its buffer exactly one frame
///                        long. Zeroed, then filled; on failure, `error` set.
void renderSetTask(const SetTask& task, FrameNumber frame, FrameSize size, const RollOff& rollOff,
    RenderKernel kernel, SetResult& result) noexcept {
    result.error = nullptr;
    result.capped.clear();
    std::ranges::fill(result.samples, 0);
    try {
        const PartialSetPlan plan = planPartialSet(task.setId, task.partials, frame, size);
        result.capped = renderPartialSet(plan, rollOff, result.samples, kernel);
    } catch (...) {
        result.error = std::current_exception();
    }
}

/// Renders a frame's sources into the frame buffer and writes the files (§6.4,
/// §7, §8.5, §5).
///
/// Each source's sets are rendered to mono and summed; the sum is spread
/// over the output channels by the source's position, once.
///
/// The sets are rendered in waves of waveSize() on up to `threads` threads,
/// each into its own buffer (ADR 0026). After each wave, the calling thread
/// takes the wave's sets in rendering order and does everything else: the
/// per-set files, the sums, the spatial stage and the outcome. So every
/// addition happens in the same order whatever the thread count, and the
/// files and the outcome are byte-for-byte those of a single thread.
///
/// @param validated   The frame's validated partials and sources.
/// @param config      Sample rate, frame size and channel mode.
/// @param rollOff     Band edges for the run.
/// @param outputDir   Where files go.
/// @param perSetWav   Whether to write per-set files: mono, before the
///                    spatial stage.
/// @param threads     Threads rendering sets, the caller's included.
/// @param kernel      Which kernel renders the sets' chunks.
/// @param[in,out] outcome  Gains the source, set and partial counts, the
///                         peak over every channel, the file and the set
///                         files.
/// @param[in,out] buffers  Scratch, resized and zeroed here.
/// @throws WavWriteError  If a file cannot be written.
/// @throws std::invalid_argument  If a plan's precondition fails, which
///         validation rules out. Thrown when the calling thread reaches that
///         set, as a single thread would.
void renderSources(const ValidatedFrame& validated, const Config& config, const RollOff& rollOff,
    const std::filesystem::path& outputDir, bool perSetWav, std::size_t threads,
    RenderKernel kernel, FrameOutcome& outcome, Buffers& buffers) {
    const auto frameSize = static_cast<std::size_t>(config.frameSize.value());
    const std::size_t channels = channelCount(config.channelMode);
    buffers.frame.assign(frameSize * channels, 0);
    buffers.source.resize(frameSize);

    const std::vector<SetTask> tasks = listSetTasks(validated);
    const std::size_t wave = waveSize(threads, frameSize);
    for (std::size_t start = 0; start < tasks.size(); start += wave) {
        const std::size_t count = std::min(wave, tasks.size() - start);
        if (buffers.wave.size() < count) {
            buffers.wave.resize(count);
        }
        for (std::size_t i = 0; i < count; ++i) {
            buffers.wave[i].samples.resize(frameSize);
        }

        // Parallel: each set into its own slot. Nothing else is touched
        // until forEachIndex() returns.
        forEachIndex(threads, count, [&](std::size_t i) {
            renderSetTask(tasks[start + i], outcome.frame, config.frameSize, rollOff, kernel,
                buffers.wave[i]);
        });

        // Serial, in rendering order: everything a single thread did after
        // rendering a set.
        for (std::size_t i = 0; i < count; ++i) {
            const SetTask& task = tasks[start + i];
            SetResult& result = buffers.wave[i];
            if (task.firstOfSource) {
                std::ranges::fill(buffers.source, 0);
            }
            if (result.error) {
                std::rethrow_exception(result.error);
            }
            outcome.cappedPhases.insert(
                outcome.cappedPhases.end(), result.capped.begin(), result.capped.end());
            if (perSetWav) {
                const std::filesystem::path setFile =
                    outputDir / setFileName(outcome.frame, task.setId);
                writeFloatWav(setFile, config.sampleRate, 1, result.samples);
                outcome.setFiles.emplace_back(task.setId, setFile);
            }
            addInto(buffers.source, result.samples);
            ++outcome.setCount;
            outcome.partialCount += task.partials.size();
            if (task.lastOfSource) {
                const SoundSourcePlan plan = planSoundSource(
                    validated.sources.at(task.sourceId), outcome.frame, config.frameSize);
                spatialiseSource(plan, config.channelMode, buffers.source, buffers.frame);
                ++outcome.sourceCount;
            }
        }
    }
    std::ranges::sort(outcome.cappedPhases);
    outcome.peak = peakOf(buffers.frame);

    const std::filesystem::path file = outputDir / frameFileName(outcome.frame);
    writeFloatWav(file, config.sampleRate, static_cast<std::int32_t>(channels), buffers.frame);
    outcome.file = file;
}

/// Renders one frame and writes its file (§6.1, one iteration).
///
/// @param[in,out] source  engine-db, as rendering reads it.
/// @param config         Sample rate and frame size.
/// @param rollOff        Band edges for the run.
/// @param frame          The frame.
/// @param outputDir      Where files go.
/// @param perSetWav      Whether to write per-set files.
/// @param threads        Threads rendering the frame's sets.
/// @param kernel         Which kernel renders the frame's sets.
/// @param[in,out] known  Partials already reported as failed, by id; grows.
/// @param[in,out] buffers  Scratch, reused between frames.
/// @return The frame's outcome.
/// @throws Nothing: every failure is recorded in the outcome.
FrameOutcome renderOneFrame(FrameSource& source, const Config& config, const RollOff& rollOff,
    FrameNumber frame, const std::filesystem::path& outputDir, bool perSetWav, std::size_t threads,
    RenderKernel kernel, std::unordered_set<std::int64_t>& known, Buffers& buffers) {
    FrameOutcome outcome{.frame = frame,
        .error = std::nullopt,
        .newFailures = {},
        .sourceCount = 0,
        .setCount = 0,
        .partialCount = 0,
        .peak = 0,
        .file = {},
        .setFiles = {},
        .cappedPhases = {}};

    // Gather and validate (§6.3, §9). A database failure fails the frame.
    GatheredFrame gathered;
    try {
        gathered = source.gatherFrame(frame, config.frameSize);
    } catch (const EngineDbError& e) {
        outcome.error = std::string{"engine-db: "} + e.what();
        return outcome;
    }
    const ValidatedFrame validated =
        validateFrame(gathered, frame, config.frameSize, known, outcome);
    if (outcome.error.has_value()) {
        return outcome;
    }

    // Prepare, render, sum and write (§6.4, §7, §5). A frame that cannot be
    // written has no file, and no per-set file either.
    try {
        renderSources(
            validated, config, rollOff, outputDir, perSetWav, threads, kernel, outcome, buffers);
    } catch (const WavWriteError& e) {
        outcome.error = e.what();
    } catch (const std::invalid_argument& e) {
        outcome.error = std::string{"not rendered: "} + e.what();
    }
    if (outcome.error.has_value()) {
        for (const auto& [setId, file] : outcome.setFiles) {
            removeQuietly(file);
        }
        outcome.setFiles.clear();
        outcome.file.clear();
    }
    return outcome;
}

} // namespace

std::size_t RenderingReport::failureCount() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(frames, [](const FrameOutcome& f) { return !f.ok(); }));
}

std::size_t RenderingReport::failedPartialCount() const noexcept {
    std::size_t total = 0;
    for (const FrameOutcome& frame : frames) {
        total += frame.newFailures.size();
    }
    return total;
}

std::string frameFileName(FrameNumber frame) {
    std::ostringstream name;
    name << "frame-" << std::setw(6) << std::setfill('0') << frame.value() << ".wav";
    return name.str();
}

std::string setFileName(FrameNumber frame, std::int64_t partialSetId) {
    std::ostringstream name;
    name << "frame-" << std::setw(6) << std::setfill('0') << frame.value() << ".set-"
         << std::setw(6) << std::setfill('0') << partialSetId << ".wav";
    return name.str();
}

RenderingReport renderFrames(FrameSource& source, const RenderRequest& request,
    const std::filesystem::path& outputDir, bool perSetWav, std::size_t threads,
    RenderKernel kernel) {
    const Config config = readConfig(source);
    const RollOff rollOff = makeRollOff(config.sampleRate);

    RenderingReport report;
    report.sampleRate = config.sampleRate;
    report.frameSize = config.frameSize;
    report.channelMode = config.channelMode;
    report.threads = resolveThreadCount(threads);
    report.kernel = kernel;

    std::unordered_set<std::int64_t> known;
    Buffers buffers;
    for (const FrameRequest& requested : request.frames) {
        report.frames.push_back(
            renderOneFrame(source, config, rollOff, FrameNumber{requested.frameNumber}, outputDir,
                perSetWav, report.threads, kernel, known, buffers));
    }
    return report;
}

} // namespace rainbohz::rendering_engine
