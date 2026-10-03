#include "rainbohz/rendering_engine/rendering.hpp"

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/frame_plan.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/wav_writer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <map>
#include <optional>
#include <span>
#include <sstream>
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
/// Implements docs/rendering.md sections 6.1, 6.3 and 9. The file is
/// organised as:
///
///   - fixed Cypher queries, prepared once by EngineDb and always bound with
///     parameters;
///   - the records as read, with every property optional because every
///     column is nullable;
///   - the gather for one frame: the entry query, one list-node query per
///     list type, and the frontier walk that follows NEXT_POINT one hop per
///     query for every partial still inside the frame;
///   - validation of one partial's gathered data into a PartialData;
///   - the run: per frame, gather, group by partial set, plan and render each
///     set (frame_plan.hpp), sum, write.
///
/// The gather is the only layer that touches engine-db, and it finishes
/// before the frame is prepared.
///
/// @see rendering.hpp

namespace rainbohz::rendering_engine {

namespace {

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

constexpr std::string_view kReadConfig =
    "MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate, c.channelMode;";

// Query 1 of §6.3: the frame's entries, each with its partial and the set
// aggregating it. Both are OPTIONAL so that an entry leading nowhere, or a
// partial in no set, is seen and refused rather than silently dropped.
constexpr std::string_view kReadEntries = "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry) "
                                          "WHERE f.frame_number = CAST($frame AS INT32) "
                                          "OPTIONAL MATCH (e)-[:FRAME_ENTRY_PARTIAL]->(p:Partial) "
                                          "OPTIONAL MATCH (ps:PartialSet)-[:HAS_PARTIAL]->(p) "
                                          "RETURN e.id, p.id, p.name, ps.id ORDER BY p.id, e.id;";

// Query 2: each entry's segment with its nominal frequency, and whether the
// segment has a predecessor segment (an entry segment starting after the
// frame start must be the head of its list, or the entry is stale).
constexpr std::string_view kReadEntrySegments =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(s:PartialIntegrableSegment) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (s)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->(n:PartialSegmentNominalFrequency) "
    "OPTIONAL MATCH (prev:PartialIntegrableSegment)-[:NEXT_POINT]->(s) "
    "RETURN e.id, s.id, s.start_time, s.end_time, s.start_phase, "
    "n.start_nominal_frequency, n.linear_component, n.linear_gradient, n.curve_component, "
    "prev.id;";

constexpr std::string_view kReadEntryPoints =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:AmplitudePhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "RETURN e.id, a.id, a.time, a.amplitude, a.amplitude_curve_type;";

constexpr std::string_view kReadEntryCompensation =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(c:PartialPhaseCompensationSegment) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "RETURN e.id, c.id, c.start_time, c.end_time, c.start_phase_delta, c.end_phase_delta;";

// Query 3: one hop along NEXT_POINT for every node still walking, by
// primary-key lookup (engine_db.hpp, DbParameter, on why UNWIND).
constexpr std::string_view kNextSegments =
    "UNWIND $ids AS id "
    "MATCH (a:PartialIntegrableSegment {id: id})-[:NEXT_POINT]->(b:PartialIntegrableSegment) "
    "OPTIONAL MATCH (b)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->(n:PartialSegmentNominalFrequency) "
    "RETURN a.id, b.id, b.start_time, b.end_time, b.start_phase, "
    "n.start_nominal_frequency, n.linear_component, n.linear_gradient, n.curve_component;";

constexpr std::string_view kNextPoints =
    "UNWIND $ids AS id "
    "MATCH (a:AmplitudePhysicalTimelinePoint {id: "
    "id})-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id, b.time, b.amplitude, b.amplitude_curve_type;";

constexpr std::string_view kNextCompensation =
    "UNWIND $ids AS id "
    "MATCH (a:PartialPhaseCompensationSegment {id: id})-[:NEXT_POINT]->"
    "(b:PartialPhaseCompensationSegment) "
    "RETURN a.id, b.id, b.start_time, b.end_time, b.start_phase_delta, b.end_phase_delta;";

/// The value of AmplitudePhysicalTimelinePoint.amplitude_curve_type for a linear
/// curve: the only one this version renders.
constexpr std::int64_t kLinearAmplitudeCurve = 0;

/// The only channel mode this version renders: mono.
constexpr std::int64_t kMonoChannelMode = 0;

// ---------------------------------------------------------------------------
// Records, as read
// ---------------------------------------------------------------------------

/// A PartialIntegrableSegment with its PartialSegmentNominalFrequency, as
/// read. Every property is nullable in the schema, so every one is optional.
struct SegmentRecord {
    std::int64_t id = 0;
    std::optional<double> startTime;
    std::optional<double> endTime;
    std::optional<double> startPhase;
    std::optional<double> startFrequency;
    std::optional<bool> linearComponent;
    std::optional<double> linearGradient;
    std::optional<bool> curveComponent;
    /// Whether the segment has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// An AmplitudePhysicalTimelinePoint as read.
struct PointRecord {
    std::int64_t id = 0;
    std::optional<double> time;
    std::optional<double> amplitude;
    std::optional<std::int64_t> curveType;
    /// Whether the point has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// A PartialPhaseCompensationSegment as read.
struct CompensationRecord {
    std::int64_t id = 0;
    std::optional<double> startTime;
    std::optional<double> endTime;
    std::optional<double> startDelta;
    std::optional<double> endDelta;
    /// Whether the segment has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// Everything gathered for one FrameEntry, before validation.
struct EntryRecord {
    std::int64_t entryId = 0;
    std::optional<std::int64_t> partialId;
    std::optional<std::string> partialName;
    std::vector<std::int64_t> partialSetIds;
    /// The entry segment(s) and, after the walk, their successors in order.
    std::vector<SegmentRecord> segments;
    /// Whether the (first) entry segment has a predecessor segment.
    bool entrySegmentHasPredecessor = false;
    /// The entry point(s) and, after the walk, their successors in order.
    std::vector<PointRecord> points;
    /// The entry compensation segment(s) and, after the walk, their
    /// successors in order.
    std::vector<CompensationRecord> compensation;
    /// The failure found during the gather, if any, so that the walk can
    /// leave a broken partial alone and report it afterwards.
    std::optional<std::string> failure;
};

/// Reads the segment columns of a row, which every segment query returns in
/// the same order from column `from`.
///
/// @param row   A row of kReadEntrySegments or kNextSegments.
/// @param from  Index of the segment's id column.
/// @return The record, with hasSuccessor false.
/// @throws EngineDbError  If a column is of the wrong type.
SegmentRecord segmentFromRow(const DbRow& row, std::size_t from) {
    return SegmentRecord{
        .id = requireInt64(row.at(from)),
        .startTime = optionalDouble(row.at(from + 1)),
        .endTime = optionalDouble(row.at(from + 2)),
        .startPhase = optionalDouble(row.at(from + 3)),
        .startFrequency = optionalDouble(row.at(from + 4)),
        .linearComponent = optionalBool(row.at(from + 5)),
        .linearGradient = optionalDouble(row.at(from + 6)),
        .curveComponent = optionalBool(row.at(from + 7)),
        .hasSuccessor = false,
    };
}

/// Reads the point columns of a row.
///
/// @param row   A row of kReadEntryPoints or kNextPoints.
/// @param from  Index of the point's id column.
/// @return The record, with hasSuccessor false.
/// @throws EngineDbError  If a column is of the wrong type.
PointRecord pointFromRow(const DbRow& row, std::size_t from) {
    return PointRecord{
        .id = requireInt64(row.at(from)),
        .time = optionalDouble(row.at(from + 1)),
        .amplitude = optionalDouble(row.at(from + 2)),
        .curveType = optionalInt64(row.at(from + 3)),
        .hasSuccessor = false,
    };
}

/// Reads the compensation segment columns of a row.
///
/// @param row   A row of kReadEntryCompensation or kNextCompensation.
/// @param from  Index of the segment's id column.
/// @return The record, with hasSuccessor false.
/// @throws EngineDbError  If a column is of the wrong type.
CompensationRecord compensationFromRow(const DbRow& row, std::size_t from) {
    return CompensationRecord{
        .id = requireInt64(row.at(from)),
        .startTime = optionalDouble(row.at(from + 1)),
        .endTime = optionalDouble(row.at(from + 2)),
        .startDelta = optionalDouble(row.at(from + 3)),
        .endDelta = optionalDouble(row.at(from + 4)),
        .hasSuccessor = false,
    };
}

// ---------------------------------------------------------------------------
// The gather (§6.3)
// ---------------------------------------------------------------------------

/// Follows NEXT_POINT one hop per query from every entry's last node, until
/// each has a node starting at or after `frameEnd`, or its list ends.
///
/// One query per hop serves every entry still walking. A walk that revisits
/// a node records a failure on its entry, as walkChain does; a node whose
/// time is NULL ends its walk, and validation reports it.
///
/// @tparam Record     SegmentRecord, PointRecord or CompensationRecord.
/// @param[in,out] db  Database to read.
/// @param cypher      kNextSegments, kNextPoints or kNextCompensation.
/// @param frameEnd    The frame's end, source time.
/// @param[in,out] entries  The entries. Each one's list grows by the nodes
///                         walked; `hasSuccessor` is set on every node found
///                         to have one.
/// @param list        Selects the list on an entry: its segments, points or
///                    compensation segments.
/// @param startOf     The start time of a record, or nullopt when NULL.
/// @param fromRow     Builds a record from a result row, from column 1.
/// @param what        The list's name, for failure messages.
/// @throws EngineDbError  If a query fails.
template <typename Record, typename List, typename StartOf, typename FromRow>
void walkFrontier(EngineDb& db, std::string_view cypher, SampleTime frameEnd,
    std::vector<EntryRecord>& entries, List list, StartOf startOf, FromRow fromRow,
    std::string_view what) {
    // Which entry each frontier node belongs to, and the nodes each entry
    // has seen (the cycle check).
    std::unordered_map<std::int64_t, std::size_t> owner;
    std::vector<std::unordered_set<std::int64_t>> seen(entries.size());
    std::vector<std::int64_t> frontier;

    const auto stillWalking = [&](std::size_t i) {
        EntryRecord& entry = entries[i];
        if (entry.failure.has_value() || list(entry).empty()) {
            return false;
        }
        const std::optional<double> start = startOf(list(entry).back());
        return start.has_value() && *start < frameEnd.value();
    };

    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (const Record& record : list(entries[i])) {
            seen[i].insert(record.id);
        }
        if (stillWalking(i)) {
            frontier.push_back(list(entries[i]).back().id);
            owner[list(entries[i]).back().id] = i;
        }
    }

