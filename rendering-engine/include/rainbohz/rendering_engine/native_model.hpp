// SPDX-License-Identifier: Apache-2.0

#pragma once

// The native backend's engine-db: the schema's data held in C++ containers,
// in memory, without Ladybug (@see ADR 0028).
//
// The schema, `engine-db/schema/engine-schema.cypher`, is the contract. The
// types here are written by hand from it, shaped for how the stages use the
// data rather than mirroring Ladybug's tables:
//
//   - The canonical tables, which a score's Parquet files fill and the
//     transform reads once in bulk, are rows and edge lists, as the schema
//     declares them.
//   - The physical tables the transform and precomputation write are shaped.
//     A timeline's points are one contiguous range, so NEXT_POINT is vector
//     order; a segment's nominal frequency is a field of the segment; a
//     FrameEntry is one row holding its partial and its six list nodes.
//
// Every type, field and relationship pair says which part of the schema it
// holds, and how, in an `@enginedb{...}` tag. A table, property or pair the
// engine does not use yet is listed in an `@enginedb_unmapped{...}` tag with
// the reason. `make check-engine-db-mapping` (scripts/check_engine_db_mapping.py)
// fails unless the tags and the schema agree in both directions, so a schema
// edit cannot pass `make test` until the native side has been looked at. The
// check is of names and coverage; the cross-backend tests check meaning.
//
// Tag forms (no commas, so that Doxygen's aliases take them whole):
//
//   @enginedb{node Table}                      the table; a SERIAL id is the row index
//   @enginedb{Table.property}                  a node or relationship property
//   @enginedb{rel REL From->To: how}           one relationship pair
//   @enginedb_unmapped{Table: why}             a table not held, with its pairs
//   @enginedb_unmapped{Table.property: why}    a property never set (always NULL)
//   @enginedb_unmapped{rel REL From->To: why}  a pair not held
//
// Ids. Every node table but Frame has a SERIAL id, which Ladybug assigns
// from 0 in insertion order and never reuses. Here the id is the row's index:
// rows are only appended, and a deleted row stays in place, marked dead. A
// score's tables load with ids equal to their row indices (ADR 0025), and
// later inserts continue from there, so ids agree with Ladybug's wherever
// Ladybug's do not depend on how WriteBatch groups its statements (@see
// PhysicalTimelines and SegmentTimelines for the two that do).
//
// Values are held in the schema's types: a FLOAT is a float and an INT8 an
// std::int8_t, so that a value read back is what Ladybug would return. NULL
// is std::nullopt. The schema's DEFAULTs (0 for curve types and amounts) are
// applied when a row is loaded without the column.
//
// Nothing here is thread-safe: the model is used from one thread at a time.

#include "rainbohz/rendering_engine/store_types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief The native backend's in-memory engine-db.
///
/// Tables of the schema this model does not hold, because nothing uses them
/// yet (AGENTS.md: one feature at a time). Each one's relationship pairs are
/// not held either.
///
/// @enginedb_unmapped{FrequencyModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{AmplitudeModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{PhaseModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{AzimuthModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{ElevationModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{DistanceModulatorPhysicalTimeline: modulators are not built yet}
/// @enginedb_unmapped{FrequencyModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{AmplitudeModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{PhaseModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{AzimuthModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{ElevationModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{DistanceModulatorPhysicalTimelinePoint: modulators are not built yet}
/// @enginedb_unmapped{FrequencyModulatorState: modulators are not built yet}
/// @enginedb_unmapped{AmplitudeModulatorState: modulators are not built yet}
/// @enginedb_unmapped{PhaseModulatorState: modulators are not built yet}
/// @enginedb_unmapped{AzimuthModulatorState: modulators are not built yet}
/// @enginedb_unmapped{ElevationModulatorState: modulators are not built yet}
/// @enginedb_unmapped{DistanceModulatorState: modulators are not built yet}
/// @enginedb_unmapped{PartialSegmentFrequencyModulator: modulators are not built yet}
/// @enginedb_unmapped{PartialSegmentNominalFrequency.curve_start_phase: curves are not built yet so
/// precomputation never sets it}
/// @enginedb_unmapped{PartialSegmentNominalFrequency.curve_frequency: curves are not built yet so
/// precomputation never sets it}
/// @enginedb_unmapped{PartialSegmentNominalFrequency.curve_scaling_factor: curves are not built yet
/// so precomputation never sets it}

namespace rainbohz::rendering_engine::native {

/// Thrown when a change to the model would break a rule of the schema: a
/// multiplicity, or a relationship to a node that does not exist.
class NativeModelError : public EngineDbError {
public:
    using EngineDbError::EngineDbError;
};

/// A relationship pair's multiplicity, as the schema declares it. Counted
/// per FROM/TO pair, as Ladybug counts it (AGENTS.md, Type System).
enum class Multiplicity : std::uint8_t {
    OneOne,   ///< At most one edge from each FROM node and one to each TO node.
    OneMany,  ///< At most one edge to each TO node.
    ManyOne,  ///< At most one edge from each FROM node.
    ManyMany, ///< No limit.
};

/// Makes room in `rows` for `extra` more, growing geometrically, so that the
/// pushes that follow cannot throw.
///
/// The model's appends change several containers. Reserving every one of
/// them first, before anything is changed, gives each append the strong
/// guarantee: if memory runs out, the model is left as it was.
///
/// @tparam Row  The element type.
/// @param[in,out] rows  The container; its capacity may grow.
/// @param extra         How many rows are about to be added.
template <typename Row>
void reserveFor(std::vector<Row>& rows, std::size_t extra) {
    if (rows.capacity() - rows.size() < extra) {
        rows.reserve(std::max(rows.size() + extra, 2 * rows.capacity()));
    }
}

/// The properties of an edge of a relationship with none.
struct NoProperties {};

/// One edge of a relationship pair.
///
/// @tparam Properties  The relationship's properties.
template <typename Properties>
struct Edge {
    std::int64_t from = 0;   ///< The FROM node's id.
    std::int64_t to = 0;     ///< The TO node's id.
    Properties properties{}; ///< The edge's own properties.
};

/// The edges of one relationship pair, in the order they were added, with
/// the pair's multiplicity enforced as they are added.
///
/// Whether the ends exist is the caller's to check: an edge list knows
/// nothing of the node tables.
///
/// @tparam Properties  The relationship's properties.
template <typename Properties = NoProperties>
class EdgeList {
public:
    /// An empty pair.
    ///
    /// @param multiplicity  The pair's multiplicity in the schema.
    explicit EdgeList(Multiplicity multiplicity) : multiplicity_{multiplicity} {}

