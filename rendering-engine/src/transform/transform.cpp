// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/transform.hpp"

#include "rainbohz/rendering_engine/transform/musical_units.hpp"
#include "rainbohz/rendering_engine/transform/polytempo.hpp"
#include "rainbohz/rendering_engine/transform/squash.hpp"
#include "rainbohz/rendering_engine/transform/store_handles.hpp"
#include "rainbohz/rendering_engine/transform/tempo_map.hpp"
#include "rainbohz/rendering_engine/transform/transform_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Transforms musical-time partial set timelines into per-partial
/// physical timelines, and spatial musical timelines into per-source ones.
///
/// Implements docs/transform.md. The file is organised as precomputation.cpp
/// is:
///
///   - the tempo maps, built and validated once each, then resolved to
///     physical time through their synchronisation edges (docs/polytempo.md);
///   - the per-set stage: validation of the timeline, its POINT_AT edges and
///     its states, and the physical time of each point;
///   - the per-partial stage: envelope nodes, placement, lifeline fit, unit
///     conversion, and the unit of output that replaces the partial's
///     earlier timelines;
///   - the per-source stage: the spatial timeline, its POINT_AT edges and
///     states, placement, unit conversion and its unit of output (§6.8);
///   - the pure stages, which are testable without a database;
///   - transformAll(), which reads everything through a TransformStore before
///     anything is written, and processes sets, partials and sources
///     independently.
///
/// What is read and how it is written belong to the store, not to this file
/// (transform_store.hpp; @see ADR 0028).
///
/// Section numbers in comments refer to docs/transform.md.
///
/// @see transform.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

/// How many owners' output (partials, or sources) is written per call to the
/// store, which the Ladybug backend writes as one transaction (§7). As in
/// precomputation; @see ADR 0022.
constexpr std::size_t kOwnersPerFlush = 64;

// ---------------------------------------------------------------------------
// Limits from the schema (engine-schema.cypher, "Units for specification of
// a score", and the curve enumerations)
// ---------------------------------------------------------------------------

constexpr std::int64_t kMinCurveType = 0;      ///< Lowest valid curve type (schema enum).
constexpr std::int64_t kMaxCurveType = 4;      ///< Highest valid curve type (schema enum).
constexpr double kMinCurveAmount = 0.0;        ///< Lowest valid curve amount.
constexpr double kMaxCurveAmount = 1.0;        ///< Highest valid curve amount.
constexpr double kMinAmplitude = -144.0;       ///< Lowest valid amplitude, dB.
constexpr double kMaxAmplitude = 0.0;          ///< Highest valid amplitude, dB.
constexpr double kMinPhaseDegrees = -180.0;    ///< Lowest valid phase, degrees.
constexpr double kMaxPhaseDegrees = 180.0;     ///< Highest valid phase, degrees.
constexpr double kMinAzimuthDegrees = -180.0;  ///< Lowest valid azimuth, degrees.
constexpr double kMaxAzimuthDegrees = 180.0;   ///< Highest valid azimuth, degrees.
constexpr double kMinElevationDegrees = -90.0; ///< Lowest valid elevation, degrees.
constexpr double kMaxElevationDegrees = 90.0;  ///< Highest valid elevation, degrees.
constexpr double kMinDistanceMetres = 1.0;     ///< Lowest valid distance, metres.
constexpr double kMaxDistanceMetres = 16384.0; ///< Highest valid distance, metres.

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Checks that a store answered for every unit it was given.
///
/// @param results  How many results the store returned.
/// @param units    How many units it was given.
/// @throws EngineDbError  If the two differ: the store broke its contract,
///         so which units were written is unknown.
void requireOneResultPerUnit(std::size_t results, std::size_t units) {
    if (results != units) {
        throw EngineDbError{"the store returned " + std::to_string(results) + " result(s) for " +
                            std::to_string(units) + " unit(s)"};
    }
}

/// Formats a double for an error message, with enough digits to identify it.
///
/// @param value  Any value, including non-finite ones.
/// @return The value in the shortest form that round-trips.
std::string formatNumber(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    return out.str();
}

/// Follows a NEXT_POINT list from its head, in C++.
///
/// Lists are acyclic by construction upstream, not by constraint (AGENTS.md,
/// Linked Lists), so the walk stops, with an error, at a revisited node.
///
/// @tparam Error  The exception to throw on a loop.
/// @param head  Id of the first node, or nullopt for an empty list.
/// @param next  For each node id, the id of its successor.
/// @param what  What the list is, for the error message.
/// @return The node ids in list order. Empty when `head` is nullopt.
/// @throws Error  If the list returns to a node it has already visited.
template <typename Error>
std::vector<std::int64_t> walkList(
    std::optional<std::int64_t> head, const IdMap& next, std::string_view what) {
    std::vector<std::int64_t> chain;
    std::unordered_set<std::int64_t> seen;
    for (std::optional<std::int64_t> at = head; at.has_value();) {
        if (!seen.insert(*at).second) {
            throw Error{std::string{what} + ": its NEXT_POINT list loops back to node id " +
                        std::to_string(*at) + " after " + std::to_string(chain.size()) + " nodes"};
        }
        chain.push_back(*at);
        const auto link = next.find(*at);
        at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
    }
    return chain;
}

/// Names a timeline point in a set-level message.
///
/// @param index  Position of the point in the set's timeline, from 0.
/// @param id     Its PartialSetMusicalTimelinePoint.id.
/// @return e.g. "point 2 (id 17)".
std::string describePoint(std::size_t index, std::int64_t id) {
    return "point " + std::to_string(index) + " (id " + std::to_string(id) + ")";
}

// ---------------------------------------------------------------------------
// Tempo maps
// ---------------------------------------------------------------------------

/// Every Score's tempo maps, resolved to physical time (docs/polytempo.md).
struct ResolvedMaps {
    /// Each map's resolved form, by TempoMap.id.
    std::unordered_map<std::int64_t, ResolvedTempoMap> maps;

    /// Score.id of the Score whose graph each map belongs to, by TempoMap.id.
    std::unordered_map<std::int64_t, std::int64_t> scoreOfMap;

