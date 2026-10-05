#include "rainbohz/rendering_engine/transform/transform.hpp"

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/transform/squash.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
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
///   - the tempo maps, built and validated once per Score;
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

using IdMap = std::unordered_map<std::int64_t, std::int64_t>;

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

constexpr std::string_view kReadConfig = "MATCH (c:EngineConfig) RETURN c.sampleRate;";

// A Score and the TempoMap that defines its time zero, if it has one.
// NEXT_POINT is ONE_ONE per pair, so there is at most one row per Score.
constexpr std::string_view kReadScores =
    "MATCH (s:Score) OPTIONAL MATCH (s)-[:NEXT_POINT]->(m:TempoMap) "
    "RETURN s.id, m.id, m.name, m.ticks_per_beat ORDER BY s.id;";

constexpr std::string_view kReadTempoMapHeads =
    "MATCH (m:TempoMap) OPTIONAL MATCH (m)-[:NEXT_POINT]->(p:TempoMapPoint) "
    "RETURN m.id, p.id;";

constexpr std::string_view kReadTempoMapPoints =
    "MATCH (p:TempoMapPoint) RETURN p.id, p.position, p.start_tempo, p.hold;";

constexpr std::string_view kReadTempoMapLinks =
    "MATCH (a:TempoMapPoint)-[:NEXT_POINT]->(b:TempoMapPoint) RETURN a.id, b.id;";

constexpr std::string_view kCountSyncEdges =
    "MATCH ()-[r:TEMPO_MAP_SYNCHRONIZATION_POINT]->() RETURN count(r);";

// Every PartialSet with a musical timeline, with the head of the timeline's
// point list, or NULL when it has no points.
constexpr std::string_view kReadMusicalSets =
    "MATCH (ps:PartialSet)-[:HAS_MUSICAL_TIMELINE]->(t:PartialSetMusicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:PartialSetMusicalTimelinePoint) "
    "RETURN ps.id, ps.name, t.id, p.id ORDER BY ps.id;";

constexpr std::string_view kReadSetScores =
    "MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(:SoundSource)"
    "-[:HAS_PARTIALSET]->(ps:PartialSet) RETURN ps.id, s.id;";

constexpr std::string_view kReadSetMembers =
    "MATCH (ps:PartialSet)-[:HAS_PARTIAL]->(p:Partial) RETURN ps.id, p.id, p.name ORDER BY p.id;";

constexpr std::string_view kReadMusicalPoints =
    "MATCH (p:PartialSetMusicalTimelinePoint) OPTIONAL MATCH (p)-[a:POINT_AT]->(m:TempoMap) "
    "RETURN p.id, p.frequency_curve_type, p.frequency_curve_amount, "
    "p.amplitude_curve_type, p.amplitude_curve_amount, m.id, a.sync_point, a.offset_seconds;";

constexpr std::string_view kReadMusicalLinks =
    "MATCH (a:PartialSetMusicalTimelinePoint)-[:NEXT_POINT]->(b:PartialSetMusicalTimelinePoint) "
    "RETURN a.id, b.id;";

// The states a timeline point defines, each with the partial it belongs to.
constexpr std::string_view kReadStates =
    "MATCH (p:PartialSetMusicalTimelinePoint)-[:POINT_STATE]->(:PartialStateSet)"
    "<-[:MEMBER_OF_SET_MANY_ONE]-(st:PartialState)-[:STATE_OF]->(partial:Partial) "
    "RETURN p.id, st.id, partial.id, st.frequency, st.amplitude, st.phase, st.phase_free_run;";

// Output from an earlier run, or physical-time input, which is replaced.
constexpr std::string_view kReadExistingFrequency =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint) "
    "RETURN p.id, t.id, f.id;";

constexpr std::string_view kReadFrequencyLinks =
    "MATCH (a:FrequencyPhysicalTimelinePoint)-[:NEXT_POINT]->(b:FrequencyPhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

constexpr std::string_view kReadExistingAmplitude =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint) "
    "RETURN p.id, t.id, a.id;";

constexpr std::string_view kReadAmplitudeLinks =
    "MATCH (a:AmplitudePhysicalTimelinePoint)-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

constexpr std::string_view kReadExistingPhase =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint) "
    "RETURN p.id, t.id, f.id;";

constexpr std::string_view kReadPhaseLinks =
    "MATCH (a:PhasePhysicalTimelinePoint)-[:NEXT_POINT]->(b:PhasePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

// Deleting a point removes its edges, including any FRAME_ENTRY_PHYSICAL_TIMELINE
// edge from an earlier precomputation; precomputation rebuilds its entries.
constexpr std::string_view kDeleteFrequencyPoints =
    "UNWIND $ids AS id MATCH (f:FrequencyPhysicalTimelinePoint {id: id}) DETACH DELETE f;";

