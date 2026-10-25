// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/native_store.hpp"
#include "rainbohz/rendering_engine/precomputation_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/// @file
/// @brief The native backend's PrecomputationStore: precomputation's bulk
/// read from a NativeModel, and its units written into it.
///
/// Each read gives what the Ladybug backend's query of the same name gives
/// (ladybug_precomputation_store.cpp); where that query has no ORDER BY,
/// rows come in id order. Each unit is checked whole before anything of it
/// is changed, so that it is written whole or not at all, as a Ladybug
/// transaction is. Section numbers refer to docs/precomputation.md.
///
/// @see precomputation_store.hpp, native_model.hpp, ADR 0028

namespace rainbohz::rendering_engine {

namespace {

using native::NativeModel;
using native::PhysicalTimelines;

/// Each owner's live timeline of one kind, as a heads query reads it: the
/// first point's id, or nullopt for an empty timeline.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @return Each owner's heads, by owner id.
template <typename Point>
std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> headsOf(
    const PhysicalTimelines<Point>& timelines) {
    std::unordered_map<std::int64_t, std::vector<std::optional<std::int64_t>>> heads;
    for (const auto& timeline : timelines.timelines()) {
        if (timeline.alive) {
            heads[timeline.owner].push_back(
                timeline.pointCount == 0
                    ? std::nullopt
                    : std::optional{static_cast<std::int64_t>(timeline.firstPoint)});
        }
    }
    return heads;
}

/// The NEXT_POINT links within each live range of a list.
///
/// @param first  Each live list's first index.
/// @param count  Each live list's length, in the same order.
/// @param[in,out] links  Gains each element's successor.
void linkRange(
    std::size_t first, std::size_t count, std::unordered_map<std::int64_t, std::int64_t>& links) {
    for (std::size_t i = 1; i < count; ++i) {
        links.emplace(
            static_cast<std::int64_t>(first + i - 1), static_cast<std::int64_t>(first + i));
    }
}

/// The NEXT_POINT links between the live points of one kind.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @return Each point id's successor's id.
template <typename Point>
std::unordered_map<std::int64_t, std::int64_t> linksOf(const PhysicalTimelines<Point>& timelines) {
    std::unordered_map<std::int64_t, std::int64_t> links;
    for (const auto& timeline : timelines.timelines()) {
        if (timeline.alive) {
            linkRange(timeline.firstPoint, timeline.pointCount, links);
        }
    }
    return links;
}

/// Calls `visit` with the id and row of every live point of one kind.
///
/// @tparam Point  The kind's point type.
/// @tparam Visit  Callable as `void(std::int64_t, const Point&)`.
/// @param timelines  The kind's timelines.
/// @param visit      Called once per live point, in id order.
template <typename Point, typename Visit>
void forEachLivePoint(const PhysicalTimelines<Point>& timelines, Visit visit) {
    const auto points = timelines.points();
    for (std::size_t id = 0; id < points.size(); ++id) {
        if (timelines.isLivePoint(static_cast<std::int64_t>(id))) {
            visit(static_cast<std::int64_t>(id), points[id]);
        }
    }
}

/// The spatial points of one kind, as precomputation reads them.
///
/// @tparam Value  The kind's value type.
/// @param timelines  The kind's timelines.
/// @return Every live point, by id.
template <typename Value>
std::unordered_map<std::int64_t, SpatialPointRecord> spatialPointsOf(
    const PhysicalTimelines<native::CurvePoint<Value>>& timelines) {
    std::unordered_map<std::int64_t, SpatialPointRecord> points;
    forEachLivePoint(timelines, [&](std::int64_t id, const native::CurvePoint<Value>& point) {
        points.emplace(id, SpatialPointRecord{.id = id,
                               .time = point.time,
                               .value = static_cast<double>(point.value),
                               .curveType = point.curveType});
    });
    return points;
}

/// Reads every input precomputation needs, as the Ladybug backend's
/// readInputs() reads them.
///
/// @param model  The model. Not modified.
/// @return The inputs.
PrecomputationInputs readNativeInputs(const NativeModel& model) {
    const native::CanonicalTables& c = model.canonical;
    const native::PhysicalTables& p = model.physical;
    PrecomputationInputs inputs;

    for (std::size_t i = 0; i < c.partials.size(); ++i) {
        inputs.partials.emplace_back(static_cast<std::int64_t>(i), c.partials[i].name);
    }

    inputs.frequencyHeads = headsOf(p.frequency);
    forEachLivePoint(p.frequency, [&](std::int64_t id, const native::CurvePoint<double>& point) {
        inputs.frequencyPoints.emplace(id, FrequencyPointRecord{.id = id,
                                               .time = point.time,
                                               .frequency = point.value,
                                               .curveType = point.curveType});
    });
    inputs.frequencyNext = linksOf(p.frequency);

    inputs.amplitudeHeads = headsOf(p.amplitude);
    forEachLivePoint(p.amplitude, [&](std::int64_t id, const native::CurvePoint<float>& point) {
        inputs.amplitudePoints.emplace(id, AmplitudePointRecord{.id = id, .time = point.time});
    });
    inputs.amplitudeNext = linksOf(p.amplitude);

    inputs.phaseHeads = headsOf(p.phase);
    forEachLivePoint(p.phase, [&](std::int64_t id, const native::PhasePoint& point) {
        inputs.phasePoints.emplace(
            id, PhasePointRecord{.id = id, .time = point.time, .phase = point.phase});
    });
    inputs.phaseNext = linksOf(p.phase);

    // ss -[:HAS_PARTIALSET]-> ps -[:HAS_PARTIAL]-> p
    std::unordered_map<std::int64_t, std::int64_t> sourceOfSet;
    for (const auto& edge : c.hasPartialSet.edges()) {
        sourceOfSet.emplace(edge.to, edge.from);
    }
    for (const auto& edge : c.hasPartial.edges()) {
        if (const auto source = sourceOfSet.find(edge.from); source != sourceOfSet.end()) {
            inputs.partialSources[edge.to].emplace_back(
                source->second, c.soundSources.at(static_cast<std::size_t>(source->second)).name);
        }
    }

    inputs.spatialHeads = {headsOf(p.azimuth), headsOf(p.elevation), headsOf(p.distance)};
    inputs.spatialPoints = {
        spatialPointsOf(p.azimuth), spatialPointsOf(p.elevation), spatialPointsOf(p.distance)};
    inputs.spatialNext = {linksOf(p.azimuth), linksOf(p.elevation), linksOf(p.distance)};

    // Output from an earlier run.
    const native::SegmentTimelines& s = model.segments;
    const auto timelines = s.timelines();
    for (std::size_t id = 0; id < timelines.size(); ++id) {
        const auto& timeline = timelines[id];
        if (!timeline.alive) {
            continue;
        }
        inputs.existing[timeline.partial].push_back(ExistingPhysicalTimeline{
            .physicalTimelineId = static_cast<std::int64_t>(id),
            .headSegment = timeline.segmentCount == 0
                               ? std::nullopt
                               : std::optional{static_cast<std::int64_t>(timeline.firstSegment)},
            .headCompensation =
                timeline.compensationCount == 0
                    ? std::nullopt
                    : std::optional{static_cast<std::int64_t>(timeline.firstCompensation)}});
        linkRange(timeline.firstSegment, timeline.segmentCount, inputs.segmentNext);
        linkRange(timeline.firstCompensation, timeline.compensationCount, inputs.compensationNext);
        for (std::size_t k = 0; k < timeline.segmentCount; ++k) {
            const std::size_t segment = timeline.firstSegment + k;
            inputs.segmentNominal.emplace(
                static_cast<std::int64_t>(segment), s.segments()[segment].nominal);
        }
    }
    const auto entries = model.frames.entries();
    for (std::size_t id = 0; id < entries.size(); ++id) {
        if (model.frames.isLiveEntry(static_cast<std::int64_t>(id))) {
            inputs.existingFrameEntries[entries[id].partial].push_back(
                static_cast<std::int64_t>(id));
        }
    }
    for (const auto& [partial, scope] : model.frames.scopes()) {
        inputs.existingFrameScope.insert(partial);
    }
    return inputs;
}

/// Why a unit cannot be written; thrown while a unit is checked, before
/// anything of it is changed, and reported as that unit's result.
class UnitRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Checks that a point a frame entry leads to is live.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @param id         The point's id.
/// @param what       The point's table, for the message.
/// @return The id.
/// @throws UnitRefused  If no live point has it.
template <typename Point>
std::int64_t requireLivePoint(
    const PhysicalTimelines<Point>& timelines, std::int64_t id, std::string_view what) {
    if (!timelines.isLivePoint(id)) {
        throw UnitRefused{"there is no " + std::string{what} + " " + std::to_string(id)};
    }
    return id;
}

/// Precomputation's reads and writes over a native model.
class NativePrecomputationStore final : public PrecomputationStore {
public:
    /// Serves precomputation from `model`.
    ///
    /// @param[in,out] model  The model. It must outlive the store.
    explicit NativePrecomputationStore(NativeModel& model) : model_{&model} {}

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return native::engineConfigsOf(*model_);
    }

