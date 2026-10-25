// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/ladybug_store.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/precomputation_store.hpp"
#include "rainbohz/rendering_engine/spatial_gains.hpp"
#include "rainbohz/rendering_engine/write_batch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// @file
/// @brief The Ladybug backend's PrecomputationStore: precomputation's bulk
/// read as fixed Cypher queries, and its writes as WriteBatch units.
///
/// The queries and the write statements are the ones precomputation.cpp ran
/// before the stores were introduced (ADR 0028), unchanged. Section numbers
/// in comments refer to docs/precomputation.md.
///
/// @see precomputation_store.hpp, ladybug_store.hpp

namespace rainbohz::rendering_engine {

namespace {

// ---------------------------------------------------------------------------
// Queries
//
// Every statement is a fixed string, so that EngineDb prepares it once and
// reuses it for every partial and every segment. Values are always bound as
// parameters, never spliced into the text.
// ---------------------------------------------------------------------------

/// Reads every Partial's id and name, in id order.
constexpr std::string_view kReadPartials = "MATCH (p:Partial) RETURN p.id, p.name ORDER BY p.id;";

/// One row per FrequencyPhysicalTimeline, with the head of its point list, or NULL
/// when the timeline has no points.
constexpr std::string_view kReadFrequencyHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:FrequencyPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:FrequencyPhysicalTimelinePoint) "
    "RETURN p.id, f.id;";

/// Reads every FrequencyPhysicalTimelinePoint's time, frequency and curve type.
constexpr std::string_view kReadFrequencyPoints =
    "MATCH (f:FrequencyPhysicalTimelinePoint) "
    "RETURN f.id, f.time, f.frequency, f.frequency_curve_type;";

/// Reads the NEXT_POINT links between FrequencyPhysicalTimelinePoints.
constexpr std::string_view kReadFrequencyLinks =
    "MATCH (a:FrequencyPhysicalTimelinePoint)-[:NEXT_POINT]->(b:FrequencyPhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// The amplitude timeline is read for its times only, so that the frame
/// entries of §6.7 can lead into it; its values are rendering's concern.
constexpr std::string_view kReadAmplitudeHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:AmplitudePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AmplitudePhysicalTimelinePoint) "
    "RETURN p.id, a.id;";

/// Reads every AmplitudePhysicalTimelinePoint's time.
constexpr std::string_view kReadAmplitudePoints =
    "MATCH (a:AmplitudePhysicalTimelinePoint) RETURN a.id, a.time;";

/// Reads the NEXT_POINT links between AmplitudePhysicalTimelinePoints.
constexpr std::string_view kReadAmplitudeLinks =
    "MATCH (a:AmplitudePhysicalTimelinePoint)-[:NEXT_POINT]->(b:AmplitudePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// The phase timeline: its targets, and the NULL points up to which the
/// partial runs free (§6.8).
constexpr std::string_view kReadPhaseHeads =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PhasePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(f:PhasePhysicalTimelinePoint) "
    "RETURN p.id, f.id;";

/// Reads every PhasePhysicalTimelinePoint's time and phase, which may be NULL.
constexpr std::string_view kReadPhasePoints =
    "MATCH (f:PhasePhysicalTimelinePoint) RETURN f.id, f.time, f.phase;";

/// Reads the NEXT_POINT links between PhasePhysicalTimelinePoints.
constexpr std::string_view kReadPhaseLinks =
    "MATCH (a:PhasePhysicalTimelinePoint)-[:NEXT_POINT]->(b:PhasePhysicalTimelinePoint) "
    "RETURN a.id, b.id;";

/// The SoundSource of each partial, through its PartialSet. HAS_PARTIAL and
/// HAS_PARTIALSET are ONE_MANY, so a partial has at most one row.
constexpr std::string_view kReadPartialSources =
    "MATCH (ss:SoundSource)-[:HAS_PARTIALSET]->(:PartialSet)-[:HAS_PARTIAL]->(p:Partial) "
    "RETURN p.id, ss.id, ss.name;";