constexpr std::string_view kDeleteFrequencyTimeline =
    "MATCH (t:FrequencyPhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;";

constexpr std::string_view kDeleteAmplitudePoints =
    "UNWIND $ids AS id MATCH (a:AmplitudePhysicalTimelinePoint {id: id}) DETACH DELETE a;";

constexpr std::string_view kDeleteAmplitudeTimeline =
    "MATCH (t:AmplitudePhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;";

constexpr std::string_view kDeletePhasePoints =
    "UNWIND $ids AS id MATCH (f:PhasePhysicalTimelinePoint {id: id}) DETACH DELETE f;";

constexpr std::string_view kDeletePhaseTimeline =
    "MATCH (t:PhasePhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;";

constexpr std::string_view kCreateFrequencyTimeline =
    "MATCH (p:Partial) WHERE p.id = $owner "
    "CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) RETURN t.id;";

constexpr std::string_view kCreateAmplitudeTimeline =
    "MATCH (p:Partial) WHERE p.id = $owner "
    "CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) RETURN t.id;";

constexpr std::string_view kCreatePhaseTimeline =
    "MATCH (p:Partial) WHERE p.id = $owner "
    "CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline) RETURN t.id;";

// One point, linked from the node before it: the timeline for the first
// point, the previous point after that. Two fixed variants per property, so
// that EngineDb prepares each once. The column types differ between the two
// point tables (frequency DOUBLE, amplitude FLOAT), and the curve columns
// are INT8 and FLOAT, so the bound values are cast explicitly.
constexpr std::string_view kCreateFirstFrequencyPoint =
    "MATCH (prev:FrequencyPhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint {time: $time, "
    "frequency: $value, frequency_curve_type: CAST($curveType AS INT8), "
    "frequency_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN f.id;";

constexpr std::string_view kCreateNextFrequencyPoint =
    "MATCH (prev:FrequencyPhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint {time: $time, "
    "frequency: $value, frequency_curve_type: CAST($curveType AS INT8), "
    "frequency_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN f.id;";

constexpr std::string_view kCreateFirstAmplitudePoint =
    "MATCH (prev:AmplitudePhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint {time: $time, "
    "amplitude: CAST($value AS FLOAT), amplitude_curve_type: CAST($curveType AS INT8), "
    "amplitude_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN a.id;";

constexpr std::string_view kCreateNextAmplitudePoint =
    "MATCH (prev:AmplitudePhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint {time: $time, "
    "amplitude: CAST($value AS FLOAT), amplitude_curve_type: CAST($curveType AS INT8), "
    "amplitude_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN a.id;";

// Phase points have no curve, and their phase may be NULL. A parameter cannot
// be NULL, so a NULL point has its own statements, which leave the property
// unset.
constexpr std::string_view kCreateFirstPhasePoint =
    "MATCH (prev:PhasePhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint {time: $time, phase: $phase}) "
    "RETURN f.id;";

constexpr std::string_view kCreateNextPhasePoint =
    "MATCH (prev:PhasePhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint {time: $time, phase: $phase}) "
    "RETURN f.id;";

constexpr std::string_view kCreateFirstFreePhasePoint =
    "MATCH (prev:PhasePhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint {time: $time}) RETURN f.id;";

constexpr std::string_view kCreateNextFreePhasePoint =
    "MATCH (prev:PhasePhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint {time: $time}) RETURN f.id;";

// Every SoundSource with a spatial musical timeline, with the head of the
// timeline's point list, or NULL when it has no points (§6.8).
constexpr std::string_view kReadSpatialSources =
    "MATCH (ss:SoundSource)-[:HAS_MUSICAL_TIMELINE]->(t:SpatialPositionMusicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:SpatialPositionMusicalTimelinePoint) "
    "RETURN ss.id, ss.name, t.id, p.id ORDER BY ss.id;";

constexpr std::string_view kReadSourceScores =
    "MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(ss:SoundSource) "
    "RETURN ss.id, s.id;";

constexpr std::string_view kReadSpatialMusicalPoints =
    "MATCH (p:SpatialPositionMusicalTimelinePoint) OPTIONAL MATCH (p)-[a:POINT_AT]->(m:TempoMap) "
    "RETURN p.id, p.azimuth_curve_type, p.azimuth_curve_amount, p.elevation_curve_type, "
    "p.elevation_curve_amount, p.distance_curve_type, p.distance_curve_amount, m.id, "
    "a.sync_point, a.offset_seconds;";