    /// Adds an edge.
    ///
    /// @param from        The FROM node's id.
    /// @param to          The TO node's id.
    /// @param properties  The edge's properties.
    /// @throws NativeModelError  If the edge would break the pair's
    ///         multiplicity. Nothing has been added.
    void add(std::int64_t from, std::int64_t to, Properties properties = {}) {
        const bool oneFrom =
            multiplicity_ == Multiplicity::OneOne || multiplicity_ == Multiplicity::OneMany;
        const bool oneTo =
            multiplicity_ == Multiplicity::OneOne || multiplicity_ == Multiplicity::ManyOne;
        if (oneTo && fromSeen_.contains(from)) {
            throw NativeModelError{"node " + std::to_string(from) +
                                   " already has an edge of this pair, which allows one"};
        }
        if (oneFrom && toSeen_.contains(to)) {
            throw NativeModelError{"node " + std::to_string(to) +
                                   " already has an edge to it of this pair, which allows one"};
        }
        edges_.push_back(Edge<Properties>{.from = from, .to = to, .properties = properties});
        if (oneTo) {
            fromSeen_.insert(from);
        }
        if (oneFrom) {
            toSeen_.insert(to);
        }
    }

    /// The edges, in the order they were added.
    ///
    /// @return A view, valid until the next add().
    std::span<const Edge<Properties>> edges() const noexcept { return edges_; }

