#include "rainbohz/rendering_engine/precomputation.hpp"

#include "rainbohz/rendering_engine/spatial_gains.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Precomputes every partial's integrable segments and frame scope.
///
/// Implements the stages of docs/precomputation.md for the core frequency
/// envelope and phase control: read the inputs, gather boundaries, build one
/// segment per pair of consecutive boundaries, integrate phase through them
/// from the initial phase, build the phase compensation segments, and write
/// the result.
/// The file is organised in that order:
///
///   - fixed Cypher queries, prepared once by EngineDb and always bound with
///     parameters;
///   - a bulk read of every partial's inputs before any partial is processed,
///     including the amplitude timelines the frame entries lead into, and the
///     phase timelines;
///   - validation helpers;
///   - the per-partial write, which replaces earlier output in one
///     transaction, so a rerun is idempotent and a failure leaves the
///     previous output in place;
///   - the pure stages, which are testable without a database;
///   - precomputeAll(), which processes partials independently: a partial
///     that fails is recorded in the report and does not stop the others.
///
/// Section numbers in comments refer to docs/precomputation.md. The phase
/// maths itself is shared with rendering and lives in segment_maths.hpp.
///
/// @see precomputation.hpp

namespace rainbohz::rendering_engine {

namespace {

// ---------------------------------------------------------------------------
// Queries
//
// Every statement is a fixed string, so that EngineDb prepares it once and
// reuses it for every partial and every segment. Values are always bound as
// parameters, never spliced into the text.
// ---------------------------------------------------------------------------

constexpr std::string_view kReadConfig = "MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate;";

constexpr std::string_view kReadPartials = "MATCH (p:Partial) RETURN p.id, p.name ORDER BY p.id;";

// One row per FrequencyPhysicalTimeline, with the head of its point list, or NULL
// when the timeline has no points.
constexpr std::string_view kReadFrequencyHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint) "
    "RETURN p.id, f.id;";

constexpr std::string_view kReadFrequencyPoints =
    "MATCH (f:FrequencyPhysicalTimelinePoint) "
    "RETURN f.id, f.time, f.frequency, f.frequency_curve_type;";

constexpr std::string_view kReadFrequencyLinks =
    "MATCH (a:FrequencyPhysicalTimelinePoint)-[:NEXT_POINT]->(b:FrequencyPhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

// The amplitude timeline is read for its times only, so that the frame
// entries of §6.7 can lead into it; its values are rendering's concern.
constexpr std::string_view kReadAmplitudeHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint) "
    "RETURN p.id, a.id;";

constexpr std::string_view kReadAmplitudePoints =
    "MATCH (a:AmplitudePhysicalTimelinePoint) RETURN a.id, a.time;";

constexpr std::string_view kReadAmplitudeLinks =
    "MATCH (a:AmplitudePhysicalTimelinePoint)-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

// The phase timeline: its targets, and the NULL points up to which the
// partial runs free (§6.8).
constexpr std::string_view kReadPhaseHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint) "
    "RETURN p.id, f.id;";

constexpr std::string_view kReadPhasePoints =
    "MATCH (f:PhasePhysicalTimelinePoint) RETURN f.id, f.time, f.phase;";

constexpr std::string_view kReadPhaseLinks =
    "MATCH (a:PhasePhysicalTimelinePoint)-[:NEXT_POINT]->(b:PhasePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

// The SoundSource of each partial, through its PartialSet. HAS_PARTIAL and
// HAS_PARTIALSET are ONE_MANY, so a partial has at most one row.
constexpr std::string_view kReadPartialSources =
    "MATCH (ss:SoundSource)-[:HAS_PARTIALSET]->(:PartialSet)-[:HAS_PARTIAL]->(p:Partial) "
    "RETURN p.id, ss.id, ss.name;";

// A SoundSource's spatial timelines (§7), read for their times and checked
// for their values, so that the frame entries of §6.7 can lead into them.
// One statement of each shape per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kReadSpatialHeads{
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:AzimuthPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AzimuthPhysicalTimelinePoint) RETURN s.id, a.id;",
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:ElevationPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:ElevationPhysicalTimelinePoint) RETURN s.id, a.id;",
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:DistancePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:DistancePhysicalTimelinePoint) RETURN s.id, a.id;",
};

constexpr std::array<std::string_view, 3> kReadSpatialPoints{
    "MATCH (a:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.azimuth, a.azimuth_curve_type;",
    "MATCH (a:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.elevation, a.elevation_curve_type;",
    "MATCH (a:DistancePhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.distance, a.distance_curve_type;",
};

constexpr std::array<std::string_view, 3> kReadSpatialLinks{
    "MATCH (a:AzimuthPhysicalTimelinePoint)-[:NEXT_POINT]->(b:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:ElevationPhysicalTimelinePoint)-[:NEXT_POINT]->(b:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:DistancePhysicalTimelinePoint)-[:NEXT_POINT]->(b:DistancePhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
};

// Output from an earlier run, which is deleted before the new output is
// written. A PartialSegmentPhysicalTimeline heads two lists: its integrable
// segments and its compensation segments.
constexpr std::string_view kReadExistingPhysicalTimelines =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PartialSegmentPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(s:PartialIntegrableSegment) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(c:PartialPhaseCompensationSegment) "
    "RETURN p.id, t.id, s.id, c.id;";

constexpr std::string_view kReadSegmentLinks =
    "MATCH (a:PartialIntegrableSegment)-[:NEXT_POINT]->(b:PartialIntegrableSegment) "
    "RETURN a.id, b.id;";

constexpr std::string_view kReadCompensationLinks =
    "MATCH (a:PartialPhaseCompensationSegment)-[:NEXT_POINT]->(b:PartialPhaseCompensationSegment) "
    "RETURN a.id, b.id;";

constexpr std::string_view kDeleteCompensation =
    "UNWIND $ids AS id MATCH (c:PartialPhaseCompensationSegment {id: id}) DETACH DELETE c;";

constexpr std::string_view kDeleteNominalOfSegment =
    "MATCH (s:PartialIntegrableSegment)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->"
    "(n:PartialSegmentNominalFrequency) WHERE s.id = $segment DETACH DELETE n;";

constexpr std::string_view kDeleteSegment =
    "MATCH (s:PartialIntegrableSegment) WHERE s.id = $segment DETACH DELETE s;";

constexpr std::string_view kDeletePhysicalTimeline =
    "MATCH (t:PartialSegmentPhysicalTimeline) WHERE t.id = $physicalTimeline DETACH DELETE t;";

constexpr std::string_view kDeleteFrameScope =
    "MATCH (p:Partial)-[r:PARTIAL_IN_FRAME]->(:Frame) WHERE p.id = $partial DELETE r;";

// Deleting a segment removes the FRAME_ENTRY_PHYSICAL_TIMELINE edges into it, but the
// FrameEntry nodes themselves, and their other edges, are the partial's
// output too and go explicitly (§10).
constexpr std::string_view kDeleteFrameEntries =
    "MATCH (e:FrameEntry)-[:FRAME_ENTRY_PARTIAL]->(p:Partial) WHERE p.id = $partial "
    "DETACH DELETE e;";

