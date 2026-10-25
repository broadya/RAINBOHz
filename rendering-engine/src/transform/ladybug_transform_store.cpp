// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/ladybug_transform_store.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/transform/transform_store.hpp"
#include "rainbohz/rendering_engine/write_batch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief The Ladybug backend's TransformStore: the transform's bulk read as
/// fixed Cypher queries, and its writes as WriteBatch units.
///
/// The queries and the write statements are the ones transform.cpp ran
/// before the stores were introduced (ADR 0028), unchanged. Section numbers
/// in comments refer to docs/transform.md.
///
/// @see transform_store.hpp, ladybug_transform_store.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

/// A Score, its ramp shape, and the TempoMap that defines its time zero, if
/// it has one. NEXT_POINT is ONE_ONE per pair, so there is at most one row
/// per Score.
constexpr std::string_view kReadScores =
    "MATCH (s:Score) OPTIONAL MATCH (s)-[:NEXT_POINT]->(m:TempoMap) "
    "RETURN s.id, m.id, s.tempo_ramp_physical_time ORDER BY s.id;";

/// Every TempoMap's own fields.
constexpr std::string_view kReadTempoMaps =
    "MATCH (m:TempoMap) RETURN m.id, m.name, m.ticks_per_beat ORDER BY m.id;";

/// Each TempoMap's point-list head, by TempoMap.id.
constexpr std::string_view kReadTempoMapHeads =
    "MATCH (m:TempoMap) OPTIONAL MATCH (m)-[:NEXT_POINT]->(p:TempoMapPoint) "
    "RETURN m.id, p.id;";

/// Reads every TempoMapPoint's fields.
constexpr std::string_view kReadTempoMapPoints =
    "MATCH (p:TempoMapPoint) RETURN p.id, p.position, p.start_tempo, p.hold;";

/// Reads the NEXT_POINT links between TempoMapPoints.
constexpr std::string_view kReadTempoMapLinks =
    "MATCH (a:TempoMapPoint)-[:NEXT_POINT]->(b:TempoMapPoint) RETURN a.id, b.id;";

/// Reads every TEMPO_MAP_SYNCHRONIZATION_POINT edge, child to parent, with
/// its two sync points. Two edges between the same maps are ordered by where
/// they sit in the parent and then in the child, a NULL after every value; the
/// order is stated in full so that both backends give it (ADR 0028).
constexpr std::string_view kReadSyncEdges =
    "MATCH (c:TempoMap)-[r:TEMPO_MAP_SYNCHRONIZATION_POINT]->(p:TempoMap) "
    "RETURN c.id, p.id, r.sync_point_child, r.sync_point_parent "
    "ORDER BY c.id, p.id, r.sync_point_parent IS NULL, r.sync_point_parent, "
    "r.sync_point_child IS NULL, r.sync_point_child;";

/// Every PartialSet with a musical timeline, with the head of the timeline's
/// point list, or NULL when it has no points.
constexpr std::string_view kReadMusicalSets =
    "MATCH (ps:PartialSet)-[:HAS_MUSICAL_TIMELINE]->(t:PartialSetMusicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:PartialSetMusicalTimelinePoint) "
    "RETURN ps.id, ps.name, t.id, p.id ORDER BY ps.id;";

/// Each PartialSet's owning Score, reached through SoundEntity and SoundSource.
constexpr std::string_view kReadSetScores =
    "MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(:SoundSource)"
    "-[:HAS_PARTIALSET]->(ps:PartialSet) RETURN ps.id, s.id;";

/// Each PartialSet's member Partials.
constexpr std::string_view kReadSetMembers =
    "MATCH (ps:PartialSet)-[:HAS_PARTIAL]->(p:Partial) RETURN ps.id, p.id, p.name ORDER BY p.id;";

/// Reads every PartialSetMusicalTimelinePoint's curve fields, and the
/// TempoMap and sync fields of its POINT_AT, if it has one.
constexpr std::string_view kReadMusicalPoints =
    "MATCH (p:PartialSetMusicalTimelinePoint) OPTIONAL MATCH (p)-[a:POINT_AT]->(m:TempoMap) "
    "RETURN p.id, p.frequency_curve_type, p.frequency_curve_amount, "
    "p.amplitude_curve_type, p.amplitude_curve_amount, m.id, a.sync_point, a.offset_seconds;";

