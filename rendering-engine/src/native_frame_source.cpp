// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/native_store.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/// @file
/// @brief The native backend's FrameSource: rendering's gather from a
/// NativeModel (docs/rendering.md §6.3).
///
/// It gives what the Ladybug backend's gather gives (ladybug_frame_source.cpp)
/// for the same data: the frame's entries ordered by partial id, then entry
/// id; each list walked forward from the node its entry leads to until a
/// node starts at or after the frame's end; and each SoundSource's spatial
/// timelines walked once. As there, `hasSuccessor` is set on a node only when
/// the walk steps past it, so the last node read keeps `false` whether or not
/// its list goes on. Where Ladybug follows a NEXT_POINT edge, this steps to
/// the next element of the list's range.
///
/// @see frame_gather.hpp, native_model.hpp, ADR 0028

namespace rainbohz::rendering_engine {

namespace {

using native::NativeModel;
using native::PhysicalTimelines;

/// The id of the next element of a list held as a range, if there is one.
///
/// @param id     An element's id.
/// @param first  Its list's first index.
/// @param count  Its list's length.
/// @return The next element's id; nullopt at the end of the list.
std::optional<std::int64_t> nextInRange(std::int64_t id, std::size_t first, std::size_t count) {
    const auto next = static_cast<std::size_t>(id) + 1;
    return next < first + count ? std::optional{static_cast<std::int64_t>(next)} : std::nullopt;
}

/// The id of the point after `id` in its physical timeline.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @param id         A live point's id.
/// @return The next point's id; nullopt at the end of the timeline.
template <typename Point>
std::optional<std::int64_t> nextPoint(const PhysicalTimelines<Point>& timelines, std::int64_t id) {
    const auto& timeline =
        timelines.timelines()[static_cast<std::size_t>(timelines.timelineOfPoint(id))];
    return nextInRange(id, timeline.firstPoint, timeline.pointCount);
}

/// Whether a point is not its timeline's first: whether it has a
/// predecessor point.
///
/// @tparam Point  The kind's point type.
/// @param timelines  The kind's timelines.
/// @param id         A live point's id.
/// @return True when another point leads to it.
template <typename Point>
bool hasPreviousPoint(const PhysicalTimelines<Point>& timelines, std::int64_t id) {
    return std::cmp_not_equal(id,
        timelines.timelines()[static_cast<std::size_t>(timelines.timelineOfPoint(id))].firstPoint);
}

/// Extends a list read from an entry, as Ladybug's frontier walk does: one
/// node at a time while the last node read starts before the frame's end
/// and has a successor.
///
/// @tparam Record   The list's record type.
/// @tparam StartOf  Callable as `std::optional<double>(const Record&)`.
/// @tparam Next     Callable as `std::optional<std::int64_t>(std::int64_t)`.
/// @tparam Read     Callable as `Record(std::int64_t)`.
/// @param[in,out] list  The list as read so far; gains the nodes walked, and
///                      `hasSuccessor` on each node walked past.
/// @param frameEnd  The frame's end, samples.
/// @param startOf   A record's start time.
/// @param next      A node's successor's id.
/// @param read      Reads a node.
template <typename Record, typename StartOf, typename Next, typename Read>
void walk(std::vector<Record>& list, SampleTime frameEnd, StartOf startOf, Next next, Read read) {
    while (!list.empty()) {
        const std::optional<double> start = startOf(list.back());
        if (!start.has_value() || *start >= frameEnd.value()) {
            return;
        }
        const std::optional<std::int64_t> successor = next(list.back().id);
        if (!successor.has_value()) {
            return;
        }
        list.back().hasSuccessor = true;
        list.push_back(read(*successor));
    }
}

/// Rendering's reads over a native model.
class NativeFrameSource final : public FrameSource {
public:
    /// Serves rendering from `model`, indexing which set and source each
    /// partial belongs to. The canonical tables do not change after a score
    /// is loaded, so the indexes hold for the source's life, and a frame's
    /// gather costs what its entries and walks cost, not the score's size.
    ///
    /// @param model  The model, its canonical tables loaded. It must outlive
    ///               the source.
    explicit NativeFrameSource(const NativeModel& model) : model_{&model} {
        for (const auto& edge : model.canonical.hasPartial.edges()) {
            setOfPartial_.emplace(edge.to, edge.from);
        }
        for (const auto& edge : model.canonical.hasPartialSet.edges()) {
            sourceOfSet_.emplace(edge.to, edge.from);
        }
    }

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return native::engineConfigsOf(*model_);
    }