    PrecomputationInputs readInputs() override { return readNativeInputs(*model_); }

    std::vector<std::optional<std::string>> writePartials(
        std::span<const PrecomputedPartial> partials) override {
        std::vector<std::optional<std::string>> results;
        results.reserve(partials.size());
        for (const PrecomputedPartial& partial : partials) {
            results.push_back(writePartial(partial));
        }
        return results;
    }

private:
    /// Writes one partial's unit (§6.2 stage 5, §9), or refuses it whole.
    ///
    /// @param unit  The unit.
    /// @return nullopt when it was written; why not otherwise.
    std::optional<std::string> writePartial(const PrecomputedPartial& unit) {
        native::SegmentTimelines& segments = model_->segments;
        native::FrameTables& frames = model_->frames;
        try {
            requireReplaceable(unit);
            const std::vector<native::SegmentTimelines::NewSegment> newSegments =
                newSegmentsOf(unit);
            const std::vector<native::CompensationRow> newCompensation = newCompensationOf(unit);
            const std::vector<native::FrameEntryRow> entries = entriesOf(unit);

            // Checked; nothing below refuses the unit.
            for (const std::int64_t entry : unit.staleFrameEntries) {
                frames.removeEntry(entry);
            }
            for (const StalePhysicalTimeline& stale : unit.stale) {
                segments.remove(stale.physicalTimelineId);
            }
            if (unit.staleFrameScope) {
                frames.clearScope(unit.partialId);
            }
            segments.append(unit.partialId, newSegments, newCompensation);
            // Counted in 64 bits, so that a last frame of INT32_MAX ends the loop.
            for (std::int64_t k = unit.frames.first.value(); k <= unit.frames.last.value(); ++k) {
                frames.mergeFrame(static_cast<std::int32_t>(k));
            }
            frames.setScope(unit.partialId, unit.frames.first.value(), unit.frames.last.value());
            for (const native::FrameEntryRow& entry : entries) {
                frames.appendEntry(entry);
            }
            return std::nullopt;
        } catch (const UnitRefused& e) {
            return e.what();
        }
    }