/// Reads the NEXT_POINT links between PartialSetMusicalTimelinePoints.
constexpr std::string_view kReadMusicalLinks =
    "MATCH (a:PartialSetMusicalTimelinePoint)-[:NEXT_POINT]->(b:PartialSetMusicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// The states a timeline point defines, each with the partial it belongs to.
constexpr std::string_view kReadStates =
    "MATCH (p:PartialSetMusicalTimelinePoint)-[:POINT_STATE]->(:PartialStateSet)"
    "<-[:MEMBER_OF_SET_MANY_ONE]-(st:PartialState)-[:STATE_OF]->(partial:Partial) "
    "RETURN p.id, st.id, partial.id, st.frequency, st.amplitude, st.phase, st.phase_free_run;";

/// Output from an earlier run, or physical-time input, which is replaced.
constexpr std::string_view kReadExistingFrequency =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint) "
    "RETURN p.id, t.id, f.id;";

/// Reads the NEXT_POINT links between FrequencyPhysicalTimelinePoints.
constexpr std::string_view kReadFrequencyLinks =
    "MATCH (a:FrequencyPhysicalTimelinePoint)-[:NEXT_POINT]->(b:FrequencyPhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// A partial's existing AmplitudePhysicalTimeline, which is replaced.
constexpr std::string_view kReadExistingAmplitude =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint) "
    "RETURN p.id, t.id, a.id;";

/// Reads the NEXT_POINT links between AmplitudePhysicalTimelinePoints.
constexpr std::string_view kReadAmplitudeLinks =
    "MATCH (a:AmplitudePhysicalTimelinePoint)-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// A partial's existing PhasePhysicalTimeline, which is replaced.
constexpr std::string_view kReadExistingPhase =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint) "
    "RETURN p.id, t.id, f.id;";

/// Reads the NEXT_POINT links between PhasePhysicalTimelinePoints.
constexpr std::string_view kReadPhaseLinks =
    "MATCH (a:PhasePhysicalTimelinePoint)-[:NEXT_POINT]->(b:PhasePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// Every SoundSource with a spatial musical timeline, with the head of the
/// timeline's point list, or NULL when it has no points (§6.8).
constexpr std::string_view kReadSpatialSources =
    "MATCH (ss:SoundSource)-[:HAS_MUSICAL_TIMELINE]->(t:SpatialPositionMusicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:SpatialPositionMusicalTimelinePoint) "
    "RETURN ss.id, ss.name, t.id, p.id ORDER BY ss.id;";

/// Each SoundSource's owning Score, reached through SoundEntity.
constexpr std::string_view kReadSourceScores =
    "MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(ss:SoundSource) "
    "RETURN ss.id, s.id;";

/// Reads every SpatialPositionMusicalTimelinePoint's curve fields, and the
/// TempoMap and sync fields of its POINT_AT, if it has one.
constexpr std::string_view kReadSpatialMusicalPoints =
    "MATCH (p:SpatialPositionMusicalTimelinePoint) OPTIONAL MATCH (p)-[a:POINT_AT]->(m:TempoMap) "
    "RETURN p.id, p.azimuth_curve_type, p.azimuth_curve_amount, p.elevation_curve_type, "
    "p.elevation_curve_amount, p.distance_curve_type, p.distance_curve_amount, m.id, "
    "a.sync_point, a.offset_seconds;";

/// Reads the NEXT_POINT links between SpatialPositionMusicalTimelinePoints.
constexpr std::string_view kReadSpatialMusicalLinks =
    "MATCH (a:SpatialPositionMusicalTimelinePoint)-[:NEXT_POINT]->"
    "(b:SpatialPositionMusicalTimelinePoint) RETURN a.id, b.id;";

// POINT_STATE is MANY_ONE, so a point has at most one state.
/// Reads each timeline point's SpatialPositionState, if it has one.
constexpr std::string_view kReadSpatialStates =
    "MATCH (p:SpatialPositionMusicalTimelinePoint)-[:POINT_STATE]->(st:SpatialPositionState) "
    "RETURN p.id, st.id, st.azimuth, st.elevation, st.distance;";