    /// How many edges the pair holds.
    ///
    /// @return The count.
    std::size_t size() const noexcept { return edges_.size(); }

private:
    Multiplicity multiplicity_;           ///< The pair's multiplicity.
    std::vector<Edge<Properties>> edges_; ///< The edges, in order.
    /// The FROM nodes that have their one edge, when the pair limits them.
    std::unordered_set<std::int64_t> fromSeen_;
    /// The TO nodes that have their one edge, when the pair limits them.
    std::unordered_set<std::int64_t> toSeen_;
};

// ---------------------------------------------------------------------------
// Canonical tables: loaded from a score's Parquet files
// ---------------------------------------------------------------------------

/// An EngineConfig node.
struct EngineConfigRow {
    std::optional<std::int32_t> frameSize;  ///< @enginedb{EngineConfig.frameSize}
    std::optional<std::int32_t> sampleRate; ///< @enginedb{EngineConfig.sampleRate}
    std::optional<std::int8_t> channelMode; ///< @enginedb{EngineConfig.channelMode}
};

/// A Score node.
struct ScoreRow {
    std::optional<std::string> name; ///< @enginedb{Score.name}
    /// @enginedb{Score.tempo_ramp_physical_time}
    std::optional<bool> tempoRampPhysicalTime;
};

/// A SoundEntity node.
struct SoundEntityRow {
    std::optional<std::string> name; ///< @enginedb{SoundEntity.name}
};

/// A SoundSource node.
struct SoundSourceRow {
    std::optional<std::string> name; ///< @enginedb{SoundSource.name}
};

/// A PartialSet node.
struct PartialSetRow {
    std::optional<std::string> name; ///< @enginedb{PartialSet.name}
};

/// A Partial node.
struct PartialRow {
    std::optional<std::string> name; ///< @enginedb{Partial.name}
};

/// A Label node.
struct LabelRow {
    std::optional<std::string> name; ///< @enginedb{Label.name}
};

/// A TempoMap node.
struct TempoMapRow {
    std::optional<std::string> name;          ///< @enginedb{TempoMap.name}
    std::optional<std::int32_t> ticksPerBeat; ///< @enginedb{TempoMap.ticks_per_beat}
};

/// A TempoMapPoint node.
struct TempoMapPointRow {
    std::optional<std::int64_t> position; ///< @enginedb{TempoMapPoint.position}
    std::optional<double> startTempo;     ///< @enginedb{TempoMapPoint.start_tempo}
    std::optional<bool> hold;             ///< @enginedb{TempoMapPoint.hold}
};

/// A PartialSetMusicalTimelinePoint node.
struct PartialSetMusicalTimelinePointRow {
    /// @enginedb{PartialSetMusicalTimelinePoint.amplitude_curve_type}
    std::optional<std::int8_t> amplitudeCurveType{0};
    /// @enginedb{PartialSetMusicalTimelinePoint.amplitude_curve_amount}
    std::optional<float> amplitudeCurveAmount{0.0F};
    /// @enginedb{PartialSetMusicalTimelinePoint.frequency_curve_type}
    std::optional<std::int8_t> frequencyCurveType{0};
    /// @enginedb{PartialSetMusicalTimelinePoint.frequency_curve_amount}
    std::optional<float> frequencyCurveAmount{0.0F};
};

/// A PartialState node.
struct PartialStateRow {
    std::optional<float> frequency;   ///< @enginedb{PartialState.frequency}
    std::optional<float> phase;       ///< @enginedb{PartialState.phase}
    std::optional<bool> phaseFreeRun; ///< @enginedb{PartialState.phase_free_run}
    std::optional<float> amplitude;   ///< @enginedb{PartialState.amplitude}
};

/// A SpatialPositionMusicalTimelinePoint node.
struct SpatialPositionMusicalTimelinePointRow {
    /// @enginedb{SpatialPositionMusicalTimelinePoint.azimuth_curve_type}
    std::optional<std::int8_t> azimuthCurveType{0};
    /// @enginedb{SpatialPositionMusicalTimelinePoint.azimuth_curve_amount}
    std::optional<float> azimuthCurveAmount{0.0F};
    /// @enginedb{SpatialPositionMusicalTimelinePoint.elevation_curve_type}
    std::optional<std::int8_t> elevationCurveType{0};
    /// @enginedb{SpatialPositionMusicalTimelinePoint.elevation_curve_amount}
    std::optional<float> elevationCurveAmount{0.0F};
    /// @enginedb{SpatialPositionMusicalTimelinePoint.distance_curve_type}
    std::optional<std::int8_t> distanceCurveType{0};
    /// @enginedb{SpatialPositionMusicalTimelinePoint.distance_curve_amount}
    std::optional<float> distanceCurveAmount{0.0F};
};

/// A SpatialPositionState node.
struct SpatialPositionStateRow {
    std::optional<double> azimuth;   ///< @enginedb{SpatialPositionState.azimuth}
    std::optional<double> elevation; ///< @enginedb{SpatialPositionState.elevation}
    std::optional<float> distance;   ///< @enginedb{SpatialPositionState.distance}
};

/// The properties of a POINT_AT edge.
struct PointAtProperties {
    std::optional<std::int64_t> syncPoint; ///< @enginedb{POINT_AT.sync_point}
    std::optional<float> offsetSeconds;    ///< @enginedb{POINT_AT.offset_seconds}
};

/// The properties of a TEMPO_MAP_SYNCHRONIZATION_POINT edge.
struct SyncPointProperties {
    /// @enginedb{TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_parent}
    std::optional<std::int64_t> syncPointParent;
    /// @enginedb{TEMPO_MAP_SYNCHRONIZATION_POINT.sync_point_child}
    std::optional<std::int64_t> syncPointChild;
};

/// The canonical model of one score, as its tables load it.
///
/// Filled once, by the native table loader, which checks that every edge's
/// ends exist; read only after that. The edge lists enforce multiplicity
/// themselves.
///
/// A node table is a vector of rows, the id being the row index. A table
/// with no properties but its id is held as its row count. A relationship
/// pair is an EdgeList, with its multiplicity from the schema.
struct CanonicalTables {
    /// @enginedb{node EngineConfig}
    std::vector<EngineConfigRow> engineConfigs;
    /// @enginedb{node Score}
    std::vector<ScoreRow> scores;
    /// @enginedb{node SoundEntity}
    std::vector<SoundEntityRow> soundEntities;
    /// @enginedb{node SoundSource}
    std::vector<SoundSourceRow> soundSources;
    /// @enginedb{node PartialSet}
    std::vector<PartialSetRow> partialSets;
    /// @enginedb{node Partial}
    std::vector<PartialRow> partials;
    /// @enginedb{node Label}
    std::vector<LabelRow> labels;
    /// @enginedb{node TempoMap}
    std::vector<TempoMapRow> tempoMaps;
    /// @enginedb{node TempoMapPoint}
    std::vector<TempoMapPointRow> tempoMapPoints;
    /// @enginedb{node PartialSetMusicalTimeline}
    std::size_t partialSetMusicalTimelines = 0;
    /// @enginedb{node PartialSetMusicalTimelinePoint}
    std::vector<PartialSetMusicalTimelinePointRow> partialSetMusicalTimelinePoints;
    /// @enginedb{node PartialStateSet}
    std::size_t partialStateSets = 0;
    /// @enginedb{node PartialState}
    std::vector<PartialStateRow> partialStates;
    /// @enginedb{node SpatialPositionMusicalTimeline}
    std::size_t spatialPositionMusicalTimelines = 0;
    /// @enginedb{node SpatialPositionMusicalTimelinePoint}
    std::vector<SpatialPositionMusicalTimelinePointRow> spatialPositionMusicalTimelinePoints;
    /// @enginedb{node SpatialPositionState}
    std::vector<SpatialPositionStateRow> spatialPositionStates;