    GatheredFrame gatherFrame(FrameNumber frame, FrameSize size) override {
        const SampleTime frameEnd =
            frameStart(frame, size) + SampleDuration{static_cast<double>(size.value())};
        const native::CanonicalTables& c = model_->canonical;
        const native::PhysicalTables& p = model_->physical;

        // ORDER BY p.id, e.id.
        std::vector<std::int64_t> ids = model_->frames.liveEntriesOf(frame.value());
        const auto rows = model_->frames.entries();
        std::ranges::sort(ids, [&](std::int64_t a, std::int64_t b) {
            return std::pair{rows[static_cast<std::size_t>(a)].partial, a} <
                   std::pair{rows[static_cast<std::size_t>(b)].partial, b};
        });

        std::vector<GatheredEntry> entries;
        entries.reserve(ids.size());
        for (const std::int64_t id : ids) {
            const native::FrameEntryRow& row = rows[static_cast<std::size_t>(id)];
            GatheredEntry entry{.entryId = id,
                .partialId = row.partial,
                .partialName = c.partials.at(static_cast<std::size_t>(row.partial)).name,
                .partialSetIds = {},
                .soundSourceIds = {},
                .soundSourceName = std::nullopt,
                .spatial = {},
                .segments = {segment(row.segment)},
                .entrySegmentHasPredecessor = hasPreviousSegment(row.segment),
                .points = {amplitudePoint(row.amplitudePoint)},
                .compensation = {compensation(row.compensation)},
                .failure = std::nullopt};
            if (const auto set = setOfPartial_.find(row.partial); set != setOfPartial_.end()) {
                entry.partialSetIds.push_back(set->second);
                if (const auto source = sourceOfSet_.find(set->second);
                    source != sourceOfSet_.end()) {
                    entry.soundSourceIds.push_back(source->second);
                    entry.soundSourceName =
                        c.soundSources.at(static_cast<std::size_t>(source->second)).name;
                }
            }
            entry.spatial[0] = {spatialPoint(p.azimuth, row.spatialPoints[0], true)};
            entry.spatial[1] = {spatialPoint(p.elevation, row.spatialPoints[1], true)};
            entry.spatial[2] = {spatialPoint(p.distance, row.spatialPoints[2], true)};
            entries.push_back(std::move(entry));
        }

        markMiscountedEntries(entries);
        for (GatheredEntry& entry : entries) {
            if (entry.failure.has_value()) {
                continue;
            }
            walk(
                entry.segments, frameEnd, [](const GatheredSegment& s) { return s.startTime; },
                [this](std::int64_t id) { return nextSegment(id); },
                [this](std::int64_t id) { return segment(id); });
            walk(
                entry.points, frameEnd, [](const GatheredPoint& a) { return a.time; },
                [&p](std::int64_t id) { return nextPoint(p.amplitude, id); },
                [this](std::int64_t id) { return amplitudePoint(id); });
            walk(
                entry.compensation, frameEnd,
                [](const GatheredCompensation& s) { return s.startTime; },
                [this](std::int64_t id) { return nextCompensation(id); },
                [this](std::int64_t id) { return compensation(id); });
        }

        std::vector<GatheredSource> sources = groupSources(entries);
        for (GatheredSource& source : sources) {
            if (source.failure.has_value()) {
                continue;
            }
            const auto startOf = [](const GatheredSpatialPoint& a) { return a.time; };
            walk(
                source.points[0], frameEnd, startOf,
                [&p](std::int64_t id) { return nextPoint(p.azimuth, id); },
                [&](std::int64_t id) { return spatialPoint(p.azimuth, id, false); });
            walk(
                source.points[1], frameEnd, startOf,
                [&p](std::int64_t id) { return nextPoint(p.elevation, id); },
                [&](std::int64_t id) { return spatialPoint(p.elevation, id, false); });
            walk(
                source.points[2], frameEnd, startOf,
                [&p](std::int64_t id) { return nextPoint(p.distance, id); },
                [&](std::int64_t id) { return spatialPoint(p.distance, id, false); });
        }
        std::map<std::int64_t, GatheredSource> bySource;
        for (GatheredSource& source : sources) {
            const std::int64_t id = source.soundSourceId;
            bySource.emplace(id, std::move(source));
        }
        return GatheredFrame{.entries = std::move(entries), .sources = std::move(bySource)};
    }

private:
    /// A PartialIntegrableSegment with its nominal frequency, as read.
    ///
    /// @param id  The segment's id.
    /// @return The record, hasSuccessor false.
    GatheredSegment segment(std::int64_t id) const {
        const native::SegmentRow& row = model_->segments.segments()[static_cast<std::size_t>(id)];
        const native::NominalFrequencyRow& nominal =
            model_->segments.nominals()[static_cast<std::size_t>(row.nominal)];
        return GatheredSegment{.id = id,
            .startTime = row.startTime,
            .endTime = row.endTime,
            .startPhase = row.startPhase,
            .startFrequency = nominal.startNominalFrequency,
            .linearComponent = nominal.linearComponent,
            .linearGradient = nominal.linearGradient,
            .curveComponent = nominal.curveComponent,
            .hasSuccessor = false};
    }