constexpr std::string_view kCreatePhysicalTimeline =
    "MATCH (p:Partial) WHERE p.id = $partial "
    "CREATE (p)-[:HAS_PHYSICAL_TIMELINE]->(t:PartialSegmentPhysicalTimeline) RETURN t.id;";

// Frame numbers are INT32 in the schema; range() yields INT64.
constexpr std::string_view kCreateFrameScope = "MATCH (p:Partial) WHERE p.id = $partial "
                                               "UNWIND range($first, $last) AS k "
                                               "MERGE (f:Frame {frame_number: CAST(k AS INT32)}) "
                                               "CREATE (p)-[:PARTIAL_IN_FRAME]->(f);";

// One FrameEntry with its eight edges, from the frame number and the ids of
// the six list nodes it leads to (§6.7): the partial's segment, amplitude
// point and compensation segment, and its SoundSource's azimuth, elevation
// and distance points. One statement per frame; UNWIND over a list parameter
// is the batching path if the count ever shows in a profile (§10). The Frame
// node exists: kCreateFrameScope has just merged it.
constexpr std::string_view kCreateFrameEntry =
    "MATCH (f:Frame), (p:Partial), (s:PartialIntegrableSegment), "
    "(a:AmplitudePhysicalTimelinePoint), (c:PartialPhaseCompensationSegment), "
    "(az:AzimuthPhysicalTimelinePoint), (el:ElevationPhysicalTimelinePoint), "
    "(d:DistancePhysicalTimelinePoint) "
    "WHERE f.frame_number = CAST($frame AS INT32) AND p.id = $partial "
    "AND s.id = $segment AND a.id = $point AND c.id = $compensation "
    "AND az.id = $azimuth AND el.id = $elevation AND d.id = $distance "
    "CREATE (f)-[:FRAME_ENTRY]->(e:FrameEntry)-[:FRAME_ENTRY_PARTIAL]->(p), "
    "(e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(s), (e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(a), "
    "(e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(c), (e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(az), "
    "(e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(el), (e)-[:FRAME_ENTRY_PHYSICAL_TIMELINE]->(d) "
    "RETURN e.id;";

// One compensation segment, linked from the PartialSegmentPhysicalTimeline
// (the first) or from the previous compensation segment (§6.8).
constexpr std::string_view kCreateFirstCompensation =
    "MATCH (prev:PartialSegmentPhysicalTimeline) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(c:PartialPhaseCompensationSegment {"
    "start_time: $startTime, end_time: $endTime, "
    "start_phase_delta: $startDelta, end_phase_delta: $endDelta}) RETURN c.id;";

constexpr std::string_view kCreateNextCompensation =
    "MATCH (prev:PartialPhaseCompensationSegment) WHERE prev.id = $previous "
    "CREATE (prev)-[:NEXT_POINT]->(c:PartialPhaseCompensationSegment {"
    "start_time: $startTime, end_time: $endTime, "
    "start_phase_delta: $startDelta, end_phase_delta: $endDelta}) RETURN c.id;";

/// The statement that creates one segment and its nominal frequency, linked
/// from the node before it in the chain.
///
/// There are four variants, from two choices: what the segment is linked from
/// (the timeline node, or the previous segment), and whether its nominal
/// frequency has a linear component. Curve properties are never written, so
/// they are NULL, as §5 requires. Each variant is a fixed string, so EngineDb
/// prepares each once.
///
/// @param afterPhysicalTimeline  True for the first segment, linked from the
///                       PartialSegmentPhysicalTimeline; false for the others, linked
///                       from the previous PartialIntegrableSegment.
/// @param linear         Whether to write linear_gradient, and
///                       linear_component true; otherwise linear_component is
///                       false and linear_gradient NULL.
/// @return The statement. It binds $previous, $startTime, $endTime,
///         $startPhase, $startFrequency, and, when `linear`, $linearGradient;
///         and returns the new segment's id.
const std::string& createSegmentStatement(bool afterPhysicalTimeline, bool linear) {
    const auto build = [](bool fromPhysicalTimeline, bool withLinear) {
        std::string cypher = fromPhysicalTimeline ? "MATCH (prev:PartialSegmentPhysicalTimeline) "
                                                  : "MATCH (prev:PartialIntegrableSegment) ";
        cypher += "WHERE prev.id = $previous "
                  "CREATE (prev)-[:NEXT_POINT]->(s:PartialIntegrableSegment {"
                  "start_time: $startTime, end_time: $endTime, start_phase: $startPhase"
                  "})-[:SEGMENT_TO_FREQUENCY_NOMINAL]->(:PartialSegmentNominalFrequency {"
                  "start_nominal_frequency: $startFrequency, ";
        cypher += withLinear ? "linear_component: true, linear_gradient: $linearGradient, "
                             : "linear_component: false, ";
        cypher += "curve_component: false}) RETURN s.id;";
        return cypher;
    };
    static const std::array<std::array<std::string, 2>, 2> kVariants{{
        {build(false, false), build(false, true)},
        {build(true, false), build(true, true)},
    }};
    return kVariants[afterPhysicalTimeline ? 1 : 0][linear ? 1 : 0];
}

// ---------------------------------------------------------------------------
// Inputs, as read in bulk before any partial is processed
// ---------------------------------------------------------------------------

/// A partial's existing PartialSegmentPhysicalTimeline, from an earlier run.
struct ExistingPhysicalTimeline {
    /// PartialSegmentPhysicalTimeline.id.
    std::int64_t physicalTimelineId = 0;

    /// Id of its first segment; nullopt when it has none.
    std::optional<std::int64_t> headSegment;

    /// Id of its first compensation segment; nullopt when it has none.
    std::optional<std::int64_t> headCompensation;
};

/// Everything precomputation reads, for every partial at once.
///
/// Reading in bulk, with a fixed handful of queries, costs far less than
/// walking each partial's lists one query per node, and it keeps all reads
/// ahead of all writes.
struct Inputs {
    /// Partial ids and names, in id order.
    std::vector<std::pair<std::int64_t, std::optional<std::string>>> partials;

    /// For each partial with a FrequencyPhysicalTimeline, the head of each of its
    /// FrequencyPhysicalTimelines' point lists (nullopt for an empty one). The schema
    /// allows at most one timeline per partial; a second is reported rather
    /// than assumed away.
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> frequencyHeads;

    /// Every FrequencyPhysicalTimelinePoint, by id.
    std::unordered_map<std::int64_t, FrequencyPointRecord> frequencyPoints;

    /// FrequencyPhysicalTimelinePoint NEXT_POINT links: id to next id.
    std::unordered_map<std::int64_t, std::int64_t> frequencyNext;

    /// For each partial with an AmplitudePhysicalTimeline, the head of each of its
    /// AmplitudePhysicalTimelines' point lists (nullopt for an empty one).
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> amplitudeHeads;

    /// Every AmplitudePhysicalTimelinePoint, by id: its time only.
    std::unordered_map<std::int64_t, AmplitudePointRecord> amplitudePoints;

    /// AmplitudePhysicalTimelinePoint NEXT_POINT links: id to next id.
    std::unordered_map<std::int64_t, std::int64_t> amplitudeNext;

    /// For each partial with a PhasePhysicalTimeline, the head of each of its
    /// PhasePhysicalTimelines' point lists (nullopt for an empty one).
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> phaseHeads;

