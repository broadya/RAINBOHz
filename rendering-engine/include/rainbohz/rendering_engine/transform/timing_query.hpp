#pragma once

// Read-only questions about a loaded score's timing: its tempo maps, its
// engine parameters, and where a musical position falls in the sample stream.
//
// These are what server mode's GET calls answer (docs/server.md sections 3.4
// and 3.5). They live in the transform because they read musical time, which
// nothing outside it does (AGENTS.md); the interface carries only plain
// integers and the physical units of units.hpp, so the server can use it
// without seeing a musical unit type.
// Nothing here knows about HTTP or JSON: each function reads engine-db and
// returns plain values, or throws a TimingQueryError whose kind says whose
// fault the failure is, which the server maps to a status code.
//
// The tempo maps form a tree, later a DAG: the root is the TempoMap of the
// Score (Score -[:NEXT_POINT]-> TempoMap), and a map's children are the maps
// with a TEMPO_MAP_SYNCHRONIZATION_POINT edge to it, the edge running from
// child to parent. Nothing here assumes a child has only one parent.
//
// A map's points are a NEXT_POINT list. They are read by walking it from the
// map, which is found by its key, in chunks of up to kWalkChunk hops: Ladybug
// refuses a variable-length pattern of more than 30 hops (precomputation.md
// section 10, "Reading the linked list"). A page of points therefore costs a
// walk from the head to the end of the page, which grows with the cursor,
// and each hop's far end is still found by a scan of TempoMapPoint,
// Ladybug's parked edge-hop cost; both are accepted for now (docs/server.md
// section 8).

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// Thrown when a timing query cannot be answered. The message says why, in
/// terms of the request or of what engine-db holds.
class TimingQueryError : public std::runtime_error {
public:
    /// Whose fault the failure is.
    enum class Kind : std::uint8_t {
        /// The request itself is unusable, e.g. ticks of a beat beyond the
        /// map's resolution.
        BadRequest,

        /// The tempo map, or the Score's root map, does not exist.
        NotFound,

        /// What engine-db holds cannot answer the question, or this version
        /// cannot: an invalid map, an unusable EngineConfig, a map that is
        /// not a root.
        Unprocessable,
    };

    /// @param kind     Whose fault the failure is.
    /// @param message  What is wrong.
    TimingQueryError(Kind kind, const std::string& message)
        : std::runtime_error{message}, kind_{kind} {}

    /// @return Whose fault the failure is.
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// How many NEXT_POINT hops one query of a list walk follows. Ladybug's limit
/// for a variable-length pattern.
inline constexpr int kWalkChunk = 30;

/// A TempoMap, as engine-db holds it. Every property but the id is nullable
/// in the schema.
struct TempoMapInfo {
    /// TempoMap.id. Valid only for the score loaded when it was read.
    std::int64_t id = 0;

    /// TempoMap.name.
    std::optional<std::string> name;

    /// TempoMap.ticks_per_beat.
    std::optional<std::int64_t> ticksPerBeat;
};

/// A child of a TempoMap: the child, and the TEMPO_MAP_SYNCHRONIZATION_POINT
/// edge that joins it to the parent it was found under.
struct TempoMapChild {
    /// The child map.
    TempoMapInfo map;

    /// The edge's sync_point_parent: where, in the parent's ticks, the child
    /// is rooted.
    std::optional<std::int64_t> syncPointParent;

    /// The edge's sync_point_child: the alignment point in the child's own
    /// ticks.
    std::optional<std::int64_t> syncPointChild;
};

/// A TempoMap with its children.
struct TempoMapNode {
    /// The map.
    TempoMapInfo map;

    /// Its children, in child id order. A child joined by two edges appears
    /// twice, once per edge.
    std::vector<TempoMapChild> children;
};

/// A TempoMapPoint, as engine-db holds it. Every property but the id is
/// nullable in the schema.
struct TempoMapPointInfo {
    /// TempoMapPoint.id.
    std::int64_t id = 0;

    /// TempoMapPoint.position, ticks.
    std::optional<std::int64_t> position;

    /// TempoMapPoint.start_tempo, BPM.
    std::optional<double> startTempo;

    /// TempoMapPoint.hold.
    std::optional<bool> hold;
};

/// One page of a TempoMap's points.
struct TempoMapPointPage {
    /// The map.
    TempoMapInfo map;

    /// The points, in list order, which is strictly increasing position.
    std::vector<TempoMapPointInfo> points;