/// The physical spatial timelines, one statement of each shape per kind, in
/// kSpatialKinds order. As for partials, existing ones are replaced.
constexpr std::array<std::string_view, 3> kReadExistingSpatial{
    "MATCH (ss:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:AzimuthPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:AzimuthPhysicalTimelinePoint) RETURN ss.id, t.id, p.id;",
    "MATCH (ss:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:ElevationPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:ElevationPhysicalTimelinePoint) "
    "RETURN ss.id, t.id, p.id;",
    "MATCH (ss:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:DistancePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:DistancePhysicalTimelinePoint) "
    "RETURN ss.id, t.id, p.id;",
};

/// Reads the NEXT_POINT links between physical spatial timeline points, one
/// statement per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kReadSpatialPhysicalLinks{
    "MATCH (a:AzimuthPhysicalTimelinePoint)-[:NEXT_POINT]->(b:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:ElevationPhysicalTimelinePoint)-[:NEXT_POINT]->(b:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:DistancePhysicalTimelinePoint)-[:NEXT_POINT]->(b:DistancePhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
};

/// The node tables of one kind of physical timeline, and what owns it.
struct TimelineTables {
    std::string_view owner;    ///< Node table of the owner: Partial or SoundSource.
    std::string_view timeline; ///< The timeline's node table.
    std::string_view point;    ///< Its points' node table.
    std::string_view value;    ///< The point's value column, which names its curve columns.
    bool floatValue = false;   ///< Whether the value column is FLOAT rather than DOUBLE.
};

/// The six kinds of physical timeline the transform writes: frequency,
/// amplitude and phase for a partial, then azimuth, elevation and distance
/// for a source, in kSpatialKinds order.
constexpr std::array<TimelineTables, 6> kTimelineTables{{
    {.owner = "Partial",
        .timeline = "FrequencyPhysicalTimeline",
        .point = "FrequencyPhysicalTimelinePoint",
        .value = "frequency",
        .floatValue = false},
    {.owner = "Partial",
        .timeline = "AmplitudePhysicalTimeline",
        .point = "AmplitudePhysicalTimelinePoint",
        .value = "amplitude",
        .floatValue = true},
    {.owner = "Partial",
        .timeline = "PhasePhysicalTimeline",
        .point = "PhasePhysicalTimelinePoint",
        .value = "phase",
        .floatValue = false},
    {.owner = "SoundSource",
        .timeline = "AzimuthPhysicalTimeline",
        .point = "AzimuthPhysicalTimelinePoint",
        .value = "azimuth",
        .floatValue = false},
    {.owner = "SoundSource",
        .timeline = "ElevationPhysicalTimeline",
        .point = "ElevationPhysicalTimelinePoint",
        .value = "elevation",
        .floatValue = false},
    {.owner = "SoundSource",
        .timeline = "DistancePhysicalTimeline",
        .point = "DistancePhysicalTimelinePoint",
        .value = "distance",
        .floatValue = true},
}};

/// Index into kTimelineTables of the frequency timeline.
constexpr std::size_t kFrequencyTimeline = 0;
/// Index of the amplitude timeline.
constexpr std::size_t kAmplitudeTimeline = 1;
/// Index of the phase timeline.
constexpr std::size_t kPhaseTimeline = 2;
/// Index of the first spatial timeline; the three follow in kSpatialKinds order.
constexpr std::size_t kFirstSpatialTimeline = 3;

/// The batch operations of one kind of physical timeline.
struct TimelineWrites {
    WriteOperation deletePoints;   ///< Points of an earlier run, by id.
    WriteOperation deleteTimeline; ///< The timeline of an earlier run, by id.
    WriteOperation timeline;       ///< A new timeline node.
    WriteOperation point;          ///< A new point. For phase: one with a phase.
    WriteOperation owns;           ///< Owner → timeline, HAS_PHYSICAL_TIMELINE.
    WriteOperation first;          ///< Timeline → first point, NEXT_POINT.
    WriteOperation next;           ///< Point → point, NEXT_POINT.
};

