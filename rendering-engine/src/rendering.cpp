// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/rendering.hpp"

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/frame_plan.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
///   - fixed Cypher queries, prepared once by EngineDb and always bound with
///     parameters;
///   - the records as read, with every property optional because every
///     column is nullable;
///   - the gather for one frame: the entry query, one list-node query per
///     list type, and the frontier walk that follows NEXT_POINT one hop per
///     query for every partial still inside the frame, and for every
///     SoundSource's spatial timelines once;
///   - validation of one partial's gathered data into a PartialData, and of
///     one source's into a SourceData;
///   - the run: per frame, gather, group by source and then by partial set,
///     plan and render each set to mono (frame_plan.hpp), sum each source's
///     sets, spread the sum over the channels by the source's position, write.
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

/// Reads the EngineConfig's frame size, sample rate and channel mode.
constexpr std::string_view kReadConfig =
    "MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate, c.channelMode;";

/// Query 1 of §6.3: the frame's entries, each with its partial and the set
/// aggregating it. Both are OPTIONAL so that an entry leading nowhere, or a
/// partial in no set, is seen and refused rather than silently dropped.
constexpr std::string_view kReadEntries =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (e)-[:FRAME_ENTRY_PARTIAL]->(p:Partial) "
    "OPTIONAL MATCH (ps:PartialSet)-[:HAS_PARTIAL]->(p) "
    "OPTIONAL MATCH (ss:SoundSource)-[:HAS_PARTIALSET]->(ps) "
    "RETURN e.id, p.id, p.name, ps.id, ss.id, ss.name ORDER BY p.id, e.id;";

/// Query 2: each entry's segment with its nominal frequency, and whether the
/// segment has a predecessor segment (an entry segment starting after the
/// frame start must be the head of its list, or the entry is stale).
constexpr std::string_view kReadEntrySegments =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(s:PartialIntegrableSegment) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (s)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->(n:PartialSegmentNominalFrequency) "
    "OPTIONAL MATCH (prev:PartialIntegrableSegment)-[:NEXT_POINT]->(s) "
    "RETURN e.id, s.id, s.start_time, s.end_time, s.start_phase, "
    "n.start_nominal_frequency, n.linear_component, n.linear_gradient, n.curve_component, "
    "prev.id;";

/// Each entry's AmplitudePhysicalTimelinePoint in the frame.
constexpr std::string_view kReadEntryPoints =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:AmplitudePhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "RETURN e.id, a.id, a.time, a.amplitude, a.amplitude_curve_type;";

/// Each entry's PartialPhaseCompensationSegment in the frame.
constexpr std::string_view kReadEntryCompensation =
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(c:PartialPhaseCompensationSegment) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "RETURN e.id, c.id, c.start_time, c.end_time, c.start_phase_delta, c.end_phase_delta;";

/// Each entry's point on its SoundSource's azimuth, elevation and distance
/// timelines, in kSpatialKinds order, and whether it has a predecessor point
/// (an entry after the frame's start must be the first point of its list).
/// The same for every partial of a source; rendering walks from it once per
/// source.
constexpr std::array<std::string_view, 3> kReadEntrySpatial{
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:AzimuthPhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (prev:AzimuthPhysicalTimelinePoint)-[:NEXT_POINT]->(a) "
    "RETURN e.id, a.id, a.time, a.azimuth, a.azimuth_curve_type, prev.id;",
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:ElevationPhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (prev:ElevationPhysicalTimelinePoint)-[:NEXT_POINT]->(a) "
    "RETURN e.id, a.id, a.time, a.elevation, a.elevation_curve_type, prev.id;",
    "MATCH (f:Frame)-[:FRAME_ENTRY]->(e:FrameEntry)"
    "-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a:DistancePhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) "
    "OPTIONAL MATCH (prev:DistancePhysicalTimelinePoint)-[:NEXT_POINT]->(a) "
    "RETURN e.id, a.id, a.time, a.distance, a.distance_curve_type, prev.id;",
};