    /// Every PhasePhysicalTimelinePoint, by id.
    std::unordered_map<std::int64_t, PhasePointRecord> phasePoints;

    /// PhasePhysicalTimelinePoint NEXT_POINT links: id to next id.
    std::unordered_map<std::int64_t, std::int64_t> phaseNext;

    /// For each partial in a set that a SoundSource aggregates, that source's
    /// id and name.
    std::unordered_map<std::int64_t,
        std::vector<std::pair<std::int64_t, std::optional<std::string>>>>
        partialSources;

    /// Per spatial kind, in kSpatialKinds order: for each SoundSource with a
    /// timeline of that kind, the head of each such timeline's point list
    /// (nullopt for an empty one).
    std::array<std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>>, 3>
        spatialHeads;

    /// Per spatial kind: every point, by id.
    std::array<std::unordered_map<std::int64_t, SpatialPointRecord>, 3> spatialPoints;

    /// Per spatial kind: NEXT_POINT links between points, id to next id.
    std::array<std::unordered_map<std::int64_t, std::int64_t>, 3> spatialNext;

    /// For each partial with output from an earlier run, its timelines.
    std::unordered_map<std::int64_t, std::vector<ExistingPhysicalTimeline>> existing;

    /// PartialIntegrableSegment NEXT_POINT links: id to next id.
    std::unordered_map<std::int64_t, std::int64_t> segmentNext;

    /// PartialPhaseCompensationSegment NEXT_POINT links: id to next id.
    std::unordered_map<std::int64_t, std::int64_t> compensationNext;
};