    /// The cursor for the next page: the last point's position. nullopt when
    /// this page ends the list.
    std::optional<std::int64_t> nextAfter;
};

/// EngineConfig.channelMode, numbered as the schema numbers it.
enum class ChannelMode : std::uint8_t {
    Mono,       ///< 0
    Stereo,     ///< 1, derived from M/S and decoded to stereo channels
    MidSide,    ///< 2, M/S
    FoaAFormat, ///< 3, first-order ambisonics, A-format
    FoaBFormat, ///< 4, first-order ambisonics, B-format
};

/// The name of a channel mode as the REST API spells it.
///
/// @param mode  A channel mode.
/// @return `"mono"`, `"stereo"`, `"mid-side"`, `"foa-a-format"` or
///         `"foa-b-format"`.
std::string_view channelModeName(ChannelMode mode) noexcept;

/// The engine parameters: what EngineConfig holds, resolved.
struct EngineParameters {
    /// EngineConfig.sampleRate.
    SampleRate sampleRate{0};

    /// EngineConfig.frameSize, or one second at the sample rate when it is
    /// NULL (resolveFrameSize()).
    FrameSize frameSize{0};

    /// EngineConfig.channelMode.
    ChannelMode channelMode = ChannelMode::Mono;
};

/// A musical position written `beats:ticks`, as HzL writes it
/// (hzl-compiler/docs/hzl-reference.md).
struct BeatsTicks {
    /// Whole beats from the start of the map.
    std::int64_t beats = 0;

    /// Ticks into the beat; below the map's ticks_per_beat.
    std::int64_t ticks = 0;
};

/// Where a musical position falls in the sample stream: the sample at or
/// before it, as a frame and a sample within that frame.
struct FramePosition {
    /// The frame holding the sample.
    FrameNumber frame{0};

    /// The sample's index within the frame.
    SampleIndex offset{0};
};

/// Parses a position written `beats:ticks`, or a bare number of beats.
///
/// Both parts are unsigned decimal integers that fit INT64: no sign, no
/// fraction, no exponent, no whitespace. Whether the ticks are below a map's
/// resolution is for framePosition() to check, since it knows the map.
///
/// @param text  The position, e.g. `"12:480"` or `"3"`.
/// @return The position; nullopt when `text` is not of that form.
std::optional<BeatsTicks> parseBeatsTicks(std::string_view text);

/// Reads the root of the tempo-map tree: the TempoMap of the Score, with its
/// children.
///
/// @param db  The loaded score. Not modified.
/// @return The root and its children.
/// @throws TimingQueryError  NotFound when there is no Score, or it has no
///         TempoMap; Unprocessable when there is more than one Score.
/// @throws EngineDbError  If a query fails.
TempoMapNode readRootTempoMap(EngineDb& db);

/// Reads a TempoMap's children.
///
/// @param db     The loaded score. Not modified.
/// @param mapId  TempoMap.id.
/// @return The map's children, in child id order.
/// @throws TimingQueryError  NotFound when there is no such map.
/// @throws EngineDbError  If a query fails.
std::vector<TempoMapChild> readTempoMapChildren(EngineDb& db, std::int64_t mapId);

/// Reads one page of a TempoMap's points.
///
/// @param db     The loaded score. Not modified.
/// @param mapId  TempoMap.id.
/// @param after  Only points whose position is greater are returned;
///               nullopt to start at the head of the list.
/// @param limit  The most points to return. Positive.
/// @return The page.
/// @throws TimingQueryError  NotFound when there is no such map;
///         Unprocessable when a point before the end of the page has a NULL
///         position, positions do not strictly increase, or the list loops.
/// @throws EngineDbError  If a query fails.
TempoMapPointPage readTempoMapPoints(
    EngineDb& db, std::int64_t mapId, std::optional<std::int64_t> after, std::int32_t limit);

/// Reads the engine parameters.
///
/// @param db  The loaded score. Not modified.
/// @return The parameters, with the frame size resolved.
/// @throws TimingQueryError  Unprocessable when there is not exactly one
///         EngineConfig, its sampleRate is NULL or not positive, its frame
///         size is unusable, or its channelMode is NULL or unknown.
/// @throws EngineDbError  If the query fails.
EngineParameters readEngineParameters(EngineDb& db);

/// Finds the sample at or before a musical position on a TempoMap.
///
/// The position is converted to seconds by the map (TempoMap,
/// docs/transform.md section 6.1), to samples at the sample rate, and
/// floored: a position between two samples gives the earlier one, and a
/// position on a sample gives that sample, as far as double rounding allows,
/// which can land one sample early. That sample is then split into a frame
/// and an offset within it.
///
/// Only a map that is the root of some Score converts in this version:
/// the time of any other map depends on synchronisation, which the transform
/// does not implement yet.
///
/// @param db     The loaded score. Not modified.
/// @param mapId  TempoMap.id.
/// @param at     The position.
/// @return The frame and offset.
/// @throws TimingQueryError  BadRequest when `at.ticks` is not below the map's
///         ticks_per_beat, or the position overflows INT64 ticks; NotFound
///         when there is no such map; Unprocessable when the map is not a
///         Score's root, is invalid, the engine parameters are unusable, or
///         the frame does not fit INT32.
/// @throws EngineDbError  If a query fails.
FramePosition framePosition(EngineDb& db, std::int64_t mapId, BeatsTicks at);

} // namespace rainbohz::rendering_engine::transform
