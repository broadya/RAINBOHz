// SPDX-License-Identifier: Apache-2.0

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
#include <stdexcept>
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

/// Whose fault a TimingQueryError is; short for TimingQueryError::Kind.
using Kind = TimingQueryError::Kind;

/// Names a map for a message.
///
/// @param mapId  TempoMap.id.
/// @return e.g. "TempoMap 3".
std::string describeMap(std::int64_t mapId) {
    return "TempoMap " + std::to_string(mapId);
}

/// Finds a TempoMap by its id.
///
/// @param[in,out] source  What the timing queries read.
/// @param mapId           TempoMap.id.
/// @return The map.
/// @throws TimingQueryError  NotFound when there is no such map.
TempoMapInfo requireMap(TimingSource& source, std::int64_t mapId) {
    std::optional<TempoMapInfo> map = source.findTempoMap(mapId);
    if (!map.has_value()) {
        throw TimingQueryError{Kind::NotFound, "there is no " + describeMap(mapId)};
    }
    return std::move(*map);
}

/// Walks a TempoMap's NEXT_POINT list from its head, a chunk of hops per
/// read, handing each point to `visit` in list order until it returns false
/// or the list ends.
///
/// Lists are acyclic by construction upstream, not by constraint (AGENTS.md,
/// Linked Lists), so the walk stops, with an error, at a revisited point.
///
/// @tparam Visit  Callable as `bool(const TempoMapPointInfo&)`.
/// @param[in,out] source  What the timing queries read.
/// @param mapId           TempoMap.id, known to exist.
/// @param visit           Called for each point; returns true to go on.
/// @throws TimingQueryError  Unprocessable when the list loops.
template <typename Visit>
void walkPoints(TimingSource& source, std::int64_t mapId, Visit visit) {
    std::unordered_set<std::int64_t> seen;
    const auto accept = [&](const TempoMapPointInfo& point) {
        if (!seen.insert(point.id).second) {
            throw TimingQueryError{Kind::Unprocessable,
                describeMap(mapId) + ": its NEXT_POINT list loops back to TempoMapPoint " +
                    std::to_string(point.id) + " after " + std::to_string(seen.size()) + " points"};
        }
        return visit(point);
    };

    const std::optional<TempoMapPointInfo> head = source.readHead(mapId);
    if (!head.has_value()) {
        return;
    }
    TempoMapPointInfo point = *head;
    if (!accept(point)) {
        return;
    }
    for (;;) {
        const std::vector<TempoMapPointInfo> chunk = source.readNextPoints(point.id);
        for (const TempoMapPointInfo& next : chunk) {
            point = next;
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

TempoMapNode readRootTempoMap(TimingSource& source) {
    const std::vector<ScoreRootRecord> scores = source.readScores();
    if (scores.empty()) {
        throw TimingQueryError{Kind::NotFound, "engine-db holds no Score"};
    }
    if (scores.size() > 1) {
        throw TimingQueryError{
            Kind::Unprocessable, "engine-db holds " + std::to_string(scores.size()) +
                                     " Scores, so the root TempoMap is ambiguous; one is expected"};
    }
    const ScoreRootRecord& score = scores.front();
    if (!score.root.has_value()) {
        throw TimingQueryError{
            Kind::NotFound, "Score " + std::to_string(score.scoreId) + " has no TempoMap"};
    }
    TempoMapNode root{.map = *score.root,
        .tempoRampPhysicalTime =
            tempoRampFromScore(score.tempoRampPhysicalTime) == TempoRamp::PhysicalTime,
        .children = {}};
    root.children = readTempoMapChildren(source, root.map.id);
    return root;
}

std::vector<TempoMapChild> readTempoMapChildren(TimingSource& source, std::int64_t mapId) {
    static_cast<void>(requireMap(source, mapId));
    return source.readChildren(mapId);
}

TempoMapPointPage readTempoMapPoints(TimingSource& source, std::int64_t mapId,
    std::optional<std::int64_t> after, std::int32_t limit) {
    TempoMapPointPage page{
        .map = requireMap(source, mapId), .points = {}, .nextAfter = std::nullopt};
    std::optional<std::int64_t> previous;
    walkPoints(source, mapId, [&](const TempoMapPointInfo& point) {
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

EngineParameters readEngineParameters(TimingSource& source) {
    const std::vector<EngineConfigRecord> rows = source.readEngineConfigs();
    if (rows.size() != 1) {
        throw TimingQueryError{Kind::Unprocessable,
            "expected exactly one EngineConfig, found " + std::to_string(rows.size())};
    }
    const std::optional<std::int64_t> frameSize = rows.front().frameSize;
    const std::optional<std::int64_t> sampleRate = rows.front().sampleRate;
    const std::optional<std::int64_t> channelMode = rows.front().channelMode;

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
    if (!channelMode.has_value()) {
        throw TimingQueryError{Kind::Unprocessable, "EngineConfig.channelMode is NULL"};
    }
    try {
        parameters.channelMode = channelModeFromInt(*channelMode);
    } catch (const std::invalid_argument& e) {
        throw TimingQueryError{Kind::Unprocessable, std::string{"EngineConfig."} + e.what()};
    }
    return parameters;
}

FramePosition framePosition(TimingSource& source, std::int64_t mapId, BeatsTicks at) {
    const TempoMapInfo map = requireMap(source, mapId);
    const std::vector<std::optional<bool>> scores = source.rampsOfScoresRootedAt(mapId);
    if (scores.empty()) {
        throw TimingQueryError{Kind::Unprocessable,
            describeMap(mapId) +
                " is not the TempoMap of a Score; only a root map converts to samples in this "
                "version. Conversion through synchronisation edges, which the transform "
                "resolves (docs/polytempo.md), is the next step for this call"};
    }
    // The ramp shape belongs to the Score. NEXT_POINT is ONE_ONE, so the map
    // has exactly one.
    const TempoRamp ramp = tempoRampFromScore(scores.front());
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
    walkPoints(source, mapId, [&](const TempoMapPointInfo& point) {
        records.push_back(TempoMapPointRecord{.id = point.id,
            .position = point.position,
            .startTempo = point.startTempo,
            .hold = point.hold});
        return true;
    });
    std::optional<TempoMap> tempoMap;
    try {
        tempoMap.emplace(TempoMap::fromRecords(map.ticksPerBeat, records, ramp));
    } catch (const TempoMapError& e) {
        throw TimingQueryError{Kind::Unprocessable, describeMap(mapId) + ": " + e.what()};
    }

    const EngineParameters parameters = readEngineParameters(source);
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