/// Reads an id-to-id link table from a two-column query.
///
/// @param db      Database to read.
/// @param cypher  A query returning (from id, to id) rows.
/// @return The links. NEXT_POINT is ONE_ONE, so no id appears twice as a
///         source. That holds only within the one FROM/TO pair a query reads:
///         Ladybug counts multiplicity per pair, so a node here can also be
///         a list head (§7).
/// @throws EngineDbError  If the query fails or returns a NULL id.
std::unordered_map<std::int64_t, std::int64_t> readLinks(EngineDb& db, std::string_view cypher) {
    std::unordered_map<std::int64_t, std::int64_t> links;
    for (const DbRow& row : db.query(cypher)) {
        links.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    return links;
}

/// Reads every input precomputation needs.
///
/// @param db  Database to read. Not modified.
/// @return The inputs of every partial.
/// @throws EngineDbError  If any query fails or returns a value of an
///         unexpected type.
Inputs readInputs(EngineDb& db) {
    Inputs inputs;

    for (const DbRow& row : db.query(kReadPartials)) {
        inputs.partials.emplace_back(requireInt64(row.at(0)), optionalString(row.at(1)));
    }

    for (const DbRow& row : db.query(kReadFrequencyHeads)) {
        inputs.frequencyHeads[requireInt64(row.at(0))].push_back(optionalInt64(row.at(1)));
    }

    for (const DbRow& row : db.query(kReadFrequencyPoints)) {
        const FrequencyPointRecord point{
            .id = requireInt64(row.at(0)),
            .time = optionalDouble(row.at(1)),
            .frequency = optionalDouble(row.at(2)),
            .curveType = optionalInt64(row.at(3)),
        };
        inputs.frequencyPoints.emplace(point.id, point);
    }

    inputs.frequencyNext = readLinks(db, kReadFrequencyLinks);

    for (const DbRow& row : db.query(kReadAmplitudeHeads)) {
        inputs.amplitudeHeads[requireInt64(row.at(0))].push_back(optionalInt64(row.at(1)));
    }

    for (const DbRow& row : db.query(kReadAmplitudePoints)) {
        const AmplitudePointRecord point{
            .id = requireInt64(row.at(0)),
            .time = optionalDouble(row.at(1)),
        };
        inputs.amplitudePoints.emplace(point.id, point);
    }

    inputs.amplitudeNext = readLinks(db, kReadAmplitudeLinks);

    for (const DbRow& row : db.query(kReadPhaseHeads)) {
        inputs.phaseHeads[requireInt64(row.at(0))].push_back(optionalInt64(row.at(1)));
    }

    for (const DbRow& row : db.query(kReadPhasePoints)) {
        const PhasePointRecord point{
            .id = requireInt64(row.at(0)),
            .time = optionalDouble(row.at(1)),
            .phase = optionalDouble(row.at(2)),
        };
        inputs.phasePoints.emplace(point.id, point);
    }

    inputs.phaseNext = readLinks(db, kReadPhaseLinks);

    for (const DbRow& row : db.query(kReadPartialSources)) {
        inputs.partialSources[requireInt64(row.at(0))].emplace_back(
            requireInt64(row.at(1)), optionalString(row.at(2)));
    }

    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        for (const DbRow& row : db.query(kReadSpatialHeads.at(k))) {
            inputs.spatialHeads.at(k)[requireInt64(row.at(0))].push_back(optionalInt64(row.at(1)));
        }
        for (const DbRow& row : db.query(kReadSpatialPoints.at(k))) {
            const SpatialPointRecord point{
                .id = requireInt64(row.at(0)),
                .time = optionalDouble(row.at(1)),
                .value = optionalDouble(row.at(2)),
                .curveType = optionalInt64(row.at(3)),
            };
            inputs.spatialPoints.at(k).emplace(point.id, point);
        }
        inputs.spatialNext.at(k) = readLinks(db, kReadSpatialLinks.at(k));
    }

    for (const DbRow& row : db.query(kReadExistingPhysicalTimelines)) {
        inputs.existing[requireInt64(row.at(0))].push_back(ExistingPhysicalTimeline{
            .physicalTimelineId = requireInt64(row.at(1)),
            .headSegment = optionalInt64(row.at(2)),
            .headCompensation = optionalInt64(row.at(3)),
        });
    }

    inputs.segmentNext = readLinks(db, kReadSegmentLinks);
    inputs.compensationNext = readLinks(db, kReadCompensationLinks);

    return inputs;
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

/// Formats a double for an error message, with enough digits to identify it.
///
/// @param value  Any value, including non-finite ones.
/// @return The value in the shortest form that round-trips, e.g. "48000" or
///         "0.1".
std::string formatNumber(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    return out.str();
}

/// Throws a PartialPrecomputationError about the point at `index`.
///
/// @param timeline  The list the point is on, e.g. "FrequencyPhysicalTimeline".
/// @param index     Position of the point in its timeline, from 0.
/// @param reason    What is wrong with it.
/// @throws PartialPrecomputationError  Always.
[[noreturn]] void failAtPoint(
    std::string_view timeline, std::size_t index, const std::string& reason) {
    throw PartialPrecomputationError{
        std::string{timeline} + " point " + std::to_string(index) + ": " + reason};
}

/// Reads a required, finite value of a point.
///
/// @param value     The property as read.
/// @param timeline  The list the point is on, for the error message.
/// @param index     Position of the point in its timeline, from 0.
/// @param name      Property name, for the error message.
/// @return The value.
/// @throws PartialPrecomputationError  If `value` is NULL or not finite.
double requireFinite(const std::optional<double>& value, std::string_view timeline,
    std::size_t index, std::string_view name) {
    if (!value.has_value()) {
        failAtPoint(timeline, index, std::string{name} + " is NULL");
    }
    if (!std::isfinite(*value)) {
        failAtPoint(timeline, index,
            std::string{name} + " is " + formatNumber(*value) + ", which is not finite");
    }
    return *value;
}

constexpr std::string_view kFrequencyList = "FrequencyPhysicalTimeline";
constexpr std::string_view kAmplitudeList = "AmplitudePhysicalTimeline";
constexpr std::string_view kPhaseList = "PhasePhysicalTimeline";

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// A PartialSegmentPhysicalTimeline from an earlier run, with the ids of the
/// two lists it heads, to be deleted.
struct StalePhysicalTimeline {
    /// PartialSegmentPhysicalTimeline.id.
    std::int64_t physicalTimelineId = 0;

    /// Its PartialIntegrableSegment ids, in chain order.
    std::vector<std::int64_t> segmentIds;

    /// Its PartialPhaseCompensationSegment ids, in chain order.
    std::vector<std::int64_t> compensationIds;
};

/// Everything to be written for one partial.
struct PartialOutput {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Timelines from an earlier run to delete.
    std::vector<StalePhysicalTimeline> stale;

    /// The segments to write, integrated.
    std::vector<Segment> segments;

    /// The compensation segments to write.
    std::vector<CompensationSegment> compensation;

    /// The frames the partial is in scope for.
    FrameRange frames;

    /// Ids of the partial's amplitude points, in list order: what the frame
    /// entries' point indexes refer to.
    std::vector<std::int64_t> amplitudePointIds;

    /// Ids of the points of the partial's SoundSource's spatial timelines,
    /// in kSpatialKinds order and list order: what the frame entries'
    /// spatial indexes refer to. Views of the run's SourceTimelines cache.
    std::array<std::span<const std::int64_t>, 3> spatialPointIds;

    /// One frame entry per frame in scope.
    std::vector<FrameEntryTarget> entries;
};

/// Replaces a partial's output in one transaction (§6.2, stage 4; §9).
///
/// Deletes the partial's FrameEntry nodes, the stale timelines, their
/// segments, nominal frequencies and compensation segments, and the partial's
/// PARTIAL_IN_FRAME edges; then creates the new timeline, segment chain and
/// nominal frequencies, compensation chain, any missing Frame nodes, the new
/// frame scope edges, and one FrameEntry per frame in scope with its eight
/// edges.
///
/// @param[in,out] db  Database to write. Modified only if every statement
///                    succeeds; otherwise the transaction is rolled back.
/// @param output      What to write.
/// @throws EngineDbError  If any statement fails. Nothing has been written.
/// @throws PartialPrecomputationError  If the partial is no longer in the
///         database. Nothing has been written.
void writePartial(EngineDb& db, const PartialOutput& output) {
    Transaction transaction{db};

    db.query(kDeleteFrameEntries, {{"partial", output.partialId}});
    for (const StalePhysicalTimeline& stale : output.stale) {
        for (const std::int64_t segmentId : stale.segmentIds) {
            db.query(kDeleteNominalOfSegment, {{"segment", segmentId}});
            db.query(kDeleteSegment, {{"segment", segmentId}});
        }
        if (!stale.compensationIds.empty()) {
            db.query(kDeleteCompensation, {{"ids", stale.compensationIds}});
        }
        db.query(kDeletePhysicalTimeline, {{"physicalTimeline", stale.physicalTimelineId}});
    }
    db.query(kDeleteFrameScope, {{"partial", output.partialId}});

    const std::vector<DbRow> created =
        db.query(kCreatePhysicalTimeline, {{"partial", output.partialId}});
    if (created.size() != 1) {
        throw PartialPrecomputationError{"the partial disappeared while it was being precomputed"};
    }
    const std::int64_t physicalTimelineId = requireInt64(created.front().at(0));
    std::int64_t previous = physicalTimelineId;

    std::vector<std::int64_t> segmentIds;
    segmentIds.reserve(output.segments.size());
    bool first = true;
    for (const Segment& segment : output.segments) {
        const std::optional<FrequencyGradient>& gradient = segment.nominal.linearGradient;
        DbParameters parameters{
            {"previous", previous},
            {"startTime", segment.startTime.value()},
            {"endTime", segment.endTime.value()},
            {"startPhase", segment.startPhase.value()},
            {"startFrequency", segment.nominal.startFrequency.value()},
        };
        if (gradient.has_value()) {
            parameters.emplace_back("linearGradient", gradient->value());
        }
        const std::vector<DbRow> rows =
            db.query(createSegmentStatement(first, gradient.has_value()), parameters);
        if (rows.size() != 1) {
            throw EngineDbError{"creating a PartialIntegrableSegment returned " +
                                std::to_string(rows.size()) + " rows, not 1"};
        }
        previous = requireInt64(rows.front().at(0));
        segmentIds.push_back(previous);
        first = false;
    }

    std::vector<std::int64_t> compensationIds;
    compensationIds.reserve(output.compensation.size());
    previous = physicalTimelineId;
    for (const CompensationSegment& segment : output.compensation) {
        const std::vector<DbRow> rows =
            db.query(compensationIds.empty() ? kCreateFirstCompensation : kCreateNextCompensation,
                {
                    {"previous", previous},
                    {"startTime", segment.startTime.value()},
                    {"endTime", segment.endTime.value()},
                    {"startDelta", segment.startDelta.value()},
                    {"endDelta", segment.endDelta.value()},
                });
        if (rows.size() != 1) {
            throw EngineDbError{"creating a PartialPhaseCompensationSegment returned " +
                                std::to_string(rows.size()) + " rows, not 1"};
        }
        previous = requireInt64(rows.front().at(0));
        compensationIds.push_back(previous);
    }

    db.query(
        kCreateFrameScope, {
                               {"partial", output.partialId},
                               {"first", static_cast<std::int64_t>(output.frames.first.value())},
                               {"last", static_cast<std::int64_t>(output.frames.last.value())},
                           });

    for (const FrameEntryTarget& entry : output.entries) {
        const std::vector<DbRow> rows = db.query(
            kCreateFrameEntry, {
                                   {"frame", static_cast<std::int64_t>(entry.frame.value())},
                                   {"partial", output.partialId},
                                   {"segment", segmentIds.at(entry.segment)},
                                   {"point", output.amplitudePointIds.at(entry.point)},
                                   {"compensation", compensationIds.at(entry.compensation)},
                                   {"azimuth", output.spatialPointIds[0][entry.spatial[0]]},
                                   {"elevation", output.spatialPointIds[1][entry.spatial[1]]},
                                   {"distance", output.spatialPointIds[2][entry.spatial[2]]},
                               });
        if (rows.size() != 1) {
            throw EngineDbError{
                "creating a FrameEntry returned " + std::to_string(rows.size()) + " rows, not 1"};
        }
    }

    transaction.commit();
}

/// A SoundSource's three spatial timelines, walked and checked once per run
/// and shared by every partial of the source (§7).
struct SourceTimelines {
    /// "SoundSource <name> (id <id>)", for messages.
    std::string description;

    /// Per kind, in kSpatialKinds order: the point ids in list order.
    std::array<std::vector<std::int64_t>, 3> ids;

    /// Per kind: the validated times, in the same order.
    std::array<std::vector<SampleTime>, 3> times;

    /// Why the timelines are unusable; nullopt when they are usable. Every
    /// partial of the source fails with it.
    std::optional<std::string> error;
};

/// The validated spatial timelines of every SoundSource seen so far in a run,
/// by SoundSource.id.
using SourceTimelineCache = std::unordered_map<std::int64_t, SourceTimelines>;

/// Walks and checks one source's spatial timelines.
///
/// @param inputs       Everything read for the run.
/// @param sourceId     SoundSource.id.
/// @param description  How to name the source in messages.
/// @return The timelines, or the reason they are unusable in `error`.
SourceTimelines readSourceTimelines(
    const Inputs& inputs, std::int64_t sourceId, std::string description) {
    SourceTimelines result{
        .description = std::move(description), .ids = {}, .times = {}, .error = std::nullopt};
    try {
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const SpatialKind kind = kSpatialKinds.at(k);
            const std::string timeline{spatialTimelineName(kind)};
            const auto heads = inputs.spatialHeads.at(k).find(sourceId);
            if (heads == inputs.spatialHeads.at(k).end()) {
                throw PartialPrecomputationError{"it has no " + timeline};
            }
            if (heads->second.size() != 1) {
                throw PartialPrecomputationError{"it has " + std::to_string(heads->second.size()) +
                                                 " " + timeline + "s; exactly one is required"};
            }
            result.ids.at(k) = walkChain(heads->second.front(), inputs.spatialNext.at(k), timeline);
            std::vector<SpatialPointRecord> records;
            records.reserve(result.ids.at(k).size());
            for (const std::int64_t pointId : result.ids.at(k)) {
                records.push_back(inputs.spatialPoints.at(k).at(pointId));
            }
            result.times.at(k) = validateSpatialTimeline(kind, records);
        }
    } catch (const PartialPrecomputationError& e) {
        result.error = e.what();
    }
    return result;
}

