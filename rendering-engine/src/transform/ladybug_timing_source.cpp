// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/ladybug_transform_store.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/// @file
/// @brief The Ladybug backend's TimingSource: the timing queries' reads as
/// fixed Cypher queries.
///
/// The queries are the ones timing_query.cpp ran before the stores were
/// introduced (ADR 0028), unchanged.
///
/// @see timing_query.hpp, ladybug_transform_store.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

/// One TempoMap by id: its id, name and ticks_per_beat.
constexpr std::string_view kFindMap =
    "MATCH (m:TempoMap {id: $id}) RETURN m.id, m.name, m.ticks_per_beat;";

/// Each Score, its ramp shape, and its root TempoMap, if it has one.
/// NEXT_POINT is ONE_ONE per pair, so there is at most one row per Score.
constexpr std::string_view kReadScores =
    "MATCH (s:Score) OPTIONAL MATCH (s)-[:NEXT_POINT]->(m:TempoMap) "
    "RETURN s.id, m.id, m.name, m.ticks_per_beat, s.tempo_ramp_physical_time ORDER BY s.id;";

/// The Score whose root TempoMap is the map $id, with its ramp shape; no row
/// when the map is not a root. NEXT_POINT is ONE_ONE, so there is at most one.
constexpr std::string_view kScoresOfMap = "MATCH (s:Score)-[:NEXT_POINT]->(m:TempoMap {id: $id}) "
                                          "RETURN s.id, s.tempo_ramp_physical_time;";

/// The child TempoMaps of the map $id, with their synchronization points.
/// The edge runs from child to parent. A child joined by two edges comes
/// once per edge, ordered by where each edge sits in the parent and then in
/// the child, a NULL after every value; the order is stated in full so that
/// both backends give it (ADR 0028).
constexpr std::string_view kReadChildren =
    "MATCH (c:TempoMap)-[s:TEMPO_MAP_SYNCHRONIZATION_POINT]->(p:TempoMap {id: $id}) "
    "RETURN c.id, c.name, c.ticks_per_beat, s.sync_point_parent, s.sync_point_child "
    "ORDER BY c.id, s.sync_point_parent IS NULL, s.sync_point_parent, "
    "s.sync_point_child IS NULL, s.sync_point_child;";

/// The first TempoMapPoint of the map $id: the head of its point list.
constexpr std::string_view kReadHead =
    "MATCH (m:TempoMap {id: $id})-[:NEXT_POINT]->(p:TempoMapPoint) "
    "RETURN p.id, p.position, p.start_tempo, p.hold;";

/// The next kWalkChunk points after one, nearest first. NEXT_POINT is ONE_ONE,
/// so each hop count reaches exactly one point.
constexpr std::string_view kReadChunk =
    "MATCH (a:TempoMapPoint {id: $id})-[e:NEXT_POINT*1..30]->(p:TempoMapPoint) "
    "RETURN p.id, p.position, p.start_tempo, p.hold, length(e) AS hops ORDER BY hops;";
static_assert(kWalkChunk == 30, "kReadChunk's upper bound is kWalkChunk");

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// Reads a TempoMap from the first three columns of a row.
///
/// @param row  A row whose columns 0 to 2 are id, name and ticks_per_beat.
/// @return The map.
TempoMapInfo mapFrom(const DbRow& row) {
    return TempoMapInfo{.id = requireInt64(row.at(0)),
        .name = optionalString(row.at(1)),
        .ticksPerBeat = optionalInt64(row.at(2))};
}

/// Reads a TempoMapPoint from the first four columns of a row.
///
/// @param row  A row whose columns 0 to 3 are id, position, start_tempo and
///             hold.
/// @return The point, as read.
TempoMapPointInfo pointFrom(const DbRow& row) {
    return TempoMapPointInfo{.id = requireInt64(row.at(0)),
        .position = optionalInt64(row.at(1)),
        .startTempo = optionalDouble(row.at(2)),
        .hold = optionalBool(row.at(3))};
}

/// The timing queries' reads over a Ladybug engine-db.
class LadybugTimingSource final : public TimingSource {
public:
    /// Serves the timing queries from `db`.
    ///
    /// @param[in,out] db  Database holding the score. It must outlive the
    ///                    source, which keeps a pointer to it.
    explicit LadybugTimingSource(EngineDb& db) : db_{&db} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return readLadybugEngineConfigs(*db_);
    }

    std::vector<ScoreRootRecord> readScores() override {
        std::vector<ScoreRootRecord> scores;
        for (const DbRow& row : db_->query(kReadScores)) {
            ScoreRootRecord score{.scoreId = requireInt64(row.at(0)),
                .root = std::nullopt,
                .tempoRampPhysicalTime = optionalBool(row.at(4))};
            if (const std::optional<std::int64_t> mapId = optionalInt64(row.at(1))) {
                score.root = TempoMapInfo{.id = *mapId,
                    .name = optionalString(row.at(2)),
                    .ticksPerBeat = optionalInt64(row.at(3))};
            }
            scores.push_back(std::move(score));
        }
        return scores;
    }

    std::optional<TempoMapInfo> findTempoMap(std::int64_t mapId) override {
        const std::vector<DbRow> rows = db_->query(kFindMap, {{"id", mapId}});
        if (rows.empty()) {
            return std::nullopt;
        }
        return mapFrom(rows.front());
    }

    std::vector<std::optional<bool>> rampsOfScoresRootedAt(std::int64_t mapId) override {
        std::vector<std::optional<bool>> ramps;
        for (const DbRow& row : db_->query(kScoresOfMap, {{"id", mapId}})) {
            ramps.push_back(optionalBool(row.at(1)));
        }
        return ramps;
    }

    std::vector<TempoMapChild> readChildren(std::int64_t mapId) override {
        std::vector<TempoMapChild> children;
        for (const DbRow& row : db_->query(kReadChildren, {{"id", mapId}})) {
            children.push_back(TempoMapChild{.map = mapFrom(row),
                .syncPointParent = optionalInt64(row.at(3)),
                .syncPointChild = optionalInt64(row.at(4))});
        }
        return children;
    }

    std::optional<TempoMapPointInfo> readHead(std::int64_t mapId) override {
        const std::vector<DbRow> rows = db_->query(kReadHead, {{"id", mapId}});
        if (rows.empty()) {
            return std::nullopt;
        }
        return pointFrom(rows.front());
    }

    std::vector<TempoMapPointInfo> readNextPoints(std::int64_t pointId) override {
        std::vector<TempoMapPointInfo> points;
        for (const DbRow& row : db_->query(kReadChunk, {{"id", pointId}})) {
            points.push_back(pointFrom(row));
        }
        return points;
    }

private:
    EngineDb* db_; ///< The database; not owned.
};

} // namespace

std::unique_ptr<TimingSource> makeLadybugTimingSource(EngineDb& db) {
    return std::make_unique<LadybugTimingSource>(db);
}

} // namespace rainbohz::rendering_engine::transform
