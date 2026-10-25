// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/native_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief The native model's segment timelines and frame tables, and its
/// census.
///
/// @see native_model.hpp

namespace rainbohz::rendering_engine::native {

namespace {

/// Converts a count to the census's integer type.
///
/// @param count  A row or edge count.
/// @return The same count.
std::int64_t asCount(std::size_t count) noexcept {
    return static_cast<std::int64_t>(count);
}

/// Adds a census line when its count is not zero, as Ladybug's census only
/// lists populated tables.
///
/// @param[in,out] into  The lines, gaining one.
/// @param name          The table's name in the schema.
/// @param count         Its rows or edges.
void tally(std::vector<RowCount>& into, std::string name, std::int64_t count) {
    if (count != 0) {
        into.emplace_back(std::move(name), count);
    }
}

/// The census lines of one kind of physical timeline: its timeline and
/// point tables.
///
/// @tparam Point          The kind's point type.
/// @param timelines       The kind's timelines.
/// @param timelineTable   The timeline table's name.
/// @param pointTable      The point table's name.
/// @param[in,out] nodes   The node lines, gaining up to two.
template <typename Point>
void tallyTimelines(const PhysicalTimelines<Point>& timelines, std::string timelineTable,
    std::string pointTable, std::vector<RowCount>& nodes) {
    tally(nodes, std::move(timelineTable), asCount(timelines.liveTimelineCount()));
    tally(nodes, std::move(pointTable), asCount(timelines.livePointCount()));
}

} // namespace

// ---------------------------------------------------------------------------
// SegmentTimelines
// ---------------------------------------------------------------------------

std::int64_t SegmentTimelines::append(std::int64_t partial, std::span<const NewSegment> segments,
    std::span<const CompensationRow> compensation) {
    if (byPartial_.contains(partial)) {
        throw NativeModelError{
            "Partial " + std::to_string(partial) + " already has a PartialSegmentPhysicalTimeline"};
    }
    // Everything that can throw comes first; what follows cannot.
    reserveFor(timelines_, 1);
    reserveFor(segments_, segments.size());
    reserveFor(segmentTimeline_, segments.size());
    reserveFor(nominals_, segments.size());
    reserveFor(compensation_, compensation.size());
    reserveFor(compensationTimeline_, compensation.size());
    const auto id = static_cast<std::int64_t>(timelines_.size());
    byPartial_.emplace(partial, id);
    timelines_.push_back(Timeline{.partial = partial,
        .firstSegment = segments_.size(),
        .segmentCount = segments.size(),
        .firstCompensation = compensation_.size(),
        .compensationCount = compensation.size(),
        .alive = true});
    for (const NewSegment& segment : segments) {
        segments_.push_back(SegmentRow{.startTime = segment.startTime,
            .endTime = segment.endTime,
            .startPhase = segment.startPhase,
            .nominal = static_cast<std::int64_t>(nominals_.size())});
        nominals_.push_back(segment.nominal);
    }
    segmentTimeline_.insert(segmentTimeline_.end(), segments.size(), id);
    compensation_.insert(compensation_.end(), compensation.begin(), compensation.end());
    compensationTimeline_.insert(compensationTimeline_.end(), compensation.size(), id);
    liveSegments_ += segments.size();
    liveCompensation_ += compensation.size();
    return id;
}

void SegmentTimelines::remove(std::int64_t id) noexcept {
    if (id < 0 || static_cast<std::size_t>(id) >= timelines_.size()) {
        return;
    }
    Timeline& timeline = timelines_[static_cast<std::size_t>(id)];
    if (!timeline.alive) {
        return;
    }
    timeline.alive = false;
    byPartial_.erase(timeline.partial);
    liveSegments_ -= timeline.segmentCount;
    liveCompensation_ -= timeline.compensationCount;
}

std::optional<std::int64_t> SegmentTimelines::timelineOf(std::int64_t partial) const {
    const auto found = byPartial_.find(partial);
    return found == byPartial_.end() ? std::nullopt : std::optional{found->second};
}

std::int64_t SegmentTimelines::timelineOfSegment(std::int64_t segment) const {
    return segmentTimeline_.at(static_cast<std::size_t>(segment));
}

std::int64_t SegmentTimelines::timelineOfCompensation(std::int64_t segment) const {
    return compensationTimeline_.at(static_cast<std::size_t>(segment));
}

bool SegmentTimelines::isLiveSegment(std::int64_t segment) const noexcept {
    return segment >= 0 && static_cast<std::size_t>(segment) < segments_.size() &&
           timelines_[static_cast<std::size_t>(segmentTimeline_[static_cast<std::size_t>(segment)])]
               .alive;
}

bool SegmentTimelines::isLiveCompensation(std::int64_t segment) const noexcept {
    return segment >= 0 && static_cast<std::size_t>(segment) < compensation_.size() &&
           timelines_[static_cast<std::size_t>(
                          compensationTimeline_[static_cast<std::size_t>(segment)])]
               .alive;
}

// ---------------------------------------------------------------------------
// FrameTables
// ---------------------------------------------------------------------------

std::int64_t FrameTables::appendEntry(const FrameEntryRow& entry) {
    if (!frames_.contains(entry.frame)) {
        throw NativeModelError{"there is no Frame " + std::to_string(entry.frame) +
                               " for a FrameEntry to be reached from"};
    }
    // Everything that can throw comes first; what follows cannot. An empty
    // index list that operator[] creates is harmless if a later step throws.
    reserveFor(entries_, 1);
    reserveFor(entryAlive_, 1);
    std::vector<std::int64_t>& ofFrame = byFrame_[entry.frame];
    std::vector<std::int64_t>& ofPartial = byPartial_[entry.partial];
    reserveFor(ofFrame, 1);
    reserveFor(ofPartial, 1);
    const auto id = static_cast<std::int64_t>(entries_.size());
    entries_.push_back(entry);
    entryAlive_.push_back(true);
    ofFrame.push_back(id);
    ofPartial.push_back(id);
    ++liveEntries_;
    return id;
}

void FrameTables::removeEntry(std::int64_t id) noexcept {
    if (!isLiveEntry(id)) {
        return;
    }
    entryAlive_[static_cast<std::size_t>(id)] = false;
    --liveEntries_;
}

bool FrameTables::isLiveEntry(std::int64_t id) const noexcept {
    return id >= 0 && static_cast<std::size_t>(id) < entries_.size() &&
           entryAlive_[static_cast<std::size_t>(id)];
}

std::vector<std::int64_t> FrameTables::liveEntriesOf(std::int32_t frame) const {
    std::vector<std::int64_t> live;
    if (const auto found = byFrame_.find(frame); found != byFrame_.end()) {
        std::ranges::copy_if(found->second, std::back_inserter(live),
            [this](std::int64_t id) { return isLiveEntry(id); });
    }
    return live;
}

std::vector<std::int64_t> FrameTables::liveEntriesOfPartial(std::int64_t partial) const {
    std::vector<std::int64_t> live;
    if (const auto found = byPartial_.find(partial); found != byPartial_.end()) {
        std::ranges::copy_if(found->second, std::back_inserter(live),
            [this](std::int64_t id) { return isLiveEntry(id); });
    }
    return live;
}

void FrameTables::setScope(std::int64_t partial, std::int32_t first, std::int32_t last) {
    if (last < first) {
        throw NativeModelError{"a frame scope from " + std::to_string(first) + " to " +
                               std::to_string(last) + " is empty"};
    }
    // The frames are consecutive, so the range is covered when its two ends
    // are and the set holds every frame between them.
    const auto from = frames_.find(first);
    const auto to = frames_.find(last);
    if (from == frames_.end() || to == frames_.end() ||
        std::distance(from, to) != static_cast<std::ptrdiff_t>(last) - first) {
        throw NativeModelError{"a frame from " + std::to_string(first) + " to " +
                               std::to_string(last) + " has no Frame node"};
    }
    scopes_.insert_or_assign(partial, std::pair{first, last});
}

std::int64_t FrameTables::scopeEdgeCount() const noexcept {
    std::int64_t total = 0;
    for (const auto& [partial, scope] : scopes_) {
        total += static_cast<std::int64_t>(scope.second) - scope.first + 1;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Census
// ---------------------------------------------------------------------------

Census censusOf(const NativeModel& model) {
    const CanonicalTables& c = model.canonical;
    const PhysicalTables& p = model.physical;
    const SegmentTimelines& s = model.segments;
    const FrameTables& f = model.frames;
    Census census;

    std::vector<RowCount>& nodes = census.nodes;
    tally(nodes, "EngineConfig", asCount(c.engineConfigs.size()));
    tally(nodes, "Score", asCount(c.scores.size()));
    tally(nodes, "SoundEntity", asCount(c.soundEntities.size()));
    tally(nodes, "SoundSource", asCount(c.soundSources.size()));
    tally(nodes, "PartialSet", asCount(c.partialSets.size()));
    tally(nodes, "Partial", asCount(c.partials.size()));
    tally(nodes, "Label", asCount(c.labels.size()));
    tally(nodes, "TempoMap", asCount(c.tempoMaps.size()));
    tally(nodes, "TempoMapPoint", asCount(c.tempoMapPoints.size()));
    tally(nodes, "PartialSetMusicalTimeline", asCount(c.partialSetMusicalTimelines));
    tally(
        nodes, "PartialSetMusicalTimelinePoint", asCount(c.partialSetMusicalTimelinePoints.size()));
    tally(nodes, "PartialStateSet", asCount(c.partialStateSets));
    tally(nodes, "PartialState", asCount(c.partialStates.size()));
    tally(nodes, "SpatialPositionMusicalTimeline", asCount(c.spatialPositionMusicalTimelines));
    tally(nodes, "SpatialPositionMusicalTimelinePoint",
        asCount(c.spatialPositionMusicalTimelinePoints.size()));
    tally(nodes, "SpatialPositionState", asCount(c.spatialPositionStates.size()));
    tallyTimelines(
        p.frequency, "FrequencyPhysicalTimeline", "FrequencyPhysicalTimelinePoint", nodes);
    tallyTimelines(
        p.amplitude, "AmplitudePhysicalTimeline", "AmplitudePhysicalTimelinePoint", nodes);
    tallyTimelines(p.phase, "PhasePhysicalTimeline", "PhasePhysicalTimelinePoint", nodes);
    tallyTimelines(p.azimuth, "AzimuthPhysicalTimeline", "AzimuthPhysicalTimelinePoint", nodes);
    tallyTimelines(
        p.elevation, "ElevationPhysicalTimeline", "ElevationPhysicalTimelinePoint", nodes);
    tallyTimelines(p.distance, "DistancePhysicalTimeline", "DistancePhysicalTimelinePoint", nodes);
    tally(nodes, "PartialSegmentPhysicalTimeline", asCount(s.liveTimelineCount()));
    tally(nodes, "PartialIntegrableSegment", asCount(s.liveSegmentCount()));
    tally(nodes, "PartialSegmentNominalFrequency", asCount(s.liveSegmentCount()));
    tally(nodes, "PartialPhaseCompensationSegment", asCount(s.liveCompensationCount()));
    tally(nodes, "Frame", asCount(f.frames().size()));
    tally(nodes, "FrameEntry", asCount(f.liveEntryCount()));

    // A list of n points has n NEXT_POINT edges: one from its timeline to
    // the first point, and one to each later point.
    const std::size_t physicalNext = p.frequency.livePointCount() + p.amplitude.livePointCount() +
                                     p.phase.livePointCount() + p.azimuth.livePointCount() +
                                     p.elevation.livePointCount() + p.distance.livePointCount() +
                                     s.liveSegmentCount() + s.liveCompensationCount();
    const std::size_t canonicalNext = c.scoreTempoMap.size() + c.tempoMapHead.size() +
                                      c.tempoMapNext.size() + c.setTimelineHead.size() +
                                      c.setTimelineNext.size() + c.sourceTimelineHead.size() +
                                      c.sourceTimelineNext.size();
    const std::size_t physicalTimelines =
        p.frequency.liveTimelineCount() + p.amplitude.liveTimelineCount() +
        p.phase.liveTimelineCount() + p.azimuth.liveTimelineCount() +
        p.elevation.liveTimelineCount() + p.distance.liveTimelineCount() + s.liveTimelineCount();
    const std::int64_t entries = asCount(f.liveEntryCount());

    std::vector<RowCount>& rels = census.relationships;
    tally(rels, "HAS_SOUNDENTITY", asCount(c.hasSoundEntity.size()));
    tally(rels, "HAS_SOUNDSOURCE", asCount(c.hasSoundSource.size()));
    tally(rels, "HAS_PARTIALSET", asCount(c.hasPartialSet.size()));
    tally(rels, "HAS_PARTIAL", asCount(c.hasPartial.size()));
    tally(rels, "HAS_LABEL", asCount(c.partialLabels.size() + c.scoreLabels.size()));
    tally(rels, "NEXT_POINT", asCount(canonicalNext + physicalNext));
    tally(rels, "TEMPO_MAP_SYNCHRONIZATION_POINT", asCount(c.tempoMapSync.size()));
    tally(rels, "HAS_MUSICAL_TIMELINE", asCount(c.setTimeline.size() + c.sourceTimeline.size()));
    tally(rels, "POINT_AT", asCount(c.setPointAt.size() + c.sourcePointAt.size()));
    tally(rels, "POINT_STATE", asCount(c.setPointState.size() + c.sourcePointState.size()));
    tally(rels, "MEMBER_OF_SET_MANY_ONE", asCount(c.stateMemberOf.size()));
    tally(rels, "STATE_OF", asCount(c.stateOf.size()));
    tally(rels, "HAS_PHYSICAL_TIMELINE", asCount(physicalTimelines));
    tally(rels, "SEGMENT_TO_FREQUENCY_NOMINAL", asCount(s.liveSegmentCount()));
    tally(rels, "PARTIAL_IN_FRAME", f.scopeEdgeCount());
    tally(rels, "FRAME_ENTRY", entries);
    tally(rels, "FRAME_ENTRY_PARTIAL", entries);
    // Every entry leads to six list nodes (docs/precomputation.md §6.7).
    tally(rels, "FRAME_ENTRY_PHYSICAL_TIMELINE", entries * 6);

    for (const RowCount& line : census.nodes) {
        census.totalNodes += line.second;
    }
    for (const RowCount& line : census.relationships) {
        census.totalRelationships += line.second;
    }
    std::ranges::sort(census.nodes);
    std::ranges::sort(census.relationships);
    return census;
}

std::vector<EngineConfigRecord> engineConfigsOf(const NativeModel& model) {
    const auto widen = [](const auto& value) {
        return value.has_value() ? std::optional{static_cast<std::int64_t>(*value)} : std::nullopt;
    };
    std::vector<EngineConfigRecord> configs;
    configs.reserve(model.canonical.engineConfigs.size());
    for (const EngineConfigRow& row : model.canonical.engineConfigs) {
        configs.push_back(EngineConfigRecord{.frameSize = widen(row.frameSize),
            .sampleRate = widen(row.sampleRate),
            .channelMode = widen(row.channelMode)});
    }
    return configs;
}

std::set<std::string> labelNamesOf(const NativeModel& model) {
    std::set<std::string> names;
    for (const LabelRow& label : model.canonical.labels) {
        if (label.name.has_value()) {
            names.insert(*label.name);
        }
    }
    return names;
}

} // namespace rainbohz::rendering_engine::native