/// Precomputes and writes one partial.
///
/// @param[in,out] db    Database to write the partial's output to.
/// @param inputs        Everything read for the run.
/// @param[in,out] sources  The spatial timelines of the sources checked so
///                      far; gains the partial's source if it is new.
/// @param partialId     Partial.id of the partial to precompute.
/// @param frameSize     Frame size for its frame scope.
/// @param[out] outcome  On success, its segmentCount, compensationCount,
///                      frames and entryCount are set. Its error is left
///                      alone.
/// @throws PartialPrecomputationError  If the partial's inputs are invalid,
///         or it disappears while being written. Nothing has been written.
/// @throws EngineDbError  If writing fails. Nothing has been written.
void precomputePartial(EngineDb& db, const Inputs& inputs, SourceTimelineCache& sources,
    std::int64_t partialId, FrameSize frameSize, PartialOutcome& outcome) {
    // Stage 1: this partial's inputs, from what was read in bulk.
    const auto oneHead = [&](const auto& heads, std::string_view timeline) {
        const auto found = heads.find(partialId);
        if (found == heads.end()) {
            throw PartialPrecomputationError{"it has no " + std::string{timeline}};
        }
        if (found->second.size() != 1) {
            throw PartialPrecomputationError{"it has " + std::to_string(found->second.size()) +
                                             " " + std::string{timeline} +
                                             "s; exactly one is required"};
        }
        return found->second.front();
    };

    std::vector<FrequencyPointRecord> records;
    for (const std::int64_t pointId : walkChain(oneHead(inputs.frequencyHeads, kFrequencyList),
             inputs.frequencyNext, kFrequencyList)) {
        records.push_back(inputs.frequencyPoints.at(pointId));
    }
    const std::vector<FrequencyPoint> envelope = validateFrequencyEnvelope(records);

    std::vector<AmplitudePointRecord> amplitudeRecords;
    std::vector<std::int64_t> amplitudePointIds = walkChain(
        oneHead(inputs.amplitudeHeads, kAmplitudeList), inputs.amplitudeNext, kAmplitudeList);
    amplitudeRecords.reserve(amplitudePointIds.size());
    for (const std::int64_t pointId : amplitudePointIds) {
        amplitudeRecords.push_back(inputs.amplitudePoints.at(pointId));
    }
    const std::vector<SampleTime> amplitudeTimes =
        validateAmplitudeTimes(amplitudeRecords, envelope);

    std::vector<PhasePointRecord> phaseRecords;
    for (const std::int64_t pointId :
        walkChain(oneHead(inputs.phaseHeads, kPhaseList), inputs.phaseNext, kPhaseList)) {
        phaseRecords.push_back(inputs.phasePoints.at(pointId));
    }
    const std::vector<PhaseTarget> targets = validatePhaseTimeline(phaseRecords, envelope);

    // The SoundSource's spatial timelines, shared with its other partials,
    // must cover this partial's lifeline (§7).
    const auto source = inputs.partialSources.find(partialId);
    if (source == inputs.partialSources.end()) {
        throw PartialPrecomputationError{"it is in no PartialSet aggregated by a SoundSource"};
    }
    if (source->second.size() != 1) {
        throw PartialPrecomputationError{"it is aggregated by " +
                                         std::to_string(source->second.size()) +
                                         " SoundSources; exactly one is required"};
    }
    const auto& [sourceId, sourceName] = source->second.front();
    auto cached = sources.find(sourceId);
    if (cached == sources.end()) {
        cached = sources
                     .emplace(sourceId, readSourceTimelines(inputs, sourceId,
                                            "SoundSource " + sourceName.value_or("(unnamed)") +
                                                " (id " + std::to_string(sourceId) + ")"))
                     .first;
    }
    const SourceTimelines& spatial = cached->second;
    if (spatial.error.has_value()) {
        throw PartialPrecomputationError{"its " + spatial.description + ": " + *spatial.error};
    }
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        try {
            checkSpatialCoverage(kSpatialKinds.at(k), spatial.times.at(k), envelope.front().time,
                envelope.back().time);
        } catch (const PartialPrecomputationError& e) {
            throw PartialPrecomputationError{"its " + spatial.description + ": " + e.what()};
        }
    }

    // Stages 2 to 5, frame scope and frame entries: pure computation.
    const std::vector<SampleTime> boundaries = gatherBoundaries(envelope);

    PartialOutput output{
        .partialId = partialId,
        .stale = {},
        .segments = buildSegments(boundaries, envelope),
        .compensation = {},
        .frames = frameScope(boundaries.front(), boundaries.back(), frameSize),
        .amplitudePointIds = std::move(amplitudePointIds),
        .spatialPointIds = {spatial.ids[0], spatial.ids[1], spatial.ids[2]},
        .entries = {},
    };
    // φ₀ may depend on the natural phase N₀, so the segments are integrated
    // from 0 first, then again from φ₀: what is written is a straight
    // integration from φ₀, exactly as if φ₀ had been known (§6.8).
    integrate(output.segments);
    integrate(output.segments, initialPhaseOf(output.segments, targets));
    output.compensation = buildCompensation(output.segments, targets);
    output.entries = frameEntries(output.segments, amplitudeTimes, output.compensation,
        {spatial.times[0], spatial.times[1], spatial.times[2]}, output.frames, frameSize);

    if (const auto existing = inputs.existing.find(partialId); existing != inputs.existing.end()) {
        for (const ExistingPhysicalTimeline& physicalTimeline : existing->second) {
            output.stale.push_back(StalePhysicalTimeline{
                .physicalTimelineId = physicalTimeline.physicalTimelineId,
                .segmentIds = walkChain(physicalTimeline.headSegment, inputs.segmentNext,
                    "existing PartialSegmentPhysicalTimeline"),
                .compensationIds = walkChain(physicalTimeline.headCompensation,
                    inputs.compensationNext, "existing PartialPhaseCompensationSegment list"),
            });
        }
    }

    writePartial(db, output);

    outcome.segmentCount = output.segments.size();
    outcome.compensationCount = output.compensation.size();
    outcome.frames = output.frames;
    outcome.entryCount = output.entries.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Pure stages