/// The batch operations the transform writes through, registered once per
/// run.
struct Writes {
    /// Per kind of timeline, in kTimelineTables order.
    std::array<TimelineWrites, 6> timelines;

    /// A phase point whose phase is NULL: free-running up to here. A row
    /// cannot hold a NULL, so it has its own insert, which leaves the
    /// property unset.
    WriteOperation freePhasePoint;
};

/// Registers every operation the transform writes through.
///
/// The value columns differ between point tables (frequency, azimuth and
/// elevation DOUBLE; amplitude and distance FLOAT), and the curve columns are
/// INT8 and FLOAT, so the row values are cast to them explicitly. Phase
/// points have no curve, and their phase may be NULL.
///
/// @param[in,out] batch  The batch to register them with.
/// @return The operations.
Writes registerWrites(WriteBatch& batch) {
    using enum DbColumnType;
    Writes writes{};
    for (std::size_t k = 0; k < kTimelineTables.size(); ++k) {
        const TimelineTables& tables = kTimelineTables.at(k);
        TimelineWrites& kind = writes.timelines.at(k);
        const std::string point{tables.point};
        const std::string column{tables.value};
        kind.deletePoints = batch.addDeleteById(tables.point);
        kind.deleteTimeline = batch.addDeleteById(tables.timeline);
        kind.timeline = batch.addNodeInsert("CREATE (n:" + std::string{tables.timeline} + ")", {});
        if (k == kPhaseTimeline) {
            kind.point =
                batch.addNodeInsert("CREATE (n:" + point + " {time: r.time, phase: r.value})",
                    {{"time", Double}, {"value", Double}});
        } else {
            std::string create = "CREATE (n:" + point + " {time: r.time, ";
            create += column;
            create += tables.floatValue ? ": CAST(r.value AS FLOAT), " : ": r.value, ";
            create += column;
            create += "_curve_type: CAST(r.curveType AS INT8), ";
            create += column;
            create += "_curve_amount: CAST(r.curveAmount AS FLOAT)})";
            kind.point =
                batch.addNodeInsert(create, {{"time", Double}, {"value", Double},
                                                {"curveType", Int64}, {"curveAmount", Double}});
        }
        kind.owns = batch.addEdgeInsert("HAS_PHYSICAL_TIMELINE", tables.owner, tables.timeline);
        kind.first = batch.addEdgeInsert("NEXT_POINT", tables.timeline, tables.point);
        kind.next = batch.addEdgeInsert("NEXT_POINT", tables.point, tables.point);
    }
    writes.freePhasePoint = batch.addNodeInsert(
        "CREATE (n:PhasePhysicalTimelinePoint {time: r.time})", {{"time", Double}});
    return writes;
}

/// Stages the deletion of an owner's existing timelines of one kind.
///
/// @param[in,out] batch  Batch to stage into, in the owner's unit.
/// @param kind           The kind's operations.
/// @param stale          The timelines, each with its point ids.
void stageDeletes(WriteBatch& batch, const TimelineWrites& kind, const StaleTimelines& stale) {
    for (const auto& [timelineId, pointIds] : stale) {
        for (const std::int64_t pointId : pointIds) {
            batch.stageRow(kind.deletePoints, {pointId});
        }
        batch.stageRow(kind.deleteTimeline, {timelineId});
    }
}

/// Stages a new timeline for `ownerId`.
///
/// @param[in,out] batch  Batch to stage into, in the owner's unit.
/// @param kind           The kind's operations.
/// @param ownerId        The partial or source the timeline belongs to.
/// @return The timeline's handle, from which the first point is linked.
NodeHandle stageTimeline(WriteBatch& batch, const TimelineWrites& kind, std::int64_t ownerId) {
    const NodeHandle timeline = batch.stageNode(kind.timeline, {});
    batch.stageEdge(kind.owns, ownerId, timeline);
    return timeline;
}