constexpr std::string_view kReadSpatialMusicalLinks =
    "MATCH (a:SpatialPositionMusicalTimelinePoint)-[:NEXT_POINT]->"
    "(b:SpatialPositionMusicalTimelinePoint) RETURN a.id, b.id;";

// POINT_STATE is MANY_ONE, so a point has at most one state.
constexpr std::string_view kReadSpatialStates =
    "MATCH (p:SpatialPositionMusicalTimelinePoint)-[:POINT_STATE]->(st:SpatialPositionState) "
    "RETURN p.id, st.id, st.azimuth, st.elevation, st.distance;";

// The physical spatial timelines, one statement of each shape per kind, in
// kSpatialKinds order. As for partials, existing ones are replaced.
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

constexpr std::array<std::string_view, 3> kReadSpatialPhysicalLinks{
    "MATCH (a:AzimuthPhysicalTimelinePoint)-[:NEXT_POINT]->(b:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:ElevationPhysicalTimelinePoint)-[:NEXT_POINT]->(b:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:DistancePhysicalTimelinePoint)-[:NEXT_POINT]->(b:DistancePhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
};

constexpr std::array<std::string_view, 3> kDeleteSpatialPoints{
    "UNWIND $ids AS id MATCH (p:AzimuthPhysicalTimelinePoint {id: id}) DETACH DELETE p;",
    "UNWIND $ids AS id MATCH (p:ElevationPhysicalTimelinePoint {id: id}) DETACH DELETE p;",
    "UNWIND $ids AS id MATCH (p:DistancePhysicalTimelinePoint {id: id}) DETACH DELETE p;",
};

constexpr std::array<std::string_view, 3> kDeleteSpatialTimeline{
    "MATCH (t:AzimuthPhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;",
    "MATCH (t:ElevationPhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;",
    "MATCH (t:DistancePhysicalTimeline) WHERE t.id = $timeline DETACH DELETE t;",
};

constexpr std::array<std::string_view, 3> kCreateSpatialTimeline{
    "MATCH (ss:SoundSource) WHERE ss.id = $owner "
    "CREATE (ss)-[:HAS_PHYSICAL_TIMELINE]->(t:AzimuthPhysicalTimeline) RETURN t.id;",
    "MATCH (ss:SoundSource) WHERE ss.id = $owner "
    "CREATE (ss)-[:HAS_PHYSICAL_TIMELINE]->(t:ElevationPhysicalTimeline) RETURN t.id;",
    "MATCH (ss:SoundSource) WHERE ss.id = $owner "
    "CREATE (ss)-[:HAS_PHYSICAL_TIMELINE]->(t:DistancePhysicalTimeline) RETURN t.id;",
};

// As for frequency and amplitude, two fixed variants per kind. Azimuth and
// elevation are DOUBLE columns, distance FLOAT.
constexpr std::array<std::string_view, 3> kCreateFirstSpatialPoint{
    "MATCH (prev:AzimuthPhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:AzimuthPhysicalTimelinePoint {time: $time, "
    "azimuth: $value, azimuth_curve_type: CAST($curveType AS INT8), "
    "azimuth_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
    "MATCH (prev:ElevationPhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:ElevationPhysicalTimelinePoint {time: $time, "
    "elevation: $value, elevation_curve_type: CAST($curveType AS INT8), "
    "elevation_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
    "MATCH (prev:DistancePhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:DistancePhysicalTimelinePoint {time: $time, "
    "distance: CAST($value AS FLOAT), distance_curve_type: CAST($curveType AS INT8), "
    "distance_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
};

constexpr std::array<std::string_view, 3> kCreateNextSpatialPoint{
    "MATCH (prev:AzimuthPhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:AzimuthPhysicalTimelinePoint {time: $time, "
    "azimuth: $value, azimuth_curve_type: CAST($curveType AS INT8), "
    "azimuth_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
    "MATCH (prev:ElevationPhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:ElevationPhysicalTimelinePoint {time: $time, "
    "elevation: $value, elevation_curve_type: CAST($curveType AS INT8), "
    "elevation_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
    "MATCH (prev:DistancePhysicalTimelinePoint) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(p:DistancePhysicalTimelinePoint {time: $time, "
    "distance: CAST($value AS FLOAT), distance_curve_type: CAST($curveType AS INT8), "
    "distance_curve_amount: CAST($curveAmount AS FLOAT)}) RETURN p.id;",
};

// ---------------------------------------------------------------------------
// Limits from the schema (engine-schema.cypher, "Units for specification of
// a score", and the curve enumerations)
// ---------------------------------------------------------------------------