// ---------------------------------------------------------------------------

std::vector<std::int64_t> walkChain(std::optional<std::int64_t> head,
    const std::unordered_map<std::int64_t, std::int64_t>& next, std::string_view what) {
    std::vector<std::int64_t> chain;
    std::unordered_set<std::int64_t> seen;
    for (std::optional<std::int64_t> at = head; at.has_value();) {
        if (!seen.insert(*at).second) {
            throw PartialPrecomputationError{
                std::string{what} + ": its NEXT_POINT list loops back to " + "node id " +
                std::to_string(*at) + " after " + std::to_string(chain.size()) + " nodes"};
        }
        chain.push_back(*at);
        const auto link = next.find(*at);
        at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
    }
    return chain;
}

std::vector<FrequencyPoint> validateFrequencyEnvelope(
    std::span<const FrequencyPointRecord> points) {
    if (points.size() < 2) {
        throw PartialPrecomputationError{"its FrequencyPhysicalTimeline has " +
                                         std::to_string(points.size()) +
                                         " point(s); at least 2 are required"};
    }

    std::vector<FrequencyPoint> envelope;
    envelope.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const FrequencyPointRecord& record = points[i];
        const SampleTime time{requireFinite(record.time, kFrequencyList, i, "time")};
        const AngularFrequency frequency{
            requireFinite(record.frequency, kFrequencyList, i, "frequency")};

        if (i == 0 && time < SampleTime{0.0}) {
            failAtPoint(kFrequencyList, i,
                "time " + formatNumber(time.value()) + " is before the start of the score");
        }
        if (i > 0 && !(time > envelope.back().time)) {
            failAtPoint(kFrequencyList, i,
                "time " + formatNumber(time.value()) + " is not after the previous point's time " +
                    formatNumber(envelope.back().time.value()));
        }
        // The curve on a point describes the path to the next point, so the
        // last point's curve type has no effect and is not checked (§4).
        if (i + 1 < points.size() && record.curveType != kLinearCurve) {
            failAtPoint(kFrequencyList, i,
                "frequency_curve_type is " +
                    (record.curveType.has_value() ? std::to_string(*record.curveType)
                                                  : std::string{"NULL"}) +
                    "; only " + std::to_string(kLinearCurve) +
                    " (linear) is supported in this version");
        }
        envelope.push_back(FrequencyPoint{.time = time, .frequency = frequency});
    }
    return envelope;
}

std::vector<SampleTime> validateAmplitudeTimes(
    std::span<const AmplitudePointRecord> points, std::span<const FrequencyPoint> envelope) {
    if (points.size() < 2) {
        throw PartialPrecomputationError{"its AmplitudePhysicalTimeline has " +
                                         std::to_string(points.size()) +
                                         " point(s); at least 2 are required"};
    }

    std::vector<SampleTime> times;
    times.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SampleTime time{requireFinite(points[i].time, kAmplitudeList, i, "time")};
        if (i > 0 && !(time > times.back())) {
            failAtPoint(kAmplitudeList, i,
                "time " + formatNumber(time.value()) + " is not after the previous point's time " +
                    formatNumber(times.back().value()));
        }
        times.push_back(time);
    }

    // The two timelines define the lifeline together, exactly (§7).
    if (times.front() != envelope.front().time) {
        failAtPoint(kAmplitudeList, 0,
            "time " + formatNumber(times.front().value()) +
                " differs from the first FrequencyPhysicalTimeline point's time " +
                formatNumber(envelope.front().time.value()) +
                "; the two timelines must start together");
    }
    if (times.back() != envelope.back().time) {
        failAtPoint(kAmplitudeList, times.size() - 1,
            "time " + formatNumber(times.back().value()) +
                " differs from the last FrequencyPhysicalTimeline point's time " +
                formatNumber(envelope.back().time.value()) +
                "; the two timelines must end together");
    }
    return times;
}

std::string_view spatialTimelineName(SpatialKind kind) noexcept {
    switch (kind) {
    case SpatialKind::Azimuth:
        return "AzimuthPhysicalTimeline";
    case SpatialKind::Elevation:
        return "ElevationPhysicalTimeline";
    case SpatialKind::Distance:
        return "DistancePhysicalTimeline";
    }
    return "unknown";
}

std::string_view spatialValueName(SpatialKind kind) noexcept {
    switch (kind) {
    case SpatialKind::Azimuth:
        return "azimuth";
    case SpatialKind::Elevation:
        return "elevation";
    case SpatialKind::Distance:
        return "distance";
    }
    return "unknown";
}

std::optional<std::string> spatialValueProblem(SpatialKind kind, double value) {
    const std::string name{spatialValueName(kind)};
    if (!std::isfinite(value)) {
        return name + " is " + formatNumber(value) + ", which is not finite";
    }
    const auto outside = [&](double lo, double hi, std::string_view range) {
        return value < lo || value > hi
                   ? std::optional<std::string>{name + " " + formatNumber(value) + " is outside " +
                                                std::string{range}}
                   : std::nullopt;
    };
    switch (kind) {
    case SpatialKind::Azimuth:
        return outside(-std::numbers::pi, std::numbers::pi, "[-π, π] radians");
    case SpatialKind::Elevation:
        return outside(-std::numbers::pi / 2.0, std::numbers::pi / 2.0, "[-π/2, π/2] radians");
    case SpatialKind::Distance:
        return outside(kMinDistanceMetres, kMaxDistanceMetres, "[1, 16384] metres");
    }
    return std::nullopt;
}

std::vector<SampleTime> validateSpatialTimeline(
    SpatialKind kind, std::span<const SpatialPointRecord> points) {
    const std::string_view timeline = spatialTimelineName(kind);
    if (points.empty()) {
        throw PartialPrecomputationError{
            "its " + std::string{timeline} + " has no points; at least 1 is required"};
    }
    std::vector<SampleTime> times;
    times.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SpatialPointRecord& record = points[i];
        const SampleTime time{requireFinite(record.time, timeline, i, "time")};
        if (i > 0 && !(time > times.back())) {
            failAtPoint(timeline, i,
                "time " + formatNumber(time.value()) + " is not after the previous point's time " +
                    formatNumber(times.back().value()));
        }
        const double value = requireFinite(record.value, timeline, i, spatialValueName(kind));
        if (const std::optional<std::string> problem = spatialValueProblem(kind, value)) {
            failAtPoint(timeline, i, *problem);
        }
        if (i + 1 < points.size() && record.curveType != kLinearCurve) {
            failAtPoint(timeline, i,
                std::string{spatialValueName(kind)} + "_curve_type is " +
                    (record.curveType.has_value() ? std::to_string(*record.curveType)
                                                  : std::string{"NULL"}) +
                    "; only " + std::to_string(kLinearCurve) +
                    " (linear) is supported in this version");
        }
        times.push_back(time);
    }
    return times;
}