    /// Each Score's own TempoMap, the root of its graph, by Score.id. A Score
    /// without one is absent.
    std::unordered_map<std::int64_t, std::int64_t> rootOfScore;
};

/// Builds, validates and resolves every TempoMap (§6.1; docs/polytempo.md
/// §7).
///
/// The maps are ordered parents first from the Scores' roots, each is built
/// as a TempoMap in its Score's ramp shape, and each is then resolved against
/// its synchronisation edges, so that a child's anchors come from parents
/// already in physical time.
///
/// @param inputs       Everything read for the run.
/// @param[out] report  Receives one TempoMapSummary per map, in resolution
///                     order.
/// @return The resolved maps, and whose Score each belongs to.
/// @throws TransformError  If any map is invalid, or the synchronisation
///         edges cannot be resolved (docs/polytempo.md §6).
ResolvedMaps buildTempoMaps(const TransformInputs& inputs, TransformReport& report) {
    std::vector<std::int64_t> mapIds;
    mapIds.reserve(inputs.tempoMaps.size());
    for (const TempoMapRecord& record : inputs.tempoMaps) {
        mapIds.push_back(record.id);
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> roots;
    std::unordered_map<std::int64_t, TempoRamp> rampOfScore;
    ResolvedMaps result;
    for (const ScoreRecord& score : inputs.scores) {
        rampOfScore.emplace(score.scoreId, tempoRampFromScore(score.tempoRampPhysicalTime));
        if (score.tempoMapId.has_value()) {
            roots.emplace_back(*score.tempoMapId, score.scoreId);
            result.rootOfScore.emplace(score.scoreId, *score.tempoMapId);
        }
    }
    try {
        TempoMapOrder order = orderTempoMaps(mapIds, inputs.syncEdges, roots);
        std::unordered_map<std::int64_t, TempoMap> nominal;
        for (const std::int64_t mapId : order.order) {
            const std::string what = "TempoMap " + std::to_string(mapId);
            const TempoMapRecord& record =
                *std::ranges::find(inputs.tempoMaps, mapId, &TempoMapRecord::id);
            const auto heads = inputs.tempoMapHeads.find(mapId);
            const std::optional<std::int64_t> head =
                heads == inputs.tempoMapHeads.end() || heads->second.empty()
                    ? std::nullopt
                    : heads->second.front();
            try {
                std::vector<TempoMapPointRecord> records;
                for (const std::int64_t id :
                    walkList<TempoMapError>(head, inputs.tempoMapNext, what)) {
                    records.push_back(inputs.tempoMapPoints.at(id));
                }
                nominal.emplace(mapId, TempoMap::fromRecords(record.ticksPerBeat, records,
                                           rampOfScore.at(order.scoreOfMap.at(mapId))));
            } catch (const TempoMapError& e) {
                throw TempoMapError{what + ": " + e.what()};
            }
        }
        result.maps = resolveTempoMapGraph(nominal, inputs.syncEdges, order);
        for (const std::int64_t mapId : order.order) {
            const TempoMap& map = nominal.at(mapId);
            TempoMapSummary summary{
                .scoreId = order.scoreOfMap.at(mapId),
                .tempoMapId = mapId,
                .name = std::ranges::find(inputs.tempoMaps, mapId, &TempoMapRecord::id)->name,
                .ticksPerBeat = map.ticksPerBeat().value(),
                .pointCount = map.points().size(),
                .rampPhysicalTime = map.ramp() == TempoRamp::PhysicalTime,
                .parentIds = {},
                .syncPointCount = 0,
            };
            for (const SyncEdgeRecord& edge : inputs.syncEdges) {
                if (edge.childId != mapId) {
                    continue;
                }
                ++summary.syncPointCount;
                if (std::ranges::find(summary.parentIds, edge.parentId) ==
                    summary.parentIds.end()) {
                    summary.parentIds.push_back(edge.parentId);
                }
            }
            std::ranges::sort(summary.parentIds);
            report.tempoMaps.push_back(std::move(summary));
        }
        result.scoreOfMap = std::move(order.scoreOfMap);
    } catch (const TempoMapError& e) {
        throw TransformError{e.what()};
    }
    return result;
}

// ---------------------------------------------------------------------------
// The per-set stage
// ---------------------------------------------------------------------------

/// A set's timeline, validated, with the physical time of each point and
/// each partial's envelope nodes (§6.2).
struct SetEnvelopes {
    /// The computed physical time of every point, by point index.
    std::vector<SampleTime> pointTimes;

    /// Each member partial's envelope nodes: frequency, then amplitude.
    std::unordered_map<std::int64_t,
        std::pair<std::vector<EnvelopeNode>, std::vector<EnvelopeNode>>>
        nodes;

    /// Each member partial's states' phase fields, one per state it has,
    /// whatever their meaning (§6.6).
    std::unordered_map<std::int64_t, std::vector<StatePhase>> states;
};

/// Checks a curve type and amount of a point, for one property.
///
/// @tparam Error  The exception of the timeline's scope: PartialSetTransformError
///                or SoundSourceTransformError.
/// @param index     Position of the point in the timeline, from 0.
/// @param id        Its id.
/// @param property  The property's column prefix, e.g. "frequency" or
///                  "azimuth".
/// @param type      The curve type as read.
/// @param amount    The curve amount as read.
/// @throws Error  If either is NULL or out of range.
template <typename Error>
void requireCurve(std::size_t index, std::int64_t id, std::string_view property,
    const std::optional<std::int64_t>& type, const std::optional<double>& amount) {
    const std::string where = describePoint(index, id) + ": " + std::string{property};
    if (!type.has_value() || *type < kMinCurveType || *type > kMaxCurveType) {
        throw Error{where + "_curve_type is " +
                    (type.has_value() ? std::to_string(*type) : "NULL") + "; it must be in [" +
                    std::to_string(kMinCurveType) + ", " + std::to_string(kMaxCurveType) + "]"};
    }
    if (!amount.has_value() || !std::isfinite(*amount) || *amount < kMinCurveAmount ||
        *amount > kMaxCurveAmount) {
        throw Error{where + "_curve_amount is " +
                    (amount.has_value() ? formatNumber(*amount) : "NULL") + "; it must be in [" +
                    formatNumber(kMinCurveAmount) + ", " + formatNumber(kMaxCurveAmount) + "]"};
    }
}

/// What pointTime() carries from one point of a timeline to the next.
struct PreviousPoint {
    std::optional<std::int64_t> sync; ///< The previous point's sync_point.
    std::optional<std::int64_t> map;  ///< The TempoMap its POINT_AT leads to.
};

/// Validates one point's POINT_AT against its predecessor and the Score's
/// maps, and returns its physical time.
///
/// @tparam Error  The exception of the timeline's scope: PartialSetTransformError
///                or SoundSourceTransformError.
/// @param index         Position of the point in the timeline, from 0.
/// @param id            Its id.
/// @param at            Its POINT_AT as read.
/// @param scoreId       The Score the timeline's owner hangs from. Every
///                      POINT_AT must lead to one of its maps.
/// @param maps          Every Score's resolved maps.
/// @param rate          Sample rate for the conversion to samples.
/// @param[in,out] previous  The previous point's sync_point and TempoMap;
///                      updated to this point's on return. Both unset for
///                      the first point.
/// @return `toSampleTime(toSeconds(sync_point) + offset_seconds)` on the map
///         the POINT_AT leads to.
/// @throws Error  If the point has no POINT_AT, or one to a map outside the
///         Score's graph, or to a different map from the previous point's
///         (ticks on different maps cannot be ordered), or a NULL or
///         negative sync_point, or a sync_point before the previous point's,
///         or a non-finite offset.
template <typename Error>
SampleTime pointTime(std::size_t index, std::int64_t id, const PointAtRecord& at,
    std::int64_t scoreId, const ResolvedMaps& maps, SampleRate rate, PreviousPoint& previous) {
    const std::string where = describePoint(index, id);
    if (!at.tempoMapId.has_value()) {
        throw Error{where + " has no POINT_AT"};
    }
    const auto owner = maps.scoreOfMap.find(*at.tempoMapId);
    if (owner == maps.scoreOfMap.end() || owner->second != scoreId) {
        throw Error{where + ": POINT_AT leads to TempoMap " + std::to_string(*at.tempoMapId) +
                    ", which is not a TempoMap of Score " + std::to_string(scoreId)};
    }
    if (previous.map.has_value() && *previous.map != *at.tempoMapId) {
        throw Error{where + ": POINT_AT leads to TempoMap " + std::to_string(*at.tempoMapId) +
                    " but the earlier points lead to TempoMap " + std::to_string(*previous.map) +
                    "; a timeline keeps time by one TempoMap"};
    }
    previous.map = at.tempoMapId;
    std::optional<std::int64_t>& previousSync = previous.sync;
    if (!at.syncPoint.has_value() || *at.syncPoint < 0) {
        throw Error{where + ": sync_point is " +
                    (at.syncPoint.has_value() ? std::to_string(*at.syncPoint) : "NULL") +
                    "; it must be >= 0"};
    }
    const std::int64_t sync = *at.syncPoint;
    if (previousSync.has_value() && sync < *previousSync) {
        throw Error{where + ": sync_point " + std::to_string(sync) +
                    " is before the previous point's " + std::to_string(*previousSync) +
                    "; points must be in non-decreasing musical order"};
    }
    previousSync = sync;
    const double offset = at.offsetSeconds.value_or(0.0);
    if (!std::isfinite(offset)) {
        throw Error{
            where + ": offset_seconds is " + formatNumber(offset) + ", which is not finite"};
    }

    const SecondsTime seconds =
        maps.maps.at(*at.tempoMapId).toSeconds(MusicalTime{sync}) + SecondsDuration{offset};
    return toSampleTime(seconds, rate);
}

/// Adds one point's states to its partials' envelope nodes (§6.2).
///
/// @param index          Position of the point in the timeline, from 0.
/// @param point          The point as read; its curve fields are valid.
/// @param states         The PartialStates reached from it.
/// @param members        The ids of the set's partials.
/// @param[in,out] envelopes  Each partial's (frequency, amplitude) envelope nodes and
///                       states' phase fields; this point's are appended.
/// @throws PartialSetTransformError  If a state belongs to a partial outside
///         the set, or two states belong to the same partial.
void gatherStates(std::size_t index, const MusicalPointRecord& point,
    std::span<const StateRecord> states, const std::unordered_set<std::int64_t>& members,
    SetEnvelopes& envelopes) {
    std::unordered_set<std::int64_t> seenPartials;
    for (const StateRecord& state : states) {
        const std::string where = describePoint(index, point.id);
        if (!members.contains(state.partialId)) {
            throw PartialSetTransformError{
                where + ": PartialState " + std::to_string(state.stateId) + " belongs to Partial " +
                std::to_string(state.partialId) + ", which is not a member of this PartialSet"};
        }
        if (!seenPartials.insert(state.partialId).second) {
            throw PartialSetTransformError{
                where + ": Partial " + std::to_string(state.partialId) +
                " has more than one PartialState; at most one is allowed per point"};
        }
        auto& [frequency, amplitude] = envelopes.nodes[state.partialId];
        if (state.frequency.has_value()) {
            frequency.push_back(EnvelopeNode{.point = index,
                .value = *state.frequency,
                .curveType = point.frequencyCurveType.value_or(0),
                .curveAmount = point.frequencyCurveAmount.value_or(0.0)});
        }
        if (state.amplitude.has_value()) {
            amplitude.push_back(EnvelopeNode{.point = index,
                .value = *state.amplitude,
                .curveType = point.amplitudeCurveType.value_or(0),
                .curveAmount = point.amplitudeCurveAmount.value_or(0.0)});
        }
        // Every state is kept, to check it lies within the lifeline; which
        // ones are phase points is decided per partial (§6.6).
        envelopes.states[state.partialId].push_back(
            StatePhase{.point = index, .degrees = state.phase, .freeRun = state.phaseFreeRun});
    }
}

/// Validates a set's timeline, POINT_AT edges and states, and computes the
/// physical time of every point and the envelope nodes of every member (§6.2).
///
/// @param inputs   Everything read for the run.
/// @param set      The set.
/// @param scoreId  The set's Score, one of whose maps every POINT_AT must
///                 lead to.
/// @param maps     Every Score's resolved maps.
/// @param rate     Sample rate for the conversion to samples.
/// @return The point times and the members' envelope nodes.
/// @throws PartialSetTransformError  If the set has no, or more than one,
///         timeline; the timeline has no points or loops; a point has no
///         POINT_AT, or one to a map outside the Score's graph or to a
///         different map from the previous point's, or a NULL or negative
///         sync_point,
///         or a sync_point before the previous point's, or a non-finite
///         offset, or a curve field out of range; or a state belongs to a
///         partial outside the set, or two states at one point belong to the
///         same partial.
SetEnvelopes gatherSet(const TransformInputs& inputs, const MusicalSetRecord& set,
    std::int64_t scoreId, const ResolvedMaps& maps, SampleRate rate) {
    if (set.timelines.size() != 1) {
        throw PartialSetTransformError{"it has " + std::to_string(set.timelines.size()) +
                                       " PartialSetMusicalTimelines; exactly one is required"};
    }
    const std::vector<std::int64_t> pointIds = walkList<PartialSetTransformError>(
        set.timelines.front().second, inputs.musicalNext, "its PartialSetMusicalTimeline");
    if (pointIds.empty()) {
        throw PartialSetTransformError{"its PartialSetMusicalTimeline has no points"};
    }

    std::unordered_set<std::int64_t> members;
    if (const auto found = inputs.membersOfSet.find(set.setId);
        found != inputs.membersOfSet.end()) {
        for (const auto& [partialId, name] : found->second) {
            members.insert(partialId);
        }
    }

    SetEnvelopes envelopes;
    envelopes.pointTimes.reserve(pointIds.size());
    PreviousPoint previous;
    static const std::vector<StateRecord> kNoStates;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const MusicalPointRecord& point = inputs.musicalPoints.at(pointIds[i]);
        envelopes.pointTimes.push_back(pointTime<PartialSetTransformError>(
            i, point.id, point.at, scoreId, maps, rate, previous));
        requireCurve<PartialSetTransformError>(
            i, point.id, "frequency", point.frequencyCurveType, point.frequencyCurveAmount);
        requireCurve<PartialSetTransformError>(
            i, point.id, "amplitude", point.amplitudeCurveType, point.amplitudeCurveAmount);
        const auto states = inputs.statesOfPoint.find(point.id);
        gatherStates(i, point, states == inputs.statesOfPoint.end() ? kNoStates : states->second,
            members, envelopes);
    }
    return envelopes;
}

