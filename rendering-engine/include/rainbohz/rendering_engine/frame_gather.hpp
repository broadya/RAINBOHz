// SPDX-License-Identifier: Apache-2.0

#pragma once

// What rendering reads from engine-db for one frame, whichever backend holds
// it (@see ADR 0028; docs/rendering.md §6.3).
//
// The gather is the only layer of rendering that touches engine-db. It starts
// from the frame's FrameEntry nodes and walks each list forward from the node
// an entry leads to, until a node starts at or after the frame's end. The
// records are unvalidated: every property is nullable in the schema, so every
// one is optional, and rendering checks them afterwards (§9).

#include "rainbohz/rendering_engine/store_types.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rainbohz::rendering_engine {

/// A PartialIntegrableSegment with its PartialSegmentNominalFrequency, as
/// read.
struct GatheredSegment {
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
struct GatheredPoint {
    std::int64_t id = 0;                   ///< AmplitudePhysicalTimelinePoint.id.
    std::optional<double> time;            ///< time, in samples.
    std::optional<double> amplitude;       ///< amplitude, in dB.
    std::optional<std::int64_t> curveType; ///< amplitude_curve_type; 0 (linear) renders.
    /// Whether the point has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// A PartialPhaseCompensationSegment as read.
struct GatheredCompensation {
    std::int64_t id = 0;              ///< PartialPhaseCompensationSegment.id.
    std::optional<double> startTime;  ///< start_time, in samples.
    std::optional<double> endTime;    ///< end_time, in samples.
    std::optional<double> startDelta; ///< start_phase_delta, in radians.
    std::optional<double> endDelta;   ///< end_phase_delta, in radians.
    /// Whether the segment has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
};

/// An azimuth, elevation or distance point as read.
struct GatheredSpatialPoint {
    std::int64_t id = 0;        ///< The azimuth, elevation or distance point's id.
    std::optional<double> time; ///< time, in samples.
    /// The azimuth or elevation in radians, or the distance in metres.
    std::optional<double> value;
    std::optional<std::int64_t> curveType; ///< The `*_curve_type`; 0 (linear) renders.
    /// Whether the point has a NEXT_POINT successor. Set by the walk.
    bool hasSuccessor = false;
    /// Whether the point has a predecessor point. Read for entry points only.
    bool hasPredecessor = false;
};

/// Everything gathered for one FrameEntry, before validation.
struct GatheredEntry {
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
    std::array<std::vector<GatheredSpatialPoint>, 3> spatial;
    /// The entry segment(s) and, after the walk, their successors in order.
    std::vector<GatheredSegment> segments;
    /// Whether the (first) entry segment has a predecessor segment.
    bool entrySegmentHasPredecessor = false;
    /// The entry point(s) and, after the walk, their successors in order.
    std::vector<GatheredPoint> points;
    /// The entry compensation segment(s) and, after the walk, their
    /// successors in order.
    std::vector<GatheredCompensation> compensation;
    /// The failure found during the gather, if any, so that the walk can
    /// leave a broken partial alone and report it afterwards.
    std::optional<std::string> failure;
};

/// Everything gathered for one SoundSource's spatial timelines in a frame,
/// before validation: one walk per kind, shared by every partial of the
/// source, from the earliest entry point any of them has.
struct GatheredSource {
    std::int64_t soundSourceId = 0; ///< SoundSource.id.
    /// "SoundSource <name> (id <id>)", for messages.
    std::string description;
    /// Per spatial kind: the earliest entry point and, after the walk, its
    /// successors in order.
    std::array<std::vector<GatheredSpatialPoint>, 3> points;
    /// The failure found during the gather, if any. Every partial of the
    /// source fails with it.
    std::optional<std::string> failure;
};

/// Everything the gather produced for one frame.
struct GatheredFrame {
    /// One record per FrameEntry, ordered by partial id, then FrameEntry.id.
    std::vector<GatheredEntry> entries;
    /// One record per SoundSource of the entries, by SoundSource.id.
    std::map<std::int64_t, GatheredSource> sources;
};

/// Marks every entry that leads to other than exactly one node of each list
/// type as failed, so that the walks leave it alone and validation reports
/// it.
///
/// @param[in,out] entries  The entries as read. `failure` is set on each
///                         miscounted one that has none yet.
void markMiscountedEntries(std::vector<GatheredEntry>& entries);

/// Groups a frame's entries by their SoundSource, ready for each source's
/// spatial timelines to be walked once.
///
/// A source's entry points depend on the frame alone, so every partial of
/// the source leads to the same three; a disagreement is a stale entry and
/// fails the source. Entries that already failed, or whose set is not in
/// exactly one source, contribute nothing.
///
/// @param entries  The frame's entries, with markMiscountedEntries() applied.
/// @return One record per source, in SoundSource.id order, each holding only
///         its three entry points, unwalked.
std::vector<GatheredSource> groupSources(const std::vector<GatheredEntry>& entries);

/// What rendering reads from, implemented by each engine-db backend.
///
/// Rendering only reads. Calls come from one thread, before any worker
/// thread of the frame starts (@see ADR 0026).
class FrameSource {
public:
    virtual ~FrameSource() = default;

    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;
    FrameSource(FrameSource&&) = delete;
    FrameSource& operator=(FrameSource&&) = delete;

    /// Reads every EngineConfig.
    ///
    /// @return One record per EngineConfig node, in no particular order.
    /// @throws EngineDbError  If the read fails.
    virtual std::vector<EngineConfigRecord> readEngineConfigs() = 0;

    /// Gathers everything frame `frame` needs, unvalidated (§6.3).
    ///
    /// @param frame  The frame.
    /// @param size   Frame size.
    /// @return One record per FrameEntry of the frame, ordered by partial id,
    ///         and one per SoundSource they lead to. Empty when there is no
    ///         Frame node or it has no entries. engine-db is not modified.
    /// @throws EngineDbError  If a read fails or finds a value of an
    ///         unexpected type.
    virtual GatheredFrame gatherFrame(FrameNumber frame, FrameSize size) = 0;

protected:
    FrameSource() = default;
};

} // namespace rainbohz::rendering_engine