void checkSpatialCoverage(SpatialKind kind, std::span<const SampleTime> times,
    SampleTime lifelineStart, SampleTime lifelineEnd) {
    const std::string timeline{spatialTimelineName(kind)};
    if (times.front() > lifelineStart) {
        throw PartialPrecomputationError{
            timeline + " starts at " + formatNumber(times.front().value()) +
            ", after the partial's lifeline starts at " + formatNumber(lifelineStart.value()) +
            "; it must cover the lifeline of every partial of the source"};
    }
    if (times.back() < lifelineEnd) {
        throw PartialPrecomputationError{
            timeline + " ends at " + formatNumber(times.back().value()) +
            ", before the partial's lifeline ends at " + formatNumber(lifelineEnd.value()) +
            "; it must cover the lifeline of every partial of the source"};
    }
}

std::vector<PhaseTarget> validatePhaseTimeline(
    std::span<const PhasePointRecord> points, std::span<const FrequencyPoint> envelope) {
    if (points.empty()) {
        throw PartialPrecomputationError{
            "its PhasePhysicalTimeline has no points; at least 1 is required, at the start of "
            "its lifeline"};
    }

    std::vector<PhaseTarget> targets;
    targets.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const PhasePointRecord& record = points[i];
        const SampleTime time{requireFinite(record.time, kPhaseList, i, "time")};
        if (i == 0 && time != envelope.front().time) {
            failAtPoint(kPhaseList, i,
                "time " + formatNumber(time.value()) +
                    " differs from the first FrequencyPhysicalTimeline point's time " +
                    formatNumber(envelope.front().time.value()) +
                    "; the phase timeline must start with the lifeline");
        }
        if (i > 0 && !(time > targets.back().time)) {
            failAtPoint(kPhaseList, i,
                "time " + formatNumber(time.value()) + " is not after the previous point's time " +
                    formatNumber(targets.back().time.value()));
        }
        if (time > envelope.back().time) {
            failAtPoint(kPhaseList, i,
                "time " + formatNumber(time.value()) +
                    " is after the last FrequencyPhysicalTimeline point's time " +
                    formatNumber(envelope.back().time.value()) + ", the end of the lifeline");
        }

        // NULL is a value here, not an absence: free-running up to this point.
        std::optional<Phase> phase;
        if (record.phase.has_value()) {
            const double radians = requireFinite(record.phase, kPhaseList, i, "phase");
            if (radians < -std::numbers::pi || radians > std::numbers::pi) {
                failAtPoint(kPhaseList, i,
                    "phase " + formatNumber(radians) +
                        " is outside [-π, π]; it is wrapped radians");
            }
            phase = Phase{radians};
        }
        targets.push_back(PhaseTarget{.time = time, .phase = phase});
    }
    return targets;
}

std::vector<SampleTime> gatherBoundaries(std::span<const FrequencyPoint> envelope) {
    std::vector<SampleTime> boundaries;
    boundaries.reserve(envelope.size());

    // Source 1: the main frequency envelope. Frequency modulators and phase
    // regions will add their times here (§6.3).
    for (const FrequencyPoint& point : envelope) {
        boundaries.push_back(point.time);
    }

    std::ranges::sort(boundaries);
    const auto duplicates = std::ranges::unique(boundaries);
    boundaries.erase(duplicates.begin(), duplicates.end());
    return boundaries;
}

std::vector<Segment> buildSegments(
    std::span<const SampleTime> boundaries, std::span<const FrequencyPoint> envelope) {
    if (boundaries.size() < 2 || envelope.size() < 2 ||
        boundaries.front() != envelope.front().time || boundaries.back() != envelope.back().time) {
        throw std::invalid_argument{
            "buildSegments: boundaries must span the envelope exactly, with at least 2 of each"};
    }

    std::vector<Segment> segments;
    segments.reserve(boundaries.size() - 1);

    // `piece` is the envelope piece [envelope[piece], envelope[piece + 1])
    // covering the current boundary. Both sequences are sorted, so it only
    // ever moves forward.
    std::size_t piece = 0;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        const SampleTime start = boundaries[i];
        const SampleTime end = boundaries[i + 1];
        if (!(end > start)) {
            throw std::invalid_argument{"buildSegments: boundaries must be strictly increasing"};
        }
        while (piece + 2 < envelope.size() && envelope[piece + 1].time <= start) {
            ++piece;
        }

        const FrequencyPoint& from = envelope[piece];
        const FrequencyPoint& to = envelope[piece + 1];
        const FrequencyGradient gradient = (to.frequency - from.frequency) / (to.time - from.time);

        // At an envelope point, take the point's own frequency rather than
        // computing it, so that no rounding is introduced where none is
        // needed (§6.4).
        const AngularFrequency startFrequency =
            start == from.time ? from.frequency : from.frequency + gradient * (start - from.time);

        segments.push_back(Segment{
            .startTime = start,
            .endTime = end,
            .nominal =
                NominalFrequency{.startFrequency = startFrequency, .linearGradient = gradient},
            .startPhase = Phase{0.0},
        });
    }
    return segments;
}

Phase integrate(std::span<Segment> segments, Phase initialPhase) {
    Phase accumulator = initialPhase;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        Segment& segment = segments[i];
        segment.startPhase = accumulator;
        accumulator += phaseIncrement(segment.nominal, segment.endTime - segment.startTime);
        if (!std::isfinite(accumulator.value())) {
            throw PartialPrecomputationError{"the phase stops being finite in segment " +
                                             std::to_string(i) + ", which starts at " +
                                             formatNumber(segment.startTime.value())};
        }
    }
    return accumulator;
}

Phase naturalPhaseAt(std::span<const Segment> segments, SampleTime at) {
    if (segments.empty() || at < segments.front().startTime || at > segments.back().endTime) {
        throw std::invalid_argument{"naturalPhaseAt: the time is outside the segments"};
    }
    // The segment with startTime ≤ at < endTime; at the lifeline's end, the
    // last one. A binary search, since targets are few and segments may be many.
    const auto after = std::ranges::upper_bound(
        segments, at, std::ranges::less{}, [](const Segment& s) { return s.startTime; });
    const Segment& covering = *std::prev(after);
    return phaseAt(covering.startPhase, covering.nominal, at - covering.startTime);
}

Phase initialPhaseOf(std::span<const Segment> fromZero, std::span<const PhaseTarget> targets) {
    // The anchor is the first target with a phase.
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (const std::optional<Phase>& phase = targets[i].phase) {
            if (i == 0) {
                return *phase;
            }
            // Running free from b₀ must arrive at the anchor's phase exactly:
            // start where the anchor's phase is, less what the envelope adds
            // on the way.
            return wrapHalfOpen(*phase - naturalPhaseAt(fromZero, targets[i].time));
        }
    }
    return Phase{0.0};
}