// ---------------------------------------------------------------------------
// The per-partial stage
// ---------------------------------------------------------------------------

/// Checks the envelope nodes' values, naming the offending point.
///
/// @param frequency  Frequency envelope nodes, Hz.
/// @param amplitude  Amplitude envelope nodes, dB.
/// @throws PartialTransformError  If a frequency is not finite, or an
///         amplitude is not finite or outside [−144, 0].
void requireValues(
    std::span<const EnvelopeNode> frequency, std::span<const EnvelopeNode> amplitude) {
    for (const EnvelopeNode& node : frequency) {
        if (!std::isfinite(node.value)) {
            throw PartialTransformError{"at point " + std::to_string(node.point) +
                                        " its frequency is " + formatNumber(node.value) +
                                        ", which is not finite"};
        }
    }
    for (const EnvelopeNode& node : amplitude) {
        if (!std::isfinite(node.value) || node.value < kMinAmplitude ||
            node.value > kMaxAmplitude) {
            throw PartialTransformError{"at point " + std::to_string(node.point) +
                                        " its amplitude is " + formatNumber(node.value) +
                                        " dB; it must be in [" + formatNumber(kMinAmplitude) +
                                        ", " + formatNumber(kMaxAmplitude) + "]"};
        }
    }
}

/// Transforms one partial: the unit of output to write for it (§6.2 to
/// §6.6, §7).
///
/// @param inputs        Everything read for the run.
/// @param envelopes     The set's point times and envelope nodes.
/// @param partialId     The partial.
/// @param rate          Sample rate, for the frequency conversion.
/// @param[out] outcome  On success, its point counts are set. They stand
///                      unless the store then fails to write the unit.
/// @return The partial's output, to be written as one unit.
/// @throws PartialTransformError  If the partial's envelope nodes are invalid,
///         or its lifeline collapses.
TransformedPartial transformPartial(const TransformInputs& inputs, const SetEnvelopes& envelopes,
    std::int64_t partialId, SampleRate rate, PartialTransformOutcome& outcome) {
    static const std::pair<std::vector<EnvelopeNode>, std::vector<EnvelopeNode>> kNoNodes;
    static const std::vector<StatePhase> kNoStates;
    const auto found = envelopes.nodes.find(partialId);
    const auto& [frequencyNodes, amplitudeNodes] =
        found == envelopes.nodes.end() ? kNoNodes : found->second;
    const auto foundStates = envelopes.states.find(partialId);
    const std::vector<StatePhase>& states =
        foundStates == envelopes.states.end() ? kNoStates : foundStates->second;

    requireSharedEndPoints(frequencyNodes, amplitudeNodes);
    requireStatesWithinLifeline(frequencyNodes, states);
    requireValues(frequencyNodes, amplitudeNodes);
    requireValidPhase(states);

    TransformedPartial output{.partialId = partialId,
        .staleFrequency = {},
        .staleAmplitude = {},
        .stalePhase = {},
        .frequency = placeNodes(frequencyNodes, envelopes.pointTimes),
        .amplitude = placeNodes(amplitudeNodes, envelopes.pointTimes),
        .phase = placePhaseNodes(phaseNodesOf(states), envelopes.pointTimes)};
    if (output.frequency.size() < 2) {
        throw PartialTransformError{
            "its lifeline collapsed to a single instant at " +
            formatNumber(output.frequency.front().time.value()) +
            " samples: every later frequency point was pulled back onto the first by its offset"};
    }
    fitToLifeline(output.amplitude, output.frequency.back().time);
    fitToLifeline(output.phase, output.frequency.back().time);
    collapseFreeRuns(output.phase);
    for (PhysicalPoint& point : output.frequency) {
        point.value = toAngular(Hertz{point.value}, rate).value();
    }

    const auto stale = [&](const auto& existing, const IdMap& next, std::string_view what) {
        std::vector<std::pair<std::int64_t, std::vector<std::int64_t>>> result;
        if (const auto timelines = existing.find(partialId); timelines != existing.end()) {
            for (const ExistingTimeline& timeline : timelines->second) {
                result.emplace_back(timeline.timelineId,
                    walkList<PartialTransformError>(timeline.head, next, what));
            }
        }
        return result;
    };
    output.staleFrequency = stale(
        inputs.existingFrequency, inputs.frequencyNext, "its existing FrequencyPhysicalTimeline");
    output.staleAmplitude = stale(
        inputs.existingAmplitude, inputs.amplitudeNext, "its existing AmplitudePhysicalTimeline");
    output.stalePhase =
        stale(inputs.existingPhase, inputs.phaseNext, "its existing PhasePhysicalTimeline");

    outcome.frequencyPoints = output.frequency.size();
    outcome.amplitudePoints = output.amplitude.size();
    outcome.phasePoints = output.phase.size();
    return output;
}

