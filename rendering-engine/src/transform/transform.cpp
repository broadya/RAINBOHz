// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/transform.hpp"

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/transform/polytempo.hpp"
#include "rainbohz/rendering_engine/transform/squash.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"
#include "rainbohz/rendering_engine/write_batch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Transforms musical-time partial set timelines into per-partial
/// physical timelines, and spatial musical timelines into per-source ones.
///
/// Implements docs/transform.md. The file is organised as precomputation.cpp
/// is:
///
///   - fixed Cypher queries, prepared once by EngineDb and bound with
///     parameters;
///   - a bulk read of everything the run needs before anything is written;
///   - the tempo maps, built and validated once each, then resolved to
///     physical time through their synchronisation edges (docs/polytempo.md);
///   - the per-set stage: validation of the timeline, its POINT_AT edges and
///     its states, and the physical time of each point;
///   - the per-partial stage: envelope nodes, placement, lifeline fit, unit
///     conversion, and the write that replaces earlier output in one
///     transaction;
///   - the per-source stage: the spatial timeline, its POINT_AT edges and
///     states, placement, unit conversion and the write (§6.8);
///   - the pure stages, which are testable without a database;
///   - transformAll(), which processes sets, partials and sources
///     independently.
///
/// Section numbers in comments refer to docs/transform.md.
///
/// @see transform.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

/// A node id to node id lookup: each node's id maps to the id of the node its
/// NEXT_POINT edge points to (readLinks(), walkList()).
using IdMap = std::unordered_map<std::int64_t, std::int64_t>;

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

/// Reads the engine's configured sample rate.
constexpr std::string_view kReadConfig = "MATCH (c:EngineConfig) RETURN c.sampleRate;";

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
/// its two sync points.
constexpr std::string_view kReadSyncEdges =
    "MATCH (c:TempoMap)-[r:TEMPO_MAP_SYNCHRONIZATION_POINT]->(p:TempoMap) "
    "RETURN c.id, p.id, r.sync_point_child, r.sync_point_parent ORDER BY c.id, p.id;";

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

/// How many owners' output (partials, or sources) is written per flush, and
/// so per transaction (§7). As in precomputation; @see ADR 0022.
constexpr std::size_t kOwnersPerFlush = 64;

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
void stageDeletes(WriteBatch& batch, const TimelineWrites& kind,
    const std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>& stale) {
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

// ---------------------------------------------------------------------------
// Limits from the schema (engine-schema.cypher, "Units for specification of
// a score", and the curve enumerations)
// ---------------------------------------------------------------------------

constexpr std::int64_t kMinCurveType = 0;      ///< Lowest valid curve type (schema enum).
constexpr std::int64_t kMaxCurveType = 4;      ///< Highest valid curve type (schema enum).
constexpr double kMinCurveAmount = 0.0;        ///< Lowest valid curve amount.
constexpr double kMaxCurveAmount = 1.0;        ///< Highest valid curve amount.
constexpr double kMinAmplitude = -144.0;       ///< Lowest valid amplitude, dB.
constexpr double kMaxAmplitude = 0.0;          ///< Highest valid amplitude, dB.
constexpr double kMinPhaseDegrees = -180.0;    ///< Lowest valid phase, degrees.
constexpr double kMaxPhaseDegrees = 180.0;     ///< Highest valid phase, degrees.
constexpr double kMinAzimuthDegrees = -180.0;  ///< Lowest valid azimuth, degrees.
constexpr double kMaxAzimuthDegrees = 180.0;   ///< Highest valid azimuth, degrees.
constexpr double kMinElevationDegrees = -90.0; ///< Lowest valid elevation, degrees.
constexpr double kMaxElevationDegrees = 90.0;  ///< Highest valid elevation, degrees.
constexpr double kMinDistanceMetres = 1.0;     ///< Lowest valid distance, metres.
constexpr double kMaxDistanceMetres = 16384.0; ///< Highest valid distance, metres.

// ---------------------------------------------------------------------------
// Inputs, as read in bulk before anything is written
// ---------------------------------------------------------------------------

/// A Score and its root TempoMap, as read.
struct ScoreRecord {
    std::int64_t scoreId = 0;               ///< The Score.
    std::optional<std::int64_t> tempoMapId; ///< Its own TempoMap; nullopt if it has none.

    /// Score.tempo_ramp_physical_time; nullopt for NULL, which means true.
    std::optional<bool> tempoRampPhysicalTime;
};

/// A TempoMap's own fields, as read.
struct TempoMapRecord {
    std::int64_t id = 0;                      ///< The TempoMap.
    std::optional<std::string> name;          ///< Its name.
    std::optional<std::int64_t> ticksPerBeat; ///< Its ticks_per_beat.
};

/// A PartialSet with a musical timeline, as read.
struct MusicalSetRecord {
    std::int64_t setId = 0;          ///< The PartialSet.
    std::optional<std::string> name; ///< The PartialSet's name.

    /// Each PartialSetMusicalTimeline of the set with the head of its point
    /// list (nullopt for an empty one). The schema allows one per set; a
    /// second is reported rather than assumed away.
    std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> timelines;
};

/// A musical timeline point's POINT_AT, as read.
struct PointAtRecord {
    /// The TempoMap its POINT_AT leads to; nullopt when it has none.
    std::optional<std::int64_t> tempoMapId;
    std::optional<std::int64_t> syncPoint; ///< POINT_AT's sync_point, in musical time.
    std::optional<double> offsetSeconds;   ///< POINT_AT's offset_seconds.
};

/// A PartialSetMusicalTimelinePoint with its POINT_AT, as read.
struct MusicalPointRecord {
    std::int64_t id = 0;                            ///< The PartialSetMusicalTimelinePoint.
    std::optional<std::int64_t> frequencyCurveType; ///< Its frequency_curve_type.
    std::optional<double> frequencyCurveAmount;     ///< Its frequency_curve_amount.
    std::optional<std::int64_t> amplitudeCurveType; ///< Its amplitude_curve_type.
    std::optional<double> amplitudeCurveAmount;     ///< Its amplitude_curve_amount.
    PointAtRecord at;                               ///< Its POINT_AT, if it has one.
};

/// A SoundSource with a spatial musical timeline, as read.
struct SpatialSourceRecord {
    std::int64_t sourceId = 0;       ///< The SoundSource.
    std::optional<std::string> name; ///< The SoundSource's name.

    /// Each SpatialPositionMusicalTimeline of the source with the head of
    /// its point list (nullopt for an empty one). The schema allows one.
    std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> timelines;
};

/// A SpatialPositionMusicalTimelinePoint with its POINT_AT, as read; the
/// curve fields in kSpatialKinds order.
struct SpatialMusicalPointRecord {
    std::int64_t id = 0; ///< The SpatialPositionMusicalTimelinePoint.
    std::array<std::optional<std::int64_t>, 3>
        curveType; ///< Azimuth, elevation, distance curve types.
    std::array<std::optional<double>, 3>
        curveAmount;  ///< Azimuth, elevation, distance curve amounts.
    PointAtRecord at; ///< Its POINT_AT, if it has one.
};

/// A SpatialPositionState reached from a timeline point, as read.
struct SpatialStateRecord {
    std::int64_t stateId = 0;        ///< The SpatialPositionState.
    std::optional<double> azimuth;   ///< Its azimuth, degrees.
    std::optional<double> elevation; ///< Its elevation, degrees.
    std::optional<double> distance;  ///< Its distance, metres.
};

/// A PartialState reached from a timeline point, as read.
struct StateRecord {
    std::int64_t stateId = 0;        ///< The PartialState.
    std::int64_t partialId = 0;      ///< The Partial it is STATE_OF.
    std::optional<double> frequency; ///< Its frequency, Hz.
    std::optional<double> amplitude; ///< Its amplitude, dB.

    /// Degrees; with phaseFreeRun, one of three meanings (§6.6).
    std::optional<double> phase;
    std::optional<bool> phaseFreeRun; ///< Its phase_free_run.
};

/// A partial's existing physical timeline of one kind, with its point ids in
/// list order, to be deleted.
struct ExistingTimeline {
    std::int64_t timelineId = 0;      ///< The physical timeline to delete.
    std::optional<std::int64_t> head; ///< Its NEXT_POINT head, or nullopt if empty.
};

/// Everything the run reads, for every set at once.
struct Inputs {
    /// Every Score, with its root TempoMap if it has one.
    std::vector<ScoreRecord> scores;
    /// Every TempoMap, in id order.
    std::vector<TempoMapRecord> tempoMaps;
    /// Each TempoMap's point-list heads, by TempoMap.id (more than one is a
    /// validation error raised later).
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> tempoMapHeads;
    /// Every TempoMapPoint's fields, by its id.
    std::unordered_map<std::int64_t, TempoMapPointRecord> tempoMapPoints;
    /// The NEXT_POINT links between TempoMapPoints.
    IdMap tempoMapNext;
    /// Every TEMPO_MAP_SYNCHRONIZATION_POINT edge.
    std::vector<SyncEdgeRecord> syncEdges;

    /// Every PartialSet with a musical timeline.
    std::vector<MusicalSetRecord> sets;
    /// Each PartialSet's owning Score, by PartialSet.id.
    std::unordered_map<std::int64_t, std::int64_t> scoreOfSet;
    /// Each PartialSet's member Partials (id and name), by PartialSet.id.
    std::unordered_map<std::int64_t,
        std::vector<std::pair<std::int64_t, std::optional<std::string>>>>
        membersOfSet;
    /// Every PartialSetMusicalTimelinePoint, by its id.
    std::unordered_map<std::int64_t, MusicalPointRecord> musicalPoints;
    /// The NEXT_POINT links between PartialSetMusicalTimelinePoints.
    IdMap musicalNext;
    /// Each timeline point's PartialStates, by point id.
    std::unordered_map<std::int64_t, std::vector<StateRecord>> statesOfPoint;

    /// Each Partial's existing FrequencyPhysicalTimelines, by Partial.id.
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingFrequency;
    /// The NEXT_POINT links between FrequencyPhysicalTimelinePoints.
    IdMap frequencyNext;
    /// Each Partial's existing AmplitudePhysicalTimelines, by Partial.id.
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingAmplitude;
    /// The NEXT_POINT links between AmplitudePhysicalTimelinePoints.
    IdMap amplitudeNext;
    /// Each Partial's existing PhasePhysicalTimelines, by Partial.id.
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingPhase;
    /// The NEXT_POINT links between PhasePhysicalTimelinePoints.
    IdMap phaseNext;

    /// Every SoundSource with a spatial musical timeline.
    std::vector<SpatialSourceRecord> sources;
    /// Each SoundSource's owning Score, by SoundSource.id.
    std::unordered_map<std::int64_t, std::int64_t> scoreOfSource;
    /// Every SpatialPositionMusicalTimelinePoint, by its id.
    std::unordered_map<std::int64_t, SpatialMusicalPointRecord> spatialPoints;
    /// The NEXT_POINT links between SpatialPositionMusicalTimelinePoints.
    IdMap spatialMusicalNext;
    /// Each timeline point's SpatialPositionState, by point id (at most one
    /// per point).
    std::unordered_map<std::int64_t, std::vector<SpatialStateRecord>> statesOfSpatialPoint;

    /// Per kind, in kSpatialKinds order: each source's existing physical
    /// timelines, and the links between their points.
    std::array<std::unordered_map<std::int64_t, std::vector<ExistingTimeline>>, 3> existingSpatial;
    /// Per kind, in kSpatialKinds order: the NEXT_POINT links between the
    /// physical spatial timeline points.
    std::array<IdMap, 3> spatialPhysicalNext;
};

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

/// Reads a bool column that may be NULL.
///
/// @param value  One value from a DbRow.
/// @return The bool, or nullopt if NULL.
/// @throws EngineDbError  If `value` is neither NULL nor a bool.
std::optional<bool> optionalBool(const DbValue& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return std::nullopt;
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag;
    }
    throw EngineDbError{"expected a BOOL column"};
}