    /// Checks that a unit's partial exists and that, once its stale timeline
    /// is removed, it has none left: HAS_PHYSICAL_TIMELINE is ONE_ONE.
    ///
    /// @param unit  The unit.
    /// @throws UnitRefused  If either does not hold.
    void requireReplaceable(const PrecomputedPartial& unit) const {
        if (unit.partialId < 0 ||
            static_cast<std::size_t>(unit.partialId) >= model_->canonical.partials.size()) {
            throw UnitRefused{"there is no Partial " + std::to_string(unit.partialId)};
        }
        const std::optional<std::int64_t> live = model_->segments.timelineOf(unit.partialId);
        if (live.has_value() && std::ranges::none_of(unit.stale, [&](const auto& stale) {
                return stale.physicalTimelineId == *live;
            })) {
            throw UnitRefused{"Partial " + std::to_string(unit.partialId) +
                              " already has a PartialSegmentPhysicalTimeline that the unit does "
                              "not replace"};
        }
    }

    /// A unit's segments, each with its nominal frequency, as the model holds
    /// them.
    ///
    /// @param unit  The unit.
    /// @return The segments, in list order.
    static std::vector<native::SegmentTimelines::NewSegment> newSegmentsOf(
        const PrecomputedPartial& unit) {
        std::vector<native::SegmentTimelines::NewSegment> result;
        result.reserve(unit.segments.size());
        for (const Segment& segment : unit.segments) {
            const std::optional<FrequencyGradient>& gradient = segment.nominal.linearGradient;
            result.push_back(
                native::SegmentTimelines::NewSegment{.startTime = segment.startTime.value(),
                    .endTime = segment.endTime.value(),
                    .startPhase = segment.startPhase.value(),
                    .nominal = native::NominalFrequencyRow{
                        .startNominalFrequency = segment.nominal.startFrequency.value(),
                        .linearComponent = gradient.has_value(),
                        .linearGradient =
                            gradient.has_value() ? std::optional{gradient->value()} : std::nullopt,
                        .curveComponent = false}});
        }
        return result;
    }