/// Transforms one set: validates it, then its partials one by one.
///
/// @param inputs          Everything read for the run.
/// @param set             The set.
/// @param maps            Every Score's resolved tempo maps.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the set's error, or its partials'
///                        outcomes.
/// @param staged          Called with the index in `outcome.partials` of each
///                        partial transformed, after its outcome is
///                        recorded, and with its output; it may write it.
void transformSet(const TransformInputs& inputs, const MusicalSetRecord& set,
    const ResolvedMaps& maps, SampleRate rate, PartialSetTransformOutcome& outcome,
    const std::function<void(std::size_t, TransformedPartial&&)>& staged) {
    SetEnvelopes envelopes;
    try {
        const auto score = inputs.scoreOfSet.find(set.setId);
        if (score == inputs.scoreOfSet.end()) {
            throw PartialSetTransformError{
                "it is not reached from any Score through SoundEntity and SoundSource"};
        }
        if (!maps.rootOfScore.contains(score->second)) {
            throw PartialSetTransformError{"its Score " + std::to_string(score->second) +
                                           " has no TempoMap, so its musical time is undefined"};
        }
        envelopes = gatherSet(inputs, set, score->second, maps, rate);
    } catch (const PartialSetTransformError& e) {
        outcome.error = e.what();
        return;
    }

    const auto members = inputs.membersOfSet.find(set.setId);
    if (members == inputs.membersOfSet.end()) {
        return;
    }
    for (const auto& [partialId, partialName] : members->second) {
        PartialTransformOutcome partial{.partialId = partialId,
            .partialName = partialName,
            .error = std::nullopt,
            .frequencyPoints = 0,
            .amplitudePoints = 0,
            .phasePoints = 0};
        std::optional<TransformedPartial> output;
        try {
            output = transformPartial(inputs, envelopes, partialId, rate, partial);
        } catch (const PartialTransformError& e) {
            partial.error = e.what();
        }
        outcome.partials.push_back(std::move(partial));
        if (output.has_value()) {
            staged(outcome.partials.size() - 1, std::move(*output));
        }
    }
}