/// Query 3: one hop along NEXT_POINT for every node still walking, by
/// primary-key lookup (engine_db.hpp, DbParameter, on why UNWIND).
constexpr std::string_view kNextSegments =
    "UNWIND $ids AS id "
    "MATCH (a:PartialIntegrableSegment {id: id})-[:NEXT_POINT]->(b:PartialIntegrableSegment) "
    "OPTIONAL MATCH (b)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->(n:PartialSegmentNominalFrequency) "
    "RETURN a.id, b.id, b.start_time, b.end_time, b.start_phase, "
    "n.start_nominal_frequency, n.linear_component, n.linear_gradient, n.curve_component;";

/// As kNextSegments, for AmplitudePhysicalTimelinePoints: each point's successor.
constexpr std::string_view kNextPoints =
    "UNWIND $ids AS id "
    "MATCH (a:AmplitudePhysicalTimelinePoint {id: "
    "id})-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id, b.time, b.amplitude, b.amplitude_curve_type;";

/// As kNextSegments, for PartialPhaseCompensationSegments: each segment's
/// successor.
constexpr std::string_view kNextCompensation =
    "UNWIND $ids AS id "
    "MATCH (a:PartialPhaseCompensationSegment {id: id})-[:NEXT_POINT]->"
    "(b:PartialPhaseCompensationSegment) "
    "RETURN a.id, b.id, b.start_time, b.end_time, b.start_phase_delta, b.end_phase_delta;";

/// As kNextSegments, for physical spatial timeline points: each point's
/// successor, one statement per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kNextSpatial{
    "UNWIND $ids AS id "
    "MATCH (a:AzimuthPhysicalTimelinePoint {id: id})-[:NEXT_POINT]->"
    "(b:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, b.id, b.time, b.azimuth, b.azimuth_curve_type;",
    "UNWIND $ids AS id "
    "MATCH (a:ElevationPhysicalTimelinePoint {id: id})-[:NEXT_POINT]->"
    "(b:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, b.id, b.time, b.elevation, b.elevation_curve_type;",
    "UNWIND $ids AS id "
    "MATCH (a:DistancePhysicalTimelinePoint {id: id})-[:NEXT_POINT]->"
    "(b:DistancePhysicalTimelinePoint) "
    "RETURN a.id, b.id, b.time, b.distance, b.distance_curve_type;",
};

/// The value of a `*_curve_type` for a linear curve: the only one this
/// version renders, for amplitude and for spatial position.
constexpr std::int64_t kLinearCurveType = 0;

// ---------------------------------------------------------------------------
// Records, as read
// ---------------------------------------------------------------------------