/// Stages a new timeline and its chain of points, linked in order.
///
/// @param[in,out] batch  Batch to stage into, in the owner's unit.
/// @param kind           The kind's operations.
/// @param ownerId        The partial or source the timeline belongs to.
/// @param points         The points, in order, in physical units.
void stageChain(WriteBatch& batch, const TimelineWrites& kind, std::int64_t ownerId,
    std::span<const PhysicalPoint> points) {
    NodeHandle previous = stageTimeline(batch, kind, ownerId);
    bool first = true;
    for (const PhysicalPoint& point : points) {
        const NodeHandle node = batch.stageNode(
            kind.point, {point.time.value(), point.value, point.curveType, point.curveAmount});
        batch.stageEdge(first ? kind.first : kind.next, previous, node);
        previous = node;
        first = false;
    }
}

/// Reads an id-to-id link table from a two-column query.
///
/// @param db      Database to read.
/// @param cypher  A query returning (from id, to id) rows.
/// @return The links.
/// @throws EngineDbError  If the query fails or returns a NULL id.
IdMap readLinks(EngineDb& db, std::string_view cypher) {
    IdMap links;
    for (const DbRow& row : db.query(cypher)) {
        links.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    return links;
}

/// Reads every spatial input the run needs (§6.8): the sources with a
/// spatial musical timeline, their Scores, their points, POINT_AT edges and
/// states, and their existing physical timelines.
///
/// @param db              Database to read. Not modified.
/// @param[in,out] inputs  Receives the spatial members.
/// @throws EngineDbError  If any query fails or returns a value of an
///         unexpected type.
void readSpatialInputs(EngineDb& db, TransformInputs& inputs) {
    for (const DbRow& row : db.query(kReadSpatialSources)) {
        const std::int64_t sourceId = requireInt64(row.at(0));
        if (inputs.sources.empty() || inputs.sources.back().sourceId != sourceId) {
            inputs.sources.push_back(SpatialSourceRecord{
                .sourceId = sourceId, .name = optionalString(row.at(1)), .timelines = {}});
        }
        inputs.sources.back().timelines.emplace_back(
            requireInt64(row.at(2)), optionalInt64(row.at(3)));
    }
    for (const DbRow& row : db.query(kReadSourceScores)) {
        inputs.scoreOfSource.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    for (const DbRow& row : db.query(kReadSpatialMusicalPoints)) {
        SpatialMusicalPointRecord point{.id = requireInt64(row.at(0)),
            .curveType = {},
            .curveAmount = {},
            .at = PointAtRecord{.tempoMapId = optionalInt64(row.at(7)),
                .syncPoint = optionalInt64(row.at(8)),
                .offsetSeconds = optionalDouble(row.at(9))}};
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            point.curveType.at(k) = optionalInt64(row.at(1 + (2 * k)));
            point.curveAmount.at(k) = optionalDouble(row.at(2 + (2 * k)));
        }
        inputs.spatialPoints.emplace(point.id, point);
    }
    inputs.spatialMusicalNext = readLinks(db, kReadSpatialMusicalLinks);
    for (const DbRow& row : db.query(kReadSpatialStates)) {
        inputs.statesOfSpatialPoint[requireInt64(row.at(0))].push_back(SpatialStateRecord{
            .stateId = requireInt64(row.at(1)),
            .azimuth = optionalDouble(row.at(2)),
            .elevation = optionalDouble(row.at(3)),
            .distance = optionalDouble(row.at(4)),
        });
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        for (const DbRow& row : db.query(kReadExistingSpatial.at(k))) {
            inputs.existingSpatial.at(k)[requireInt64(row.at(0))].push_back(ExistingTimeline{
                .timelineId = requireInt64(row.at(1)), .head = optionalInt64(row.at(2))});
        }
        inputs.spatialPhysicalNext.at(k) = readLinks(db, kReadSpatialPhysicalLinks.at(k));
    }
}

/// Reads every input the run needs.
///
/// @param db  Database to read. Not modified.
/// @return The inputs.
/// @throws EngineDbError  If any query fails or returns a value of an
///         unexpected type.
TransformInputs readInputs(EngineDb& db) {
    TransformInputs inputs;

    for (const DbRow& row : db.query(kReadScores)) {
        inputs.scores.push_back(ScoreRecord{
            .scoreId = requireInt64(row.at(0)),
            .tempoMapId = optionalInt64(row.at(1)),
            .tempoRampPhysicalTime = optionalBool(row.at(2)),
        });
    }
    for (const DbRow& row : db.query(kReadTempoMaps)) {
        inputs.tempoMaps.push_back(TempoMapRecord{.id = requireInt64(row.at(0)),
            .name = optionalString(row.at(1)),
            .ticksPerBeat = optionalInt64(row.at(2))});
    }
    for (const DbRow& row : db.query(kReadTempoMapHeads)) {
        inputs.tempoMapHeads[requireInt64(row.at(0))].push_back(optionalInt64(row.at(1)));
    }
    for (const DbRow& row : db.query(kReadTempoMapPoints)) {
        const TempoMapPointRecord point{
            .id = requireInt64(row.at(0)),
            .position = optionalInt64(row.at(1)),
            .startTempo = optionalDouble(row.at(2)),
            .hold = optionalBool(row.at(3)),
        };
        inputs.tempoMapPoints.emplace(point.id, point);
    }
    inputs.tempoMapNext = readLinks(db, kReadTempoMapLinks);
    for (const DbRow& row : db.query(kReadSyncEdges)) {
        inputs.syncEdges.push_back(SyncEdgeRecord{.childId = requireInt64(row.at(0)),
            .parentId = requireInt64(row.at(1)),
            .syncPointChild = optionalInt64(row.at(2)),
            .syncPointParent = optionalInt64(row.at(3))});
    }

    for (const DbRow& row : db.query(kReadMusicalSets)) {
        const std::int64_t setId = requireInt64(row.at(0));
        if (inputs.sets.empty() || inputs.sets.back().setId != setId) {
            inputs.sets.push_back(MusicalSetRecord{
                .setId = setId, .name = optionalString(row.at(1)), .timelines = {}});
        }
        inputs.sets.back().timelines.emplace_back(
            requireInt64(row.at(2)), optionalInt64(row.at(3)));
    }
    for (const DbRow& row : db.query(kReadSetScores)) {
        inputs.scoreOfSet.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    for (const DbRow& row : db.query(kReadSetMembers)) {
        inputs.membersOfSet[requireInt64(row.at(0))].emplace_back(
            requireInt64(row.at(1)), optionalString(row.at(2)));
    }
    for (const DbRow& row : db.query(kReadMusicalPoints)) {
        const MusicalPointRecord point{
            .id = requireInt64(row.at(0)),
            .frequencyCurveType = optionalInt64(row.at(1)),
            .frequencyCurveAmount = optionalDouble(row.at(2)),
            .amplitudeCurveType = optionalInt64(row.at(3)),
            .amplitudeCurveAmount = optionalDouble(row.at(4)),
            .at = PointAtRecord{.tempoMapId = optionalInt64(row.at(5)),
                .syncPoint = optionalInt64(row.at(6)),
                .offsetSeconds = optionalDouble(row.at(7))},
        };
        inputs.musicalPoints.emplace(point.id, point);
    }
    inputs.musicalNext = readLinks(db, kReadMusicalLinks);
    for (const DbRow& row : db.query(kReadStates)) {
        inputs.statesOfPoint[requireInt64(row.at(0))].push_back(StateRecord{
            .stateId = requireInt64(row.at(1)),
            .partialId = requireInt64(row.at(2)),
            .frequency = optionalDouble(row.at(3)),
            .amplitude = optionalDouble(row.at(4)),
            .phase = optionalDouble(row.at(5)),
            .phaseFreeRun = optionalBool(row.at(6)),
        });
    }

    for (const DbRow& row : db.query(kReadExistingFrequency)) {
        inputs.existingFrequency[requireInt64(row.at(0))].push_back(ExistingTimeline{
            .timelineId = requireInt64(row.at(1)), .head = optionalInt64(row.at(2))});
    }
    inputs.frequencyNext = readLinks(db, kReadFrequencyLinks);
    for (const DbRow& row : db.query(kReadExistingAmplitude)) {
        inputs.existingAmplitude[requireInt64(row.at(0))].push_back(ExistingTimeline{
            .timelineId = requireInt64(row.at(1)), .head = optionalInt64(row.at(2))});
    }
    inputs.amplitudeNext = readLinks(db, kReadAmplitudeLinks);
    for (const DbRow& row : db.query(kReadExistingPhase)) {
        inputs.existingPhase[requireInt64(row.at(0))].push_back(ExistingTimeline{
            .timelineId = requireInt64(row.at(1)), .head = optionalInt64(row.at(2))});
    }
    inputs.phaseNext = readLinks(db, kReadPhaseLinks);

    readSpatialInputs(db, inputs);
    return inputs;
}

/// Stages the replacement of a partial's three physical timelines as one
/// unit of `batch` (§7).
///
/// @param[in,out] batch  Batch to stage into. Gains one unit, written whole
///                       or not at all when the batch flushes.
/// @param writes         The batch's registered operations.
/// @param output         What to write.
void stagePartial(WriteBatch& batch, const Writes& writes, const TransformedPartial& output) {
    batch.beginUnit();
    const TimelineWrites& phase = writes.timelines.at(kPhaseTimeline);
    stageDeletes(batch, writes.timelines.at(kFrequencyTimeline), output.staleFrequency);
    stageDeletes(batch, writes.timelines.at(kAmplitudeTimeline), output.staleAmplitude);
    stageDeletes(batch, phase, output.stalePhase);

    stageChain(batch, writes.timelines.at(kFrequencyTimeline), output.partialId, output.frequency);
    stageChain(batch, writes.timelines.at(kAmplitudeTimeline), output.partialId, output.amplitude);

    NodeHandle previous = stageTimeline(batch, phase, output.partialId);
    bool first = true;
    for (const PhasePoint& point : output.phase) {
        const NodeHandle node =
            point.phase.has_value()
                ? batch.stageNode(phase.point, {point.time.value(), point.phase->value()})
                : batch.stageNode(writes.freePhasePoint, {point.time.value()});
        batch.stageEdge(first ? phase.first : phase.next, previous, node);
        previous = node;
        first = false;
    }
}

/// Stages the replacement of a source's three spatial physical timelines as
/// one unit of `batch` (§7).
///
/// @param[in,out] batch  Batch to stage into. Gains one unit, written whole
///                       or not at all when the batch flushes.
/// @param writes         The batch's registered operations.
/// @param sourceId       The source.
/// @param stale          Per kind, the existing timelines to delete, each with
///                       its point ids.
/// @param points         Per kind, the points to write, in physical units.
void stageSource(WriteBatch& batch, const Writes& writes, std::int64_t sourceId,
    const std::array<StaleTimelines, 3>& stale, const SpatialTriple<PhysicalPoint>& points) {
    batch.beginUnit();
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        stageDeletes(batch, writes.timelines.at(kFirstSpatialTimeline + k), stale.at(k));
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        stageChain(batch, writes.timelines.at(kFirstSpatialTimeline + k), sourceId, points.at(k));
    }
}

/// The transform's reads and writes over a Ladybug engine-db.
class LadybugTransformStore final : public TransformStore {
public:
    /// Serves the transform from `db`.
    ///
    /// @param[in,out] db  Database holding the score. It must outlive the
    ///                    store, which keeps a pointer to it.
    explicit LadybugTransformStore(EngineDb& db) : db_{&db} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return readLadybugEngineConfigs(*db_);
    }

    TransformInputs readInputs() override { return transform::readInputs(*db_); }

    std::vector<std::optional<std::string>> writePartials(
        std::span<const TransformedPartial> partials) override {
        WriteBatch batch{*db_};
        const Writes writes = registerWrites(batch);
        for (const TransformedPartial& partial : partials) {
            stagePartial(batch, writes, partial);
        }
        return batch.flush();
    }

    std::vector<std::optional<std::string>> writeSources(
        std::span<const TransformedSource> sources) override {
        WriteBatch batch{*db_};
        const Writes writes = registerWrites(batch);
        for (const TransformedSource& source : sources) {
            stageSource(batch, writes, source.sourceId, source.stale, source.points);
        }
        return batch.flush();
    }

private:
    EngineDb* db_; ///< The database; not owned.
};

} // namespace

TransformStorePtr makeLadybugTransformStore(EngineDb& db) {
    return TransformStorePtr{new LadybugTransformStore{db}};
}

} // namespace rainbohz::rendering_engine::transform