    /// @enginedb{rel HAS_SOUNDENTITY Score->SoundEntity: edges}
    EdgeList<> hasSoundEntity{Multiplicity::OneMany};
    /// @enginedb{rel HAS_SOUNDSOURCE SoundEntity->SoundSource: edges}
    EdgeList<> hasSoundSource{Multiplicity::OneMany};
    /// @enginedb{rel HAS_PARTIALSET SoundSource->PartialSet: edges}
    EdgeList<> hasPartialSet{Multiplicity::OneMany};
    /// @enginedb{rel HAS_PARTIAL PartialSet->Partial: edges}
    EdgeList<> hasPartial{Multiplicity::OneMany};
    /// @enginedb{rel HAS_LABEL Partial->Label: edges}
    EdgeList<> partialLabels{Multiplicity::ManyMany};
    /// @enginedb{rel HAS_LABEL Score->Label: edges}
    EdgeList<> scoreLabels{Multiplicity::ManyMany};
    /// @enginedb{rel NEXT_POINT Score->TempoMap: edges}
    EdgeList<> scoreTempoMap{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT TempoMap->TempoMapPoint: edges}
    EdgeList<> tempoMapHead{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT TempoMapPoint->TempoMapPoint: edges}
    EdgeList<> tempoMapNext{Multiplicity::OneOne};
    /// @enginedb{rel TEMPO_MAP_SYNCHRONIZATION_POINT TempoMap->TempoMap: edges}
    EdgeList<SyncPointProperties> tempoMapSync{Multiplicity::ManyMany};
    /// @enginedb{rel HAS_MUSICAL_TIMELINE PartialSet->PartialSetMusicalTimeline: edges}
    EdgeList<> setTimeline{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT PartialSetMusicalTimeline->PartialSetMusicalTimelinePoint: edges}
    EdgeList<> setTimelineHead{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT PartialSetMusicalTimelinePoint->PartialSetMusicalTimelinePoint:
    /// edges}
    EdgeList<> setTimelineNext{Multiplicity::OneOne};
    /// @enginedb{rel POINT_AT PartialSetMusicalTimelinePoint->TempoMap: edges}
    EdgeList<PointAtProperties> setPointAt{Multiplicity::ManyOne};
    /// @enginedb{rel POINT_STATE PartialSetMusicalTimelinePoint->PartialStateSet: edges}
    EdgeList<> setPointState{Multiplicity::ManyOne};
    /// @enginedb{rel MEMBER_OF_SET_MANY_ONE PartialState->PartialStateSet: edges}
    EdgeList<> stateMemberOf{Multiplicity::ManyOne};
    /// @enginedb{rel STATE_OF PartialState->Partial: edges}
    EdgeList<> stateOf{Multiplicity::ManyOne};
    /// @enginedb{rel HAS_MUSICAL_TIMELINE SoundSource->SpatialPositionMusicalTimeline: edges}
    EdgeList<> sourceTimeline{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT
    /// SpatialPositionMusicalTimeline->SpatialPositionMusicalTimelinePoint: edges}
    EdgeList<> sourceTimelineHead{Multiplicity::OneOne};
    /// @enginedb{rel NEXT_POINT
    /// SpatialPositionMusicalTimelinePoint->SpatialPositionMusicalTimelinePoint: edges}
    EdgeList<> sourceTimelineNext{Multiplicity::OneOne};
    /// @enginedb{rel POINT_AT SpatialPositionMusicalTimelinePoint->TempoMap: edges}
    EdgeList<PointAtProperties> sourcePointAt{Multiplicity::ManyOne};
    /// @enginedb{rel POINT_STATE SpatialPositionMusicalTimelinePoint->SpatialPositionState: edges}
    EdgeList<> sourcePointState{Multiplicity::ManyOne};
};

// ---------------------------------------------------------------------------
// Physical timelines: written by the transform
// ---------------------------------------------------------------------------

/// A point of a frequency, amplitude, azimuth, elevation or distance
/// physical timeline.
///
/// @tparam Value  The value column's type: double, or float for amplitude
///                and distance.
template <typename Value>
struct CurvePoint {
    double time = 0.0;         ///< `time`, samples.
    Value value{};             ///< The value column (`frequency`, `amplitude`, ...).
    std::int8_t curveType = 0; ///< The `*_curve_type` column.
    float curveAmount = 0.0F;  ///< The `*_curve_amount` column.
};

/// A point of a phase physical timeline.
struct PhasePoint {
    double time = 0.0;           ///< `time`, samples.
    std::optional<double> phase; ///< `phase`, radians; NULL where the partial runs free.
};

/// The physical timelines of one kind, each owned by one partial or source,
/// with their points.
///
/// A timeline's points are one contiguous range of points(), in list order:
/// NEXT_POINT from the timeline leads to the first of them, and from each
/// point to the next in the range. A point's id is its index in points().
/// Removing a timeline marks it and its points dead; their ids are not
/// reused.
///
/// The phase table's ids differ from Ladybug's: WriteBatch inserts phase
/// points with a phase and those without in two statements, so Ladybug
/// numbers each group in turn, while here they are numbered in list order.
/// Nothing reads a phase point's id but its own list.
///
/// @tparam Point  CurvePoint<double>, CurvePoint<float> or PhasePoint.
template <typename Point>
class PhysicalTimelines {
public:
    /// One timeline.
    struct Timeline {
        std::int64_t owner = 0;     ///< The owning Partial or SoundSource.
        std::size_t firstPoint = 0; ///< Index of its first point in points().
        std::size_t pointCount = 0; ///< How many points it has.
        bool alive = true;          ///< False once removed.
    };

    /// Appends a timeline for `owner` with its points.
    ///
    /// @param owner   The owning Partial or SoundSource.
    /// @param points  Its points, in list order.
    /// @return The new timeline's id.
    /// @throws NativeModelError  If `owner` already has a live timeline of
    ///         this kind: HAS_PHYSICAL_TIMELINE is ONE_ONE. Nothing has been
    ///         added; nor if memory runs out (the strong guarantee).
    std::int64_t append(std::int64_t owner, std::span<const Point> points) {
        if (byOwner_.contains(owner)) {
            throw NativeModelError{
                "node " + std::to_string(owner) + " already has a physical timeline of this kind"};
        }
        // Everything that can throw comes first; what follows cannot.
        reserveFor(timelines_, 1);
        reserveFor(points_, points.size());
        reserveFor(pointTimeline_, points.size());
        const auto id = static_cast<std::int64_t>(timelines_.size());
        byOwner_.emplace(owner, id);
        timelines_.push_back(Timeline{.owner = owner,
            .firstPoint = points_.size(),
            .pointCount = points.size(),
            .alive = true});
        points_.insert(points_.end(), points.begin(), points.end());
        pointTimeline_.insert(pointTimeline_.end(), points.size(), id);
        livePoints_ += points.size();
        return id;
    }

    /// Removes a timeline and its points. As a Ladybug delete by id, an id
    /// that names no live timeline is ignored.
    ///
    /// @param id  The timeline's id.
    void remove(std::int64_t id) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= timelines_.size()) {
            return;
        }
        Timeline& timeline = timelines_[static_cast<std::size_t>(id)];
        if (!timeline.alive) {
            return;
        }
        timeline.alive = false;
        byOwner_.erase(timeline.owner);
        livePoints_ -= timeline.pointCount;
    }