/// Reads every spatial input the run needs (§6.8): the sources with a
/// spatial musical timeline, their Scores, their points, POINT_AT edges and
/// states, and their existing physical timelines.
///
/// @param db              Database to read. Not modified.
/// @param[in,out] inputs  Receives the spatial members.
/// @throws EngineDbError  If any query fails or returns a value of an
///         unexpected type.
void readSpatialInputs(EngineDb& db, Inputs& inputs) {
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
Inputs readInputs(EngineDb& db) {
    Inputs inputs;

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

/// Follows a NEXT_POINT list from its head, in C++.
///
/// Lists are acyclic by construction upstream, not by constraint (AGENTS.md,
/// Linked Lists), so the walk stops, with an error, at a revisited node.
///
/// @tparam Error  The exception to throw on a loop.
/// @param head  Id of the first node, or nullopt for an empty list.
/// @param next  For each node id, the id of its successor.
/// @param what  What the list is, for the error message.
/// @return The node ids in list order. Empty when `head` is nullopt.
/// @throws Error  If the list returns to a node it has already visited.
template <typename Error>
std::vector<std::int64_t> walkList(
    std::optional<std::int64_t> head, const IdMap& next, std::string_view what) {
    std::vector<std::int64_t> chain;
    std::unordered_set<std::int64_t> seen;
    for (std::optional<std::int64_t> at = head; at.has_value();) {
        if (!seen.insert(*at).second) {
            throw Error{std::string{what} + ": its NEXT_POINT list loops back to node id " +
                        std::to_string(*at) + " after " + std::to_string(chain.size()) + " nodes"};
        }
        chain.push_back(*at);
        const auto link = next.find(*at);
        at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
    }
    return chain;
}

/// Names a timeline point in a set-level message.
///
/// @param index  Position of the point in the set's timeline, from 0.
/// @param id     Its PartialSetMusicalTimelinePoint.id.
/// @return e.g. "point 2 (id 17)".
std::string describePoint(std::size_t index, std::int64_t id) {
    return "point " + std::to_string(index) + " (id " + std::to_string(id) + ")";
}

// ---------------------------------------------------------------------------
// Tempo maps
// ---------------------------------------------------------------------------

/// Every Score's tempo maps, resolved to physical time (docs/polytempo.md).
struct ResolvedMaps {
    /// Each map's resolved form, by TempoMap.id.
    std::unordered_map<std::int64_t, ResolvedTempoMap> maps;

    /// Score.id of the Score whose graph each map belongs to, by TempoMap.id.
    std::unordered_map<std::int64_t, std::int64_t> scoreOfMap;

    /// Each Score's own TempoMap, the root of its graph, by Score.id. A Score
    /// without one is absent.
    std::unordered_map<std::int64_t, std::int64_t> rootOfScore;
};

/// Builds, validates and resolves every TempoMap (§6.1; docs/polytempo.md
/// §7).
///
/// The maps are ordered parents first from the Scores' roots, each is built
/// as a TempoMap in its Score's ramp shape, and each is then resolved against
/// its synchronisation edges, so that a child's anchors come from parents
/// already in physical time.
///
/// @param inputs       Everything read for the run.
/// @param[out] report  Receives one TempoMapSummary per map, in resolution
///                     order.
/// @return The resolved maps, and whose Score each belongs to.
/// @throws TransformError  If any map is invalid, or the synchronisation
///         edges cannot be resolved (docs/polytempo.md §6).
ResolvedMaps buildTempoMaps(const Inputs& inputs, TransformReport& report) {
    std::vector<std::int64_t> mapIds;
    mapIds.reserve(inputs.tempoMaps.size());
    for (const TempoMapRecord& record : inputs.tempoMaps) {
        mapIds.push_back(record.id);
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> roots;
    std::unordered_map<std::int64_t, TempoRamp> rampOfScore;
    ResolvedMaps result;
    for (const ScoreRecord& score : inputs.scores) {
        rampOfScore.emplace(score.scoreId, tempoRampFromScore(score.tempoRampPhysicalTime));
        if (score.tempoMapId.has_value()) {
            roots.emplace_back(*score.tempoMapId, score.scoreId);
            result.rootOfScore.emplace(score.scoreId, *score.tempoMapId);
        }
    }
    try {
        TempoMapOrder order = orderTempoMaps(mapIds, inputs.syncEdges, roots);
        std::unordered_map<std::int64_t, TempoMap> nominal;
        for (const std::int64_t mapId : order.order) {
            const std::string what = "TempoMap " + std::to_string(mapId);
            const TempoMapRecord& record =
                *std::ranges::find(inputs.tempoMaps, mapId, &TempoMapRecord::id);
            const auto heads = inputs.tempoMapHeads.find(mapId);
            const std::optional<std::int64_t> head =
                heads == inputs.tempoMapHeads.end() || heads->second.empty()
                    ? std::nullopt
                    : heads->second.front();
            try {
                std::vector<TempoMapPointRecord> records;
                for (const std::int64_t id :
                    walkList<TempoMapError>(head, inputs.tempoMapNext, what)) {
                    records.push_back(inputs.tempoMapPoints.at(id));
                }
                nominal.emplace(mapId, TempoMap::fromRecords(record.ticksPerBeat, records,
                                           rampOfScore.at(order.scoreOfMap.at(mapId))));
            } catch (const TempoMapError& e) {
                throw TempoMapError{what + ": " + e.what()};
            }
        }
        result.maps = resolveTempoMapGraph(nominal, inputs.syncEdges, order);
        for (const std::int64_t mapId : order.order) {
            const TempoMap& map = nominal.at(mapId);
            TempoMapSummary summary{
                .scoreId = order.scoreOfMap.at(mapId),
                .tempoMapId = mapId,
                .name = std::ranges::find(inputs.tempoMaps, mapId, &TempoMapRecord::id)->name,
                .ticksPerBeat = map.ticksPerBeat().value(),
                .pointCount = map.points().size(),
                .rampPhysicalTime = map.ramp() == TempoRamp::PhysicalTime,
                .parentIds = {},
                .syncPointCount = 0,
            };
            for (const SyncEdgeRecord& edge : inputs.syncEdges) {
                if (edge.childId != mapId) {
                    continue;
                }
                ++summary.syncPointCount;
                if (std::ranges::find(summary.parentIds, edge.parentId) ==
                    summary.parentIds.end()) {
                    summary.parentIds.push_back(edge.parentId);
                }
            }
            std::ranges::sort(summary.parentIds);
            report.tempoMaps.push_back(std::move(summary));
        }
        result.scoreOfMap = std::move(order.scoreOfMap);
    } catch (const TempoMapError& e) {
        throw TransformError{e.what()};
    }
    return result;
}

// ---------------------------------------------------------------------------
// The per-set stage
// ---------------------------------------------------------------------------

/// A set's timeline, validated, with the physical time of each point and
/// each partial's envelope nodes (§6.2).
struct SetEnvelopes {
    /// The computed physical time of every point, by point index.
    std::vector<SampleTime> pointTimes;

    /// Each member partial's envelope nodes: frequency, then amplitude.
    std::unordered_map<std::int64_t,
        std::pair<std::vector<EnvelopeNode>, std::vector<EnvelopeNode>>>
        nodes;

    /// Each member partial's states' phase fields, one per state it has,
    /// whatever their meaning (§6.6).
    std::unordered_map<std::int64_t, std::vector<StatePhase>> states;
};

/// Checks a curve type and amount of a point, for one property.
///
/// @tparam Error  The exception of the timeline's scope: PartialSetTransformError
///                or SoundSourceTransformError.
/// @param index     Position of the point in the timeline, from 0.
/// @param id        Its id.
/// @param property  The property's column prefix, e.g. "frequency" or
///                  "azimuth".
/// @param type      The curve type as read.
/// @param amount    The curve amount as read.
/// @throws Error  If either is NULL or out of range.
template <typename Error>
void requireCurve(std::size_t index, std::int64_t id, std::string_view property,
    const std::optional<std::int64_t>& type, const std::optional<double>& amount) {
    const std::string where = describePoint(index, id) + ": " + std::string{property};
    if (!type.has_value() || *type < kMinCurveType || *type > kMaxCurveType) {
        throw Error{where + "_curve_type is " +
                    (type.has_value() ? std::to_string(*type) : "NULL") + "; it must be in [" +
                    std::to_string(kMinCurveType) + ", " + std::to_string(kMaxCurveType) + "]"};
    }
    if (!amount.has_value() || !std::isfinite(*amount) || *amount < kMinCurveAmount ||
        *amount > kMaxCurveAmount) {
        throw Error{where + "_curve_amount is " +
                    (amount.has_value() ? formatNumber(*amount) : "NULL") + "; it must be in [" +
                    formatNumber(kMinCurveAmount) + ", " + formatNumber(kMaxCurveAmount) + "]"};
    }
}

/// What pointTime() carries from one point of a timeline to the next.
struct PreviousPoint {
    std::optional<std::int64_t> sync; ///< The previous point's sync_point.
    std::optional<std::int64_t> map;  ///< The TempoMap its POINT_AT leads to.
};

/// Validates one point's POINT_AT against its predecessor and the Score's
/// maps, and returns its physical time.
///
/// @tparam Error  The exception of the timeline's scope: PartialSetTransformError
///                or SoundSourceTransformError.
/// @param index         Position of the point in the timeline, from 0.
/// @param id            Its id.
/// @param at            Its POINT_AT as read.
/// @param scoreId       The Score the timeline's owner hangs from. Every
///                      POINT_AT must lead to one of its maps.
/// @param maps          Every Score's resolved maps.
/// @param rate          Sample rate for the conversion to samples.
/// @param[in,out] previous  The previous point's sync_point and TempoMap;
///                      updated to this point's on return. Both unset for
///                      the first point.
/// @return `toSampleTime(toSeconds(sync_point) + offset_seconds)` on the map
///         the POINT_AT leads to.
/// @throws Error  If the point has no POINT_AT, or one to a map outside the
///         Score's graph, or to a different map from the previous point's
///         (ticks on different maps cannot be ordered), or a NULL or
///         negative sync_point, or a sync_point before the previous point's,
///         or a non-finite offset.
template <typename Error>
SampleTime pointTime(std::size_t index, std::int64_t id, const PointAtRecord& at,
    std::int64_t scoreId, const ResolvedMaps& maps, SampleRate rate, PreviousPoint& previous) {
    const std::string where = describePoint(index, id);
    if (!at.tempoMapId.has_value()) {
        throw Error{where + " has no POINT_AT"};
    }
    const auto owner = maps.scoreOfMap.find(*at.tempoMapId);
    if (owner == maps.scoreOfMap.end() || owner->second != scoreId) {
        throw Error{where + ": POINT_AT leads to TempoMap " + std::to_string(*at.tempoMapId) +
                    ", which is not a TempoMap of Score " + std::to_string(scoreId)};
    }
    if (previous.map.has_value() && *previous.map != *at.tempoMapId) {
        throw Error{where + ": POINT_AT leads to TempoMap " + std::to_string(*at.tempoMapId) +
                    " but the earlier points lead to TempoMap " + std::to_string(*previous.map) +
                    "; a timeline keeps time by one TempoMap"};
    }
    previous.map = at.tempoMapId;
    std::optional<std::int64_t>& previousSync = previous.sync;
    if (!at.syncPoint.has_value() || *at.syncPoint < 0) {
        throw Error{where + ": sync_point is " +
                    (at.syncPoint.has_value() ? std::to_string(*at.syncPoint) : "NULL") +
                    "; it must be >= 0"};
    }
    const std::int64_t sync = *at.syncPoint;
    if (previousSync.has_value() && sync < *previousSync) {
        throw Error{where + ": sync_point " + std::to_string(sync) +
                    " is before the previous point's " + std::to_string(*previousSync) +
                    "; points must be in non-decreasing musical order"};
    }
    previousSync = sync;
    const double offset = at.offsetSeconds.value_or(0.0);
    if (!std::isfinite(offset)) {
        throw Error{
            where + ": offset_seconds is " + formatNumber(offset) + ", which is not finite"};
    }

    const SecondsTime seconds =
        maps.maps.at(*at.tempoMapId).toSeconds(MusicalTime{sync}) + SecondsDuration{offset};
    return toSampleTime(seconds, rate);
}

/// Adds one point's states to its partials' envelope nodes (§6.2).
///
/// @param index          Position of the point in the timeline, from 0.
/// @param point          The point as read; its curve fields are valid.
/// @param states         The PartialStates reached from it.
/// @param members        The ids of the set's partials.
/// @param[in,out] envelopes  Each partial's (frequency, amplitude) envelope nodes and
///                       states' phase fields; this point's are appended.
/// @throws PartialSetTransformError  If a state belongs to a partial outside
///         the set, or two states belong to the same partial.
void gatherStates(std::size_t index, const MusicalPointRecord& point,
    std::span<const StateRecord> states, const std::unordered_set<std::int64_t>& members,
    SetEnvelopes& envelopes) {
    std::unordered_set<std::int64_t> seenPartials;
    for (const StateRecord& state : states) {
        const std::string where = describePoint(index, point.id);
        if (!members.contains(state.partialId)) {
            throw PartialSetTransformError{
                where + ": PartialState " + std::to_string(state.stateId) + " belongs to Partial " +
                std::to_string(state.partialId) + ", which is not a member of this PartialSet"};
        }
        if (!seenPartials.insert(state.partialId).second) {
            throw PartialSetTransformError{
                where + ": Partial " + std::to_string(state.partialId) +
                " has more than one PartialState; at most one is allowed per point"};
        }
        auto& [frequency, amplitude] = envelopes.nodes[state.partialId];
        if (state.frequency.has_value()) {
            frequency.push_back(EnvelopeNode{.point = index,
                .value = *state.frequency,
                .curveType = point.frequencyCurveType.value_or(0),
                .curveAmount = point.frequencyCurveAmount.value_or(0.0)});
        }
        if (state.amplitude.has_value()) {
            amplitude.push_back(EnvelopeNode{.point = index,
                .value = *state.amplitude,
                .curveType = point.amplitudeCurveType.value_or(0),
                .curveAmount = point.amplitudeCurveAmount.value_or(0.0)});
        }
        // Every state is kept, to check it lies within the lifeline; which
        // ones are phase points is decided per partial (§6.6).
        envelopes.states[state.partialId].push_back(
            StatePhase{.point = index, .degrees = state.phase, .freeRun = state.phaseFreeRun});
    }
}

/// Validates a set's timeline, POINT_AT edges and states, and computes the
/// physical time of every point and the envelope nodes of every member (§6.2).
///
/// @param inputs   Everything read for the run.
/// @param set      The set.
/// @param scoreId  The set's Score, one of whose maps every POINT_AT must
///                 lead to.
/// @param maps     Every Score's resolved maps.
/// @param rate     Sample rate for the conversion to samples.
/// @return The point times and the members' envelope nodes.
/// @throws PartialSetTransformError  If the set has no, or more than one,
///         timeline; the timeline has no points or loops; a point has no
///         POINT_AT, or one to a map outside the Score's graph or to a
///         different map from the previous point's, or a NULL or negative
///         sync_point,
///         or a sync_point before the previous point's, or a non-finite
///         offset, or a curve field out of range; or a state belongs to a
///         partial outside the set, or two states at one point belong to the
///         same partial.
SetEnvelopes gatherSet(const Inputs& inputs, const MusicalSetRecord& set, std::int64_t scoreId,
    const ResolvedMaps& maps, SampleRate rate) {
    if (set.timelines.size() != 1) {
        throw PartialSetTransformError{"it has " + std::to_string(set.timelines.size()) +
                                       " PartialSetMusicalTimelines; exactly one is required"};
    }
    const std::vector<std::int64_t> pointIds = walkList<PartialSetTransformError>(
        set.timelines.front().second, inputs.musicalNext, "its PartialSetMusicalTimeline");
    if (pointIds.empty()) {
        throw PartialSetTransformError{"its PartialSetMusicalTimeline has no points"};
    }

    std::unordered_set<std::int64_t> members;
    if (const auto found = inputs.membersOfSet.find(set.setId);
        found != inputs.membersOfSet.end()) {
        for (const auto& [partialId, name] : found->second) {
            members.insert(partialId);
        }
    }

    SetEnvelopes envelopes;
    envelopes.pointTimes.reserve(pointIds.size());
    PreviousPoint previous;
    static const std::vector<StateRecord> kNoStates;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const MusicalPointRecord& point = inputs.musicalPoints.at(pointIds[i]);
        envelopes.pointTimes.push_back(pointTime<PartialSetTransformError>(
            i, point.id, point.at, scoreId, maps, rate, previous));
        requireCurve<PartialSetTransformError>(
            i, point.id, "frequency", point.frequencyCurveType, point.frequencyCurveAmount);
        requireCurve<PartialSetTransformError>(
            i, point.id, "amplitude", point.amplitudeCurveType, point.amplitudeCurveAmount);
        const auto states = inputs.statesOfPoint.find(point.id);
        gatherStates(i, point, states == inputs.statesOfPoint.end() ? kNoStates : states->second,
            members, envelopes);
    }
    return envelopes;
}

// ---------------------------------------------------------------------------
// The per-partial stage
// ---------------------------------------------------------------------------

/// Everything to be written for one partial.
struct PartialOutput {
    std::int64_t partialId = 0; ///< The Partial.

    /// Existing timelines to delete, each with its point ids.
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> staleFrequency;
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>
        staleAmplitude; ///< As staleFrequency, amplitude.
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>
        stalePhase; ///< As staleFrequency, phase.

    /// The points to write, in physical units.
    std::vector<PhysicalPoint> frequency;
    std::vector<PhysicalPoint> amplitude; ///< As frequency, amplitude.
    std::vector<PhasePoint> phase;        ///< As frequency, phase.
};

/// Stages the replacement of a partial's three physical timelines as one
/// unit of `batch` (§7).
///
/// @param[in,out] batch  Batch to stage into. Gains one unit, written whole
///                       or not at all when the batch flushes.
/// @param writes         The batch's registered operations.
/// @param output         What to write.
void stagePartial(WriteBatch& batch, const Writes& writes, const PartialOutput& output) {
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

/// Checks the envelope nodes' values, naming the offending point.
///
/// @param frequency  Frequency envelope nodes, Hz.
/// @param amplitude  Amplitude envelope nodes, dB.
/// @throws PartialTransformError  If a frequency is not finite, or an
///         amplitude is not finite or outside [−144, 0].
void requireValues(
    std::span<const EnvelopeNode> frequency, std::span<const EnvelopeNode> amplitude) {
    for (const EnvelopeNode& node : frequency) {
        if (!std::isfinite(node.value)) {
            throw PartialTransformError{"at point " + std::to_string(node.point) +
                                        " its frequency is " + formatNumber(node.value) +
                                        ", which is not finite"};
        }
    }
    for (const EnvelopeNode& node : amplitude) {
        if (!std::isfinite(node.value) || node.value < kMinAmplitude ||
            node.value > kMaxAmplitude) {
            throw PartialTransformError{"at point " + std::to_string(node.point) +
                                        " its amplitude is " + formatNumber(node.value) +
                                        " dB; it must be in [" + formatNumber(kMinAmplitude) +
                                        ", " + formatNumber(kMaxAmplitude) + "]"};
        }
    }
}

/// Transforms one partial and stages its output (§6.2 to §6.6, §7).
///
/// @param[in,out] batch  Batch the partial's output is staged into, as one
///                      unit; it is written when the batch flushes.
/// @param writes        The batch's registered operations.
/// @param inputs        Everything read for the run.
/// @param envelopes     The set's point times and envelope nodes.
/// @param partialId     The partial.
/// @param rate          Sample rate, for the frequency conversion.
/// @param[out] outcome  On success, its point counts are set. They stand
///                      unless the batch then fails to write the unit.
/// @throws PartialTransformError  If the partial's envelope nodes are invalid,
///         or its lifeline collapses. Nothing has been staged.
void transformPartial(WriteBatch& batch, const Writes& writes, const Inputs& inputs,
    const SetEnvelopes& envelopes, std::int64_t partialId, SampleRate rate,
    PartialTransformOutcome& outcome) {
    static const std::pair<std::vector<EnvelopeNode>, std::vector<EnvelopeNode>> kNoNodes;
    static const std::vector<StatePhase> kNoStates;
    const auto found = envelopes.nodes.find(partialId);
    const auto& [frequencyNodes, amplitudeNodes] =
        found == envelopes.nodes.end() ? kNoNodes : found->second;
    const auto foundStates = envelopes.states.find(partialId);
    const std::vector<StatePhase>& states =
        foundStates == envelopes.states.end() ? kNoStates : foundStates->second;

    requireSharedEndPoints(frequencyNodes, amplitudeNodes);
    requireStatesWithinLifeline(frequencyNodes, states);
    requireValues(frequencyNodes, amplitudeNodes);
    requireValidPhase(states);

    PartialOutput output{.partialId = partialId,
        .staleFrequency = {},
        .staleAmplitude = {},
        .stalePhase = {},
        .frequency = placeNodes(frequencyNodes, envelopes.pointTimes),
        .amplitude = placeNodes(amplitudeNodes, envelopes.pointTimes),
        .phase = placePhaseNodes(phaseNodesOf(states), envelopes.pointTimes)};
    if (output.frequency.size() < 2) {
        throw PartialTransformError{
            "its lifeline collapsed to a single instant at " +
            formatNumber(output.frequency.front().time.value()) +
            " samples: every later frequency point was pulled back onto the first by its offset"};
    }
    fitToLifeline(output.amplitude, output.frequency.back().time);
    fitToLifeline(output.phase, output.frequency.back().time);
    collapseFreeRuns(output.phase);
    for (PhysicalPoint& point : output.frequency) {
        point.value = toAngular(Hertz{point.value}, rate).value();
    }

    const auto stale = [&](const auto& existing, const IdMap& next, std::string_view what) {
        std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> result;
        if (const auto timelines = existing.find(partialId); timelines != existing.end()) {
            for (const ExistingTimeline& timeline : timelines->second) {
                result.emplace_back(timeline.timelineId,
                    walkList<PartialTransformError>(timeline.head, next, what));
            }
        }
        return result;
    };
    output.staleFrequency = stale(
        inputs.existingFrequency, inputs.frequencyNext, "its existing FrequencyPhysicalTimeline");
    output.staleAmplitude = stale(
        inputs.existingAmplitude, inputs.amplitudeNext, "its existing AmplitudePhysicalTimeline");
    output.stalePhase =
        stale(inputs.existingPhase, inputs.phaseNext, "its existing PhasePhysicalTimeline");

    stagePartial(batch, writes, output);

    outcome.frequencyPoints = output.frequency.size();
    outcome.amplitudePoints = output.amplitude.size();
    outcome.phasePoints = output.phase.size();
}

/// Transforms one set: validates it, then its partials one by one.
///
/// @param[in,out] batch   Batch each partial's output is staged into.
/// @param writes          The batch's registered operations.
/// @param inputs          Everything read for the run.
/// @param set             The set.
/// @param maps            Every Score's resolved tempo maps.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the set's error, or its partials'
///                        outcomes.
/// @param staged          Called with the index in `outcome.partials` of each
///                        partial staged, after its outcome is recorded; it
///                        may flush the batch.
void transformSet(WriteBatch& batch, const Writes& writes, const Inputs& inputs,
    const MusicalSetRecord& set, const ResolvedMaps& maps, SampleRate rate,
    PartialSetTransformOutcome& outcome, const std::function<void(std::size_t)>& staged) {
    SetEnvelopes envelopes;
    try {
        const auto score = inputs.scoreOfSet.find(set.setId);
        if (score == inputs.scoreOfSet.end()) {
            throw PartialSetTransformError{
                "it is not reached from any Score through SoundEntity and SoundSource"};
        }
        if (!maps.rootOfScore.contains(score->second)) {
            throw PartialSetTransformError{"its Score " + std::to_string(score->second) +
                                           " has no TempoMap, so its musical time is undefined"};
        }
        envelopes = gatherSet(inputs, set, score->second, maps, rate);
    } catch (const PartialSetTransformError& e) {
        outcome.error = e.what();
        return;
    }

    const auto members = inputs.membersOfSet.find(set.setId);
    if (members == inputs.membersOfSet.end()) {
        return;
    }
    for (const auto& [partialId, partialName] : members->second) {
        PartialTransformOutcome partial{.partialId = partialId,
            .partialName = partialName,
            .error = std::nullopt,
            .frequencyPoints = 0,
            .amplitudePoints = 0,
            .phasePoints = 0};
        bool isStaged = false;
        try {
            transformPartial(batch, writes, inputs, envelopes, partialId, rate, partial);
            isStaged = true;
        } catch (const PartialTransformError& e) {
            partial.error = e.what();
        }
        outcome.partials.push_back(std::move(partial));
        if (isStaged) {
            staged(outcome.partials.size() - 1);
        }
    }
}

// ---------------------------------------------------------------------------
// The per-source stage (§6.8)
// ---------------------------------------------------------------------------

/// The column prefixes of the three spatial properties, in kSpatialKinds
/// order, for messages.
constexpr std::array<std::string_view, 3> kSpatialProperties{"azimuth", "elevation", "distance"};

/// A source's timeline, validated, with the physical time of each point and
/// its three envelopes.
struct SourceEnvelopes {
    /// The computed physical time of every point, by point index.
    std::vector<SampleTime> pointTimes;

    /// The azimuth (degrees), elevation (degrees) and distance (metres)
    /// envelopes.
    SpatialTriple<EnvelopeNode> envelopes;
};

/// Validates a source's spatial timeline, POINT_AT edges and states, and
/// computes the physical time of every point and its three envelopes.
///
/// @param inputs   Everything read for the run.
/// @param source   The source.
/// @param scoreId  The source's Score, one of whose maps every POINT_AT must
///                 lead to.
/// @param maps     Every Score's resolved maps.
/// @param rate     Sample rate for the conversion to samples.
/// @return The point times and the envelopes.
/// @throws SoundSourceTransformError  If the source has no, or more than one,
///         timeline; the timeline has no points or loops; a point's POINT_AT
///         or curve fields are invalid (as for a set); a point has more than
///         one state; the first point does not define all three properties;
///         or a value is out of range.
SourceEnvelopes gatherSource(const Inputs& inputs, const SpatialSourceRecord& source,
    std::int64_t scoreId, const ResolvedMaps& maps, SampleRate rate) {
    if (source.timelines.size() != 1) {
        throw SoundSourceTransformError{
            "it has " + std::to_string(source.timelines.size()) +
            " SpatialPositionMusicalTimelines; exactly one is required"};
    }
    const std::vector<std::int64_t> pointIds =
        walkList<SoundSourceTransformError>(source.timelines.front().second,
            inputs.spatialMusicalNext, "its SpatialPositionMusicalTimeline");
    if (pointIds.empty()) {
        throw SoundSourceTransformError{"its SpatialPositionMusicalTimeline has no points"};
    }

    SourceEnvelopes result;
    result.pointTimes.reserve(pointIds.size());
    std::vector<SpatialState> states;
    std::vector<SpatialCurves> curves;
    PreviousPoint previous;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const SpatialMusicalPointRecord& point = inputs.spatialPoints.at(pointIds[i]);
        result.pointTimes.push_back(pointTime<SoundSourceTransformError>(
            i, point.id, point.at, scoreId, maps, rate, previous));
        SpatialCurves pointCurves;
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            requireCurve<SoundSourceTransformError>(i, point.id, kSpatialProperties.at(k),
                point.curveType.at(k), point.curveAmount.at(k));
            // Checked non-NULL by requireCurve().
            pointCurves.curveType.at(k) = point.curveType.at(k).value_or(0);
            pointCurves.curveAmount.at(k) = point.curveAmount.at(k).value_or(0.0);
        }
        curves.push_back(pointCurves);

        const auto found = inputs.statesOfSpatialPoint.find(point.id);
        if (found == inputs.statesOfSpatialPoint.end()) {
            continue;
        }
        if (found->second.size() != 1) {
            throw SoundSourceTransformError{describePoint(i, point.id) + " has " +
                                            std::to_string(found->second.size()) +
                                            " SpatialPositionStates; at most one is allowed"};
        }
        const SpatialStateRecord& state = found->second.front();
        states.push_back(SpatialState{.point = i,
            .azimuth = state.azimuth,
            .elevation = state.elevation,
            .distance = state.distance});
    }

    requireCompletePosition(states);
    requireSpatialValues(states);
    result.envelopes = spatialNodesOf(states, curves);
    return result;
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
    const std::array<std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>, 3>& stale,
    const SpatialTriple<PhysicalPoint>& points) {
    batch.beginUnit();
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        stageDeletes(batch, writes.timelines.at(kFirstSpatialTimeline + k), stale.at(k));
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        stageChain(batch, writes.timelines.at(kFirstSpatialTimeline + k), sourceId, points.at(k));
    }
}