constexpr std::int64_t kMinCurveType = 0;
constexpr std::int64_t kMaxCurveType = 4;
constexpr double kMinCurveAmount = 0.0;
constexpr double kMaxCurveAmount = 1.0;
constexpr double kMinAmplitude = -144.0;
constexpr double kMaxAmplitude = 0.0;
constexpr double kMinPhaseDegrees = -180.0;
constexpr double kMaxPhaseDegrees = 180.0;
constexpr double kMinAzimuthDegrees = -180.0;
constexpr double kMaxAzimuthDegrees = 180.0;
constexpr double kMinElevationDegrees = -90.0;
constexpr double kMaxElevationDegrees = 90.0;
constexpr double kMinDistanceMetres = 1.0;
constexpr double kMaxDistanceMetres = 16384.0;

// ---------------------------------------------------------------------------
// Inputs, as read in bulk before anything is written
// ---------------------------------------------------------------------------

/// A Score and its TempoMap, as read.
struct ScoreRecord {
    std::int64_t scoreId = 0;
    std::optional<std::int64_t> tempoMapId;
    std::optional<std::string> tempoMapName;
    std::optional<std::int64_t> ticksPerBeat;
};

/// A PartialSet with a musical timeline, as read.
struct MusicalSetRecord {
    std::int64_t setId = 0;
    std::optional<std::string> name;

    /// Each PartialSetMusicalTimeline of the set with the head of its point
    /// list (nullopt for an empty one). The schema allows one per set; a
    /// second is reported rather than assumed away.
    std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> timelines;
};

/// A musical timeline point's POINT_AT, as read.
struct PointAtRecord {
    /// The TempoMap its POINT_AT leads to; nullopt when it has none.
    std::optional<std::int64_t> tempoMapId;
    std::optional<std::int64_t> syncPoint;
    std::optional<double> offsetSeconds;
};

/// A PartialSetMusicalTimelinePoint with its POINT_AT, as read.
struct MusicalPointRecord {
    std::int64_t id = 0;
    std::optional<std::int64_t> frequencyCurveType;
    std::optional<double> frequencyCurveAmount;
    std::optional<std::int64_t> amplitudeCurveType;
    std::optional<double> amplitudeCurveAmount;
    PointAtRecord at;
};

/// A SoundSource with a spatial musical timeline, as read.
struct SpatialSourceRecord {
    std::int64_t sourceId = 0;
    std::optional<std::string> name;

    /// Each SpatialPositionMusicalTimeline of the source with the head of
    /// its point list (nullopt for an empty one). The schema allows one.
    std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> timelines;
};

/// A SpatialPositionMusicalTimelinePoint with its POINT_AT, as read; the
/// curve fields in kSpatialKinds order.
struct SpatialMusicalPointRecord {
    std::int64_t id = 0;
    std::array<std::optional<std::int64_t>, 3> curveType;
    std::array<std::optional<double>, 3> curveAmount;
    PointAtRecord at;
};

/// A SpatialPositionState reached from a timeline point, as read.
struct SpatialStateRecord {
    std::int64_t stateId = 0;
    std::optional<double> azimuth;
    std::optional<double> elevation;
    std::optional<double> distance;
};

/// A PartialState reached from a timeline point, as read.
struct StateRecord {
    std::int64_t stateId = 0;
    std::int64_t partialId = 0;
    std::optional<double> frequency;
    std::optional<double> amplitude;

    /// Degrees; with phaseFreeRun, one of three meanings (§6.6).
    std::optional<double> phase;
    std::optional<bool> phaseFreeRun;
};

/// A partial's existing physical timeline of one kind, with its point ids in
/// list order, to be deleted.
struct ExistingTimeline {
    std::int64_t timelineId = 0;
    std::optional<std::int64_t> head;
};

/// Everything the run reads, for every set at once.
struct Inputs {
    std::vector<ScoreRecord> scores;
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> tempoMapHeads;
    std::unordered_map<std::int64_t, TempoMapPointRecord> tempoMapPoints;
    IdMap tempoMapNext;
    std::int64_t syncEdges = 0;

    std::vector<MusicalSetRecord> sets;
    std::unordered_map<std::int64_t, std::int64_t> scoreOfSet;
    std::unordered_map<std::int64_t,
        std::vector<std::pair<std::int64_t, std::optional<std::string>>>>
        membersOfSet;
    std::unordered_map<std::int64_t, MusicalPointRecord> musicalPoints;
    IdMap musicalNext;
    std::unordered_map<std::int64_t, std::vector<StateRecord>> statesOfPoint;

    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingFrequency;
    IdMap frequencyNext;
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingAmplitude;
    IdMap amplitudeNext;
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingPhase;
    IdMap phaseNext;