    /// The live timeline `owner` has, if any.
    ///
    /// @param owner  A Partial or SoundSource id.
    /// @return Its timeline's id; nullopt when it has none.
    std::optional<std::int64_t> timelineOf(std::int64_t owner) const {
        const auto found = byOwner_.find(owner);
        return found == byOwner_.end() ? std::nullopt : std::optional{found->second};
    }

    /// Every timeline, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const Timeline> timelines() const noexcept { return timelines_; }

    /// Every point, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const Point> points() const noexcept { return points_; }

    /// A timeline's points, in list order.
    ///
    /// @param id  A timeline id from timelines().
    /// @return A view, valid until the next append().
    std::span<const Point> pointsOf(std::int64_t id) const {
        const Timeline& timeline = timelines_.at(static_cast<std::size_t>(id));
        return std::span<const Point>{points_}.subspan(timeline.firstPoint, timeline.pointCount);
    }

    /// The timeline a point belongs to.
    ///
    /// @param point  A point id from points().
    /// @return Its timeline's id.
    std::int64_t timelineOfPoint(std::int64_t point) const {
        return pointTimeline_.at(static_cast<std::size_t>(point));
    }

    /// Whether a point id names a point of a live timeline.
    ///
    /// @param point  Any id.
    /// @return True when the point exists and its timeline is live.
    bool isLivePoint(std::int64_t point) const noexcept {
        return point >= 0 && static_cast<std::size_t>(point) < points_.size() &&
               timelines_[static_cast<std::size_t>(pointTimeline_[static_cast<std::size_t>(point)])]
                   .alive;
    }

    /// How many live timelines there are.
    ///
    /// @return The count.
    std::size_t liveTimelineCount() const noexcept { return byOwner_.size(); }

    /// How many points the live timelines have.
    ///
    /// @return The count.
    std::size_t livePointCount() const noexcept { return livePoints_; }

private:
    std::vector<Timeline> timelines_;                        ///< Every timeline, by id.
    std::vector<Point> points_;                              ///< Every point, by id.
    std::vector<std::int64_t> pointTimeline_;                ///< Each point's timeline id.
    std::unordered_map<std::int64_t, std::int64_t> byOwner_; ///< Live timeline of each owner.
    std::size_t livePoints_ = 0;                             ///< Points of live timelines.
};

/// Every physical timeline the transform writes.
struct PhysicalTables {
    /// @enginedb{node FrequencyPhysicalTimeline}
    /// @enginedb{node FrequencyPhysicalTimelinePoint}
    /// @enginedb{FrequencyPhysicalTimelinePoint.time: CurvePoint::time}
    /// @enginedb{FrequencyPhysicalTimelinePoint.frequency: CurvePoint::value}
    /// @enginedb{FrequencyPhysicalTimelinePoint.frequency_curve_type: CurvePoint::curveType}
    /// @enginedb{FrequencyPhysicalTimelinePoint.frequency_curve_amount: CurvePoint::curveAmount}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE Partial->FrequencyPhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT FrequencyPhysicalTimeline->FrequencyPhysicalTimelinePoint: the
    /// first point of the timeline's range}
    /// @enginedb{rel NEXT_POINT FrequencyPhysicalTimelinePoint->FrequencyPhysicalTimelinePoint: the
    /// next point in the range}
    PhysicalTimelines<CurvePoint<double>> frequency;

    /// @enginedb{node AmplitudePhysicalTimeline}
    /// @enginedb{node AmplitudePhysicalTimelinePoint}
    /// @enginedb{AmplitudePhysicalTimelinePoint.time: CurvePoint::time}
    /// @enginedb{AmplitudePhysicalTimelinePoint.amplitude: CurvePoint::value}
    /// @enginedb{AmplitudePhysicalTimelinePoint.amplitude_curve_type: CurvePoint::curveType}
    /// @enginedb{AmplitudePhysicalTimelinePoint.amplitude_curve_amount: CurvePoint::curveAmount}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE Partial->AmplitudePhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT AmplitudePhysicalTimeline->AmplitudePhysicalTimelinePoint: the
    /// first point of the timeline's range}
    /// @enginedb{rel NEXT_POINT AmplitudePhysicalTimelinePoint->AmplitudePhysicalTimelinePoint: the
    /// next point in the range}
    PhysicalTimelines<CurvePoint<float>> amplitude;

    /// @enginedb{node PhasePhysicalTimeline}
    /// @enginedb{node PhasePhysicalTimelinePoint}
    /// @enginedb{PhasePhysicalTimelinePoint.time: PhasePoint::time}
    /// @enginedb{PhasePhysicalTimelinePoint.phase: PhasePoint::phase}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE Partial->PhasePhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT PhasePhysicalTimeline->PhasePhysicalTimelinePoint: the first point
    /// of the timeline's range}
    /// @enginedb{rel NEXT_POINT PhasePhysicalTimelinePoint->PhasePhysicalTimelinePoint: the next
    /// point in the range}
    PhysicalTimelines<PhasePoint> phase;

    /// @enginedb{node AzimuthPhysicalTimeline}
    /// @enginedb{node AzimuthPhysicalTimelinePoint}
    /// @enginedb{AzimuthPhysicalTimelinePoint.time: CurvePoint::time}
    /// @enginedb{AzimuthPhysicalTimelinePoint.azimuth: CurvePoint::value}
    /// @enginedb{AzimuthPhysicalTimelinePoint.azimuth_curve_type: CurvePoint::curveType}
    /// @enginedb{AzimuthPhysicalTimelinePoint.azimuth_curve_amount: CurvePoint::curveAmount}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE SoundSource->AzimuthPhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT AzimuthPhysicalTimeline->AzimuthPhysicalTimelinePoint: the first
    /// point of the timeline's range}
    /// @enginedb{rel NEXT_POINT AzimuthPhysicalTimelinePoint->AzimuthPhysicalTimelinePoint: the
    /// next point in the range}
    PhysicalTimelines<CurvePoint<double>> azimuth;

