#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief Read-only timing queries for server mode: tempo maps, engine
/// parameters, and musical position to frame and offset.
///
/// @see timing_query.hpp, docs/server.md

namespace rainbohz::rendering_engine::transform {

namespace {

using Kind = TimingQueryError::Kind;

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

constexpr std::string_view kFindMap =
    "MATCH (m:TempoMap {id: $id}) RETURN m.id, m.name, m.ticks_per_beat;";

// NEXT_POINT is ONE_ONE per pair, so there is at most one row per Score.
constexpr std::string_view kReadScores =
    "MATCH (s:Score) OPTIONAL MATCH (s)-[:NEXT_POINT]->(m:TempoMap) "
    "RETURN s.id, m.id, m.name, m.ticks_per_beat ORDER BY s.id;";

constexpr std::string_view kScoresOfMap =
    "MATCH (s:Score)-[:NEXT_POINT]->(m:TempoMap {id: $id}) RETURN s.id;";

// The edge runs from child to parent.
constexpr std::string_view kReadChildren =
    "MATCH (c:TempoMap)-[s:TEMPO_MAP_SYNCHRONIZATION_POINT]->(p:TempoMap {id: $id}) "
    "RETURN c.id, c.name, c.ticks_per_beat, s.sync_point_parent, s.sync_point_child "
    "ORDER BY c.id;";

constexpr std::string_view kReadHead =
    "MATCH (m:TempoMap {id: $id})-[:NEXT_POINT]->(p:TempoMapPoint) "
    "RETURN p.id, p.position, p.start_tempo, p.hold;";

// The next kWalkChunk points after one, nearest first. NEXT_POINT is ONE_ONE,
// so each hop count reaches exactly one point.
constexpr std::string_view kReadChunk =
    "MATCH (a:TempoMapPoint {id: $id})-[e:NEXT_POINT*1..30]->(p:TempoMapPoint) "
    "RETURN p.id, p.position, p.start_tempo, p.hold, length(e) AS hops ORDER BY hops;";
static_assert(kWalkChunk == 30, "kReadChunk's upper bound is kWalkChunk");

constexpr std::string_view kReadConfig =
    "MATCH (c:EngineConfig) RETURN c.frameSize, c.sampleRate, c.channelMode;";

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

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

/// Names a map for a message.
///
/// @param mapId  TempoMap.id.
/// @return e.g. "TempoMap 3".
std::string describeMap(std::int64_t mapId) {
    return "TempoMap " + std::to_string(mapId);
}

/// Finds a TempoMap by its id.
///
/// @param db     Database to read.
/// @param mapId  TempoMap.id.
/// @return The map.
/// @throws TimingQueryError  NotFound when there is no such map.
TempoMapInfo requireMap(EngineDb& db, std::int64_t mapId) {
    const std::vector<DbRow> rows = db.query(kFindMap, {{"id", mapId}});
    if (rows.empty()) {
        throw TimingQueryError{Kind::NotFound, "there is no " + describeMap(mapId)};
    }
    return mapFrom(rows.front());
}

/// Walks a TempoMap's NEXT_POINT list from its head, a chunk of hops per
/// query, handing each point to `visit` in list order until it returns false
/// or the list ends.
///
/// Lists are acyclic by construction upstream, not by constraint (AGENTS.md,
/// Linked Lists), so the walk stops, with an error, at a revisited point.
///
/// @tparam Visit  Callable as `bool(const TempoMapPointInfo&)`.
/// @param db     Database to read.
/// @param mapId  TempoMap.id, known to exist.
/// @param visit  Called for each point; returns true to go on.
/// @throws TimingQueryError  Unprocessable when the list loops.
template <typename Visit>
void walkPoints(EngineDb& db, std::int64_t mapId, Visit visit) {
    std::unordered_set<std::int64_t> seen;
    const auto accept = [&](const TempoMapPointInfo& point) {
        if (!seen.insert(point.id).second) {
            throw TimingQueryError{Kind::Unprocessable,
                describeMap(mapId) + ": its NEXT_POINT list loops back to TempoMapPoint " +
                    std::to_string(point.id) + " after " + std::to_string(seen.size()) + " points"};
        }
        return visit(point);
    };

    const std::vector<DbRow> head = db.query(kReadHead, {{"id", mapId}});
    if (head.empty()) {
        return;
    }
    TempoMapPointInfo point = pointFrom(head.front());
    if (!accept(point)) {
        return;
    }
    for (;;) {
        const std::vector<DbRow> chunk = db.query(kReadChunk, {{"id", point.id}});
        for (const DbRow& row : chunk) {
            point = pointFrom(row);
            if (!accept(point)) {
                return;
            }
        }
        if (chunk.size() < static_cast<std::size_t>(kWalkChunk)) {
            return;
        }
    }
}

/// Parses an unsigned decimal integer that fits INT64.
///
/// @param text  Digits only.
/// @return The value; nullopt when `text` is empty, holds anything but
///         digits, or is too large.
std::optional<std::int64_t> parseCount(std::string_view text) noexcept {
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* const last = std::to_address(text.cend());
    const auto [end, error] = std::from_chars(std::to_address(text.cbegin()), last, value);
    if (error != std::errc{} || end != last) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::string_view channelModeName(ChannelMode mode) noexcept {
    switch (mode) {
    case ChannelMode::Mono:
        return "mono";
    case ChannelMode::Stereo:
        return "stereo";
    case ChannelMode::MidSide:
        return "mid-side";
    case ChannelMode::FoaAFormat:
        return "foa-a-format";
    case ChannelMode::FoaBFormat:
        return "foa-b-format";
    }
    return "unknown";
}

std::optional<BeatsTicks> parseBeatsTicks(std::string_view text) {
    const std::size_t colon = text.find(':');
    const std::optional<std::int64_t> beats = parseCount(text.substr(0, colon));
    if (!beats.has_value()) {
        return std::nullopt;
    }
    if (colon == std::string_view::npos) {
        return BeatsTicks{.beats = *beats, .ticks = 0};
    }
    const std::optional<std::int64_t> ticks = parseCount(text.substr(colon + 1));
    if (!ticks.has_value()) {
        return std::nullopt;
    }
    return BeatsTicks{.beats = *beats, .ticks = *ticks};
}

TempoMapNode readRootTempoMap(EngineDb& db) {
    const std::vector<DbRow> scores = db.query(kReadScores);
    if (scores.empty()) {
        throw TimingQueryError{Kind::NotFound, "engine-db holds no Score"};
    }
    if (scores.size() > 1) {
        throw TimingQueryError{
            Kind::Unprocessable, "engine-db holds " + std::to_string(scores.size()) +
                                     " Scores, so the root TempoMap is ambiguous; one is expected"};
    }
    const DbRow& score = scores.front();
    if (!optionalInt64(score.at(1)).has_value()) {
        throw TimingQueryError{Kind::NotFound,
            "Score " + std::to_string(requireInt64(score.at(0))) + " has no TempoMap"};
    }
    TempoMapNode root{.map = TempoMapInfo{.id = requireInt64(score.at(1)),
                          .name = optionalString(score.at(2)),
                          .ticksPerBeat = optionalInt64(score.at(3))},
        .children = {}};
    root.children = readTempoMapChildren(db, root.map.id);
    return root;
}

std::vector<TempoMapChild> readTempoMapChildren(EngineDb& db, std::int64_t mapId) {
    static_cast<void>(requireMap(db, mapId));
    std::vector<TempoMapChild> children;
    for (const DbRow& row : db.query(kReadChildren, {{"id", mapId}})) {
        children.push_back(TempoMapChild{.map = mapFrom(row),
            .syncPointParent = optionalInt64(row.at(3)),
            .syncPointChild = optionalInt64(row.at(4))});
    }
    return children;
}

TempoMapPointPage readTempoMapPoints(
    EngineDb& db, std::int64_t mapId, std::optional<std::int64_t> after, std::int32_t limit) {
    TempoMapPointPage page{.map = requireMap(db, mapId), .points = {}, .nextAfter = std::nullopt};
    std::optional<std::int64_t> previous;
    walkPoints(db, mapId, [&](const TempoMapPointInfo& point) {
        const std::string what = describeMap(mapId) + ": TempoMapPoint " + std::to_string(point.id);
        // The cursor is a position, so positions must order the list.
        if (!point.position.has_value()) {
            throw TimingQueryError{Kind::Unprocessable, what + " has no position"};
        }
        if (previous.has_value() && *point.position <= *previous) {
            throw TimingQueryError{Kind::Unprocessable,
                what + " is at " + std::to_string(*point.position) +
                    ", not after the point before it at " + std::to_string(*previous)};
        }
        previous = point.position;
        if (after.has_value() && *point.position <= *after) {
            return true;
        }
        if (page.points.size() == static_cast<std::size_t>(limit)) {
            page.nextAfter = page.points.back().position;
            return false;
        }
        page.points.push_back(point);
        return true;
    });
    return page;
}

EngineParameters readEngineParameters(EngineDb& db) {
    const std::vector<DbRow> rows = db.query(kReadConfig);
    if (rows.size() != 1) {
        throw TimingQueryError{Kind::Unprocessable,
            "expected exactly one EngineConfig, found " + std::to_string(rows.size())};
    }
    const std::optional<std::int64_t> frameSize = optionalInt64(rows.front().at(0));
    const std::optional<std::int64_t> sampleRate = optionalInt64(rows.front().at(1));
    const std::optional<std::int64_t> channelMode = optionalInt64(rows.front().at(2));

    if (!sampleRate.has_value() || *sampleRate <= 0 ||
        *sampleRate > std::numeric_limits<std::int32_t>::max()) {
        throw TimingQueryError{Kind::Unprocessable,
            "EngineConfig.sampleRate is " +
                (sampleRate.has_value() ? std::to_string(*sampleRate) : "NULL") +
                "; it must be positive"};
    }
    EngineParameters parameters{.sampleRate = SampleRate{static_cast<std::int32_t>(*sampleRate)}};
    try {
        parameters.frameSize = resolveFrameSize(frameSize, sampleRate);
    } catch (const PrecomputationError& e) {
        throw TimingQueryError{Kind::Unprocessable, e.what()};
    }
    if (!channelMode.has_value() || *channelMode < 0 ||
        *channelMode > static_cast<std::int64_t>(ChannelMode::FoaBFormat)) {
        throw TimingQueryError{Kind::Unprocessable,
            "EngineConfig.channelMode is " +
                (channelMode.has_value() ? std::to_string(*channelMode) : "NULL") +
                "; it must be 0 (mono), 1 (stereo), 2 (M/S), 3 (FOA A-format) or 4 (FOA "
                "B-format)"};
    }
    parameters.channelMode = static_cast<ChannelMode>(*channelMode);
    return parameters;
}

FramePosition framePosition(EngineDb& db, std::int64_t mapId, BeatsTicks at) {
    const TempoMapInfo map = requireMap(db, mapId);
    if (db.query(kScoresOfMap, {{"id", mapId}}).empty()) {
        throw TimingQueryError{Kind::Unprocessable,
            describeMap(mapId) +
                " is not the TempoMap of a Score; only a root map converts to samples in this "
                "version, since synchronised maps are not implemented"};
    }
    if (!map.ticksPerBeat.has_value() || *map.ticksPerBeat <= 0 ||
        *map.ticksPerBeat > std::numeric_limits<std::int32_t>::max()) {
        throw TimingQueryError{
            Kind::Unprocessable, describeMap(mapId) + " has no usable ticks_per_beat"};
    }
    const std::int64_t resolution = *map.ticksPerBeat;
    if (at.ticks >= resolution) {
        throw TimingQueryError{
            Kind::BadRequest, std::to_string(at.beats) + ":" + std::to_string(at.ticks) + " has " +
                                  std::to_string(at.ticks) + " ticks; " + describeMap(mapId) +
                                  " has " + std::to_string(resolution) + " ticks per beat"};
    }
    if (at.beats > (std::numeric_limits<std::int64_t>::max() - at.ticks) / resolution) {
        throw TimingQueryError{
            Kind::BadRequest, std::to_string(at.beats) + ":" + std::to_string(at.ticks) +
                                  " is beyond the last tick of " + describeMap(mapId)};
    }
    const MusicalTime position{(at.beats * resolution) + at.ticks};

    std::vector<TempoMapPointRecord> records;
    walkPoints(db, mapId, [&](const TempoMapPointInfo& point) {
        records.push_back(TempoMapPointRecord{.id = point.id,
            .position = point.position,
            .startTempo = point.startTempo,
            .hold = point.hold});
        return true;
    });
    std::optional<TempoMap> tempoMap;
    try {
        tempoMap.emplace(TempoMap::fromRecords(map.ticksPerBeat, records));
    } catch (const TempoMapError& e) {
        throw TimingQueryError{Kind::Unprocessable, describeMap(mapId) + ": " + e.what()};
    }

    const EngineParameters parameters = readEngineParameters(db);
    // Floored, so that a position between samples gives the one before it.
    const SampleTime sample{
        std::floor(toSampleTime(tempoMap->toSeconds(position), parameters.sampleRate).value())};
    const std::optional<FrameNumber> frame = frameContaining(sample, parameters.frameSize);
    if (!frame.has_value()) {
        throw TimingQueryError{
            Kind::Unprocessable, std::to_string(at.beats) + ":" + std::to_string(at.ticks) +
                                     " falls after the last frame a frame number can name"};
    }
    // Both terms are whole numbers of samples, and the difference is below the
    // frame size, so the conversion is exact.
    const auto offset =
        static_cast<std::int32_t>((sample - frameStart(*frame, parameters.frameSize)).value());
    return FramePosition{.frame = *frame, .offset = SampleIndex{offset}};
}

} // namespace rainbohz::rendering_engine::transform