    /// A unit's compensation segments, as the model holds them.
    ///
    /// @param unit  The unit.
    /// @return The segments, in list order.
    static std::vector<native::CompensationRow> newCompensationOf(const PrecomputedPartial& unit) {
        std::vector<native::CompensationRow> result;
        result.reserve(unit.compensation.size());
        for (const CompensationSegment& segment : unit.compensation) {
            result.push_back(native::CompensationRow{.startTime = segment.startTime.value(),
                .endTime = segment.endTime.value(),
                .startPhaseDelta = segment.startDelta.value(),
                .endPhaseDelta = segment.endDelta.value()});
        }
        return result;
    }

    /// A unit's frame entries, their indexes resolved to the ids of the rows
    /// they lead to (§6.7). The unit's own segments take the next ids of
    /// their tables when it is written.
    ///
    /// @param unit  The unit.
    /// @return The entries.
    /// @throws UnitRefused  If the frame scope is empty, an entry is for a
    ///         frame outside it, or an entry leads past the end of a list or
    ///         to a point that is not live.
    std::vector<native::FrameEntryRow> entriesOf(const PrecomputedPartial& unit) const {
        const native::PhysicalTables& physical = model_->physical;
        const auto firstSegment = static_cast<std::int64_t>(model_->segments.segments().size());
        const auto firstCompensation =
            static_cast<std::int64_t>(model_->segments.compensation().size());
        if (unit.frames.last < unit.frames.first) {
            throw UnitRefused{"its frame scope is empty"};
        }
        std::vector<native::FrameEntryRow> entries;
        entries.reserve(unit.entries.size());
        for (const FrameEntryTarget& entry : unit.entries) {
            // An entry's Frame must be one the unit creates (FRAME_ENTRY).
            if (entry.frame < unit.frames.first || entry.frame > unit.frames.last) {
                throw UnitRefused{"a FrameEntry is for frame " +
                                  std::to_string(entry.frame.value()) +
                                  ", outside the partial's frame scope"};
            }
            if (entry.segment >= unit.segments.size() ||
                entry.compensation >= unit.compensation.size() ||
                entry.point >= unit.amplitudePointIds.size()) {
                throw UnitRefused{"a FrameEntry leads past the end of its partial's lists"};
            }
            native::FrameEntryRow row{.frame = entry.frame.value(),
                .partial = unit.partialId,
                .segment = firstSegment + static_cast<std::int64_t>(entry.segment),
                .amplitudePoint = requireLivePoint(physical.amplitude,
                    unit.amplitudePointIds[entry.point], "AmplitudePhysicalTimelinePoint"),
                .compensation = firstCompensation + static_cast<std::int64_t>(entry.compensation),
                .spatialPoints = {}};
            for (std::size_t k = 0; k < 3; ++k) {
                if (entry.spatial.at(k) >= unit.spatialPointIds.at(k).size()) {
                    throw UnitRefused{"a FrameEntry leads past the end of its source's lists"};
                }
            }
            const auto spatialId = [&](std::size_t k) {
                return unit.spatialPointIds.at(k)[entry.spatial.at(k)];
            };
            row.spatialPoints[0] =
                requireLivePoint(physical.azimuth, spatialId(0), "AzimuthPhysicalTimelinePoint");
            row.spatialPoints[1] = requireLivePoint(
                physical.elevation, spatialId(1), "ElevationPhysicalTimelinePoint");
            row.spatialPoints[2] =
                requireLivePoint(physical.distance, spatialId(2), "DistancePhysicalTimelinePoint");
            entries.push_back(row);
        }
        return entries;
    }

    NativeModel* model_; ///< The model; not owned.
};

} // namespace

std::unique_ptr<PrecomputationStore> makeNativePrecomputationStore(native::NativeModel& model) {
    return std::make_unique<NativePrecomputationStore>(model);
}

} // namespace rainbohz::rendering_engine