/// Transforms one source's spatial position and stages it (§6.8, §7).
///
/// @param[in,out] batch   Batch the source's output is staged into, as one
///                        unit; it is written when the batch flushes.
/// @param writes          The batch's registered operations.
/// @param inputs          Everything read for the run.
/// @param source          The source.
/// @param maps            Every Score's resolved tempo maps.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the source's error, or its point counts,
///                        which stand unless the batch then fails to write
///                        the unit.
/// @return True when the source's output was staged.
bool transformSource(WriteBatch& batch, const Writes& writes, const Inputs& inputs,
    const SpatialSourceRecord& source, const ResolvedMaps& maps, SampleRate rate,
    SoundSourceTransformOutcome& outcome) {
    try {
        const auto score = inputs.scoreOfSource.find(source.sourceId);
        if (score == inputs.scoreOfSource.end()) {
            throw SoundSourceTransformError{"it is not reached from any Score through SoundEntity"};
        }
        if (!maps.rootOfScore.contains(score->second)) {
            throw SoundSourceTransformError{"its Score " + std::to_string(score->second) +
                                            " has no TempoMap, so its musical time is undefined"};
        }
        const SourceEnvelopes envelopes = gatherSource(inputs, source, score->second, maps, rate);
        const SpatialTriple<PhysicalPoint> points =
            placeSpatialNodes(envelopes.envelopes, envelopes.pointTimes);

        std::array<std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>, 3> stale;
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const auto& existing = inputs.existingSpatial.at(k);
            if (const auto timelines = existing.find(source.sourceId);
                timelines != existing.end()) {
                for (const ExistingTimeline& timeline : timelines->second) {
                    stale.at(k).emplace_back(timeline.timelineId,
                        walkList<SoundSourceTransformError>(timeline.head,
                            inputs.spatialPhysicalNext.at(k),
                            "its existing " +
                                std::string{spatialTimelineName(kSpatialKinds.at(k))}));
                }
            }
        }
        stageSource(batch, writes, source.sourceId, stale, points);
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            outcome.points.at(k) = points.at(k).size();
        }
        return true;
    } catch (const SoundSourceTransformError& e) {
        outcome.error = e.what();
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pure stages
// ---------------------------------------------------------------------------

void requireSharedEndPoints(
    std::span<const EnvelopeNode> frequency, std::span<const EnvelopeNode> amplitude) {
    const auto requireTwo = [](std::span<const EnvelopeNode> nodes, std::string_view property) {
        if (nodes.size() < 2) {
            throw PartialTransformError{"its " + std::string{property} + " is defined at " +
                                        std::to_string(nodes.size()) +
                                        " point(s); at least two are required"};
        }
    };
    requireTwo(frequency, "frequency");
    requireTwo(amplitude, "amplitude");
    if (frequency.front().point != amplitude.front().point) {
        throw PartialTransformError{
            "its frequency is first defined at point " + std::to_string(frequency.front().point) +
            " but its amplitude at point " + std::to_string(amplitude.front().point) +
            "; both must be defined at the partial's first point"};
    }
    if (frequency.back().point != amplitude.back().point) {
        throw PartialTransformError{
            "its frequency is last defined at point " + std::to_string(frequency.back().point) +
            " but its amplitude at point " + std::to_string(amplitude.back().point) +
            "; both must be defined at the partial's last point"};
    }
}

std::vector<PhysicalPoint> placeNodes(
    std::span<const EnvelopeNode> nodes, std::span<const SampleTime> pointTimes) {
    std::vector<SampleTime> times;
    times.reserve(nodes.size());
    for (const EnvelopeNode& node : nodes) {
        times.push_back(pointTimes[node.point]);
    }
    std::vector<PhysicalPoint> points;
    for (const Placement& placement : squash(times)) {
        const EnvelopeNode& node = nodes[placement.index];
        points.push_back(PhysicalPoint{.time = placement.time,
            .value = node.value,
            .curveType = node.curveType,
            .curveAmount = node.curveAmount});
    }
    return points;
}

void fitToLifeline(std::vector<PhysicalPoint>& amplitude, SampleTime end) {
    PhysicalPoint last = amplitude.back();
    last.time = end;
    amplitude.pop_back();
    std::erase_if(amplitude, [end](const PhysicalPoint& point) { return point.time >= end; });
    amplitude.push_back(last);
}

void requireStatesWithinLifeline(
    std::span<const EnvelopeNode> frequency, std::span<const StatePhase> states) {
    // The caller has checked that frequency has at least two envelope nodes, and each
    // sits on a point where the partial has a state, so states is not empty.
    if (states.empty() || states.front().point != frequency.front().point) {
        throw PartialTransformError{
            "it has a state at point " +
            (states.empty() ? std::string{"(none)"} : std::to_string(states.front().point)) +
            ", before its frequency is first defined at point " +
            std::to_string(frequency.front().point) +
            "; every state must lie within the lifeline the frequency defines"};
    }
    if (states.back().point != frequency.back().point) {
        throw PartialTransformError{
            "it has a state at point " + std::to_string(states.back().point) +
            ", after its frequency is last defined at point " +
            std::to_string(frequency.back().point) +
            "; every state must lie within the lifeline the frequency defines"};
    }
}

void requireValidPhase(std::span<const StatePhase> states) {
    for (const StatePhase& state : states) {
        const std::string where = "at point " + std::to_string(state.point);
        if (state.degrees.has_value()) {
            if (state.freeRun != false) {
                throw PartialTransformError{where + " its phase is " +
                                            formatNumber(*state.degrees) +
                                            " degrees with phase_free_run " +
                                            (state.freeRun.has_value() ? "true" : "NULL") +
                                            "; a phase target needs phase_free_run false"};
            }
            if (!std::isfinite(*state.degrees) || *state.degrees < kMinPhaseDegrees ||
                *state.degrees > kMaxPhaseDegrees) {
                throw PartialTransformError{
                    where + " its phase is " + formatNumber(*state.degrees) +
                    " degrees; a target must be in [" + formatNumber(kMinPhaseDegrees) + ", " +
                    formatNumber(kMaxPhaseDegrees) + "]"};
            }
        } else if (state.freeRun == false) {
            throw PartialTransformError{
                where + " its phase_free_run is false with no phase; false needs a phase target, "
                        "true means free-running, and NULL plays no role in phase"};
        }
    }
}

std::vector<PhaseEnvelopeNode> phaseNodesOf(std::span<const StatePhase> states) {
    std::vector<PhaseEnvelopeNode> nodes;
    for (const StatePhase& state : states) {
        const bool isTarget = state.degrees.has_value();
        const bool isFree = state.freeRun == true;
        // A default first state still gives a NULL point: precomputation
        // needs one at the lifeline's start, and running free to the first
        // target is what the default means there.
        if (isTarget || isFree || nodes.empty()) {
            nodes.push_back(PhaseEnvelopeNode{.point = state.point, .degrees = state.degrees});
        }
    }
    return nodes;
}

std::vector<PhasePoint> placePhaseNodes(
    std::span<const PhaseEnvelopeNode> nodes, std::span<const SampleTime> pointTimes) {
    std::vector<SampleTime> times;
    times.reserve(nodes.size());
    for (const PhaseEnvelopeNode& node : nodes) {
        times.push_back(pointTimes[node.point]);
    }
    std::vector<PhasePoint> points;
    for (const Placement& placement : squash(times)) {
        const PhaseEnvelopeNode& node = nodes[placement.index];
        points.push_back(PhasePoint{.time = placement.time,
            .phase = node.degrees.has_value()
                         ? std::optional<Phase>{toPhase(Degrees{*node.degrees})}
                         : std::nullopt});
    }
    return points;
}

void fitToLifeline(std::vector<PhasePoint>& phase, SampleTime end) {
    PhasePoint last = phase.back();
    last.time = std::min(last.time, end);
    phase.pop_back();
    std::erase_if(phase, [end](const PhasePoint& point) { return point.time >= end; });
    phase.push_back(last);
}

void collapseFreeRuns(std::vector<PhasePoint>& phase) {
    std::vector<PhasePoint> kept;
    kept.reserve(phase.size());
    for (const PhasePoint& point : phase) {
        // The last two kept are both NULL: the later one is the middle of a
        // run, superseded by this point as the run's end.
        const std::size_t n = kept.size();
        if (!point.phase.has_value() && n >= 2 && !kept[n - 1].phase.has_value() &&
            !kept[n - 2].phase.has_value()) {
            kept.back() = point;
        } else {
            kept.push_back(point);
        }
    }
    phase = std::move(kept);
}

void requireCompletePosition(std::span<const SpatialState> states) {
    if (states.empty() || states.front().point != 0) {
        throw SoundSourceTransformError{
            "point 0 has no SpatialPositionState; the first point must define azimuth, "
            "elevation and distance"};
    }
    const SpatialState& first = states.front();
    std::string missing;
    const auto note = [&missing](const std::optional<double>& value, std::string_view name) {
        if (!value.has_value()) {
            missing += missing.empty() ? "" : ", ";
            missing += name;
        }
    };
    note(first.azimuth, "azimuth");
    note(first.elevation, "elevation");
    note(first.distance, "distance");
    if (!missing.empty()) {
        throw SoundSourceTransformError{"at point 0 its " + missing +
                                        " is NULL; the first point must define azimuth, "
                                        "elevation and distance"};
    }
}

void requireSpatialValues(std::span<const SpatialState> states) {
    const auto check = [](const SpatialState& state, const std::optional<double>& value,
                           std::string_view name, std::string_view unit, double low, double high) {
        if (value.has_value() && (!std::isfinite(*value) || *value < low || *value > high)) {
            throw SoundSourceTransformError{"at point " + std::to_string(state.point) + " its " +
                                            std::string{name} + " is " + formatNumber(*value) +
                                            " " + std::string{unit} + "; it must be in [" +
                                            formatNumber(low) + ", " + formatNumber(high) + "]"};
        }
    };
    for (const SpatialState& state : states) {
        check(state, state.azimuth, "azimuth", "degrees", kMinAzimuthDegrees, kMaxAzimuthDegrees);
        check(state, state.elevation, "elevation", "degrees", kMinElevationDegrees,
            kMaxElevationDegrees);
        check(state, state.distance, "distance", "metres", kMinDistanceMetres, kMaxDistanceMetres);
    }
}

SpatialTriple<EnvelopeNode> spatialNodesOf(
    std::span<const SpatialState> states, std::span<const SpatialCurves> curves) {
    SpatialTriple<EnvelopeNode> envelopes;
    for (const SpatialState& state : states) {
        const SpatialCurves& curve = curves[state.point];
        const std::array<std::optional<double>, 3> values{
            state.azimuth, state.elevation, state.distance};
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const std::optional<double> value = values.at(k);
            if (value.has_value()) {
                envelopes.at(k).push_back(EnvelopeNode{.point = state.point,
                    .value = *value,
                    .curveType = curve.curveType.at(k),
                    .curveAmount = curve.curveAmount.at(k)});
            }
        }
    }
    return envelopes;
}