    /// @enginedb{node ElevationPhysicalTimeline}
    /// @enginedb{node ElevationPhysicalTimelinePoint}
    /// @enginedb{ElevationPhysicalTimelinePoint.time: CurvePoint::time}
    /// @enginedb{ElevationPhysicalTimelinePoint.elevation: CurvePoint::value}
    /// @enginedb{ElevationPhysicalTimelinePoint.elevation_curve_type: CurvePoint::curveType}
    /// @enginedb{ElevationPhysicalTimelinePoint.elevation_curve_amount: CurvePoint::curveAmount}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE SoundSource->ElevationPhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT ElevationPhysicalTimeline->ElevationPhysicalTimelinePoint: the
    /// first point of the timeline's range}
    /// @enginedb{rel NEXT_POINT ElevationPhysicalTimelinePoint->ElevationPhysicalTimelinePoint: the
    /// next point in the range}
    PhysicalTimelines<CurvePoint<double>> elevation;

    /// @enginedb{node DistancePhysicalTimeline}
    /// @enginedb{node DistancePhysicalTimelinePoint}
    /// @enginedb{DistancePhysicalTimelinePoint.time: CurvePoint::time}
    /// @enginedb{DistancePhysicalTimelinePoint.distance: CurvePoint::value}
    /// @enginedb{DistancePhysicalTimelinePoint.distance_curve_type: CurvePoint::curveType}
    /// @enginedb{DistancePhysicalTimelinePoint.distance_curve_amount: CurvePoint::curveAmount}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE SoundSource->DistancePhysicalTimeline: Timeline::owner}
    /// @enginedb{rel NEXT_POINT DistancePhysicalTimeline->DistancePhysicalTimelinePoint: the first
    /// point of the timeline's range}
    /// @enginedb{rel NEXT_POINT DistancePhysicalTimelinePoint->DistancePhysicalTimelinePoint: the
    /// next point in the range}
    PhysicalTimelines<CurvePoint<float>> distance;
};

// ---------------------------------------------------------------------------
// Segment timelines and frame entries: written by precomputation
// ---------------------------------------------------------------------------

/// A PartialSegmentNominalFrequency node. Its curve properties are never
/// set, so they are NULL.
struct NominalFrequencyRow {
    /// @enginedb{PartialSegmentNominalFrequency.start_nominal_frequency}
    double startNominalFrequency = 0.0;
    /// @enginedb{PartialSegmentNominalFrequency.linear_component}
    bool linearComponent = false;
    /// @enginedb{PartialSegmentNominalFrequency.linear_gradient}
    std::optional<double> linearGradient;
    /// @enginedb{PartialSegmentNominalFrequency.curve_component}
    bool curveComponent = false;
};

/// A PartialIntegrableSegment node.
struct SegmentRow {
    double startTime = 0.0;  ///< @enginedb{PartialIntegrableSegment.start_time}
    double endTime = 0.0;    ///< @enginedb{PartialIntegrableSegment.end_time}
    double startPhase = 0.0; ///< @enginedb{PartialIntegrableSegment.start_phase}
    /// The id of the segment's nominal frequency in SegmentTimelines::nominals().
    /// @enginedb{rel SEGMENT_TO_FREQUENCY_NOMINAL
    /// PartialIntegrableSegment->PartialSegmentNominalFrequency: SegmentRow::nominal}
    std::int64_t nominal = 0;
};

/// A PartialPhaseCompensationSegment node.
struct CompensationRow {
    double startTime = 0.0; ///< @enginedb{PartialPhaseCompensationSegment.start_time}
    double endTime = 0.0;   ///< @enginedb{PartialPhaseCompensationSegment.end_time}
    /// @enginedb{PartialPhaseCompensationSegment.start_phase_delta}
    double startPhaseDelta = 0.0;
    /// @enginedb{PartialPhaseCompensationSegment.end_phase_delta}
    double endPhaseDelta = 0.0;
};

/// The PartialSegmentPhysicalTimelines precomputation writes, one per
/// partial, each heading two lists: its integrable segments and its
/// compensation segments.
///
/// Like a PhysicalTimelines, each list is one contiguous range, in list
/// order, and ids are indices that are never reused. Each segment has its
/// own nominal frequency, appended with it, so the nominal frequencies are
/// numbered in segment order. Ladybug numbers them differently, because
/// WriteBatch inserts those with a linear component and those without in two
/// statements; nothing reads a nominal frequency's id but its segment.
class SegmentTimelines {
public:
    /// One PartialSegmentPhysicalTimeline.
    struct Timeline {
        std::int64_t partial = 0;          ///< The owning Partial.
        std::size_t firstSegment = 0;      ///< Index of its first segment.
        std::size_t segmentCount = 0;      ///< How many segments it has.
        std::size_t firstCompensation = 0; ///< Index of its first compensation segment.
        std::size_t compensationCount = 0; ///< How many compensation segments it has.
        bool alive = true;                 ///< False once removed.
    };

    /// A segment to append, with its nominal frequency.
    struct NewSegment {
        double startTime = 0.0;      ///< start_time, samples.
        double endTime = 0.0;        ///< end_time, samples.
        double startPhase = 0.0;     ///< start_phase, radians.
        NominalFrequencyRow nominal; ///< Its nominal frequency.
    };