/// A SoundSource's spatial timelines (§7), read for their times and checked
/// for their values, so that the frame entries of §6.7 can lead into them.
/// One statement of each shape per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kReadSpatialHeads{
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:AzimuthPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:AzimuthPhysicalTimelinePoint) RETURN s.id, a.id;",
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:ElevationPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:ElevationPhysicalTimelinePoint) RETURN s.id, a.id;",
    "MATCH (s:SoundSource)-[:HAS_PHYSICAL_TIMELINE]->(t:DistancePhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(a:DistancePhysicalTimelinePoint) RETURN s.id, a.id;",
};

/// Reads every azimuth, elevation and distance point's time, value and curve
/// type, one statement per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kReadSpatialPoints{
    "MATCH (a:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.azimuth, a.azimuth_curve_type;",
    "MATCH (a:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.elevation, a.elevation_curve_type;",
    "MATCH (a:DistancePhysicalTimelinePoint) "
    "RETURN a.id, a.time, a.distance, a.distance_curve_type;",
};

/// Reads the NEXT_POINT links between azimuth, elevation and distance points,
/// one statement per kind, in kSpatialKinds order.
constexpr std::array<std::string_view, 3> kReadSpatialLinks{
    "MATCH (a:AzimuthPhysicalTimelinePoint)-[:NEXT_POINT]->(b:AzimuthPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:ElevationPhysicalTimelinePoint)-[:NEXT_POINT]->(b:ElevationPhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
    "MATCH (a:DistancePhysicalTimelinePoint)-[:NEXT_POINT]->(b:DistancePhysicalTimelinePoint) "
    "RETURN a.id, b.id;",
};

/// Output from an earlier run, which is deleted before the new output is
/// written. A PartialSegmentPhysicalTimeline heads two lists: its integrable
/// segments and its compensation segments.
constexpr std::string_view kReadExistingPhysicalTimelines =
    "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:PartialSegmentPhysicalTimeline) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(s:PartialIntegrableSegment) "
    "OPTIONAL MATCH (t)-[:NEXT_POINT]->(c:PartialPhaseCompensationSegment) "
    "RETURN p.id, t.id, s.id, c.id;";

/// Reads the NEXT_POINT links between PartialIntegrableSegments.
constexpr std::string_view kReadSegmentLinks =
    "MATCH (a:PartialIntegrableSegment)-[:NEXT_POINT]->(b:PartialIntegrableSegment) "
    "RETURN a.id, b.id;";

/// Reads the NEXT_POINT links between PartialPhaseCompensationSegments.
constexpr std::string_view kReadCompensationLinks =
    "MATCH (a:PartialPhaseCompensationSegment)-[:NEXT_POINT]->(b:PartialPhaseCompensationSegment) "
    "RETURN a.id, b.id;";

/// Every FrameEntry of an earlier run, with the partial it belongs to, so that
/// it can be deleted by id rather than found through the partial (§10).
constexpr std::string_view kReadExistingFrameEntries =
    "MATCH (e:FrameEntry)-[:FRAME_ENTRY_PARTIAL]->(p:Partial) RETURN p.id, e.id;";

/// The PartialSegmentNominalFrequency of every existing PartialIntegrableSegment.
constexpr std::string_view kReadExistingNominals =
    "MATCH (s:PartialIntegrableSegment)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->"
    "(n:PartialSegmentNominalFrequency) RETURN s.id, n.id;";

/// Every Partial that already has PARTIAL_IN_FRAME edges.
constexpr std::string_view kReadExistingFrameScope =
    "MATCH (p:Partial)-[:PARTIAL_IN_FRAME]->(:Frame) RETURN DISTINCT p.id;";

/// The batch operations precomputation writes through, one per table or
/// relationship pair (§10). Registered once per run.
struct Writes {
    /// @name Deletes of an earlier run's output, by id
    ///@{
    WriteOperation deleteFrameEntries; ///< FrameEntry nodes.
    WriteOperation deleteNominals;     ///< PartialSegmentNominalFrequency nodes.
    WriteOperation deleteSegments;     ///< PartialIntegrableSegment nodes.
    WriteOperation deleteCompensation; ///< PartialPhaseCompensationSegment nodes.
    WriteOperation deleteTimelines;    ///< PartialSegmentPhysicalTimeline nodes.
    WriteOperation deleteFrameScope;   ///< A partial's PARTIAL_IN_FRAME edges.
    ///@}