    /// The segment after `id` in its timeline.
    ///
    /// @param id  A live segment's id.
    /// @return The next segment's id; nullopt at the end.
    std::optional<std::int64_t> nextSegment(std::int64_t id) const {
        const auto& timeline =
            model_->segments
                .timelines()[static_cast<std::size_t>(model_->segments.timelineOfSegment(id))];
        return nextInRange(id, timeline.firstSegment, timeline.segmentCount);
    }

    /// Whether a segment is not its timeline's first.
    ///
    /// @param id  A live segment's id.
    /// @return True when another segment leads to it.
    bool hasPreviousSegment(std::int64_t id) const {
        return std::cmp_not_equal(
            id, model_->segments
                    .timelines()[static_cast<std::size_t>(model_->segments.timelineOfSegment(id))]
                    .firstSegment);
    }

    /// An AmplitudePhysicalTimelinePoint, as read.
    ///
    /// @param id  The point's id.
    /// @return The record, hasSuccessor false.
    GatheredPoint amplitudePoint(std::int64_t id) const {
        const native::CurvePoint<float>& point =
            model_->physical.amplitude.points()[static_cast<std::size_t>(id)];
        return GatheredPoint{.id = id,
            .time = point.time,
            .amplitude = static_cast<double>(point.value),
            .curveType = point.curveType,
            .hasSuccessor = false};
    }

    /// A PartialPhaseCompensationSegment, as read.
    ///
    /// @param id  The segment's id.
    /// @return The record, hasSuccessor false.
    GatheredCompensation compensation(std::int64_t id) const {
        const native::CompensationRow& row =
            model_->segments.compensation()[static_cast<std::size_t>(id)];
        return GatheredCompensation{.id = id,
            .startTime = row.startTime,
            .endTime = row.endTime,
            .startDelta = row.startPhaseDelta,
            .endDelta = row.endPhaseDelta,
            .hasSuccessor = false};
    }

    /// The compensation segment after `id` in its timeline.
    ///
    /// @param id  A live compensation segment's id.
    /// @return The next one's id; nullopt at the end.
    std::optional<std::int64_t> nextCompensation(std::int64_t id) const {
        const auto& timeline =
            model_->segments
                .timelines()[static_cast<std::size_t>(model_->segments.timelineOfCompensation(id))];
        return nextInRange(id, timeline.firstCompensation, timeline.compensationCount);
    }

    /// An azimuth, elevation or distance point, as read.
    ///
    /// @tparam Value  The kind's value type.
    /// @param timelines  The kind's timelines.
    /// @param id         The point's id.
    /// @param isEntry    Whether it is an entry's point, whose predecessor
    ///                   is read; a walked point's is not.
    /// @return The record, hasSuccessor false.
    template <typename Value>
    static GatheredSpatialPoint spatialPoint(
        const PhysicalTimelines<native::CurvePoint<Value>>& timelines, std::int64_t id,
        bool isEntry) {
        const native::CurvePoint<Value>& point = timelines.points()[static_cast<std::size_t>(id)];
        return GatheredSpatialPoint{.id = id,
            .time = point.time,
            .value = static_cast<double>(point.value),
            .curveType = point.curveType,
            .hasSuccessor = false,
            .hasPredecessor = isEntry && hasPreviousPoint(timelines, id)};
    }

    const NativeModel* model_; ///< The model; not owned.
    /// Each partial's PartialSet (HAS_PARTIAL is ONE_MANY: at most one).
    std::unordered_map<std::int64_t, std::int64_t> setOfPartial_;
    /// Each PartialSet's SoundSource (HAS_PARTIALSET is ONE_MANY: at most one).
    std::unordered_map<std::int64_t, std::int64_t> sourceOfSet_;
};

} // namespace

std::unique_ptr<FrameSource> makeNativeFrameSource(const native::NativeModel& model) {
    return std::make_unique<NativeFrameSource>(model);
}

} // namespace rainbohz::rendering_engine