SpatialTriple<PhysicalPoint> placeSpatialNodes(
    const SpatialTriple<EnvelopeNode>& envelopes, std::span<const SampleTime> pointTimes) {
    SpatialTriple<PhysicalPoint> points;
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        points.at(k) = placeNodes(envelopes.at(k), pointTimes);
    }
    for (PhysicalPoint& point : points.at(0)) {
        point.value = toAzimuth(AzimuthDegrees{point.value}).value();
    }
    for (PhysicalPoint& point : points.at(1)) {
        point.value = toElevation(ElevationDegrees{point.value}).value();
    }
    return points;
}

// ---------------------------------------------------------------------------
// Reports and the run
// ---------------------------------------------------------------------------

std::size_t PartialSetTransformOutcome::failureCount() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(partials, [](const PartialTransformOutcome& p) { return !p.ok(); }));
}

std::size_t TransformReport::partialCount() const noexcept {
    std::size_t total = 0;
    for (const PartialSetTransformOutcome& set : sets) {
        total += set.partials.size();
    }
    return total;
}

std::size_t TransformReport::failureCount() const noexcept {
    std::size_t total = 0;
    for (const PartialSetTransformOutcome& set : sets) {
        total += set.error.has_value() ? 1 : set.failureCount();
    }
    total += static_cast<std::size_t>(std::ranges::count_if(
        sources, [](const SoundSourceTransformOutcome& s) { return !s.ok(); }));
    return total;
}