std::vector<CompensationSegment> buildCompensation(
    std::span<const Segment> segments, std::span<const PhaseTarget> targets) {
    const SampleTime lifelineEnd = segments.back().endTime;

    // The anchor -- the first target with a phase -- was met by φ₀, so it and
    // everything before it need no compensation.
    const auto anchor =
        std::ranges::find_if(targets, [](const PhaseTarget& t) { return t.phase.has_value(); });

    std::vector<CompensationSegment> result;
    result.reserve(targets.size());
    Phase delta{0.0};
    for (auto target = std::next(targets.begin()); target != targets.end(); ++target) {
        const Phase before = delta;
        if (target > anchor && target->phase.has_value()) {
            const Phase actual = naturalPhaseAt(segments, target->time) + delta;
            delta += wrapHalfOpen(*target->phase - actual);
        }
        result.push_back(CompensationSegment{.startTime = std::prev(target)->time,
            .endTime = target->time,
            .startDelta = before,
            .endDelta = delta});
    }
    if (targets.back().time < lifelineEnd) {
        result.push_back(CompensationSegment{.startTime = targets.back().time,
            .endTime = lifelineEnd,
            .startDelta = delta,
            .endDelta = delta});
    }
    return result;
}

NominalCompensation compensationFrom(const CompensationSegment& segment, SampleTime at) noexcept {
    const Phase rise = segment.endDelta - segment.startDelta;
    const SampleDuration length = segment.endTime - segment.startTime;
    // Interpolated rather than startDelta + slope·τ, so that the start is
    // exactly startDelta and a later re-basing does not accumulate rounding.
    return NominalCompensation{
        .startDelta = segment.startDelta + rise * ((at - segment.startTime) / length),
        .slope = rise / length,
    };
}

FrameRange frameScope(SampleTime start, SampleTime end, FrameSize size) {
    const std::optional<FrameNumber> first = frameContaining(start, size);
    const std::optional<FrameNumber> last = lastFrameBefore(end, size);
    if (!first.has_value() || !last.has_value()) {
        throw PartialPrecomputationError{
            "its lifeline [" + formatNumber(start.value()) + ", " + formatNumber(end.value()) +
            ") reaches frames whose numbers do not fit INT32 at a frame size of " +
            std::to_string(size.value())};
    }
    return FrameRange{.first = *first, .last = *last};
}

std::vector<FrameEntryTarget> frameEntries(std::span<const Segment> segments,
    std::span<const SampleTime> amplitudeTimes, std::span<const CompensationSegment> compensation,
    const std::array<std::span<const SampleTime>, 3>& spatialTimes, FrameRange frames,
    FrameSize size) {
    std::vector<FrameEntryTarget> entries;
    const SampleTime lifelineStart = segments.front().startTime;

    // A source's spatial entry depends on the frame alone, not on the
    // partial: the last point at or before the frame's start, or the first
    // point when the timeline starts inside the frame. Every partial of the
    // source therefore leads to the same points. The timelines span all of
    // the source's partials, so the cursor starts by binary search at the
    // first frame's start.
    std::array<std::size_t, 3> spatial{};
    for (std::size_t k = 0; k < spatial.size(); ++k) {
        const auto after =
            std::ranges::upper_bound(spatialTimes.at(k), frameStart(frames.first, size));
        spatial.at(k) =
            after == spatialTimes.at(k).begin()
                ? 0
                : static_cast<std::size_t>(std::distance(spatialTimes.at(k).begin(), after) - 1);
    }

    // The cursors only ever move forward, because the frame starts increase.
    std::size_t segment = 0;
    std::size_t point = 0;
    std::size_t ramp = 0;
    for (std::int32_t k = frames.first.value(); k <= frames.last.value(); ++k) {
        const FrameNumber frame{k};
        const SampleTime entryTime = std::max(lifelineStart, frameStart(frame, size));

        while (segment + 1 < segments.size() && segments[segment].endTime <= entryTime) {
            ++segment;
        }
        while (point + 1 < amplitudeTimes.size() && amplitudeTimes[point + 1] <= entryTime) {
            ++point;
        }
        while (ramp + 1 < compensation.size() && compensation[ramp].endTime <= entryTime) {
            ++ramp;
        }
        for (std::size_t q = 0; q < spatial.size(); ++q) {
            const std::span<const SampleTime> times = spatialTimes.at(q);
            const SampleTime fs = frameStart(frame, size);
            while (spatial.at(q) + 1 < times.size() && times[spatial.at(q) + 1] <= fs) {
                ++spatial.at(q);
            }
        }
        entries.push_back(FrameEntryTarget{.frame = frame,
            .segment = segment,
            .point = point,
            .compensation = ramp,
            .spatial = spatial});
    }
    return entries;
}

FrameSize resolveFrameSize(
    std::optional<std::int64_t> frameSize, std::optional<std::int64_t> sampleRate) {
    const auto usable = [](std::int64_t value) {
        return value > 0 && value <= std::numeric_limits<std::int32_t>::max();
    };
    if (frameSize.has_value()) {
        if (!usable(*frameSize)) {
            throw PrecomputationError{"EngineConfig.frameSize is " + std::to_string(*frameSize) +
                                      "; it must be positive"};
        }
        return FrameSize{static_cast<std::int32_t>(*frameSize)};
    }
    if (!sampleRate.has_value()) {
        throw PrecomputationError{
            "EngineConfig.frameSize and EngineConfig.sampleRate are both NULL; one is needed "
            "for the frame size"};
    }
    if (!usable(*sampleRate)) {
        throw PrecomputationError{
            "EngineConfig.sampleRate is " + std::to_string(*sampleRate) + "; it must be positive"};
    }
    return FrameSize::oneSecond(SampleRate{static_cast<std::int32_t>(*sampleRate)});
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

std::size_t PrecomputationReport::failureCount() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(partials, [](const PartialOutcome& p) { return !p.ok(); }));
}

PrecomputationReport precomputeAll(EngineDb& db) {
    const std::vector<DbRow> config = db.query(kReadConfig);
    if (config.size() != 1) {
        throw PrecomputationError{
            "expected exactly one EngineConfig, found " + std::to_string(config.size())};
    }
    PrecomputationReport report;
    report.frameSize =
        resolveFrameSize(optionalInt64(config.front().at(0)), optionalInt64(config.front().at(1)));

    const Inputs inputs = readInputs(db);
    SourceTimelineCache sources;

    // Partials are independent; id order only makes the report deterministic.
    for (const auto& [partialId, partialName] : inputs.partials) {
        PartialOutcome outcome{
            .partialId = partialId,
            .partialName = partialName,
            .error = std::nullopt,
            .segmentCount = 0,
            .compensationCount = 0,
            .frames = std::nullopt,
            .entryCount = 0,
        };
        try {
            precomputePartial(db, inputs, sources, partialId, report.frameSize, outcome);
        } catch (const PartialPrecomputationError& e) {
            outcome.error = e.what();
        } catch (const EngineDbError& e) {
            outcome.error = std::string{"engine-db: "} + e.what();
        }
        report.partials.push_back(std::move(outcome));
    }
    return report;
}

} // namespace rainbohz::rendering_engine
