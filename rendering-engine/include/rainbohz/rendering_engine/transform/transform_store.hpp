// SPDX-License-Identifier: Apache-2.0

#pragma once

// What the transform reads from engine-db and writes back, whichever backend
// holds it (@see ADR 0028; docs/transform.md).
//
// The transform reads everything in one bulk read, resolves the tempo maps,
// transforms each set's partials and each SoundSource on its own, and writes
// each partial's and each source's output as one all-or-nothing unit. The
// records here are that contract. They carry ids and raw representations as
// engine-db holds them; transform.cpp validates them and wraps them in unit
// types.
//
// This header names the tempo map's records, so, like everything that reads
// musical time, it is included only from the transform directories.

#include "rainbohz/rendering_engine/store_types.hpp"
#include "rainbohz/rendering_engine/transform/polytempo.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// A node id to node id lookup: each node's id maps to the id of the node its
/// NEXT_POINT edge points to (readLinks(), walkList()).
using IdMap = std::unordered_map<std::int64_t, std::int64_t>;

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
///
/// Lists are given as heads and NEXT_POINT links, as the schema holds them,
/// so that the transform itself checks them for loops whichever backend read
/// them.
struct TransformInputs {
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

// ---------------------------------------------------------------------------
// Outputs, one unit per partial or source
// ---------------------------------------------------------------------------

/// Existing physical timelines of one kind to delete, each as its id and the
/// ids of its points.
using StaleTimelines = std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>>;

/// Everything to be written for one partial: one unit, written whole or not
/// at all (§7).
///
/// The unit deletes the partial's existing frequency, amplitude and phase
/// physical timelines with their points, by id, and creates the three new
/// ones: each a timeline node owned by the partial through
/// HAS_PHYSICAL_TIMELINE, and its points linked in order by NEXT_POINT. A
/// phase point whose phase is nullopt is written with a NULL phase.
struct TransformedPartial {
    std::int64_t partialId = 0; ///< The Partial.

    /// Existing timelines to delete, each with its point ids.
    StaleTimelines staleFrequency;
    StaleTimelines staleAmplitude; ///< As staleFrequency, amplitude.
    StaleTimelines stalePhase;     ///< As staleFrequency, phase.

    /// The points to write, in physical units.
    std::vector<PhysicalPoint> frequency;
    std::vector<PhysicalPoint> amplitude; ///< As frequency, amplitude.
    std::vector<PhasePoint> phase;        ///< As frequency, phase.
};

/// Everything to be written for one SoundSource: one unit, written whole or
/// not at all (§6.8, §7).
///
/// The unit deletes the source's existing azimuth, elevation and distance
/// physical timelines with their points, by id, and creates the three new
/// ones, each owned by the source through HAS_PHYSICAL_TIMELINE, its points
/// linked in order by NEXT_POINT.
struct TransformedSource {
    std::int64_t sourceId = 0; ///< The SoundSource.

    /// Per kind, in kSpatialKinds order: existing timelines to delete.
    std::array<StaleTimelines, 3> stale;

    /// Per kind, in kSpatialKinds order: the points to write, in physical
    /// units (azimuth and elevation in radians, distance in metres).
    SpatialTriple<PhysicalPoint> points;
};

/// What the transform reads from and writes to, implemented by each
/// engine-db backend.
///
/// Calls come from one thread, in the order transformAll() makes them:
/// readEngineConfigs(), readInputs(), then writePartials() and
/// writeSources() as often as needed.
class TransformStore {
public:
    virtual ~TransformStore() = default;

    TransformStore(const TransformStore&) = delete;
    TransformStore& operator=(const TransformStore&) = delete;
    TransformStore(TransformStore&&) = delete;
    TransformStore& operator=(TransformStore&&) = delete;

    /// Reads every EngineConfig.
    ///
    /// @return One record per EngineConfig node, in no particular order.
    /// @throws EngineDbError  If the read fails.
    virtual std::vector<EngineConfigRecord> readEngineConfigs() = 0;

    /// Reads every input the transform needs.
    ///
    /// @return The inputs. engine-db is not modified.
    /// @throws EngineDbError  If the read fails or finds a value of an
    ///         unexpected type.
    virtual TransformInputs readInputs() = 0;

    /// Writes each partial's output as a unit of its own.
    ///
    /// @param partials  The units, in order.
    /// @return One entry per unit, in order: nullopt when it was written, or
    ///         the reason it was not, in which case nothing of it was
    ///         written. One failed unit does not stop the others.
    /// @throws EngineDbError  If engine-db cannot be written at all.
    virtual std::vector<std::optional<std::string>> writePartials(
        std::span<const TransformedPartial> partials) = 0;

    /// Writes each source's output as a unit of its own.
    ///
    /// @param sources  The units, in order.
    /// @return As writePartials().
    /// @throws EngineDbError  If engine-db cannot be written at all.
    virtual std::vector<std::optional<std::string>> writeSources(
        std::span<const TransformedSource> sources) = 0;

protected:
    TransformStore() = default;
};

} // namespace rainbohz::rendering_engine::transform