    while (!frontier.empty()) {
        std::vector<std::int64_t> next;
        std::unordered_map<std::int64_t, std::size_t> nextOwner;
        for (const DbRow& row : db.query(cypher, {{"ids", frontier}})) {
            const std::size_t i = owner.at(requireInt64(row.at(0)));
            EntryRecord& entry = entries[i];
            Record record = fromRow(row, 1);
            list(entry).back().hasSuccessor = true;
            if (!seen[i].insert(record.id).second) {
                entry.failure = std::string{what} + ": its NEXT_POINT list loops back to node id " +
                                std::to_string(record.id);
                continue;
            }
            list(entry).push_back(std::move(record));
            if (stillWalking(i)) {
                next.push_back(list(entry).back().id);
                nextOwner[list(entry).back().id] = i;
            }
        }
        frontier = std::move(next);
        owner = std::move(nextOwner);
    }
}

/// Gathers everything frame `frame` needs, unvalidated (§6.3).
///
/// @param[in,out] db  Database to read. Not modified.
/// @param frame       The frame.
/// @param size        Frame size.
/// @return One record per FrameEntry of the frame, ordered by partial id.
///         Empty when there is no Frame node or it has no entries.
/// @throws EngineDbError  If a query fails or returns a value of an
///         unexpected type.
std::vector<EntryRecord> gatherFrame(EngineDb& db, FrameNumber frame, FrameSize size) {
    const SampleTime frameEnd =
        frameStart(frame, size) + SampleDuration{static_cast<double>(size.value())};
    const DbParameters byFrame{{"frame", static_cast<std::int64_t>(frame.value())}};

    std::vector<EntryRecord> entries;
    std::unordered_map<std::int64_t, std::size_t> byEntry;
    for (const DbRow& row : db.query(kReadEntries, byFrame)) {
        const std::int64_t entryId = requireInt64(row.at(0));
        auto found = byEntry.find(entryId);
        if (found == byEntry.end()) {
            found = byEntry.emplace(entryId, entries.size()).first;
            entries.push_back(EntryRecord{.entryId = entryId,
                .partialId = optionalInt64(row.at(1)),
                .partialName = optionalString(row.at(2)),
                .partialSetIds = {},
                .segments = {},
                .entrySegmentHasPredecessor = false,
                .points = {},
                .compensation = {},
                .failure = std::nullopt});
        }
        if (const std::optional<std::int64_t> setId = optionalInt64(row.at(3))) {
            entries[found->second].partialSetIds.push_back(*setId);
        }
    }

    for (const DbRow& row : db.query(kReadEntrySegments, byFrame)) {
        EntryRecord& entry = entries[byEntry.at(requireInt64(row.at(0)))];
        entry.segments.push_back(segmentFromRow(row, 1));
        entry.entrySegmentHasPredecessor = optionalInt64(row.at(9)).has_value();
    }
    for (const DbRow& row : db.query(kReadEntryPoints, byFrame)) {
        entries[byEntry.at(requireInt64(row.at(0)))].points.push_back(pointFromRow(row, 1));
    }
    for (const DbRow& row : db.query(kReadEntryCompensation, byFrame)) {
        entries[byEntry.at(requireInt64(row.at(0)))].compensation.push_back(
            compensationFromRow(row, 1));
    }

    // An entry with other than one node of a list type fails validation;
    // the walk must not extend it as if it were sound.
    for (EntryRecord& entry : entries) {
        if (entry.segments.size() != 1 || entry.points.size() != 1 ||
            entry.compensation.size() != 1) {
            entry.failure = "its FrameEntry leads to " + std::to_string(entry.segments.size()) +
                            " segment(s), " + std::to_string(entry.points.size()) +
                            " amplitude point(s) and " + std::to_string(entry.compensation.size()) +
                            " phase compensation segment(s); exactly one of each is required, "
                            "so it was not precomputed, or not for this frame";
        }
    }

    walkFrontier<SegmentRecord>(
        db, kNextSegments, frameEnd, entries,
        [](EntryRecord& e) -> std::vector<SegmentRecord>& { return e.segments; },
        [](const SegmentRecord& s) { return s.startTime; }, segmentFromRow,
        "PartialSegmentPhysicalTimeline");
    walkFrontier<PointRecord>(
        db, kNextPoints, frameEnd, entries,
        [](EntryRecord& e) -> std::vector<PointRecord>& { return e.points; },
        [](const PointRecord& p) { return p.time; }, pointFromRow, "AmplitudePhysicalTimeline");
    walkFrontier<CompensationRecord>(
        db, kNextCompensation, frameEnd, entries,
        [](EntryRecord& e) -> std::vector<CompensationRecord>& { return e.compensation; },
        [](const CompensationRecord& c) { return c.startTime; }, compensationFromRow,
        "PartialPhaseCompensationSegment list");
    return entries;
}

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
std::vector<Segment> validateSegments(const EntryRecord& entry) {
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < entry.segments.size(); ++i) {
        const SegmentRecord& record = entry.segments[i];
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
    const EntryRecord& entry, std::span<const Segment> segments, SampleTime fs, SampleTime fe) {
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
std::vector<AmplitudePoint> validateAmplitude(const EntryRecord& entry, SampleTime entryTime) {
    std::vector<AmplitudePoint> amplitude;
    for (std::size_t i = 0; i < entry.points.size(); ++i) {
        const PointRecord& record = entry.points[i];
        const std::string where =
            "amplitude point " + std::to_string(i) + " (id " + std::to_string(record.id) + ")";
        const SampleTime time{requireFinite(record.time, where, "time")};
        const Decibels value{requireFinite(record.amplitude, where, "amplitude")};
        if (i > 0 && !(time > amplitude.back().time)) {
            throw PartialRenderError{where + ": time " + formatNumber(time.value()) +
                                     " is not later than the previous point's time " +
                                     formatNumber(amplitude.back().time.value())};
        }
        if (i + 1 < entry.points.size() && record.curveType != kLinearAmplitudeCurve) {
            throw PartialRenderError{
                where + ": amplitude_curve_type is " +
                (record.curveType.has_value() ? std::to_string(*record.curveType)
                                              : std::string{"NULL"}) +
                "; only " + std::to_string(kLinearAmplitudeCurve) +
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
void validateLifelineEnds(const EntryRecord& entry, std::span<const Segment> segments,
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
std::vector<CompensationSegment> validateCompensation(const EntryRecord& entry,
    std::span<const Segment> segments, SampleTime entryTime, SampleTime fe) {
    std::vector<CompensationSegment> compensation;
    for (std::size_t i = 0; i < entry.compensation.size(); ++i) {
        const CompensationRecord& record = entry.compensation[i];
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
PartialData validateEntry(const EntryRecord& entry, FrameNumber frame, FrameSize size) {
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

    const SampleTime fs = frameStart(frame, size);
    const SampleTime fe = fs + SampleDuration{static_cast<double>(size.value())};

    PartialData data{
        .partialId = *entry.partialId,
        .partialName = entry.partialName,
        .partialSetId = entry.partialSetIds.front(),
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

// ---------------------------------------------------------------------------
// The run (§6.1)
// ---------------------------------------------------------------------------

/// The configuration a run needs, read once.
struct Config {
    SampleRate sampleRate{0};
    FrameSize frameSize{0};
};

/// Reads and checks the EngineConfig.
///
/// @param[in,out] db  Database to read.
/// @return The sample rate and frame size.
/// @throws RenderingError  If there is not exactly one EngineConfig, its
///         sampleRate is NULL or not positive, its frame size is unusable,
///         or its channelMode is not mono.
/// @throws EngineDbError  If the query fails.
Config readConfig(EngineDb& db) {
    const std::vector<DbRow> rows = db.query(kReadConfig);
    if (rows.size() != 1) {
        throw RenderingError{
            "expected exactly one EngineConfig, found " + std::to_string(rows.size())};
    }
    const std::optional<std::int64_t> frameSize = optionalInt64(rows.front().at(0));
    const std::optional<std::int64_t> sampleRate = optionalInt64(rows.front().at(1));
    const std::optional<std::int64_t> channelMode = optionalInt64(rows.front().at(2));

    if (!sampleRate.has_value() || *sampleRate <= 0 ||
        *sampleRate > std::numeric_limits<std::int32_t>::max()) {
        throw RenderingError{"EngineConfig.sampleRate is " +
                             (sampleRate.has_value() ? std::to_string(*sampleRate) : "NULL") +
                             "; a positive sample rate is needed to render"};
    }
    if (channelMode != kMonoChannelMode) {
        throw RenderingError{"EngineConfig.channelMode is " +
                             (channelMode.has_value() ? std::to_string(*channelMode) : "NULL") +
                             "; only 0 (mono) is implemented in this version"};
    }
    Config config{.sampleRate = SampleRate{static_cast<std::int32_t>(*sampleRate)},
        .frameSize = FrameSize{0}};
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
std::string describe(const EntryRecord& data) {
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

/// The partials of one frame, validated and grouped by partial set, in set
/// id order.
using PartialsBySet = std::map<std::int64_t, std::vector<PartialData>>;

/// Validates every entry of a frame (§9), recording failures.
///
/// @param entries        The gathered entries.
/// @param frame          The frame.
/// @param size           Frame size.
/// @param[in,out] known  Partials already reported as failed, by id; grows.
/// @param[in,out] outcome  Gains a newFailures entry for each partial failing
///                         for the first time, and its error when any partial
///                         failed.
/// @return The validated partials by set; empty when the frame failed.
PartialsBySet validateFrame(const std::vector<EntryRecord>& entries, FrameNumber frame,
    FrameSize size, std::unordered_set<std::int64_t>& known, FrameOutcome& outcome) {
    PartialsBySet bySet;
    std::unordered_set<std::int64_t> seenPartials;
    std::vector<std::string> failed;
    for (const EntryRecord& entry : entries) {
        try {
            if (entry.partialId.has_value() && !seenPartials.insert(*entry.partialId).second) {
                throw PartialRenderError{"it has more than one FrameEntry for this frame"};
            }
            PartialData data = validateEntry(entry, frame, size);
            bySet[data.partialSetId].push_back(std::move(data));
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
        bySet.clear();
    }
    return bySet;
}

/// Renders a frame's sets into the frame buffer and writes the files (§6.4,
/// §7, §5).
///
/// @param bySet       The frame's validated partials by set.
/// @param config      Sample rate and frame size.
/// @param rollOff     Band edges for the run.
/// @param outputDir   Where files go.
/// @param perSetWav   Whether to write per-set files.
/// @param[in,out] outcome      Gains the set and partial counts, the peak,
///                             the file and the set files.
/// @param[in,out] frameBuffer  Scratch of one frame, zeroed here.
/// @param[in,out] setBuffer    Scratch of one frame, reused between sets.
/// @throws WavWriteError  If a file cannot be written.
/// @throws std::invalid_argument  If a plan's precondition fails, which
///         validation rules out.
void renderSets(const PartialsBySet& bySet, const Config& config, const RollOff& rollOff,
    const std::filesystem::path& outputDir, bool perSetWav, FrameOutcome& outcome,
    std::vector<SampleValue>& frameBuffer, std::vector<SampleValue>& setBuffer) {
    const auto frameSize = static_cast<std::size_t>(config.frameSize.value());
    frameBuffer.assign(frameSize, 0);
    setBuffer.resize(frameSize);
    for (const auto& [setId, partials] : bySet) {
        const PartialSetPlan plan =
            planPartialSet(setId, partials, outcome.frame, config.frameSize);
        std::ranges::fill(setBuffer, 0);
        renderPartialSet(plan, rollOff, setBuffer);
        if (perSetWav) {
            const std::filesystem::path setFile = outputDir / setFileName(outcome.frame, setId);
            writeFloatWav(setFile, config.sampleRate, 1, setBuffer);
            outcome.setFiles.emplace_back(setId, setFile);
        }
        addInto(frameBuffer, setBuffer);
        ++outcome.setCount;
        outcome.partialCount += partials.size();
    }
    outcome.peak = peakOf(frameBuffer);

    const std::filesystem::path file = outputDir / frameFileName(outcome.frame);
    writeFloatWav(file, config.sampleRate, 1, frameBuffer);
    outcome.file = file;
}

/// Renders one frame and writes its file (§6.1, one iteration).
///
/// @param[in,out] db     Database to read.
/// @param config         Sample rate and frame size.
/// @param rollOff        Band edges for the run.
/// @param frame          The frame.
/// @param outputDir      Where files go.
/// @param perSetWav      Whether to write per-set files.
/// @param[in,out] known  Partials already reported as failed, by id; grows.
/// @param[in,out] frameBuffer  Scratch of one frame, reused between frames.
/// @param[in,out] setBuffer    Scratch of one frame, reused between sets.
/// @return The frame's outcome.
/// @throws Nothing: every failure is recorded in the outcome.
FrameOutcome renderOneFrame(EngineDb& db, const Config& config, const RollOff& rollOff,
    FrameNumber frame, const std::filesystem::path& outputDir, bool perSetWav,
    std::unordered_set<std::int64_t>& known, std::vector<SampleValue>& frameBuffer,
    std::vector<SampleValue>& setBuffer) {
    FrameOutcome outcome{.frame = frame,
        .error = std::nullopt,
        .newFailures = {},
        .setCount = 0,
        .partialCount = 0,
        .peak = 0,
        .file = {},
        .setFiles = {}};

    // Gather and validate (§6.3, §9). A database failure fails the frame.
    std::vector<EntryRecord> entries;
    try {
        entries = gatherFrame(db, frame, config.frameSize);
    } catch (const EngineDbError& e) {
        outcome.error = std::string{"engine-db: "} + e.what();
        return outcome;
    }
    const PartialsBySet bySet = validateFrame(entries, frame, config.frameSize, known, outcome);
    if (outcome.error.has_value()) {
        return outcome;
    }

    // Prepare, render, sum and write (§6.4, §7, §5). A frame that cannot be
    // written has no file, and no per-set file either.
    try {
        renderSets(bySet, config, rollOff, outputDir, perSetWav, outcome, frameBuffer, setBuffer);
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

RenderingReport renderFrames(EngineDb& db, const RenderRequest& request,
    const std::filesystem::path& outputDir, bool perSetWav) {
    const Config config = readConfig(db);
    const RollOff rollOff = makeRollOff(config.sampleRate);

    RenderingReport report;
    report.sampleRate = config.sampleRate;
    report.frameSize = config.frameSize;

    std::unordered_set<std::int64_t> known;
    std::vector<SampleValue> frameBuffer;
    std::vector<SampleValue> setBuffer;
    for (const FrameRequest& requested : request.frames) {
        report.frames.push_back(
            renderOneFrame(db, config, rollOff, FrameNumber{requested.frameNumber}, outputDir,
                perSetWav, known, frameBuffer, setBuffer));
    }
    return report;
}

} // namespace rainbohz::rendering_engine