// ---------------------------------------------------------------------------
// The per-source stage (§6.8)
// ---------------------------------------------------------------------------

/// The column prefixes of the three spatial properties, in kSpatialKinds
/// order, for messages.
constexpr std::array<std::string_view, 3> kSpatialProperties{"azimuth", "elevation", "distance"};

/// A source's timeline, validated, with the physical time of each point and
/// its three envelopes.
struct SourceEnvelopes {
    /// The computed physical time of every point, by point index.
    std::vector<SampleTime> pointTimes;

    /// The azimuth (degrees), elevation (degrees) and distance (metres)
    /// envelopes.
    SpatialTriple<EnvelopeNode> envelopes;
};

/// Validates a source's spatial timeline, POINT_AT edges and states, and
/// computes the physical time of every point and its three envelopes.
///
/// @param inputs   Everything read for the run.
/// @param source   The source.
/// @param scoreId  The source's Score, one of whose maps every POINT_AT must
///                 lead to.
/// @param maps     Every Score's resolved maps.
/// @param rate     Sample rate for the conversion to samples.
/// @return The point times and the envelopes.
/// @throws SoundSourceTransformError  If the source has no, or more than one,
///         timeline; the timeline has no points or loops; a point's POINT_AT
///         or curve fields are invalid (as for a set); a point has more than
///         one state; the first point does not define all three properties;
///         or a value is out of range.
SourceEnvelopes gatherSource(const TransformInputs& inputs, const SpatialSourceRecord& source,
    std::int64_t scoreId, const ResolvedMaps& maps, SampleRate rate) {
    if (source.timelines.size() != 1) {
        throw SoundSourceTransformError{
            "it has " + std::to_string(source.timelines.size()) +
            " SpatialPositionMusicalTimelines; exactly one is required"};
    }
    const std::vector<std::int64_t> pointIds =
        walkList<SoundSourceTransformError>(source.timelines.front().second,
            inputs.spatialMusicalNext, "its SpatialPositionMusicalTimeline");
    if (pointIds.empty()) {
        throw SoundSourceTransformError{"its SpatialPositionMusicalTimeline has no points"};
    }

    SourceEnvelopes result;
    result.pointTimes.reserve(pointIds.size());
    std::vector<SpatialState> states;
    std::vector<SpatialCurves> curves;
    PreviousPoint previous;
    for (std::size_t i = 0; i < pointIds.size(); ++i) {
        const SpatialMusicalPointRecord& point = inputs.spatialPoints.at(pointIds[i]);
        result.pointTimes.push_back(pointTime<SoundSourceTransformError>(
            i, point.id, point.at, scoreId, maps, rate, previous));
        SpatialCurves pointCurves;
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            requireCurve<SoundSourceTransformError>(i, point.id, kSpatialProperties.at(k),
                point.curveType.at(k), point.curveAmount.at(k));
            // Checked non-NULL by requireCurve().
            pointCurves.curveType.at(k) = point.curveType.at(k).value_or(0);
            pointCurves.curveAmount.at(k) = point.curveAmount.at(k).value_or(0.0);
        }
        curves.push_back(pointCurves);

        const auto found = inputs.statesOfSpatialPoint.find(point.id);
        if (found == inputs.statesOfSpatialPoint.end()) {
            continue;
        }
        if (found->second.size() != 1) {
            throw SoundSourceTransformError{describePoint(i, point.id) + " has " +
                                            std::to_string(found->second.size()) +
                                            " SpatialPositionStates; at most one is allowed"};
        }
        const SpatialStateRecord& state = found->second.front();
        states.push_back(SpatialState{.point = i,
            .azimuth = state.azimuth,
            .elevation = state.elevation,
            .distance = state.distance});
    }

    requireCompletePosition(states);
    requireSpatialValues(states);
    result.envelopes = spatialNodesOf(states, curves);
    return result;
}

