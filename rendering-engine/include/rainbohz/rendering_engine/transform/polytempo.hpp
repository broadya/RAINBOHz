// SPDX-License-Identifier: Apache-2.0

#pragma once

// Polytempo: several tempo maps in one Score, tied together by
// TEMPO_MAP_SYNCHRONIZATION_POINT edges, resolved to physical time.
//
// The design is docs/polytempo.md; section numbers in comments refer to it.
// In outline:
//
//   - the edges form a DAG whose single root is the Score's map, which plays
//     exactly as written; maps are resolved parents first (§4, §7);
//   - a child's sync points are TempoMapPoints of the child, each anchored to
//     the physical time its parent has at `sync_point_parent`. The child's
//     time at a sync point equals its anchor exactly: that rule overrides
//     every other (§4);
//   - one sync point shifts the child (§5.2); two or more stretch each region
//     between consecutive sync points uniformly, then compensate at interior
//     sync points so that the tempo is continuous where ramps meet (§5.3 to
//     §5.11). Holds are never bent;
//   - every resolved tempo must lie in the schema's [1, 1000] BPM, or the
//     map is rejected (§6).
//
// The result of resolving a map is a ResolvedTempoMap: a list of segments in
// physical time, each with its own start time and its start and end tempo,
// that converts a tick with the same closed forms as TempoMap. A map with no
// sync points resolves to its nominal segments, bit for bit as TempoMap
// converts them.
//
// Everything here is pure: it takes records and returns values. transform.cpp
// reads the graph from engine-db and writes nothing of the result back; the
// resolved maps exist only for the duration of a transform run.

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// One sync point of a child map: a TempoMapPoint of the child, and the
/// physical time it must land on.
struct SyncAnchor {
    /// Position of the child's TempoMapPoint, `sync_point_child`.
    MusicalTime childTick;

    /// The parent's physical time at `sync_point_parent`, once the parent is
    /// resolved. May be negative.
    SecondsTime anchor;
};

/// One segment of a resolved map (§7 step 8): a span of the child's ticks
/// over which the tempo runs from `startTempo` to `endTempo` in the Score's
/// ramp shape, starting at `startSeconds`.
///
/// Segment bounds are doubles because a moved change point (§5.10) puts a
/// virtual point at a fractional tick. A segment that starts at a sync point
/// has its `startSeconds` assigned from the anchor, not summed, so sync
/// points coincide bit for bit with their parents.
struct ResolvedSegment {
    /// Where the segment starts, in ticks of the map. Whole for a
    /// TempoMapPoint; fractional for a virtual point.
    double startTicks = 0.0;

    /// The segment's length in ticks; +infinity on the last segment, whose
    /// tempo holds forever.
    double lengthTicks = 0.0;

    /// When the segment starts.
    SecondsTime startSeconds;

    /// The tempo at the segment's start.
    Tempo startTempo{0.0};

    /// The tempo at the segment's end. Equal to `startTempo` for a hold, or
    /// for the final segment.
    Tempo endTempo{0.0};
};

/// A tempo map in physical time: what every tick of the map converts
/// through once synchronisation has been resolved.
class ResolvedTempoMap {
public:
    /// Builds a resolved map from its segments.
    ///
    /// @param resolution  Ticks per beat. Positive.
    /// @param ramp        The shape of every ramp, from the Score.
    /// @param segments    At least one, in order of `startTicks`, the first
    ///                    at tick 0, the last of infinite length with equal
    ///                    tempi. Not validated.
    ResolvedTempoMap(
        TicksPerBeat resolution, TempoRamp ramp, std::vector<ResolvedSegment> segments);

    /// The resolved form of a map with no synchronisation: its own segments,
    /// from time zero, as `TempoMap::toSeconds` converts them (§5.1).
    ///
    /// @param map  A validated map.
    /// @return A map whose toSeconds() agrees with `map.toSeconds()` bit for
    ///         bit at every position.
    static ResolvedTempoMap nominal(const TempoMap& map);

    /// The seconds from the Score's time zero to a position on this map.
    ///
    /// Finds the segment containing `at` by binary search and adds
    /// secondsThrough() for the part of it before `at` to the segment's start
    /// time. After the last segment's start the last tempo holds.
    ///
    /// @param at  A position on this map. Must not be before tick 0.
    /// @return The position in seconds. Negative when the map's start was
    ///         shifted before time zero (§10): the squash rule places such
    ///         material at 0 s (@see ADR 0014).
    /// @throws std::invalid_argument  If `at` is negative.
    SecondsTime toSeconds(MusicalTime at) const;

    /// The map's resolution.
    ///
    /// @return Ticks per beat, as given to the constructor.
    TicksPerBeat ticksPerBeat() const noexcept { return resolution_; }

    /// The shape of the map's ramps.
    ///
    /// @return The shape given to the constructor.
    TempoRamp ramp() const noexcept { return ramp_; }

