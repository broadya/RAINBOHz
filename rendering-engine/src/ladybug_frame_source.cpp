// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief The Ladybug backend's FrameSource: rendering's gather as fixed
/// Cypher queries.
///
/// The gather runs the entry query, one list-node query per list type, and
/// the frontier walk that follows NEXT_POINT one hop per query for every
/// partial still inside the frame, and for every SoundSource's spatial
/// timelines once. The queries are the ones rendering.cpp ran before the
/// stores were introduced (ADR 0028), unchanged. Section numbers refer to
/// docs/rendering.md.
///
/// @see frame_gather.hpp, ladybug_store.hpp

namespace rainbohz::rendering_engine {

namespace {

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

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

/// Reads the spatial point columns of a row.
///
/// @param row   A row of a kReadEntrySpatial or kNextSpatial query.
/// @param from  Index of the point's id column.
/// @return The record, with hasSuccessor false.
/// @throws EngineDbError  If a column is of the wrong type.
GatheredSpatialPoint spatialFromRow(const DbRow& row, std::size_t from) {
    return GatheredSpatialPoint{
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
GatheredSegment segmentFromRow(const DbRow& row, std::size_t from) {
    return GatheredSegment{
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
GatheredPoint pointFromRow(const DbRow& row, std::size_t from) {
    return GatheredPoint{
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
GatheredCompensation compensationFromRow(const DbRow& row, std::size_t from) {
    return GatheredCompensation{
        .id = requireInt64(row.at(from)),
        .startTime = optionalDouble(row.at(from + 1)),
        .endTime = optionalDouble(row.at(from + 2)),
        .startDelta = optionalDouble(row.at(from + 3)),
        .endDelta = optionalDouble(row.at(from + 4)),
        .hasSuccessor = false,
    };
}

/// Follows NEXT_POINT one hop per query from every entry's last node, until
/// each has a node starting at or after `frameEnd`, or its list ends.
///
/// One query per hop serves every entry still walking. A walk that revisits
/// a node records a failure on its entry, as walkChain does; a node whose
/// time is NULL ends its walk, and validation reports it.
///
/// @tparam Record     GatheredSegment, GatheredPoint, GatheredCompensation or
///                    GatheredSpatialPoint.
/// @tparam Owner      What holds the lists and a `failure`: GatheredEntry, or
///                    GatheredSource for the spatial timelines.
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

/// Gathers the spatial timelines of the sources a frame's entries lead to
/// (§6.3): groups the entries by source (groupSources()), then walks each
/// source's timelines once, however many partials it has, from its entry
/// points.
///
/// @param[in,out] db  Database to read.
/// @param entries     The frame's entries, walked.
/// @param frameEnd    The frame's end, source time.
/// @return One record per source, by SoundSource.id.
/// @throws EngineDbError  If a query fails.
std::map<std::int64_t, GatheredSource> gatherSources(
    EngineDb& db, const std::vector<GatheredEntry>& entries, SampleTime frameEnd) {
    std::vector<GatheredSource> sources = groupSources(entries);
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        walkFrontier<GatheredSpatialPoint>(
            db, kNextSpatial.at(k), frameEnd, sources,
            [k](GatheredSource& r) -> std::vector<GatheredSpatialPoint>& { return r.points.at(k); },
            [](const GatheredSpatialPoint& p) { return p.time; }, spatialFromRow,
            spatialTimelineName(kSpatialKinds.at(k)));
    }

    std::map<std::int64_t, GatheredSource> result;
    for (GatheredSource& source : sources) {
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

    std::vector<GatheredEntry> entries;
    std::unordered_map<std::int64_t, std::size_t> byEntry;
    for (const DbRow& row : db.query(kReadEntries, byFrame)) {
        const std::int64_t entryId = requireInt64(row.at(0));
        auto found = byEntry.find(entryId);
        if (found == byEntry.end()) {
            found = byEntry.emplace(entryId, entries.size()).first;
            entries.push_back(GatheredEntry{.entryId = entryId,
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
        GatheredEntry& entry = entries[found->second];
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
        GatheredEntry& entry = entries[byEntry.at(requireInt64(row.at(0)))];
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
            GatheredSpatialPoint point = spatialFromRow(row, 1);
            point.hasPredecessor = optionalInt64(row.at(5)).has_value();
            entries[byEntry.at(requireInt64(row.at(0)))].spatial.at(k).push_back(point);
        }
    }

    markMiscountedEntries(entries);

    walkFrontier<GatheredSegment>(
        db, kNextSegments, frameEnd, entries,
        [](GatheredEntry& e) -> std::vector<GatheredSegment>& { return e.segments; },
        [](const GatheredSegment& s) { return s.startTime; }, segmentFromRow,
        "PartialSegmentPhysicalTimeline");
    walkFrontier<GatheredPoint>(
        db, kNextPoints, frameEnd, entries,
        [](GatheredEntry& e) -> std::vector<GatheredPoint>& { return e.points; },
        [](const GatheredPoint& p) { return p.time; }, pointFromRow, "AmplitudePhysicalTimeline");
    walkFrontier<GatheredCompensation>(
        db, kNextCompensation, frameEnd, entries,
        [](GatheredEntry& e) -> std::vector<GatheredCompensation>& { return e.compensation; },
        [](const GatheredCompensation& c) { return c.startTime; }, compensationFromRow,
        "PartialPhaseCompensationSegment list");

    std::map<std::int64_t, GatheredSource> sources = gatherSources(db, entries, frameEnd);
    return GatheredFrame{.entries = std::move(entries), .sources = std::move(sources)};
}

/// rendering's reads over a Ladybug engine-db.
class LadybugFrameSource final : public FrameSource {
public:
    /// Serves rendering from `db`.
    ///
    /// @param[in,out] db  Database holding the precomputed score. It must
    ///                    outlive the source, which keeps a pointer to it.
    explicit LadybugFrameSource(EngineDb& db) : db_{&db} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return readLadybugEngineConfigs(*db_);
    }

    GatheredFrame gatherFrame(FrameNumber frame, FrameSize size) override {
        return rendering_engine::gatherFrame(*db_, frame, size);
    }

private:
    EngineDb* db_; ///< The database; not owned.
};

} // namespace

std::unique_ptr<FrameSource> makeLadybugFrameSource(EngineDb& db) {
    return std::make_unique<LadybugFrameSource>(db);
}

} // namespace rainbohz::rendering_engine