    /// @name Nodes
    ///@{
    WriteOperation mergeFrame;    ///< Frame nodes, created if missing.
    WriteOperation timeline;      ///< PartialSegmentPhysicalTimeline.
    WriteOperation segment;       ///< PartialIntegrableSegment.
    WriteOperation nominalLinear; ///< PartialSegmentNominalFrequency with a linear component.
    WriteOperation nominalFlat;   ///< PartialSegmentNominalFrequency without one.
    WriteOperation compensation;  ///< PartialPhaseCompensationSegment.
    WriteOperation frameEntry;    ///< FrameEntry.
    ///@}

    /// @name Relationships
    ///@{
    WriteOperation hasTimeline;       ///< Partial → PartialSegmentPhysicalTimeline.
    WriteOperation firstSegment;      ///< Timeline → first segment, NEXT_POINT.
    WriteOperation nextSegment;       ///< Segment → segment, NEXT_POINT.
    WriteOperation firstCompensation; ///< Timeline → first compensation segment.
    WriteOperation nextCompensation;  ///< Compensation → compensation segment.
    WriteOperation segmentNominal;    ///< Segment → its nominal frequency.
    WriteOperation partialInFrame;    ///< Partial → Frame, by frame number.
    WriteOperation frameToEntry;      ///< Frame → FrameEntry.
    WriteOperation entryPartial;      ///< FrameEntry → Partial.
    /// FrameEntry → the six list nodes it leads to (§6.7): segment, amplitude
    /// point, compensation segment, then azimuth, elevation and distance
    /// points.
    std::array<WriteOperation, 6> entryLists;
    ///@}
};

/// Registers every operation precomputation writes through.
///
/// @param[in,out] batch  The batch to register them with.
/// @return The operations.
Writes registerWrites(WriteBatch& batch) {
    using enum DbColumnType;
    constexpr std::string_view kNext = "NEXT_POINT";
    constexpr std::string_view kTimeline = "PartialSegmentPhysicalTimeline";
    constexpr std::string_view kSegment = "PartialIntegrableSegment";
    constexpr std::string_view kCompensation = "PartialPhaseCompensationSegment";
    constexpr std::string_view kEntryList = "FRAME_ENTRY_PHYSICAL_TIMELINE";
    const auto byId = [&](std::string_view table) { return batch.addDeleteById(table); };
    const auto edge = [&](std::string_view rel, std::string_view from, std::string_view to) {
        return batch.addEdgeInsert(rel, from, to);
    };
    Writes writes{};
    writes.deleteFrameEntries = byId("FrameEntry");
    writes.deleteNominals = byId("PartialSegmentNominalFrequency");
    writes.deleteSegments = byId(kSegment);
    writes.deleteCompensation = byId(kCompensation);
    writes.deleteTimelines = byId(kTimeline);
    writes.deleteFrameScope = batch.addStatement(WritePhase::Delete,
        "UNWIND $rows AS r MATCH (p:Partial {id: r.id})-[e:PARTIAL_IN_FRAME]->(:Frame) DELETE e;",
        {{"id", Int64}});

    // Frame numbers are INT32 in the schema; the row carries an INT64.
    writes.mergeFrame = batch.addStatement(WritePhase::Node,
        "UNWIND $rows AS r MERGE (:Frame {frame_number: CAST(r.frame AS INT32)});",
        {{"frame", Int64}}, true);
    writes.timeline = batch.addNodeInsert("CREATE (n:PartialSegmentPhysicalTimeline)", {});
    writes.segment = batch.addNodeInsert("CREATE (n:PartialIntegrableSegment {start_time: "
                                         "r.startTime, end_time: r.endTime, start_phase: "
                                         "r.startPhase})",
        {{"startTime", Double}, {"endTime", Double}, {"startPhase", Double}});
    // Curve properties are never written, so they are NULL, as §5 requires;
    // a flat nominal frequency leaves linear_gradient NULL the same way.
    writes.nominalLinear = batch.addNodeInsert(
        "CREATE (n:PartialSegmentNominalFrequency {start_nominal_frequency: r.startFrequency, "
        "linear_component: true, linear_gradient: r.linearGradient, curve_component: false})",
        {{"startFrequency", Double}, {"linearGradient", Double}});
    writes.nominalFlat = batch.addNodeInsert(
        "CREATE (n:PartialSegmentNominalFrequency {start_nominal_frequency: r.startFrequency, "
        "linear_component: false, curve_component: false})",
        {{"startFrequency", Double}});
    writes.compensation = batch.addNodeInsert(
        "CREATE (n:PartialPhaseCompensationSegment {start_time: r.startTime, end_time: "
        "r.endTime, start_phase_delta: r.startDelta, end_phase_delta: r.endDelta})",
        {{"startTime", Double}, {"endTime", Double}, {"startDelta", Double}, {"endDelta", Double}});
    writes.frameEntry = batch.addNodeInsert("CREATE (n:FrameEntry)", {});

    writes.hasTimeline = edge("HAS_PHYSICAL_TIMELINE", "Partial", kTimeline);
    writes.firstSegment = edge(kNext, kTimeline, kSegment);
    writes.nextSegment = edge(kNext, kSegment, kSegment);
    writes.firstCompensation = edge(kNext, kTimeline, kCompensation);
    writes.nextCompensation = edge(kNext, kCompensation, kCompensation);
    writes.segmentNominal =
        edge("SEGMENT_TO_FREQUENCY_NOMINAL", kSegment, "PartialSegmentNominalFrequency");
    writes.partialInFrame = edge("PARTIAL_IN_FRAME", "Partial", "Frame");
    writes.frameToEntry = edge("FRAME_ENTRY", "Frame", "FrameEntry");
    writes.entryPartial = edge("FRAME_ENTRY_PARTIAL", "FrameEntry", "Partial");
    writes.entryLists = {
        edge(kEntryList, "FrameEntry", kSegment),
        edge(kEntryList, "FrameEntry", "AmplitudePhysicalTimelinePoint"),
        edge(kEntryList, "FrameEntry", kCompensation),
        edge(kEntryList, "FrameEntry", "AzimuthPhysicalTimelinePoint"),
        edge(kEntryList, "FrameEntry", "ElevationPhysicalTimelinePoint"),
        edge(kEntryList, "FrameEntry", "DistancePhysicalTimelinePoint"),
    };
    return writes;
}