TransformReport transformAll(EngineDb& db) {
    const std::vector<DbRow> config = db.query(kReadConfig);
    if (config.size() != 1) {
        throw TransformError{
            "expected exactly one EngineConfig, found " + std::to_string(config.size())};
    }
    const std::optional<std::int64_t> sampleRate = optionalInt64(config.front().at(0));
    if (!sampleRate.has_value() || *sampleRate <= 0 ||
        *sampleRate > std::numeric_limits<std::int32_t>::max()) {
        throw TransformError{
            "EngineConfig.sampleRate is " +
            (sampleRate.has_value() ? std::to_string(*sampleRate) : std::string{"NULL"}) +
            "; it must be positive"};
    }
    TransformReport report;
    report.sampleRate = SampleRate{static_cast<std::int32_t>(*sampleRate)};

    const Inputs inputs = readInputs(db);
    const ResolvedMaps maps = buildTempoMaps(inputs, report);
    WriteBatch batch{db};
    const Writes writes = registerWrites(batch);

    // Each staged partial's place in the report, as (set, partial) indexes,
    // and each staged source's index, in the order of their units. A unit the
    // batch fails to write takes back the counts staging recorded.
    std::vector<std::pair<std::size_t, std::size_t>> stagedPartials;
    std::vector<std::size_t> stagedSources;
    const auto flush = [&] {
        const std::vector<std::optional<std::string>> results = batch.flush();
        for (std::size_t unit = 0; unit < results.size(); ++unit) {
            if (!results[unit].has_value()) {
                continue;
            }
            const std::string error = "engine-db: " + *results[unit];
            if (unit < stagedPartials.size()) {
                const auto [set, index] = stagedPartials[unit];
                PartialTransformOutcome& partial = report.sets.at(set).partials.at(index);
                partial.error = error;
                partial.frequencyPoints = 0;
                partial.amplitudePoints = 0;
                partial.phasePoints = 0;
            } else {
                SoundSourceTransformOutcome& source =
                    report.sources.at(stagedSources.at(unit - stagedPartials.size()));
                source.error = error;
                source.points = {};
            }
        }
        stagedPartials.clear();
        stagedSources.clear();
    };

    // Sets are independent; id order only makes the report deterministic.
    for (const MusicalSetRecord& set : inputs.sets) {
        report.sets.push_back(PartialSetTransformOutcome{
            .setId = set.setId, .setName = set.name, .error = std::nullopt, .partials = {}});
        const std::size_t setIndex = report.sets.size() - 1;
        transformSet(batch, writes, inputs, set, maps, report.sampleRate, report.sets.back(),
            [&](std::size_t partialIndex) {
                stagedPartials.emplace_back(setIndex, partialIndex);
                if (stagedPartials.size() == kOwnersPerFlush) {
                    flush();
                }
            });
    }
    flush();
    // Sources are independent of sets and of each other (§6.8).
    for (const SpatialSourceRecord& source : inputs.sources) {
        report.sources.push_back(SoundSourceTransformOutcome{.sourceId = source.sourceId,
            .sourceName = source.name,
            .error = std::nullopt,
            .points = {}});
        if (transformSource(
                batch, writes, inputs, source, maps, report.sampleRate, report.sources.back())) {
            stagedSources.push_back(report.sources.size() - 1);
            if (stagedSources.size() == kOwnersPerFlush) {
                flush();
            }
        }
    }
    flush();
    return report;
}

} // namespace rainbohz::rendering_engine::transform