    std::vector<SpatialSourceRecord> sources;
    std::unordered_map<std::int64_t, std::int64_t> scoreOfSource;
    std::unordered_map<std::int64_t, SpatialMusicalPointRecord> spatialPoints;
    IdMap spatialMusicalNext;
    std::unordered_map<std::int64_t, std::vector<SpatialStateRecord>> statesOfSpatialPoint;

    /// Per kind, in kSpatialKinds order: each source's existing physical
    /// timelines, and the links between their points.
    std::array<std::unordered_map<std::int64_t, std::vector<ExistingTimeline>>, 3> existingSpatial;
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
            .tempoMapName = optionalString(row.at(2)),
            .ticksPerBeat = optionalInt64(row.at(3)),
        });
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
    inputs.syncEdges = requireInt64(db.query(kCountSyncEdges).at(0).at(0));

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

/// A Score's validated TempoMap, with its id.
struct ScoreTempoMap {
    std::int64_t mapId = 0;
    TempoMap map;
};

/// Builds and validates the TempoMap of every Score that has one (§6.1).
///
/// @param inputs       Everything read for the run.
/// @param[out] report  Receives one TempoMapSummary per map, in Score order.
/// @return Each Score's map, by Score.id. Scores without one are absent.
/// @throws TransformError  If any map is invalid, or any synchronisation edge
///         exists.
std::map<std::int64_t, ScoreTempoMap> buildTempoMaps(
    const Inputs& inputs, TransformReport& report) {
    if (inputs.syncEdges != 0) {
        throw TransformError{"engine-db holds " + std::to_string(inputs.syncEdges) +
                             " TEMPO_MAP_SYNCHRONIZATION_POINT edge(s); synchronised tempo "
                             "maps are not supported by this version, which converts through "
                             "one TempoMap per Score"};
    }
    std::map<std::int64_t, ScoreTempoMap> maps;
    for (const ScoreRecord& score : inputs.scores) {
        if (!score.tempoMapId.has_value()) {
            continue;
        }
        const std::int64_t mapId = *score.tempoMapId;
        const std::string what =
            "TempoMap " + std::to_string(mapId) + " of Score " + std::to_string(score.scoreId);
        const auto heads = inputs.tempoMapHeads.find(mapId);
        const std::optional<std::int64_t> head =
            heads == inputs.tempoMapHeads.end() || heads->second.empty() ? std::nullopt
                                                                         : heads->second.front();
        std::vector<TempoMapPointRecord> records;
        try {
            for (const std::int64_t id : walkList<TempoMapError>(head, inputs.tempoMapNext, what)) {
                records.push_back(inputs.tempoMapPoints.at(id));
            }
            TempoMap map = TempoMap::fromRecords(score.ticksPerBeat, records);
            report.tempoMaps.push_back(TempoMapSummary{
                .scoreId = score.scoreId,
                .tempoMapId = mapId,
                .name = score.tempoMapName,
                .ticksPerBeat = map.ticksPerBeat().value(),
                .pointCount = map.points().size(),
            });
            maps.emplace(score.scoreId, ScoreTempoMap{.mapId = mapId, .map = std::move(map)});
        } catch (const TempoMapError& e) {
            throw TransformError{what + ": " + e.what()};
        }
    }
    return maps;
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

/// Validates one point's POINT_AT against its predecessor and the Score's
/// map, and returns its physical time.
///
/// @tparam Error  The exception of the timeline's scope: PartialSetTransformError
///                or SoundSourceTransformError.
/// @param index         Position of the point in the timeline, from 0.
/// @param id            Its id.
/// @param at            Its POINT_AT as read.
/// @param mapId         The TempoMap every POINT_AT must lead to.
/// @param map           That map.
/// @param rate          Sample rate for the conversion to samples.
/// @param[in,out] previousSync  The previous point's sync_point; updated to
///                      this point's on return.
/// @return `toSampleTime(toSeconds(sync_point) + offset_seconds)`.
/// @throws Error  If the point has no POINT_AT, or one to another map, or a
///         NULL or negative sync_point, or a sync_point before the previous
///         point's, or a non-finite offset.
template <typename Error>
SampleTime pointTime(std::size_t index, std::int64_t id, const PointAtRecord& at,
    std::int64_t mapId, const TempoMap& map, SampleRate rate,
    std::optional<std::int64_t>& previousSync) {
    const std::string where = describePoint(index, id);
    if (!at.tempoMapId.has_value()) {
        throw Error{where + " has no POINT_AT"};
    }
    if (*at.tempoMapId != mapId) {
        throw Error{where + ": POINT_AT leads to TempoMap " + std::to_string(*at.tempoMapId) +
                    ", not the Score's TempoMap " + std::to_string(mapId)};
    }
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

    const SecondsTime seconds = map.toSeconds(MusicalTime{sync}) + SecondsDuration{offset};
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
/// @param inputs  Everything read for the run.
/// @param set     The set.
/// @param map     The TempoMap of the set's Score.
/// @param mapId   That map's id, which every POINT_AT must lead to.
/// @param rate    Sample rate for the conversion to samples.
/// @return The point times and the members' envelope nodes.
/// @throws PartialSetTransformError  If the set has no, or more than one,
///         timeline; the timeline has no points or loops; a point has no
///         POINT_AT, or one to another map, or a NULL or negative sync_point,
///         or a sync_point before the previous point's, or a non-finite
///         offset, or a curve field out of range; or a state belongs to a
///         partial outside the set, or two states at one point belong to the
///         same partial.
SetEnvelopes gatherSet(const Inputs& inputs, const MusicalSetRecord& set, const TempoMap& map,
    std::int64_t mapId, SampleRate rate) {
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
    std::optional<std::int64_t> previousSync;
    static const std::vector<StateRecord> kNoStates;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const MusicalPointRecord& point = inputs.musicalPoints.at(pointIds[i]);
        envelopes.pointTimes.push_back(pointTime<PartialSetTransformError>(
            i, point.id, point.at, mapId, map, rate, previousSync));
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
    std::int64_t partialId = 0;

    /// Existing timelines to delete, each with its point ids.
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> staleFrequency;
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> staleAmplitude;
    std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> stalePhase;

    /// The points to write, in physical units.
    std::vector<PhysicalPoint> frequency;
    std::vector<PhysicalPoint> amplitude;
    std::vector<PhasePoint> phase;
};

/// Writes one chain of points, linked from a freshly created timeline.
///
/// @tparam Error  The exception of the owner's scope: PartialTransformError
///                or SoundSourceTransformError.
/// @param[in,out] db     Database to write, inside the caller's transaction.
/// @param createTimeline  Statement creating the timeline for $owner and
///                        returning its id.
/// @param createFirst     Statement creating a point linked from the timeline.
/// @param createNext      Statement creating a point linked from a point.
/// @param ownerId         The partial or source the timeline belongs to.
/// @param owner           What the owner is, for the error message.
/// @param points          The points, in order.
/// @throws EngineDbError  If a statement fails.
/// @throws Error  If the owner is no longer in the database.
template <typename Error>
void writeChain(EngineDb& db, std::string_view createTimeline, std::string_view createFirst,
    std::string_view createNext, std::int64_t ownerId, std::string_view owner,
    std::span<const PhysicalPoint> points) {
    const std::vector<DbRow> created = db.query(createTimeline, {{"owner", ownerId}});
    if (created.size() != 1) {
        throw Error{"the " + std::string{owner} + " disappeared while it was being transformed"};
    }
    std::int64_t previous = requireInt64(created.front().at(0));
    bool first = true;
    for (const PhysicalPoint& point : points) {
        const std::vector<DbRow> rows =
            db.query(first ? createFirst : createNext, {
                                                           {"previous", previous},
                                                           {"time", point.time.value()},
                                                           {"value", point.value},
                                                           {"curveType", point.curveType},
                                                           {"curveAmount", point.curveAmount},
                                                       });
        if (rows.size() != 1) {
            throw EngineDbError{"creating a timeline point returned " +
                                std::to_string(rows.size()) + " rows, not 1"};
        }
        previous = requireInt64(rows.front().at(0));
        first = false;
    }
}

/// Writes a partial's phase points, linked from a fresh PhasePhysicalTimeline.
///
/// @param[in,out] db  Database to write, inside the caller's transaction.
/// @param partialId   The partial.
/// @param points      The points, in order; NULL ones are written with
///                    `phase` unset.
/// @throws EngineDbError  If a statement fails.
/// @throws PartialTransformError  If the partial is no longer in the
///         database.
void writePhaseChain(EngineDb& db, std::int64_t partialId, std::span<const PhasePoint> points) {
    const std::vector<DbRow> created = db.query(kCreatePhaseTimeline, {{"owner", partialId}});
    if (created.size() != 1) {
        throw PartialTransformError{"the partial disappeared while it was being transformed"};
    }
    std::int64_t previous = requireInt64(created.front().at(0));
    bool first = true;
    for (const PhasePoint& point : points) {
        DbParameters parameters{{"previous", previous}, {"time", point.time.value()}};
        std::string_view statement;
        if (point.phase.has_value()) {
            parameters.emplace_back("phase", point.phase->value());
            statement = first ? kCreateFirstPhasePoint : kCreateNextPhasePoint;
        } else {
            statement = first ? kCreateFirstFreePhasePoint : kCreateNextFreePhasePoint;
        }
        const std::vector<DbRow> rows = db.query(statement, parameters);
        if (rows.size() != 1) {
            throw EngineDbError{
                "creating a phase point returned " + std::to_string(rows.size()) + " rows, not 1"};
        }
        previous = requireInt64(rows.front().at(0));
        first = false;
    }
}

/// Replaces a partial's three physical timelines in one transaction (§7).
///
/// @param[in,out] db  Database to write. Modified only if every statement
///                    succeeds; otherwise the transaction is rolled back.
/// @param output      What to write.
/// @throws EngineDbError  If any statement fails. Nothing has been written.
/// @throws PartialTransformError  If the partial is no longer in the
///         database. Nothing has been written.
void writePartial(EngineDb& db, const PartialOutput& output) {
    Transaction transaction{db};

    for (const auto& [timelineId, pointIds] : output.staleFrequency) {
        if (!pointIds.empty()) {
            db.query(kDeleteFrequencyPoints, {{"ids", pointIds}});
        }
        db.query(kDeleteFrequencyTimeline, {{"timeline", timelineId}});
    }
    for (const auto& [timelineId, pointIds] : output.staleAmplitude) {
        if (!pointIds.empty()) {
            db.query(kDeleteAmplitudePoints, {{"ids", pointIds}});
        }
        db.query(kDeleteAmplitudeTimeline, {{"timeline", timelineId}});
    }
    for (const auto& [timelineId, pointIds] : output.stalePhase) {
        if (!pointIds.empty()) {
            db.query(kDeletePhasePoints, {{"ids", pointIds}});
        }
        db.query(kDeletePhaseTimeline, {{"timeline", timelineId}});
    }

    writeChain<PartialTransformError>(db, kCreateFrequencyTimeline, kCreateFirstFrequencyPoint,
        kCreateNextFrequencyPoint, output.partialId, "partial", output.frequency);
    writeChain<PartialTransformError>(db, kCreateAmplitudeTimeline, kCreateFirstAmplitudePoint,
        kCreateNextAmplitudePoint, output.partialId, "partial", output.amplitude);
    writePhaseChain(db, output.partialId, output.phase);

    transaction.commit();
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

/// Transforms and writes one partial (§6.2 to §6.6, §7).
///
/// @param[in,out] db    Database to write.
/// @param inputs        Everything read for the run.
/// @param envelopes     The set's point times and envelope nodes.
/// @param partialId     The partial.
/// @param rate          Sample rate, for the frequency conversion.
/// @param[out] outcome  On success, its point counts are set.
/// @throws PartialTransformError  If the partial's envelope nodes are invalid, its
///         lifeline collapses, or it disappears while being written. Nothing
///         has been written.
/// @throws EngineDbError  If writing fails. Nothing has been written.
void transformPartial(EngineDb& db, const Inputs& inputs, const SetEnvelopes& envelopes,
    std::int64_t partialId, SampleRate rate, PartialTransformOutcome& outcome) {
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

    writePartial(db, output);

    outcome.frequencyPoints = output.frequency.size();
    outcome.amplitudePoints = output.amplitude.size();
    outcome.phasePoints = output.phase.size();
}

/// Transforms one set: validates it, then its partials one by one.
///
/// @param[in,out] db      Database to write.
/// @param inputs          Everything read for the run.
/// @param set             The set.
/// @param maps            Every Score's TempoMap, by Score.id.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the set's error, or its partials'
///                        outcomes.
void transformSet(EngineDb& db, const Inputs& inputs, const MusicalSetRecord& set,
    const std::map<std::int64_t, ScoreTempoMap>& maps, SampleRate rate,
    PartialSetTransformOutcome& outcome) {
    SetEnvelopes envelopes;
    try {
        const auto score = inputs.scoreOfSet.find(set.setId);
        if (score == inputs.scoreOfSet.end()) {
            throw PartialSetTransformError{
                "it is not reached from any Score through SoundEntity and SoundSource"};
        }
        const auto map = maps.find(score->second);
        if (map == maps.end()) {
            throw PartialSetTransformError{"its Score " + std::to_string(score->second) +
                                           " has no TempoMap, so its musical time is undefined"};
        }
        envelopes = gatherSet(inputs, set, map->second.map, map->second.mapId, rate);
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
        try {
            transformPartial(db, inputs, envelopes, partialId, rate, partial);
        } catch (const PartialTransformError& e) {
            partial.error = e.what();
        } catch (const EngineDbError& e) {
            partial.error = std::string{"engine-db: "} + e.what();
        }
        outcome.partials.push_back(std::move(partial));
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
/// @param inputs  Everything read for the run.
/// @param source  The source.
/// @param map     The TempoMap of the source's Score.
/// @param mapId   That map's id, which every POINT_AT must lead to.
/// @param rate    Sample rate for the conversion to samples.
/// @return The point times and the envelopes.
/// @throws SoundSourceTransformError  If the source has no, or more than one,
///         timeline; the timeline has no points or loops; a point's POINT_AT
///         or curve fields are invalid (as for a set); a point has more than
///         one state; the first point does not define all three properties;
///         or a value is out of range.
SourceEnvelopes gatherSource(const Inputs& inputs, const SpatialSourceRecord& source,
    const TempoMap& map, std::int64_t mapId, SampleRate rate) {
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
    std::optional<std::int64_t> previousSync;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const SpatialMusicalPointRecord& point = inputs.spatialPoints.at(pointIds[i]);
        result.pointTimes.push_back(pointTime<SoundSourceTransformError>(
            i, point.id, point.at, mapId, map, rate, previousSync));
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

/// Replaces a source's three spatial physical timelines in one transaction
/// (§7).
///
/// @param[in,out] db  Database to write. Modified only if every statement
///                    succeeds; otherwise the transaction is rolled back.
/// @param sourceId    The source.
/// @param stale       Per kind, the existing timelines to delete, each with
///                    its point ids.
/// @param points      Per kind, the points to write, in physical units.
/// @throws EngineDbError  If any statement fails. Nothing has been written.
/// @throws SoundSourceTransformError  If the source is no longer in the
///         database. Nothing has been written.
void writeSource(EngineDb& db, std::int64_t sourceId,
    const std::array<std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>, 3>& stale,
    const SpatialTriple<PhysicalPoint>& points) {
    Transaction transaction{db};
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        for (const auto& [timelineId, pointIds] : stale.at(k)) {
            if (!pointIds.empty()) {
                db.query(kDeleteSpatialPoints.at(k), {{"ids", pointIds}});
            }
            db.query(kDeleteSpatialTimeline.at(k), {{"timeline", timelineId}});
        }
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        writeChain<SoundSourceTransformError>(db, kCreateSpatialTimeline.at(k),
            kCreateFirstSpatialPoint.at(k), kCreateNextSpatialPoint.at(k), sourceId, "source",
            points.at(k));
    }
    transaction.commit();
}

/// Transforms and writes one source's spatial position (§6.8, §7).
///
/// @param[in,out] db      Database to write.
/// @param inputs          Everything read for the run.
/// @param source          The source.
/// @param maps            Every Score's TempoMap, by Score.id.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the source's error, or its point counts.
void transformSource(EngineDb& db, const Inputs& inputs, const SpatialSourceRecord& source,
    const std::map<std::int64_t, ScoreTempoMap>& maps, SampleRate rate,
    SoundSourceTransformOutcome& outcome) {
    try {
        const auto score = inputs.scoreOfSource.find(source.sourceId);
        if (score == inputs.scoreOfSource.end()) {
            throw SoundSourceTransformError{"it is not reached from any Score through SoundEntity"};
        }
        const auto map = maps.find(score->second);
        if (map == maps.end()) {
            throw SoundSourceTransformError{"its Score " + std::to_string(score->second) +
                                            " has no TempoMap, so its musical time is undefined"};
        }
        const SourceEnvelopes envelopes =
            gatherSource(inputs, source, map->second.map, map->second.mapId, rate);
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
        writeSource(db, source.sourceId, stale, points);
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            outcome.points.at(k) = points.at(k).size();
        }
    } catch (const SoundSourceTransformError& e) {
        outcome.error = e.what();
    } catch (const EngineDbError& e) {
        outcome.error = std::string{"engine-db: "} + e.what();
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
    const std::map<std::int64_t, ScoreTempoMap> maps = buildTempoMaps(inputs, report);

    // Sets are independent; id order only makes the report deterministic.
    for (const MusicalSetRecord& set : inputs.sets) {
        PartialSetTransformOutcome outcome{
            .setId = set.setId, .setName = set.name, .error = std::nullopt, .partials = {}};
        transformSet(db, inputs, set, maps, report.sampleRate, outcome);
        report.sets.push_back(std::move(outcome));
    }
    // Sources are independent of sets and of each other (§6.8).
    for (const SpatialSourceRecord& source : inputs.sources) {
        SoundSourceTransformOutcome outcome{.sourceId = source.sourceId,
            .sourceName = source.name,
            .error = std::nullopt,
            .points = {}};
        transformSource(db, inputs, source, maps, report.sampleRate, outcome);
        report.sources.push_back(std::move(outcome));
    }
    return report;
}

} // namespace rainbohz::rendering_engine::transform