// ---------------------------------------------------------------------------
// Inputs, as read in bulk before any partial is processed
// ---------------------------------------------------------------------------

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
PrecomputationInputs readInputs(EngineDb& db) {
    PrecomputationInputs inputs;

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
    inputs.segmentNominal = readLinks(db, kReadExistingNominals);

    for (const DbRow& row : db.query(kReadExistingFrameEntries)) {
        inputs.existingFrameEntries[requireInt64(row.at(0))].push_back(requireInt64(row.at(1)));
    }
    for (const DbRow& row : db.query(kReadExistingFrameScope)) {
        inputs.existingFrameScope.insert(requireInt64(row.at(0)));
    }

    return inputs;
}

/// Stages the replacement of a partial's output as one unit of `batch`
/// (§6.2, stage 5; §9).
///
/// The unit deletes, by id, the partial's FrameEntry nodes, its stale
/// timelines with their segments, nominal frequencies and compensation
/// segments, and its PARTIAL_IN_FRAME edges; then creates the new timeline,
/// segment chain and nominal frequencies, compensation chain, any missing
/// Frame nodes, the frame scope edges, and one FrameEntry per frame in scope
/// with its eight edges. New nodes are referred to by handle until the batch
/// flushes, so nothing here waits for an id.
///
/// @param[in,out] batch  Batch to stage into. Gains one unit; nothing is
///                       written until it flushes, and then the unit is
///                       written whole or not at all.
/// @param writes         The batch's registered operations.
/// @param output         What to write.
void stagePartial(WriteBatch& batch, const Writes& writes, const PrecomputedPartial& output) {
    batch.beginUnit();
    const auto stageIds = [&](WriteOperation operation, const std::vector<std::int64_t>& ids) {
        for (const std::int64_t id : ids) {
            batch.stageRow(operation, {id});
        }
    };

    stageIds(writes.deleteFrameEntries, output.staleFrameEntries);
    for (const StalePhysicalTimeline& stale : output.stale) {
        stageIds(writes.deleteNominals, stale.nominalIds);
        stageIds(writes.deleteSegments, stale.segmentIds);
        stageIds(writes.deleteCompensation, stale.compensationIds);
        batch.stageRow(writes.deleteTimelines, {stale.physicalTimelineId});
    }
    if (output.staleFrameScope) {
        batch.stageRow(writes.deleteFrameScope, {output.partialId});
    }

    const NodeHandle timeline = batch.stageNode(writes.timeline, {});
    batch.stageEdge(writes.hasTimeline, output.partialId, timeline);

    std::vector<NodeHandle> segments;
    segments.reserve(output.segments.size());
    for (const Segment& segment : output.segments) {
        const NodeHandle node = batch.stageNode(writes.segment,
            {segment.startTime.value(), segment.endTime.value(), segment.startPhase.value()});
        const std::optional<FrequencyGradient>& gradient = segment.nominal.linearGradient;
        const NodeHandle nominal =
            gradient.has_value()
                ? batch.stageNode(writes.nominalLinear,
                      {segment.nominal.startFrequency.value(), gradient->value()})
                : batch.stageNode(writes.nominalFlat, {segment.nominal.startFrequency.value()});
        batch.stageEdge(segments.empty() ? writes.firstSegment : writes.nextSegment,
            segments.empty() ? timeline : segments.back(), node);
        batch.stageEdge(writes.segmentNominal, node, nominal);
        segments.push_back(node);
    }

    std::vector<NodeHandle> compensation;
    compensation.reserve(output.compensation.size());
    for (const CompensationSegment& segment : output.compensation) {
        const NodeHandle node = batch.stageNode(
            writes.compensation, {segment.startTime.value(), segment.endTime.value(),
                                     segment.startDelta.value(), segment.endDelta.value()});
        batch.stageEdge(compensation.empty() ? writes.firstCompensation : writes.nextCompensation,
            compensation.empty() ? timeline : compensation.back(), node);
        compensation.push_back(node);
    }

    // Counted in 64 bits, so that a last frame of INT32_MAX ends the loop.
    for (std::int64_t k = output.frames.first.value(); k <= output.frames.last.value(); ++k) {
        batch.stageRow(writes.mergeFrame, {k});
        batch.stageEdge(writes.partialInFrame, output.partialId, k);
    }

    for (const FrameEntryTarget& entry : output.entries) {
        const NodeHandle node = batch.stageNode(writes.frameEntry, {});
        batch.stageEdge(writes.frameToEntry, std::int64_t{entry.frame.value()}, node);
        batch.stageEdge(writes.entryPartial, node, output.partialId);
        batch.stageEdge(writes.entryLists[0], node, segments.at(entry.segment));
        batch.stageEdge(writes.entryLists[1], node, output.amplitudePointIds.at(entry.point));
        batch.stageEdge(writes.entryLists[2], node, compensation.at(entry.compensation));
        for (std::size_t k = 0; k < 3; ++k) {
            batch.stageEdge(writes.entryLists.at(3 + k), node,
                output.spatialPointIds.at(k)[entry.spatial.at(k)]);
        }
    }
}

/// precomputation's reads and writes over a Ladybug engine-db.
class LadybugPrecomputationStore final : public PrecomputationStore {
public:
    /// Serves precomputation from `db`.
    ///
    /// @param[in,out] db  Database holding the score. It must outlive the
    ///                    store, which keeps a pointer to it.
    explicit LadybugPrecomputationStore(EngineDb& db) : db_{&db} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return readLadybugEngineConfigs(*db_);
    }

    PrecomputationInputs readInputs() override { return rendering_engine::readInputs(*db_); }

    std::vector<std::optional<std::string>> writePartials(
        std::span<const PrecomputedPartial> partials) override {
        WriteBatch batch{*db_};
        const Writes writes = registerWrites(batch);
        for (const PrecomputedPartial& partial : partials) {
            stagePartial(batch, writes, partial);
        }
        return batch.flush();
    }

private:
    EngineDb* db_; ///< The database; not owned.
};

} // namespace

std::unique_ptr<PrecomputationStore> makeLadybugPrecomputationStore(EngineDb& db) {
    return std::make_unique<LadybugPrecomputationStore>(db);
}

} // namespace rainbohz::rendering_engine