/// Transforms one source's spatial position: the unit of output to write for
/// it (§6.8, §7).
///
/// @param inputs          Everything read for the run.
/// @param source          The source.
/// @param maps            Every Score's resolved tempo maps.
/// @param rate            Sample rate.
/// @param[out] outcome    Receives the source's error, or its point counts,
///                        which stand unless the store then fails to write
///                        the unit.
/// @return The source's output; nullopt when it failed, the reason being in
///         `outcome`.
std::optional<TransformedSource> transformSource(const TransformInputs& inputs,
    const SpatialSourceRecord& source, const ResolvedMaps& maps, SampleRate rate,
    SoundSourceTransformOutcome& outcome) {
    try {
        const auto score = inputs.scoreOfSource.find(source.sourceId);
        if (score == inputs.scoreOfSource.end()) {
            throw SoundSourceTransformError{"it is not reached from any Score through SoundEntity"};
        }
        if (!maps.rootOfScore.contains(score->second)) {
            throw SoundSourceTransformError{"its Score " + std::to_string(score->second) +
                                            " has no TempoMap, so its musical time is undefined"};
        }
        const SourceEnvelopes envelopes = gatherSource(inputs, source, score->second, maps, rate);
        TransformedSource output{.sourceId = source.sourceId,
            .stale = {},
            .points = placeSpatialNodes(envelopes.envelopes, envelopes.pointTimes)};
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const auto& existing = inputs.existingSpatial.at(k);
            if (const auto timelines = existing.find(source.sourceId);
                timelines != existing.end()) {
                for (const ExistingTimeline& timeline : timelines->second) {
                    output.stale.at(k).emplace_back(timeline.timelineId,
                        walkList<SoundSourceTransformError>(timeline.head,
                            inputs.spatialPhysicalNext.at(k),
                            "its existing " +
                                std::string{spatialTimelineName(kSpatialKinds.at(k))}));
                }
            }
        }
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            outcome.points.at(k) = output.points.at(k).size();
        }
        return output;
    } catch (const SoundSourceTransformError& e) {
        outcome.error = e.what();
        return std::nullopt;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pure stages
// ---------------------------------------------------------------------------

void requireSharedEndPoints(
    std::span<const EnvelopeNode> frequency, std::span<const EnvelopeNode> amplitude) {
    const auto requireTwo = [](std::span<const EnvelopeNode> nodes, std::string_view property) {
        if (nodes.size() < 2) {
            throw PartialTransformError{"its " + std::string{property} + " is defined at " +
                                        std::to_string(nodes.size()) +
                                        " point(s); at least two are required"};
        }
    };
    requireTwo(frequency, "frequency");
    requireTwo(amplitude, "amplitude");
    if (frequency.front().point != amplitude.front().point) {
        throw PartialTransformError{
            "its frequency is first defined at point " + std::to_string(frequency.front().point) +
            " but its amplitude at point " + std::to_string(amplitude.front().point) +
            "; both must be defined at the partial's first point"};
    }
    if (frequency.back().point != amplitude.back().point) {
        throw PartialTransformError{
            "its frequency is last defined at point " + std::to_string(frequency.back().point) +
            " but its amplitude at point " + std::to_string(amplitude.back().point) +
            "; both must be defined at the partial's last point"};
    }
}

std::vector<PhysicalPoint> placeNodes(
    std::span<const EnvelopeNode> nodes, std::span<const SampleTime> pointTimes) {
    std::vector<SampleTime> times;
    times.reserve(nodes.size());
    for (const EnvelopeNode& node : nodes) {
        times.push_back(pointTimes[node.point]);
    }
    std::vector<PhysicalPoint> points;
    for (const Placement& placement : squash(times)) {
        const EnvelopeNode& node = nodes[placement.index];
        points.push_back(PhysicalPoint{.time = placement.time,
            .value = node.value,
            .curveType = node.curveType,
            .curveAmount = node.curveAmount});
    }
    return points;
}

void fitToLifeline(std::vector<PhysicalPoint>& amplitude, SampleTime end) {
    PhysicalPoint last = amplitude.back();
    last.time = end;
    amplitude.pop_back();
    std::erase_if(amplitude, [end](const PhysicalPoint& point) { return point.time >= end; });
    amplitude.push_back(last);
}

void requireStatesWithinLifeline(
    std::span<const EnvelopeNode> frequency, std::span<const StatePhase> states) {
    // The caller has checked that frequency has at least two envelope nodes, and each
    // sits on a point where the partial has a state, so states is not empty.
    if (states.empty() || states.front().point != frequency.front().point) {
        throw PartialTransformError{
            "it has a state at point " +
            (states.empty() ? std::string{"(none)"} : std::to_string(states.front().point)) +
            ", before its frequency is first defined at point " +
            std::to_string(frequency.front().point) +
            "; every state must lie within the lifeline the frequency defines"};
    }
    if (states.back().point != frequency.back().point) {
        throw PartialTransformError{
            "it has a state at point " + std::to_string(states.back().point) +
            ", after its frequency is last defined at point " +
            std::to_string(frequency.back().point) +
            "; every state must lie within the lifeline the frequency defines"};
    }
}

void requireValidPhase(std::span<const StatePhase> states) {
    for (const StatePhase& state : states) {
        const std::string where = "at point " + std::to_string(state.point);
        if (state.degrees.has_value()) {
            if (state.freeRun != false) {
                throw PartialTransformError{where + " its phase is " +
                                            formatNumber(*state.degrees) +
                                            " degrees with phase_free_run " +
                                            (state.freeRun.has_value() ? "true" : "NULL") +
                                            "; a phase target needs phase_free_run false"};
            }
            if (!std::isfinite(*state.degrees) || *state.degrees < kMinPhaseDegrees ||
                *state.degrees > kMaxPhaseDegrees) {
                throw PartialTransformError{
                    where + " its phase is " + formatNumber(*state.degrees) +
                    " degrees; a target must be in [" + formatNumber(kMinPhaseDegrees) + ", " +
                    formatNumber(kMaxPhaseDegrees) + "]"};
            }
        } else if (state.freeRun == false) {
            throw PartialTransformError{
                where + " its phase_free_run is false with no phase; false needs a phase target, "
                        "true means free-running, and NULL plays no role in phase"};
        }
    }
}

std::vector<PhaseEnvelopeNode> phaseNodesOf(std::span<const StatePhase> states) {
    std::vector<PhaseEnvelopeNode> nodes;
    for (const StatePhase& state : states) {
        const bool isTarget = state.degrees.has_value();
        const bool isFree = state.freeRun == true;
        // A default first state still gives a NULL point: precomputation
        // needs one at the lifeline's start, and running free to the first
        // target is what the default means there.
        if (isTarget || isFree || nodes.empty()) {
            nodes.push_back(PhaseEnvelopeNode{.point = state.point, .degrees = state.degrees});
        }
    }
    return nodes;
}

std::vector<PhasePoint> placePhaseNodes(
    std::span<const PhaseEnvelopeNode> nodes, std::span<const SampleTime> pointTimes) {
    std::vector<SampleTime> times;
    times.reserve(nodes.size());
    for (const PhaseEnvelopeNode& node : nodes) {
        times.push_back(pointTimes[node.point]);
    }
    std::vector<PhasePoint> points;
    for (const Placement& placement : squash(times)) {
        const PhaseEnvelopeNode& node = nodes[placement.index];
        points.push_back(PhasePoint{.time = placement.time,
            .phase = node.degrees.has_value()
                         ? std::optional<Phase>{toPhase(Degrees{*node.degrees})}
                         : std::nullopt});
    }
    return points;
}

void fitToLifeline(std::vector<PhasePoint>& phase, SampleTime end) {
    PhasePoint last = phase.back();
    last.time = std::min(last.time, end);
    phase.pop_back();
    std::erase_if(phase, [end](const PhasePoint& point) { return point.time >= end; });
    phase.push_back(last);
}

void collapseFreeRuns(std::vector<PhasePoint>& phase) {
    std::vector<PhasePoint> kept;
    kept.reserve(phase.size());
    for (const PhasePoint& point : phase) {
        // The last two kept are both NULL: the later one is the middle of a
        // run, superseded by this point as the run's end.
        const std::size_t n = kept.size();
        if (!point.phase.has_value() && n >= 2 && !kept[n - 1].phase.has_value() &&
            !kept[n - 2].phase.has_value()) {
            kept.back() = point;
        } else {
            kept.push_back(point);
        }
    }
    phase = std::move(kept);
}

void requireCompletePosition(std::span<const SpatialState> states) {
    if (states.empty() || states.front().point != 0) {
        throw SoundSourceTransformError{
            "point 0 has no SpatialPositionState; the first point must define azimuth, "
            "elevation and distance"};
    }
    const SpatialState& first = states.front();
    std::string missing;
    const auto note = [&missing](const std::optional<double>& value, std::string_view name) {
        if (!value.has_value()) {
            missing += missing.empty() ? "" : ", ";
            missing += name;
        }
    };
    note(first.azimuth, "azimuth");
    note(first.elevation, "elevation");
    note(first.distance, "distance");
    if (!missing.empty()) {
        throw SoundSourceTransformError{"at point 0 its " + missing +
                                        " is NULL; the first point must define azimuth, "
                                        "elevation and distance"};
    }
}

void requireSpatialValues(std::span<const SpatialState> states) {
    const auto check = [](const SpatialState& state, const std::optional<double>& value,
                           std::string_view name, std::string_view unit, double low, double high) {
        if (value.has_value() && (!std::isfinite(*value) || *value < low || *value > high)) {
            throw SoundSourceTransformError{"at point " + std::to_string(state.point) + " its " +
                                            std::string{name} + " is " + formatNumber(*value) +
                                            " " + std::string{unit} + "; it must be in [" +
                                            formatNumber(low) + ", " + formatNumber(high) + "]"};
        }
    };
    for (const SpatialState& state : states) {
        check(state, state.azimuth, "azimuth", "degrees", kMinAzimuthDegrees, kMaxAzimuthDegrees);
        check(state, state.elevation, "elevation", "degrees", kMinElevationDegrees,
            kMaxElevationDegrees);
        check(state, state.distance, "distance", "metres", kMinDistanceMetres, kMaxDistanceMetres);
    }
}

SpatialTriple<EnvelopeNode> spatialNodesOf(
    std::span<const SpatialState> states, std::span<const SpatialCurves> curves) {
    SpatialTriple<EnvelopeNode> envelopes;
    for (const SpatialState& state : states) {
        const SpatialCurves& curve = curves[state.point];
        const std::array<std::optional<double>, 3> values{
            state.azimuth, state.elevation, state.distance};
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const std::optional<double> value = values.at(k);
            if (value.has_value()) {
                envelopes.at(k).push_back(EnvelopeNode{.point = state.point,
                    .value = *value,
                    .curveType = curve.curveType.at(k),
                    .curveAmount = curve.curveAmount.at(k)});
            }
        }
    }
    return envelopes;
}