    /// Appends a timeline for `partial` with its two lists.
    ///
    /// @param partial       The owning Partial.
    /// @param segments      Its integrable segments, in list order.
    /// @param compensation  Its compensation segments, in list order.
    /// @return The new timeline's id.
    /// @throws NativeModelError  If `partial` already has a live timeline:
    ///         HAS_PHYSICAL_TIMELINE is ONE_ONE. Nothing has been added; nor
    ///         if memory runs out (the strong guarantee).
    std::int64_t append(std::int64_t partial, std::span<const NewSegment> segments,
        std::span<const CompensationRow> compensation);

    /// Removes a timeline, its segments, their nominal frequencies and its
    /// compensation segments. An id that names no live timeline is ignored.
    ///
    /// @param id  The timeline's id.
    void remove(std::int64_t id) noexcept;

    /// The live timeline `partial` has, if any.
    ///
    /// @param partial  A Partial id.
    /// @return Its timeline's id; nullopt when it has none.
    std::optional<std::int64_t> timelineOf(std::int64_t partial) const;

    /// Every timeline, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const Timeline> timelines() const noexcept { return timelines_; }

    /// Every segment, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const SegmentRow> segments() const noexcept { return segments_; }

    /// Every nominal frequency, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const NominalFrequencyRow> nominals() const noexcept { return nominals_; }

    /// Every compensation segment, live or dead, by id.
    ///
    /// @return A view, valid until the next append().
    std::span<const CompensationRow> compensation() const noexcept { return compensation_; }

    /// The timeline a segment belongs to.
    ///
    /// @param segment  A segment id from segments().
    /// @return Its timeline's id.
    std::int64_t timelineOfSegment(std::int64_t segment) const;

    /// The timeline a compensation segment belongs to.
    ///
    /// @param segment  A compensation segment id from compensation().
    /// @return Its timeline's id.
    std::int64_t timelineOfCompensation(std::int64_t segment) const;

    /// Whether an id names a segment of a live timeline.
    ///
    /// @param segment  Any id.
    /// @return True when it does.
    bool isLiveSegment(std::int64_t segment) const noexcept;

    /// Whether an id names a compensation segment of a live timeline.
    ///
    /// @param segment  Any id.
    /// @return True when it does.
    bool isLiveCompensation(std::int64_t segment) const noexcept;

    /// How many live timelines there are.
    ///
    /// @return The count.
    std::size_t liveTimelineCount() const noexcept { return byPartial_.size(); }

    /// How many segments the live timelines have, which is also how many
    /// nominal frequencies are live.
    ///
    /// @return The count.
    std::size_t liveSegmentCount() const noexcept { return liveSegments_; }

    /// How many compensation segments the live timelines have.
    ///
    /// @return The count.
    std::size_t liveCompensationCount() const noexcept { return liveCompensation_; }

private:
    std::vector<Timeline> timelines_;                ///< Every timeline, by id.
    std::vector<SegmentRow> segments_;               ///< Every segment, by id.
    std::vector<std::int64_t> segmentTimeline_;      ///< Each segment's timeline id.
    std::vector<NominalFrequencyRow> nominals_;      ///< Every nominal frequency, by id.
    std::vector<CompensationRow> compensation_;      ///< Every compensation segment, by id.
    std::vector<std::int64_t> compensationTimeline_; ///< Each compensation segment's timeline id.
    std::unordered_map<std::int64_t, std::int64_t> byPartial_; ///< Live timeline of each partial.
    std::size_t liveSegments_ = 0;                             ///< Segments of live timelines.
    std::size_t liveCompensation_ = 0; ///< Compensation segments of live timelines.
};

/// A FrameEntry node with the eight edges that leave or reach it.
struct FrameEntryRow {
    /// The Frame it is reached from.
    /// @enginedb{rel FRAME_ENTRY Frame->FrameEntry: FrameEntryRow::frame}
    std::int32_t frame = 0;
    /// The Partial it leads to.
    /// @enginedb{rel FRAME_ENTRY_PARTIAL FrameEntry->Partial: FrameEntryRow::partial}
    std::int64_t partial = 0;
    /// The PartialIntegrableSegment the frame's rendering starts from.
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->PartialIntegrableSegment:
    /// FrameEntryRow::segment}
    std::int64_t segment = 0;
    /// The AmplitudePhysicalTimelinePoint it starts from.
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->AmplitudePhysicalTimelinePoint:
    /// FrameEntryRow::amplitudePoint}
    std::int64_t amplitudePoint = 0;
    /// The PartialPhaseCompensationSegment it starts from.
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->PartialPhaseCompensationSegment:
    /// FrameEntryRow::compensation}
    std::int64_t compensation = 0;
    /// The azimuth, elevation and distance points it starts from, in
    /// kSpatialKinds order.
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->AzimuthPhysicalTimelinePoint:
    /// FrameEntryRow::spatialPoints}
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->ElevationPhysicalTimelinePoint:
    /// FrameEntryRow::spatialPoints}
    /// @enginedb{rel FRAME_ENTRY_PHYSICAL_TIMELINE FrameEntry->DistancePhysicalTimelinePoint:
    /// FrameEntryRow::spatialPoints}
    std::array<std::int64_t, 3> spatialPoints{};
};

/// The Frame nodes, the FrameEntry nodes, and the frame scope of each
/// partial.
class FrameTables {
public:
    /// Creates a Frame node unless it exists, as Cypher's MERGE does. Frame
    /// nodes are never deleted.
    ///
    /// @param frame  The frame number.
    void mergeFrame(std::int32_t frame) { frames_.insert(frame); }

    /// Every Frame node's number, in order.
    ///
    /// @return The frame numbers.
    const std::set<std::int32_t>& frames() const noexcept { return frames_; }