/// A PartialIntegrableSegment with its PartialSegmentNominalFrequency, as
/// read. Every property is nullable in the schema, so every one is optional.
struct SegmentRecord {
    std::int64_t id = 0;                  ///< PartialIntegrableSegment.id.
    std::optional<double> startTime;      ///< start_time, in samples.
    std::optional<double> endTime;        ///< end_time, in samples.
    std::optional<double> startPhase;     ///< start_phase, in radians.
    std::optional<double> startFrequency; ///< start_nominal_frequency, radians per sample.
    std::optional<bool> linearComponent;  ///< linear_component: whether it has a gradient.
    std::optional<double> linearGradient; ///< linear_gradient, radians per sample².
    std::optional<bool> curveComponent;   ///< curve_component; must not be true.
    /// Whether the segment has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// An AmplitudePhysicalTimelinePoint as read.
struct PointRecord {
    std::int64_t id = 0;                   ///< AmplitudePhysicalTimelinePoint.id.
    std::optional<double> time;            ///< time, in samples.
    std::optional<double> amplitude;       ///< amplitude, in dB.
    std::optional<std::int64_t> curveType; ///< amplitude_curve_type; kLinearCurveType renders.
    /// Whether the point has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// A PartialPhaseCompensationSegment as read.
struct CompensationRecord {
    std::int64_t id = 0;              ///< PartialPhaseCompensationSegment.id.
    std::optional<double> startTime;  ///< start_time, in samples.
    std::optional<double> endTime;    ///< end_time, in samples.
    std::optional<double> startDelta; ///< start_phase_delta, in radians.
    std::optional<double> endDelta;   ///< end_phase_delta, in radians.
    /// Whether the segment has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// An azimuth, elevation or distance point as read.
struct SpatialRecord {
    std::int64_t id = 0;        ///< The azimuth, elevation or distance point's id.
    std::optional<double> time; ///< time, in samples.
    /// The azimuth or elevation in radians, or the distance in metres.
    std::optional<double> value;
    std::optional<std::int64_t> curveType; ///< The `*_curve_type`; kLinearCurveType renders.
    /// Whether the point has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
    /// Whether the point has a predecessor point. Read for entry points only.
    bool hasPredecessor = false;
};

/// Everything gathered for one FrameEntry, before validation.
struct EntryRecord {
    std::int64_t entryId = 0;               ///< FrameEntry.id.
    std::optional<std::int64_t> partialId;  ///< The entry's Partial.id; NULL if it leads nowhere.
    std::optional<std::string> partialName; ///< That Partial's name, which may be NULL.
    /// The PartialSet(s) aggregating the partial, distinct; valid with exactly one.
    std::vector<std::int64_t> partialSetIds;
    /// The SoundSource(s) aggregating the partial's set(s), distinct.
    std::vector<std::int64_t> soundSourceIds;
    /// The (first) source's name.
    std::optional<std::string> soundSourceName;
    /// Per spatial kind, in kSpatialKinds order: the entry's point(s) on its
    /// source's timeline. Not walked: the source's walk is shared.
    std::array<std::vector<SpatialRecord>, 3> spatial;
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

/// Everything gathered for one SoundSource's spatial timelines in a frame,
/// before validation: one walk per kind, shared by every partial of the
/// source, from the earliest entry point any of them has.
struct SourceRecord {
    std::int64_t soundSourceId = 0; ///< SoundSource.id.
    /// "SoundSource <name> (id <id>)", for messages.
    std::string description;
    /// Per spatial kind: the earliest entry point and, after the walk, its
    /// successors in order.
    std::array<std::vector<SpatialRecord>, 3> points;
    /// The failure found during the gather, if any. Every partial of the
    /// source fails with it.
    std::optional<std::string> failure;
};

/// Everything the gather produced for one frame.
struct GatheredFrame {
    /// One record per FrameEntry, ordered by partial id.
    std::vector<EntryRecord> entries;
    /// One record per SoundSource of the entries, by SoundSource.id.
    std::map<std::int64_t, SourceRecord> sources;
};

/// Reads the spatial point columns of a row.
///
/// @param row   A row of a kReadEntrySpatial or kNextSpatial query.
/// @param from  Index of the point's id column.
/// @return The record, with hasSuccessor false.
/// @throws EngineDbError  If a column is of the wrong type.
SpatialRecord spatialFromRow(const DbRow& row, std::size_t from) {
    return SpatialRecord{
        .id = requireInt64(row.at(from)),
        .time = optionalDouble(row.at(from + 1)),
        .value = optionalDouble(row.at(from + 2)),
        .curveType = optionalInt64(row.at(from + 3)),
        .hasSuccessor = false,
        .hasPredecessor = false,
    };
}

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
/// @tparam Record     SegmentRecord, PointRecord, CompensationRecord or
///                    SpatialRecord.
/// @tparam Owner      What holds the lists and a `failure`: EntryRecord, or
///                    SourceRecord for the spatial timelines.
/// @param[in,out] db  Database to read.
/// @param cypher      kNextSegments, kNextPoints, kNextCompensation or one of
///                    kNextSpatial.
/// @param frameEnd    The frame's end, source time.
/// @param[in,out] entries  The owners. Each one's list grows by the nodes
///                         walked; `hasSuccessor` is set on every node found
///                         to have one.
/// @param list        Selects the list on an owner.
/// @param startOf     The start time of a record, or nullopt when NULL.
/// @param fromRow     Builds a record from a result row, from column 1.
/// @param what        The list's name, for failure messages.
/// @throws EngineDbError  If a query fails.
template <typename Record, typename Owner, typename List, typename StartOf, typename FromRow>
void walkFrontier(EngineDb& db, std::string_view cypher, SampleTime frameEnd,
    std::vector<Owner>& entries, List list, StartOf startOf, FromRow fromRow,
    std::string_view what) {
    // Which entry each frontier node belongs to, and the nodes each entry
    // has seen (the cycle check).
    std::unordered_map<std::int64_t, std::size_t> owner;
    std::vector<std::unordered_set<std::int64_t>> seen(entries.size());
    std::vector<std::int64_t> frontier;

    const auto stillWalking = [&](std::size_t i) {
        Owner& entry = entries[i];
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
            Owner& entry = entries[i];
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

/// Marks every entry that leads to other than exactly one node of each list
/// type as failed, so that the walks leave it alone and validation reports
/// it.
///
/// @param[in,out] entries  The entries as read. `failure` is set on each
///                         miscounted one that has none yet.
void markMiscountedEntries(std::vector<EntryRecord>& entries) {
    for (EntryRecord& entry : entries) {
        if (entry.segments.size() != 1 || entry.points.size() != 1 ||
            entry.compensation.size() != 1) {
            entry.failure = "its FrameEntry leads to " + std::to_string(entry.segments.size()) +
                            " segment(s), " + std::to_string(entry.points.size()) +
                            " amplitude point(s) and " + std::to_string(entry.compensation.size()) +
                            " phase compensation segment(s); exactly one of each is required, "
                            "so it was not precomputed, or not for this frame";
        }
        for (std::size_t k = 0; k < kSpatialKinds.size() && !entry.failure.has_value(); ++k) {
            if (entry.spatial.at(k).size() != 1) {
                entry.failure = "its FrameEntry leads to " +
                                std::to_string(entry.spatial.at(k).size()) + " " +
                                std::string{spatialTimelineName(kSpatialKinds.at(k))} +
                                " point(s); exactly one is required, so it was not "
                                "precomputed, or not for this frame";
            }
        }
    }
}

/// Gathers the spatial timelines of the sources a frame's entries lead to
/// (§6.3).
///
/// A source's entry points depend on the frame alone, so every partial of
/// the source leads to the same three; a disagreement is a stale entry and
/// fails the source. Each source's timelines are then walked once, however
/// many partials it has, from those points. Entries that already failed, or
/// whose set is not in exactly one source, contribute nothing.
///
/// @param[in,out] db  Database to read.
/// @param entries     The frame's entries, walked.
/// @param frameEnd    The frame's end, source time.
/// @return One record per source, by SoundSource.id.
/// @throws EngineDbError  If a query fails.
std::map<std::int64_t, SourceRecord> gatherSources(
    EngineDb& db, const std::vector<EntryRecord>& entries, SampleTime frameEnd) {
    std::map<std::int64_t, SourceRecord> bySource;
    for (const EntryRecord& entry : entries) {
        if (entry.failure.has_value() || entry.soundSourceIds.size() != 1) {
            continue;
        }
        const std::int64_t sourceId = entry.soundSourceIds.front();
        auto [found, added] = bySource.try_emplace(sourceId);
        SourceRecord& source = found->second;
        if (added) {
            source.soundSourceId = sourceId;
            source.description = "SoundSource " + entry.soundSourceName.value_or("(unnamed)") +
                                 " (id " + std::to_string(sourceId) + ")";
        }
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const SpatialRecord& candidate = entry.spatial.at(k).front();
            std::vector<SpatialRecord>& start = source.points.at(k);
            if (start.empty()) {
                start.assign(1, candidate);
            } else if (start.front().id != candidate.id) {
                source.failure = "its partials' FrameEntry nodes lead to different " +
                                 std::string{spatialTimelineName(kSpatialKinds.at(k))} +
                                 " points (ids " + std::to_string(start.front().id) + " and " +
                                 std::to_string(candidate.id) +
                                 "); every partial of a source must share them, so the entries "
                                 "are stale";
            }
        }
    }
    std::vector<SourceRecord> sources;
    sources.reserve(bySource.size());
    for (auto& [id, source] : bySource) {
        sources.push_back(std::move(source));
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        walkFrontier<SpatialRecord>(
            db, kNextSpatial.at(k), frameEnd, sources,
            [k](SourceRecord& r) -> std::vector<SpatialRecord>& { return r.points.at(k); },
            [](const SpatialRecord& p) { return p.time; }, spatialFromRow,
            spatialTimelineName(kSpatialKinds.at(k)));
    }

    std::map<std::int64_t, SourceRecord> result;
    for (SourceRecord& source : sources) {
        const std::int64_t id = source.soundSourceId;
        result.emplace(id, std::move(source));
    }
    return result;
}

/// Gathers everything frame `frame` needs, unvalidated (§6.3).
///
/// @param[in,out] db  Database to read. Not modified.
/// @param frame       The frame.
/// @param size        Frame size.
/// @return One record per FrameEntry of the frame, ordered by partial id,
///         and one per SoundSource they lead to. Empty when there is no
///         Frame node or it has no entries.
/// @throws EngineDbError  If a query fails or returns a value of an
///         unexpected type.
GatheredFrame gatherFrame(EngineDb& db, FrameNumber frame, FrameSize size) {
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
                .soundSourceIds = {},
                .soundSourceName = std::nullopt,
                .spatial = {},
                .segments = {},
                .entrySegmentHasPredecessor = false,
                .points = {},
                .compensation = {},
                .failure = std::nullopt});
        }
        EntryRecord& entry = entries[found->second];
        if (const std::optional<std::int64_t> setId = optionalInt64(row.at(3))) {
            if (std::ranges::find(entry.partialSetIds, *setId) == entry.partialSetIds.end()) {
                entry.partialSetIds.push_back(*setId);
            }
        }
        if (const std::optional<std::int64_t> sourceId = optionalInt64(row.at(4))) {
            if (std::ranges::find(entry.soundSourceIds, *sourceId) == entry.soundSourceIds.end()) {
                entry.soundSourceIds.push_back(*sourceId);
                if (entry.soundSourceIds.size() == 1) {
                    entry.soundSourceName = optionalString(row.at(5));
                }
            }
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
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        for (const DbRow& row : db.query(kReadEntrySpatial.at(k), byFrame)) {
            SpatialRecord point = spatialFromRow(row, 1);
            point.hasPredecessor = optionalInt64(row.at(5)).has_value();
            entries[byEntry.at(requireInt64(row.at(0)))].spatial.at(k).push_back(point);
        }
    }