SpatialTriple<PhysicalPoint> placeSpatialNodes(
    const SpatialTriple<EnvelopeNode>& envelopes, std::span<const SampleTime> pointTimes) {
    SpatialTriple<PhysicalPoint> points;
    for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
        points.at(k) = placeNodes(envelopes.at(k), pointTimes);
    }
    for (PhysicalPoint& point : points.at(0)) {
        point.value = toAzimuth(AzimuthDegrees{point.value}).value();
    }
    for (PhysicalPoint& point : points.at(1)) {
        point.value = toElevation(ElevationDegrees{point.value}).value();
    }
    return points;
}

// ---------------------------------------------------------------------------
// Reports and the run
// ---------------------------------------------------------------------------

std::size_t PartialSetTransformOutcome::failureCount() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(partials, [](const PartialTransformOutcome& p) { return !p.ok(); }));
}

std::size_t TransformReport::partialCount() const noexcept {
    std::size_t total = 0;
    for (const PartialSetTransformOutcome& set : sets) {
        total += set.partials.size();
    }
    return total;
}

std::size_t TransformReport::failureCount() const noexcept {
    std::size_t total = 0;
    for (const PartialSetTransformOutcome& set : sets) {
        total += set.error.has_value() ? 1 : set.failureCount();
    }
    total += static_cast<std::size_t>(std::ranges::count_if(
        sources, [](const SoundSourceTransformOutcome& s) { return !s.ok(); }));
    return total;
}