    /// The map's segments, in order.
    ///
    /// @return The segments; never empty. Valid while this map is.
    const std::vector<ResolvedSegment>& segments() const noexcept { return segments_; }

private:
    /// Ticks per beat, as given to the constructor.
    TicksPerBeat resolution_;

    /// The shape of every ramp, as given to the constructor.
    TempoRamp ramp_;

    /// The segments, in order of `startTicks`; never empty.
    std::vector<ResolvedSegment> segments_;
};

/// Resolves one child map against the anchors of its sync points (§5, §7).
///
/// With one anchor the map is shifted (§5.2). With two or more, each region
/// between consecutive sync points is stretched by one factor, interior sync
/// points where ramps meet take the geometric-mean tempo and their regions
/// are re-solved to hit their anchors (§5.6), holds are never bent (§5.7),
/// single-segment regions follow §5.8 to §5.11, and the segments outside the
/// synced span adapt as §5.4 says. Every root find is a bracketed bisection
/// to adjacent doubles, so nothing here is an approximation beyond the
/// arithmetic itself.
///
/// @param nominal  The child's map as written, validated.
/// @param anchors  One per sync point, in any order; at least one. Each
///                 `childTick` must be the position of one of the map's
///                 points.
/// @return The resolved map. Its time at every `childTick` is that anchor's
///         `anchor` exactly.
/// @throws TempoMapError  If `anchors` is empty; a `childTick` is not a
///         point's position, or two are equal; the anchors do not strictly
///         increase in tick order; a region cannot be fitted (§5.9, §5.10:
///         no valid candidate, equal end tempi between two holds, or a
///         stretch with both ends pinned that cannot fill its time); or any
///         resolved tempo falls outside [1, 1000] BPM. The message names the
///         sync point or segment by tick and gives the value; it does not
///         name the map, which the caller knows.
ResolvedTempoMap resolveSynchronised(const TempoMap& nominal, std::span<const SyncAnchor> anchors);

/// A TEMPO_MAP_SYNCHRONIZATION_POINT edge as read from engine-db, before
/// validation. Both ticks are nullable in the schema.
struct SyncEdgeRecord {
    /// The child TempoMap, the edge's FROM.
    std::int64_t childId = 0;

    /// The parent TempoMap, the edge's TO.
    std::int64_t parentId = 0;

    /// `sync_point_child`, ticks of the child.
    std::optional<std::int64_t> syncPointChild;

    /// `sync_point_parent`, ticks of the parent.
    std::optional<std::int64_t> syncPointParent;
};

/// The order in which a Score's maps resolve, and whose Score each belongs
/// to.
struct TempoMapOrder {
    /// Every TempoMap.id, parents before children, roots first in Score
    /// order.
    std::vector<std::int64_t> order;

    /// Score.id of the Score whose root each map descends from, by
    /// TempoMap.id.
    std::unordered_map<std::int64_t, std::int64_t> scoreOfMap;
};

/// Orders every TempoMap parents first, from the Scores' roots (§4, §7).
///
/// @param mapIds  Every TempoMap.id in the database.
/// @param edges   Every synchronisation edge.
/// @param roots   Each Score's root map as (TempoMap.id, Score.id); a Score
///                without a map is absent.
/// @return The order and each map's Score.
/// @throws TempoMapError  If an edge names a map not in `mapIds`; a root map
///         has a parent; a map is reached from two Scores; a map is reached
///         from no Score (an orphan); or the edges contain a cycle. The
///         message names the map.
TempoMapOrder orderTempoMaps(std::span<const std::int64_t> mapIds,
    std::span<const SyncEdgeRecord> edges,
    std::span<const std::pair<std::int64_t, std::int64_t>> roots);

/// Resolves every map of every Score, parents first (§7, §5.12).
///
/// Each root resolves to its nominal segments. Each child gathers one anchor
/// per edge, the parent's resolved time at `sync_point_parent`, and resolves
/// against them with resolveSynchronised(); which parent an anchor came from
/// plays no further part.
///
/// @param maps   Every map's validated nominal form, by TempoMap.id. Each is
///               built with its Score's ramp shape.
/// @param edges  Every synchronisation edge; every tick non-NULL.
/// @param order  From orderTempoMaps() over the same maps and edges.
/// @return The resolved maps, by TempoMap.id.
/// @throws TempoMapError  If an edge's tick is NULL or `sync_point_parent` is
///         negative, or resolveSynchronised() rejects a map. The message
///         starts with "TempoMap <id>".
std::unordered_map<std::int64_t, ResolvedTempoMap> resolveTempoMapGraph(
    const std::unordered_map<std::int64_t, TempoMap>& maps, std::span<const SyncEdgeRecord> edges,
    const TempoMapOrder& order);

} // namespace rainbohz::rendering_engine::transform