    markMiscountedEntries(entries);

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

    std::map<std::int64_t, SourceRecord> sources = gatherSources(db, entries, frameEnd);
    return GatheredFrame{.entries = std::move(entries), .sources = std::move(sources)};
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
    SpatialKind kind, std::span<const SpatialRecord> records) {
    const std::string timeline{spatialTimelineName(kind)};
    std::vector<std::pair<SampleTime, double>> points;
    points.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const SpatialRecord& record = records[i];
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
void validateSourceEntryPoint(SpatialKind kind, std::span<const SpatialRecord> walk,
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
SourceData validateSource(const SourceRecord& source, SampleTime fs) {
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
void validateSourceEntry(const EntryRecord& entry, const PartialData& data,
    const SourceRecord& source, const SourceData& validated, SampleTime fs) {
    const SampleTime entryTime = std::max(data.segments.front().startTime, fs);
    const std::array<std::vector<SampleTime>, 3> times{
        timesOf(validated.azimuth), timesOf(validated.elevation), timesOf(validated.distance)};
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        const std::string timeline{spatialTimelineName(kSpatialKinds.at(k))};
        const std::vector<SpatialRecord>& walk = source.points.at(k);
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
/// @param[in,out] db  Database to read.
/// @return The sample rate, frame size and channel mode.
/// @throws RenderingError  If there is not exactly one EngineConfig, its
///         sampleRate is NULL or not positive, its frame size is unusable,
///         or its channelMode is NULL or not 0 to 7.
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
    for (const EntryRecord& entry : gathered.entries) {
        try {
            if (entry.partialId.has_value() && !seenPartials.insert(*entry.partialId).second) {
                throw PartialRenderError{"it has more than one FrameEntry for this frame"};
            }
            PartialData data = validateEntry(entry, frame, size);
            if (const auto failure = sourceFailures.find(data.soundSourceId);
                failure != sourceFailures.end()) {
                throw PartialRenderError{failure->second};
            }
            const SourceRecord& source = gathered.sources.at(data.soundSourceId);
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

/// Scratch buffers reused between sets, sources and frames, so that a frame
/// allocates nothing once the first has been rendered.
struct Buffers {
    /// The frame, interleaved by channel.
    std::vector<SampleValue> frame;
    /// One source's mono sum.
    std::vector<SampleValue> source;
    /// One set's mono samples.
    std::vector<SampleValue> set;
};

/// Renders a frame's sources into the frame buffer and writes the files (§6.4,
/// §7, §8.5, §5).
///
/// Each source's sets are rendered to mono and summed; the sum is spread
/// over the output channels by the source's position, once.
///
/// @param validated   The frame's validated partials and sources.
/// @param config      Sample rate, frame size and channel mode.
/// @param rollOff     Band edges for the run.
/// @param outputDir   Where files go.
/// @param perSetWav   Whether to write per-set files: mono, before the
///                    spatial stage.
/// @param[in,out] outcome  Gains the source, set and partial counts, the
///                         peak over every channel, the file and the set
///                         files.
/// @param[in,out] buffers  Scratch, resized and zeroed here.
/// @throws WavWriteError  If a file cannot be written.
/// @throws std::invalid_argument  If a plan's precondition fails, which
///         validation rules out.
void renderSources(const ValidatedFrame& validated, const Config& config, const RollOff& rollOff,
    const std::filesystem::path& outputDir, bool perSetWav, FrameOutcome& outcome,
    Buffers& buffers) {
    const auto frameSize = static_cast<std::size_t>(config.frameSize.value());
    const std::size_t channels = channelCount(config.channelMode);
    buffers.frame.assign(frameSize * channels, 0);
    buffers.source.resize(frameSize);
    buffers.set.resize(frameSize);
    for (const auto& [sourceId, sets] : validated.bySource) {
        std::ranges::fill(buffers.source, 0);
        for (const auto& [setId, partials] : sets) {
            const PartialSetPlan plan =
                planPartialSet(setId, partials, outcome.frame, config.frameSize);
            std::ranges::fill(buffers.set, 0);
            renderPartialSet(plan, rollOff, buffers.set);
            if (perSetWav) {
                const std::filesystem::path setFile = outputDir / setFileName(outcome.frame, setId);
                writeFloatWav(setFile, config.sampleRate, 1, buffers.set);
                outcome.setFiles.emplace_back(setId, setFile);
            }
            addInto(buffers.source, buffers.set);
            ++outcome.setCount;
            outcome.partialCount += partials.size();
        }
        const SoundSourcePlan plan =
            planSoundSource(validated.sources.at(sourceId), outcome.frame, config.frameSize);
        spatialiseSource(plan, config.channelMode, buffers.source, buffers.frame);
        ++outcome.sourceCount;
    }
    outcome.peak = peakOf(buffers.frame);

    const std::filesystem::path file = outputDir / frameFileName(outcome.frame);
    writeFloatWav(file, config.sampleRate, static_cast<std::int32_t>(channels), buffers.frame);
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
/// @param[in,out] buffers  Scratch, reused between frames.
/// @return The frame's outcome.
/// @throws Nothing: every failure is recorded in the outcome.
FrameOutcome renderOneFrame(EngineDb& db, const Config& config, const RollOff& rollOff,
    FrameNumber frame, const std::filesystem::path& outputDir, bool perSetWav,
    std::unordered_set<std::int64_t>& known, Buffers& buffers) {
    FrameOutcome outcome{.frame = frame,
        .error = std::nullopt,
        .newFailures = {},
        .sourceCount = 0,
        .setCount = 0,
        .partialCount = 0,
        .peak = 0,
        .file = {},
        .setFiles = {}};

    // Gather and validate (§6.3, §9). A database failure fails the frame.
    GatheredFrame gathered;
    try {
        gathered = gatherFrame(db, frame, config.frameSize);
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
        renderSources(validated, config, rollOff, outputDir, perSetWav, outcome, buffers);
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
    report.channelMode = config.channelMode;

    std::unordered_set<std::int64_t> known;
    Buffers buffers;
    for (const FrameRequest& requested : request.frames) {
        report.frames.push_back(renderOneFrame(db, config, rollOff,
            FrameNumber{requested.frameNumber}, outputDir, perSetWav, known, buffers));
    }
    return report;
}

} // namespace rainbohz::rendering_engine
