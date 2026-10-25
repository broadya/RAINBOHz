// SPDX-License-Identifier: Apache-2.0

#pragma once

// What precomputation reads from engine-db and writes back, whichever backend
// holds it (@see ADR 0028).
//
// Precomputation reads every partial's inputs in one bulk read, computes each
// partial on its own, and writes each partial's output as one all-or-nothing
// unit. The records here are that contract. They carry ids and raw
// representations as engine-db holds them; precomputation.cpp validates them
// and wraps them in unit types.

#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/store_types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine {

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
/// ahead of all writes. Lists are given as heads and NEXT_POINT links, as the
/// schema holds them, so that precomputation itself checks them for loops
/// whichever backend read them.
struct PrecomputationInputs {
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

    /// For each PartialIntegrableSegment with a nominal frequency, the
    /// PartialSegmentNominalFrequency's id.
    std::unordered_map<std::int64_t, std::int64_t> segmentNominal;

    /// For each partial with FrameEntry nodes from an earlier run, their ids.
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> existingFrameEntries;

    /// Partials that already have PARTIAL_IN_FRAME edges.
    std::unordered_set<std::int64_t> existingFrameScope;
};

/// A PartialSegmentPhysicalTimeline from an earlier run, with the ids of the
/// two lists it heads, to be deleted.
struct StalePhysicalTimeline {
    /// PartialSegmentPhysicalTimeline.id.
    std::int64_t physicalTimelineId = 0;

    /// Its PartialIntegrableSegment ids, in chain order.
    std::vector<std::int64_t> segmentIds;

    /// Its PartialPhaseCompensationSegment ids, in chain order.
    std::vector<std::int64_t> compensationIds;

    /// The PartialSegmentNominalFrequency ids of its segments.
    std::vector<std::int64_t> nominalIds;
};

/// Everything to be written for one partial: one unit, written whole or not
/// at all.
///
/// The unit replaces the partial's earlier output. It deletes, by id, the
/// partial's stale FrameEntry nodes, its stale timelines with their segments,
/// nominal frequencies and compensation segments, and its PARTIAL_IN_FRAME
/// edges. It then creates the new timeline, the segment chain with one
/// nominal frequency per segment, the compensation chain, any missing Frame
/// nodes, the frame-scope edges, and one FrameEntry per frame in scope
/// leading to its partial and to six list nodes (docs/precomputation.md
/// §6.7, §9).
struct PrecomputedPartial {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Timelines from an earlier run to delete.
    std::vector<StalePhysicalTimeline> stale;

    /// FrameEntry nodes from an earlier run to delete.
    std::vector<std::int64_t> staleFrameEntries;

    /// Whether the partial has PARTIAL_IN_FRAME edges from an earlier run.
    bool staleFrameScope = false;

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
    /// spatial indexes refer to. Views of storage precomputeAll() keeps for
    /// the whole run, so they outlive every write of the run.
    std::array<std::span<const std::int64_t>, 3> spatialPointIds;

    /// One frame entry per frame in scope.
    std::vector<FrameEntryTarget> entries;
};

/// What precomputation reads from and writes to, implemented by each
/// engine-db backend.
///
/// Calls come from one thread, in the order precomputeAll() makes them:
/// readEngineConfigs(), readInputs(), then writePartials() as often as needed.
class PrecomputationStore {
public:
    virtual ~PrecomputationStore() = default;

    PrecomputationStore(const PrecomputationStore&) = delete;
    PrecomputationStore& operator=(const PrecomputationStore&) = delete;
    PrecomputationStore(PrecomputationStore&&) = delete;
    PrecomputationStore& operator=(PrecomputationStore&&) = delete;

    /// Reads every EngineConfig.
    ///
    /// @return One record per EngineConfig node, in no particular order.
    /// @throws EngineDbError  If the read fails.
    virtual std::vector<EngineConfigRecord> readEngineConfigs() = 0;

    /// Reads every input precomputation needs, for every partial.
    ///
    /// @return The inputs. engine-db is not modified.
    /// @throws EngineDbError  If the read fails or finds a value of an
    ///         unexpected type.
    virtual PrecomputationInputs readInputs() = 0;

    /// Writes each partial's output as a unit of its own.
    ///
    /// @param partials  The units, in order. Every id they name exists, or
    ///                  was read by readInputs() in this run.
    /// @return One entry per unit, in order: nullopt when it was written, or
    ///         the reason it was not, in which case nothing of it was
    ///         written. One failed unit does not stop the others.
    /// @throws EngineDbError  If engine-db cannot be written at all.
    virtual std::vector<std::optional<std::string>> writePartials(
        std::span<const PrecomputedPartial> partials) = 0;

protected:
    PrecomputationStore() = default;
};

} // namespace rainbohz::rendering_engine