    /// Appends a FrameEntry.
    ///
    /// @param entry  The entry. Its frame must already exist (mergeFrame()).
    /// @return The new entry's id.
    /// @throws NativeModelError  If its frame has no Frame node. Nothing has
    ///         been added; nor if memory runs out (the strong guarantee).
    std::int64_t appendEntry(const FrameEntryRow& entry);

    /// Removes a FrameEntry and its edges. An id that names no live entry is
    /// ignored.
    ///
    /// @param id  The entry's id.
    void removeEntry(std::int64_t id) noexcept;

    /// Every FrameEntry, live or dead, by id.
    ///
    /// @return A view, valid until the next appendEntry().
    std::span<const FrameEntryRow> entries() const noexcept { return entries_; }

    /// Whether an id names a live FrameEntry.
    ///
    /// @param id  Any id.
    /// @return True when it does.
    bool isLiveEntry(std::int64_t id) const noexcept;

    /// The live entries of a frame, in id order.
    ///
    /// @param frame  The frame number.
    /// @return Their ids; empty when the frame has none or does not exist.
    std::vector<std::int64_t> liveEntriesOf(std::int32_t frame) const;

    /// The live entries of a partial, in id order.
    ///
    /// @param partial  A Partial id.
    /// @return Their ids.
    std::vector<std::int64_t> liveEntriesOfPartial(std::int64_t partial) const;

    /// How many FrameEntry nodes are live.
    ///
    /// @return The count.
    std::size_t liveEntryCount() const noexcept { return liveEntries_; }

    /// Sets a partial's frame scope, replacing any it had: one
    /// PARTIAL_IN_FRAME edge for each frame from `first` to `last`.
    ///
    /// @param partial  A Partial id.
    /// @param first    The first frame in scope.
    /// @param last     The last frame in scope; not before `first`.
    /// @throws NativeModelError  If a frame in the range has no Frame node.
    void setScope(std::int64_t partial, std::int32_t first, std::int32_t last);

    /// Removes a partial's frame scope, its PARTIAL_IN_FRAME edges.
    ///
    /// @param partial  A Partial id.
    void clearScope(std::int64_t partial) noexcept { scopes_.erase(partial); }

    /// The partials that have a frame scope.
    ///
    /// @return Each partial's first and last frame in scope, by Partial id.
    const std::unordered_map<std::int64_t, std::pair<std::int32_t, std::int32_t>>&
    scopes() const noexcept {
        return scopes_;
    }

    /// How many PARTIAL_IN_FRAME edges the scopes stand for.
    ///
    /// @return The count.
    std::int64_t scopeEdgeCount() const noexcept;

private:
    std::set<std::int32_t> frames_;      ///< Every Frame node's number.
    std::vector<FrameEntryRow> entries_; ///< Every FrameEntry, by id.
    std::vector<bool> entryAlive_;       ///< Whether each FrameEntry is live.
    /// Every entry of each frame, live or dead, in id order.
    std::unordered_map<std::int32_t, std::vector<std::int64_t>> byFrame_;
    /// Every entry of each partial, live or dead, in id order.
    std::unordered_map<std::int64_t, std::vector<std::int64_t>> byPartial_;
    /// Each partial's first and last frame in scope.
    std::unordered_map<std::int64_t, std::pair<std::int32_t, std::int32_t>> scopes_;
    std::size_t liveEntries_ = 0; ///< FrameEntry nodes that are live.
};

/// One score's engine-db, held natively.
struct NativeModel {
    /// What the score's tables load.
    CanonicalTables canonical;

    /// What the transform writes.
    PhysicalTables physical;

    /// What precomputation writes: the segment timelines.
    /// @enginedb{node PartialSegmentPhysicalTimeline}
    /// @enginedb{node PartialIntegrableSegment}
    /// @enginedb{node PartialSegmentNominalFrequency}
    /// @enginedb{node PartialPhaseCompensationSegment}
    /// @enginedb{rel HAS_PHYSICAL_TIMELINE Partial->PartialSegmentPhysicalTimeline:
    /// Timeline::partial}
    /// @enginedb{rel NEXT_POINT PartialSegmentPhysicalTimeline->PartialIntegrableSegment: the first
    /// segment of the timeline's range}
    /// @enginedb{rel NEXT_POINT PartialIntegrableSegment->PartialIntegrableSegment: the next
    /// segment in the range}
    /// @enginedb{rel NEXT_POINT PartialSegmentPhysicalTimeline->PartialPhaseCompensationSegment:
    /// the first compensation segment of the timeline's range}
    /// @enginedb{rel NEXT_POINT PartialPhaseCompensationSegment->PartialPhaseCompensationSegment:
    /// the next compensation segment in the range}
    SegmentTimelines segments;

    /// What precomputation writes: the frames.
    /// @enginedb{node Frame}
    /// @enginedb{Frame.frame_number: the frame number in FrameTables::frames}
    /// @enginedb{node FrameEntry}
    /// @enginedb{rel PARTIAL_IN_FRAME Partial->Frame: the frame range in FrameTables::scopes}
    FrameTables frames;
};

/// Counts what a model holds, as Ladybug's census would count the same
/// data: live rows per node table and edges per relationship table, under
/// the schema's names, tables with no rows left out.
///
/// @param model  The model.
/// @return The census, each list sorted by name.
Census censusOf(const NativeModel& model);

/// Every EngineConfig of a model, as the stores' readEngineConfigs() give
/// them: each property widened to int64, NULL as nullopt.
///
/// @param model  The model.
/// @return One record per EngineConfig.
std::vector<EngineConfigRecord> engineConfigsOf(const NativeModel& model);

/// The names of every Label in a model, a Label with no name left out.
///
/// @param model  The model.
/// @return The names, sorted.
std::set<std::string> labelNamesOf(const NativeModel& model);

} // namespace rainbohz::rendering_engine::native