void TransformStoreDeleter::operator()(TransformStore* store) const noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the deleter of TransformStorePtr.
    delete store;
}

TransformReport transformAll(TransformStore& store) {
    const std::vector<EngineConfigRecord> config = store.readEngineConfigs();
    if (config.size() != 1) {
        throw TransformError{
            "expected exactly one EngineConfig, found " + std::to_string(config.size())};
    }
    const std::optional<std::int64_t> sampleRate = config.front().sampleRate;
    if (!sampleRate.has_value() || *sampleRate <= 0 ||
        *sampleRate > std::numeric_limits<std::int32_t>::max()) {
        throw TransformError{
            "EngineConfig.sampleRate is " +
            (sampleRate.has_value() ? std::to_string(*sampleRate) : std::string{"NULL"}) +
            "; it must be positive"};
    }
    TransformReport report;
    report.sampleRate = SampleRate{static_cast<std::int32_t>(*sampleRate)};

    const TransformInputs inputs = store.readInputs();
    const ResolvedMaps maps = buildTempoMaps(inputs, report);

    // The units not yet written, each with its place in the report: a
    // partial as (set, partial) indexes, a source as its index. A unit the
    // store fails to write takes back the counts the stage recorded.
    std::vector<TransformedPartial> pendingPartials;
    std::vector<std::pair<std::size_t, std::size_t>> stagedPartials;
    std::vector<TransformedSource> pendingSources;
    std::vector<std::size_t> stagedSources;
    const auto flushPartials = [&] {
        if (pendingPartials.empty()) {
            return;
        }
        const std::vector<std::optional<std::string>> results =
            store.writePartials(pendingPartials);
        requireOneResultPerUnit(results.size(), pendingPartials.size());
        for (std::size_t unit = 0; unit < results.size(); ++unit) {
            if (results[unit].has_value()) {
                const auto [set, index] = stagedPartials.at(unit);
                PartialTransformOutcome& partial = report.sets.at(set).partials.at(index);
                partial.error = "engine-db: " + *results[unit];
                partial.frequencyPoints = 0;
                partial.amplitudePoints = 0;
                partial.phasePoints = 0;
            }
        }
        pendingPartials.clear();
        stagedPartials.clear();
    };
    const auto flushSources = [&] {
        if (pendingSources.empty()) {
            return;
        }
        const std::vector<std::optional<std::string>> results = store.writeSources(pendingSources);
        requireOneResultPerUnit(results.size(), pendingSources.size());
        for (std::size_t unit = 0; unit < results.size(); ++unit) {
            if (results[unit].has_value()) {
                SoundSourceTransformOutcome& source = report.sources.at(stagedSources.at(unit));
                source.error = "engine-db: " + *results[unit];
                source.points = {};
            }
        }
        pendingSources.clear();
        stagedSources.clear();
    };

    // Sets are independent; id order only makes the report deterministic.
    for (const MusicalSetRecord& set : inputs.sets) {
        report.sets.push_back(PartialSetTransformOutcome{
            .setId = set.setId, .setName = set.name, .error = std::nullopt, .partials = {}});
        const std::size_t setIndex = report.sets.size() - 1;
        transformSet(inputs, set, maps, report.sampleRate, report.sets.back(),
            [&](std::size_t partialIndex, TransformedPartial&& output) {
                pendingPartials.push_back(std::move(output));
                stagedPartials.emplace_back(setIndex, partialIndex);
                if (stagedPartials.size() == kOwnersPerFlush) {
                    flushPartials();
                }
            });
    }
    flushPartials();
    // Sources are independent of sets and of each other (§6.8).
    for (const SpatialSourceRecord& source : inputs.sources) {
        report.sources.push_back(SoundSourceTransformOutcome{.sourceId = source.sourceId,
            .sourceName = source.name,
            .error = std::nullopt,
            .points = {}});
        std::optional<TransformedSource> output =
            transformSource(inputs, source, maps, report.sampleRate, report.sources.back());
        if (output.has_value()) {
            pendingSources.push_back(std::move(*output));
            stagedSources.push_back(report.sources.size() - 1);
            if (stagedSources.size() == kOwnersPerFlush) {
                flushSources();
            }
        }
    }
    flushSources();
    return report;
}

} // namespace rainbohz::rendering_engine::transform
