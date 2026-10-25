// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/frame_gather.hpp"

#include "rainbohz/rendering_engine/precomputation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief The parts of rendering's gather that do not depend on the
/// engine-db backend: the checks on what a frame's entries lead to, and their
/// grouping by SoundSource (docs/rendering.md §6.3).
///
/// @see frame_gather.hpp

namespace rainbohz::rendering_engine {

void markMiscountedEntries(std::vector<GatheredEntry>& entries) {
    for (GatheredEntry& entry : entries) {
        if (entry.segments.size() != 1 || entry.points.size() != 1 ||
            entry.compensation.size() != 1) {
            entry.failure = "its FrameEntry leads to " + std::to_string(entry.segments.size()) +
                            " segment(s), " + std::to_string(entry.points.size()) +
                            " amplitude point(s) and " + std::to_string(entry.compensation.size()) +
                            " phase compensation segment(s); exactly one of each is required, "
                            "so it was not precomputed, or not for this frame";
        }
        for (std::size_t k = 0; k < kSpatialKinds.size() && !entry.failure.has_value(); ++k) {
            if (entry.spatial.at(k).size() != 1) {
                entry.failure = "its FrameEntry leads to " +
                                std::to_string(entry.spatial.at(k).size()) + " " +
                                std::string{spatialTimelineName(kSpatialKinds.at(k))} +
                                " point(s); exactly one is required, so it was not "
                                "precomputed, or not for this frame";
            }
        }
    }
}

std::vector<GatheredSource> groupSources(const std::vector<GatheredEntry>& entries) {
    std::map<std::int64_t, GatheredSource> bySource;
    for (const GatheredEntry& entry : entries) {
        if (entry.failure.has_value() || entry.soundSourceIds.size() != 1) {
            continue;
        }
        const std::int64_t sourceId = entry.soundSourceIds.front();
        auto [found, added] = bySource.try_emplace(sourceId);
        GatheredSource& source = found->second;
        if (added) {
            source.soundSourceId = sourceId;
            source.description = "SoundSource " + entry.soundSourceName.value_or("(unnamed)") +
                                 " (id " + std::to_string(sourceId) + ")";
        }
        for (std::size_t k = 0; k < kSpatialKinds.size(); ++k) {
            const GatheredSpatialPoint& candidate = entry.spatial.at(k).front();
            std::vector<GatheredSpatialPoint>& start = source.points.at(k);
            if (start.empty()) {
                start.assign(1, candidate);
            } else if (start.front().id != candidate.id) {
                source.failure = "its partials' FrameEntry nodes lead to different " +
                                 std::string{spatialTimelineName(kSpatialKinds.at(k))} +
                                 " points (ids " + std::to_string(start.front().id) + " and " +
                                 std::to_string(candidate.id) +
                                 "); every partial of a source must share them, so the entries "
                                 "are stale";
            }
        }
    }
    std::vector<GatheredSource> sources;
    sources.reserve(bySource.size());
    for (auto& [id, source] : bySource) {
        sources.push_back(std::move(source));
    }
    return sources;
}

} // namespace rainbohz::rendering_engine
