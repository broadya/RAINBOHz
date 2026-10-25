// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/native_transform_store.hpp"

#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/transform/transform_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief The native backend's TransformStore: the transform's bulk read
/// from a NativeModel, and its units written into it.
///
/// Each read gives what the Ladybug backend's query of the same name gives
/// (ladybug_transform_store.cpp): the same rows, and, where the query has an
/// ORDER BY, the same order. Where it has none, rows come in id order. Each
/// unit is checked whole before anything of it is changed, so that a unit
/// is written whole or not at all, as a Ladybug transaction is.
///
/// @see transform_store.hpp, native_model.hpp, ADR 0028

namespace rainbohz::rendering_engine::transform {

namespace {

using native::CanonicalTables;
using native::CurvePoint;
using native::Edge;
using native::EdgeList;
using native::NativeModel;
using native::NoProperties;
using native::PhysicalTimelines;

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// Widens a FLOAT or INT8 the schema holds to what a query returns: a double
/// or an int64.
///
/// @tparam To    double or std::int64_t.
/// @tparam From  The field's type.
/// @param value  The field.
/// @return The widened value; nullopt for NULL.
template <typename To, typename From>
std::optional<To> widened(const std::optional<From>& value) {
    return value.has_value() ? std::optional<To>{static_cast<To>(*value)} : std::nullopt;
}

/// Each FROM node's one edge of a pair whose FROM side is ONE (ONE_ONE or
/// MANY_ONE), by FROM id.
///
/// @tparam Properties  The relationship's properties.
/// @param edges  The pair.
/// @return The edges, by FROM id.
template <typename Properties>
std::unordered_map<std::int64_t, const Edge<Properties>*> byFrom(
    const EdgeList<Properties>& edges) {
    std::unordered_map<std::int64_t, const Edge<Properties>*> result;
    for (const Edge<Properties>& edge : edges.edges()) {
        result.emplace(edge.from, &edge);
    }
    return result;
}

/// Each TO node's one parent along a pair whose TO side is ONE (ONE_MANY or
/// ONE_ONE), by TO id.
///
/// @param edges  The pair.
/// @return The FROM id of each TO node's edge.
std::unordered_map<std::int64_t, std::int64_t> parentOf(const EdgeList<>& edges) {
    std::unordered_map<std::int64_t, std::int64_t> result;
    for (const Edge<NoProperties>& edge : edges.edges()) {
        result.emplace(edge.to, edge.from);
    }
    return result;
}

/// An edge list as an id-to-id link table, as readLinks() reads one.
///
/// @param edges  A NEXT_POINT pair.
/// @return Each FROM id's TO id.
IdMap linksOf(const EdgeList<>& edges) {
    IdMap links;
    for (const Edge<NoProperties>& edge : edges.edges()) {
        links.emplace(edge.from, edge.to);
    }
    return links;
}

/// The live timelines of one kind, as the existing-timeline queries read
/// them: for each owner, its timeline's id and the head of its point list.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @return Each owner's timelines, by owner id.
template <typename Point>
std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> existingOf(
    const PhysicalTimelines<Point>& timelines) {
    std::unordered_map<std::int64_t, std::vector<ExistingTimeline>> result;
    const auto all = timelines.timelines();
    for (std::size_t id = 0; id < all.size(); ++id) {
        if (!all[id].alive) {
            continue;
        }
        result[all[id].owner].push_back(
            ExistingTimeline{.timelineId = static_cast<std::int64_t>(id),
                .head = all[id].pointCount == 0
                            ? std::nullopt
                            : std::optional{static_cast<std::int64_t>(all[id].firstPoint)}});
    }
    return result;
}

/// The NEXT_POINT links between the live points of one kind: each point to
/// the next in its timeline's range.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @return Each point id's successor's id.
template <typename Point>
IdMap physicalLinksOf(const PhysicalTimelines<Point>& timelines) {
    IdMap links;
    for (const auto& timeline : timelines.timelines()) {
        if (!timeline.alive) {
            continue;
        }
        for (std::size_t i = 1; i < timeline.pointCount; ++i) {
            links.emplace(static_cast<std::int64_t>(timeline.firstPoint + i - 1),
                static_cast<std::int64_t>(timeline.firstPoint + i));
        }
    }
    return links;
}

/// A musical timeline point's POINT_AT, as read.
///
/// @param at     The point's POINT_AT edges, by point id.
/// @param point  The point's id.
/// @return Its POINT_AT; all nullopt when it has none.
PointAtRecord pointAtOf(
    const std::unordered_map<std::int64_t, const Edge<native::PointAtProperties>*>& at,
    std::int64_t point) {
    const auto found = at.find(point);
    if (found == at.end()) {
        return PointAtRecord{
            .tempoMapId = std::nullopt, .syncPoint = std::nullopt, .offsetSeconds = std::nullopt};
    }
    return PointAtRecord{.tempoMapId = found->second->to,
        .syncPoint = found->second->properties.syncPoint,
        .offsetSeconds = widened<double>(found->second->properties.offsetSeconds)};
}

/// The Score each SoundSource belongs to, through its SoundEntity: what
/// `MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(ss)`
/// reaches.
///
/// @param c  The canonical tables.
/// @return Each reached SoundSource's Score, by SoundSource id.
std::unordered_map<std::int64_t, std::int64_t> scoreOfSourceOf(const CanonicalTables& c) {
    const std::unordered_map<std::int64_t, std::int64_t> scoreOfEntity = parentOf(c.hasSoundEntity);
    std::unordered_map<std::int64_t, std::int64_t> result;
    for (const auto& edge : c.hasSoundSource.edges()) {
        if (const auto score = scoreOfEntity.find(edge.from); score != scoreOfEntity.end()) {
            result.emplace(edge.to, score->second);
        }
    }
    return result;
}

/// A sort key that puts NULL after every value, as `x IS NULL, x` orders
/// in Cypher.
///
/// @param value  The value.
/// @return Whether it is NULL, then the value.
std::pair<bool, std::int64_t> nullsLast(const std::optional<std::int64_t>& value) {
    return {!value.has_value(), value.value_or(0)};
}

/// Reads the Scores and the tempo maps: their points, links and
/// synchronisation edges.
///
/// @param c                The canonical tables.
/// @param[in,out] inputs  Receives the tempo-map members.
void readTempoMapInputs(const CanonicalTables& c, TransformInputs& inputs) {
    // Scores, in id order, each with its root TempoMap.
    const auto rootOf = byFrom(c.scoreTempoMap);
    for (std::size_t s = 0; s < c.scores.size(); ++s) {
        const auto id = static_cast<std::int64_t>(s);
        const auto root = rootOf.find(id);
        inputs.scores.push_back(ScoreRecord{.scoreId = id,
            .tempoMapId = root == rootOf.end() ? std::nullopt : std::optional{root->second->to},
            .tempoRampPhysicalTime = c.scores[s].tempoRampPhysicalTime});
    }

    // Tempo maps, their heads, points, links and synchronisation edges.
    const auto headOf = byFrom(c.tempoMapHead);
    for (std::size_t m = 0; m < c.tempoMaps.size(); ++m) {
        const auto id = static_cast<std::int64_t>(m);
        inputs.tempoMaps.push_back(TempoMapRecord{.id = id,
            .name = c.tempoMaps[m].name,
            .ticksPerBeat = widened<std::int64_t>(c.tempoMaps[m].ticksPerBeat)});
        const auto head = headOf.find(id);
        inputs.tempoMapHeads[id].push_back(
            head == headOf.end() ? std::nullopt : std::optional{head->second->to});
    }
    for (std::size_t p = 0; p < c.tempoMapPoints.size(); ++p) {
        const auto id = static_cast<std::int64_t>(p);
        inputs.tempoMapPoints.emplace(id, TempoMapPointRecord{.id = id,
                                              .position = c.tempoMapPoints[p].position,
                                              .startTempo = c.tempoMapPoints[p].startTempo,
                                              .hold = c.tempoMapPoints[p].hold});
    }
    inputs.tempoMapNext = linksOf(c.tempoMapNext);
    for (const auto& edge : c.tempoMapSync.edges()) {
        inputs.syncEdges.push_back(SyncEdgeRecord{.childId = edge.from,
            .parentId = edge.to,
            .syncPointChild = edge.properties.syncPointChild,
            .syncPointParent = edge.properties.syncPointParent});
    }
    // ORDER BY c.id, p.id, then each sync point with a NULL after every value,
    // as kReadSyncEdges states it.
    std::ranges::stable_sort(
        inputs.syncEdges, [](const SyncEdgeRecord& a, const SyncEdgeRecord& b) {
            return std::tuple{a.childId, a.parentId, nullsLast(a.syncPointParent),
                       nullsLast(a.syncPointChild)} < std::tuple{b.childId, b.parentId,
                                                          nullsLast(b.syncPointParent),
                                                          nullsLast(b.syncPointChild)};
        });
}

/// Reads the musical sets: their timelines, Scores, members, points, POINT_AT
/// edges and states.
///
/// @param c                The canonical tables.
/// @param scoreOfSource    Each SoundSource's Score.
/// @param[in,out] inputs  Receives the set members.
void readSetInputs(const CanonicalTables& c,
    const std::unordered_map<std::int64_t, std::int64_t>& scoreOfSource, TransformInputs& inputs) {
    // Musical sets: those with a timeline, in set id order.
    const auto setHeadOf = byFrom(c.setTimelineHead);
    std::vector<std::pair<std::int64_t, std::int64_t>> setTimelines;
    for (const auto& edge : c.setTimeline.edges()) {
        setTimelines.emplace_back(edge.from, edge.to);
    }
    std::ranges::sort(setTimelines);
    for (const auto& [set, timeline] : setTimelines) {
        if (inputs.sets.empty() || inputs.sets.back().setId != set) {
            inputs.sets.push_back(MusicalSetRecord{.setId = set,
                .name = c.partialSets.at(static_cast<std::size_t>(set)).name,
                .timelines = {}});
        }
        const auto head = setHeadOf.find(timeline);
        inputs.sets.back().timelines.emplace_back(
            timeline, head == setHeadOf.end() ? std::nullopt : std::optional{head->second->to});
    }

    // Each set's Score, and its members in partial id order.
    for (const auto& edge : c.hasPartialSet.edges()) {
        if (const auto score = scoreOfSource.find(edge.from); score != scoreOfSource.end()) {
            inputs.scoreOfSet.emplace(edge.to, score->second);
        }
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> members;
    for (const auto& edge : c.hasPartial.edges()) {
        members.emplace_back(edge.to, edge.from);
    }
    std::ranges::sort(members);
    for (const auto& [partial, set] : members) {
        inputs.membersOfSet[set].emplace_back(
            partial, c.partials.at(static_cast<std::size_t>(partial)).name);
    }

    // The sets' timeline points, links and states.
    const auto setAt = byFrom(c.setPointAt);
    for (std::size_t p = 0; p < c.partialSetMusicalTimelinePoints.size(); ++p) {
        const auto id = static_cast<std::int64_t>(p);
        const auto& row = c.partialSetMusicalTimelinePoints[p];
        inputs.musicalPoints.emplace(
            id, MusicalPointRecord{.id = id,
                    .frequencyCurveType = widened<std::int64_t>(row.frequencyCurveType),
                    .frequencyCurveAmount = widened<double>(row.frequencyCurveAmount),
                    .amplitudeCurveType = widened<std::int64_t>(row.amplitudeCurveType),
                    .amplitudeCurveAmount = widened<double>(row.amplitudeCurveAmount),
                    .at = pointAtOf(setAt, id)});
    }
    inputs.musicalNext = linksOf(c.setTimelineNext);
    {
        // point -[:POINT_STATE]-> set <-[:MEMBER_OF_SET_MANY_ONE]- state -[:STATE_OF]-> partial
        std::unordered_map<std::int64_t, std::vector<std::int64_t>> pointsOfSet;
        for (const auto& edge : c.setPointState.edges()) {
            pointsOfSet[edge.to].push_back(edge.from);
        }
        const auto partialOf = byFrom(c.stateOf);
        for (const auto& member : c.stateMemberOf.edges()) {
            const auto points = pointsOfSet.find(member.to);
            const auto partial = partialOf.find(member.from);
            if (points == pointsOfSet.end() || partial == partialOf.end()) {
                continue;
            }
            const auto& state = c.partialStates.at(static_cast<std::size_t>(member.from));
            for (const std::int64_t point : points->second) {
                inputs.statesOfPoint[point].push_back(StateRecord{.stateId = member.from,
                    .partialId = partial->second->to,
                    .frequency = widened<double>(state.frequency),
                    .amplitude = widened<double>(state.amplitude),
                    .phase = widened<double>(state.phase),
                    .phaseFreeRun = state.phaseFreeRun});
            }
        }
        // No ORDER BY: rows come in state id order.
        for (auto& [point, states] : inputs.statesOfPoint) {
            std::ranges::stable_sort(states, {}, &StateRecord::stateId);
        }
    }
}

/// Reads the spatial musical timelines of the SoundSources, and their
/// existing physical timelines (§6.8).
///
/// @param model            The model.
/// @param[in,out] inputs  Receives the spatial members; scoreOfSource must be
///                        set already.
void readSpatialInputs(const NativeModel& model, TransformInputs& inputs) {
    const CanonicalTables& c = model.canonical;
    // Spatial musical timelines: sources with one, in source id order.
    const auto sourceHeadOf = byFrom(c.sourceTimelineHead);
    std::vector<std::pair<std::int64_t, std::int64_t>> sourceTimelines;
    for (const auto& edge : c.sourceTimeline.edges()) {
        sourceTimelines.emplace_back(edge.from, edge.to);
    }
    std::ranges::sort(sourceTimelines);
    for (const auto& [source, timeline] : sourceTimelines) {
        if (inputs.sources.empty() || inputs.sources.back().sourceId != source) {
            inputs.sources.push_back(SpatialSourceRecord{.sourceId = source,
                .name = c.soundSources.at(static_cast<std::size_t>(source)).name,
                .timelines = {}});
        }
        const auto head = sourceHeadOf.find(timeline);
        inputs.sources.back().timelines.emplace_back(
            timeline, head == sourceHeadOf.end() ? std::nullopt : std::optional{head->second->to});
    }
    const auto sourceAt = byFrom(c.sourcePointAt);
    for (std::size_t p = 0; p < c.spatialPositionMusicalTimelinePoints.size(); ++p) {
        const auto id = static_cast<std::int64_t>(p);
        const auto& row = c.spatialPositionMusicalTimelinePoints[p];
        inputs.spatialPoints.emplace(
            id, SpatialMusicalPointRecord{.id = id,
                    .curveType = {widened<std::int64_t>(row.azimuthCurveType),
                        widened<std::int64_t>(row.elevationCurveType),
                        widened<std::int64_t>(row.distanceCurveType)},
                    .curveAmount = {widened<double>(row.azimuthCurveAmount),
                        widened<double>(row.elevationCurveAmount),
                        widened<double>(row.distanceCurveAmount)},
                    .at = pointAtOf(sourceAt, id)});
    }
    inputs.spatialMusicalNext = linksOf(c.sourceTimelineNext);
    for (const auto& edge : c.sourcePointState.edges()) {
        const auto& state = c.spatialPositionStates.at(static_cast<std::size_t>(edge.to));
        inputs.statesOfSpatialPoint[edge.from].push_back(SpatialStateRecord{.stateId = edge.to,
            .azimuth = state.azimuth,
            .elevation = state.elevation,
            .distance = widened<double>(state.distance)});
    }
    inputs.existingSpatial = {existingOf(model.physical.azimuth),
        existingOf(model.physical.elevation), existingOf(model.physical.distance)};
    inputs.spatialPhysicalNext = {physicalLinksOf(model.physical.azimuth),
        physicalLinksOf(model.physical.elevation), physicalLinksOf(model.physical.distance)};
}

/// Reads every input the transform needs, as the Ladybug backend's
/// readInputs() reads them.
///
/// @param model  The model. Not modified.
/// @return The inputs.
TransformInputs readNativeInputs(const NativeModel& model) {
    const CanonicalTables& c = model.canonical;
    TransformInputs inputs;
    readTempoMapInputs(c, inputs);
    inputs.scoreOfSource = scoreOfSourceOf(c);
    readSetInputs(c, inputs.scoreOfSource, inputs);
    // Existing physical timelines of partials, and their links.
    inputs.existingFrequency = existingOf(model.physical.frequency);
    inputs.frequencyNext = physicalLinksOf(model.physical.frequency);
    inputs.existingAmplitude = existingOf(model.physical.amplitude);
    inputs.amplitudeNext = physicalLinksOf(model.physical.amplitude);
    inputs.existingPhase = existingOf(model.physical.phase);
    inputs.phaseNext = physicalLinksOf(model.physical.phase);
    readSpatialInputs(model, inputs);
    return inputs;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// Why a unit cannot be written; thrown while a unit is checked, before
/// anything of it is changed, and reported as that unit's result.
class UnitRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Converts a written value to a FLOAT column, as `CAST(... AS FLOAT)` does.
///
/// @param value  The value.
/// @param what   The column, for the message.
/// @return The value as a float.
/// @throws UnitRefused  If it is finite and beyond FLOAT's range.
float asFloat(double value, std::string_view what) {
    if (std::isfinite(value) &&
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw UnitRefused{std::string{what} + " " + std::to_string(value) +
                          " is beyond the range of the schema's FLOAT"};
    }
    return static_cast<float>(value);
}

/// Converts a written curve type to an INT8 column, as `CAST(... AS INT8)`
/// does.
///
/// @param value  The curve type.
/// @return It as an INT8.
/// @throws UnitRefused  If it does not fit.
std::int8_t asInt8(std::int64_t value) {
    if (value < std::numeric_limits<std::int8_t>::min() ||
        value > std::numeric_limits<std::int8_t>::max()) {
        throw UnitRefused{
            "curve type " + std::to_string(value) + " does not fit the schema's INT8"};
    }
    return static_cast<std::int8_t>(value);
}

/// Converts written points to the model's points of a curve timeline.
///
/// @tparam Value  The value column's type.
/// @param points  The points as the transform writes them.
/// @param what    The value column, for a message.
/// @return The model's points.
/// @throws UnitRefused  If a value or curve does not fit its column.
template <typename Value>
std::vector<CurvePoint<Value>> curvePoints(
    std::span<const PhysicalPoint> points, std::string_view what) {
    std::vector<CurvePoint<Value>> result;
    result.reserve(points.size());
    for (const PhysicalPoint& point : points) {
        Value value{};
        if constexpr (std::is_same_v<Value, float>) {
            value = asFloat(point.value, what);
        } else {
            value = point.value;
        }
        result.push_back(CurvePoint<Value>{.time = point.time.value(),
            .value = value,
            .curveType = asInt8(point.curveType),
            .curveAmount = asFloat(point.curveAmount, "curve amount")});
    }
    return result;
}

/// Checks that, once a unit's stale timelines of one kind are removed, the
/// owner has none left: HAS_PHYSICAL_TIMELINE is ONE_ONE.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @param owner      The owner.
/// @param stale      The timelines the unit removes.
/// @param what       The kind, for the message.
/// @throws UnitRefused  If the owner keeps a live timeline.
template <typename Point>
void requireReplaceable(const PhysicalTimelines<Point>& timelines, std::int64_t owner,
    const StaleTimelines& stale, std::string_view what) {
    const std::optional<std::int64_t> live = timelines.timelineOf(owner);
    if (live.has_value() && std::ranges::none_of(stale,
                                [&](const auto& timeline) { return timeline.first == *live; })) {
        throw UnitRefused{"node " + std::to_string(owner) + " already has a " + std::string{what} +
                          " that the unit does not replace"};
    }
}

/// Removes a unit's stale timelines of one kind.
///
/// @tparam Point  The kind's point type.
/// @param[in,out] timelines  The kind's timelines; the stale ones are removed.
/// @param stale              The timelines to remove.
template <typename Point>
void removeStale(PhysicalTimelines<Point>& timelines, const StaleTimelines& stale) noexcept {
    for (const auto& [timeline, points] : stale) {
        timelines.remove(timeline);
    }
}

/// The transform's reads and writes over a native model.
class NativeTransformStore final : public TransformStore {
public:
    /// Serves the transform from `model`.
    ///
    /// @param[in,out] model  The model. It must outlive the store.
    explicit NativeTransformStore(NativeModel& model) : model_{&model} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return native::engineConfigsOf(*model_);
    }

    TransformInputs readInputs() override { return readNativeInputs(*model_); }

    std::vector<std::optional<std::string>> writePartials(
        std::span<const TransformedPartial> partials) override {
        std::vector<std::optional<std::string>> results;
        results.reserve(partials.size());
        for (const TransformedPartial& partial : partials) {
            results.push_back(writePartial(partial));
        }
        return results;
    }

    std::vector<std::optional<std::string>> writeSources(
        std::span<const TransformedSource> sources) override {
        std::vector<std::optional<std::string>> results;
        results.reserve(sources.size());
        for (const TransformedSource& source : sources) {
            results.push_back(writeSource(source));
        }
        return results;
    }

private:
    /// Writes one partial's unit, or refuses it whole.
    ///
    /// @param unit  The unit.
    /// @return nullopt when it was written; why not otherwise.
    std::optional<std::string> writePartial(const TransformedPartial& unit) {
        native::PhysicalTables& p = model_->physical;
        try {
            if (unit.partialId < 0 ||
                static_cast<std::size_t>(unit.partialId) >= model_->canonical.partials.size()) {
                throw UnitRefused{"there is no Partial " + std::to_string(unit.partialId)};
            }
            requireReplaceable(
                p.frequency, unit.partialId, unit.staleFrequency, "FrequencyPhysicalTimeline");
            requireReplaceable(
                p.amplitude, unit.partialId, unit.staleAmplitude, "AmplitudePhysicalTimeline");
            requireReplaceable(p.phase, unit.partialId, unit.stalePhase, "PhasePhysicalTimeline");
            const std::vector<CurvePoint<double>> frequency =
                curvePoints<double>(unit.frequency, "frequency");
            const std::vector<CurvePoint<float>> amplitude =
                curvePoints<float>(unit.amplitude, "amplitude");
            std::vector<native::PhasePoint> phase;
            phase.reserve(unit.phase.size());
            for (const PhasePoint& point : unit.phase) {
                phase.push_back(native::PhasePoint{.time = point.time.value(),
                    .phase = point.phase.has_value() ? std::optional{point.phase->value()}
                                                     : std::nullopt});
            }

            // Checked; nothing below refuses the unit.
            removeStale(p.frequency, unit.staleFrequency);
            removeStale(p.amplitude, unit.staleAmplitude);
            removeStale(p.phase, unit.stalePhase);
            p.frequency.append(unit.partialId, frequency);
            p.amplitude.append(unit.partialId, amplitude);
            p.phase.append(unit.partialId, phase);
            return std::nullopt;
        } catch (const UnitRefused& e) {
            return e.what();
        }
    }

    /// Writes one source's unit, or refuses it whole.
    ///
    /// @param unit  The unit.
    /// @return nullopt when it was written; why not otherwise.
    std::optional<std::string> writeSource(const TransformedSource& unit) {
        native::PhysicalTables& p = model_->physical;
        try {
            if (unit.sourceId < 0 ||
                static_cast<std::size_t>(unit.sourceId) >= model_->canonical.soundSources.size()) {
                throw UnitRefused{"there is no SoundSource " + std::to_string(unit.sourceId)};
            }
            requireReplaceable(p.azimuth, unit.sourceId, unit.stale[0], "AzimuthPhysicalTimeline");
            requireReplaceable(
                p.elevation, unit.sourceId, unit.stale[1], "ElevationPhysicalTimeline");
            requireReplaceable(
                p.distance, unit.sourceId, unit.stale[2], "DistancePhysicalTimeline");
            const std::vector<CurvePoint<double>> azimuth =
                curvePoints<double>(unit.points[0], "azimuth");
            const std::vector<CurvePoint<double>> elevation =
                curvePoints<double>(unit.points[1], "elevation");
            const std::vector<CurvePoint<float>> distance =
                curvePoints<float>(unit.points[2], "distance");

            // Checked; nothing below refuses the unit.
            removeStale(p.azimuth, unit.stale[0]);
            removeStale(p.elevation, unit.stale[1]);
            removeStale(p.distance, unit.stale[2]);
            p.azimuth.append(unit.sourceId, azimuth);
            p.elevation.append(unit.sourceId, elevation);
            p.distance.append(unit.sourceId, distance);
            return std::nullopt;
        } catch (const UnitRefused& e) {
            return e.what();
        }
    }

    NativeModel* model_; ///< The model; not owned.
};

} // namespace

TransformStorePtr makeNativeTransformStore(native::NativeModel& model) {
    return TransformStorePtr{new NativeTransformStore{model}};
}

} // namespace rainbohz::rendering_engine::transform
