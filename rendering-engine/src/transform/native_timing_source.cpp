// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/native_model.hpp"
#include "rainbohz/rendering_engine/transform/native_transform_store.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

/// @file
/// @brief The native backend's TimingSource: the timing queries' reads from
/// a NativeModel's canonical tables.
///
/// Each read gives what the Ladybug backend's query of the same name gives
/// (ladybug_timing_source.cpp), in the same order. The tempo maps' lists are
/// indexed once, when the source is made: the canonical tables do not change
/// after a score is loaded.
///
/// @see timing_query.hpp, native_model.hpp, ADR 0028

namespace rainbohz::rendering_engine::transform {

namespace {

using native::NativeModel;

/// The timing queries' reads over a native model.
class NativeTimingSource final : public TimingSource {
public:
    /// Serves the timing queries from `model`, indexing its tempo maps.
    ///
    /// @param model  The model, its canonical tables loaded. It must outlive
    ///               the source.
    explicit NativeTimingSource(const NativeModel& model) : model_{&model} {
        const native::CanonicalTables& c = model.canonical;
        for (const auto& edge : c.scoreTempoMap.edges()) {
            rootOfScore_.emplace(edge.from, edge.to);
        }
        for (const auto& edge : c.tempoMapHead.edges()) {
            headOfMap_.emplace(edge.from, edge.to);
        }
        for (const auto& edge : c.tempoMapNext.edges()) {
            nextOfPoint_.emplace(edge.from, edge.to);
        }
    }

    std::vector<EngineConfigRecord> readEngineConfigs() override {
        return native::engineConfigsOf(*model_);
    }

    std::vector<ScoreRootRecord> readScores() override {
        std::vector<ScoreRootRecord> scores;
        const auto& rows = model_->canonical.scores;
        for (std::size_t s = 0; s < rows.size(); ++s) {
            const auto id = static_cast<std::int64_t>(s);
            const auto root = rootOfScore_.find(id);
            scores.push_back(ScoreRootRecord{.scoreId = id,
                .root = root == rootOfScore_.end() ? std::nullopt : findTempoMap(root->second),
                .tempoRampPhysicalTime = rows[s].tempoRampPhysicalTime});
        }
        return scores;
    }

    std::optional<TempoMapInfo> findTempoMap(std::int64_t mapId) override {
        const auto& maps = model_->canonical.tempoMaps;
        if (mapId < 0 || static_cast<std::size_t>(mapId) >= maps.size()) {
            return std::nullopt;
        }
        const native::TempoMapRow& map = maps[static_cast<std::size_t>(mapId)];
        return TempoMapInfo{.id = mapId, .name = map.name, .ticksPerBeat = widen(map.ticksPerBeat)};
    }

    std::vector<std::optional<bool>> rampsOfScoresRootedAt(std::int64_t mapId) override {
        std::vector<std::optional<bool>> ramps;
        for (const auto& edge : model_->canonical.scoreTempoMap.edges()) {
            if (edge.to == mapId) {
                ramps.push_back(model_->canonical.scores.at(static_cast<std::size_t>(edge.from))
                        .tempoRampPhysicalTime);
            }
        }
        return ramps;
    }

    std::vector<TempoMapChild> readChildren(std::int64_t mapId) override {
        std::vector<TempoMapChild> children;
        for (const auto& edge : model_->canonical.tempoMapSync.edges()) {
            if (edge.to == mapId) {
                std::optional<TempoMapInfo> child = findTempoMap(edge.from);
                if (!child.has_value()) {
                    throw EngineDbError{"a synchronisation edge leads from TempoMap " +
                                        std::to_string(edge.from) + ", which does not exist"};
                }
                children.push_back(TempoMapChild{.map = std::move(*child),
                    .syncPointParent = edge.properties.syncPointParent,
                    .syncPointChild = edge.properties.syncPointChild});
            }
        }
        // ORDER BY c.id, then each sync point with a NULL after every value,
        // as the Ladybug query states it.
        const auto key = [](const TempoMapChild& child) {
            return std::tuple{child.map.id, !child.syncPointParent.has_value(),
                child.syncPointParent.value_or(0), !child.syncPointChild.has_value(),
                child.syncPointChild.value_or(0)};
        };
        std::ranges::stable_sort(children, {}, key);
        return children;
    }

    std::optional<TempoMapPointInfo> readHead(std::int64_t mapId) override {
        const auto head = headOfMap_.find(mapId);
        return head == headOfMap_.end() ? std::nullopt : std::optional{pointInfo(head->second)};
    }

    std::vector<TempoMapPointInfo> readNextPoints(std::int64_t pointId) override {
        std::vector<TempoMapPointInfo> points;
        for (auto next = nextOfPoint_.find(pointId);
            next != nextOfPoint_.end() && points.size() < static_cast<std::size_t>(kWalkChunk);
            next = nextOfPoint_.find(next->second)) {
            points.push_back(pointInfo(next->second));
        }
        return points;
    }

private:
    /// Widens a schema integer to what a query returns.
    ///
    /// @tparam T  The field's integer type.
    /// @param value  The field.
    /// @return It as an int64; nullopt for NULL.
    template <typename T>
    static std::optional<std::int64_t> widen(const std::optional<T>& value) {
        return value.has_value() ? std::optional{static_cast<std::int64_t>(*value)} : std::nullopt;
    }

    /// A TempoMapPoint as read.
    ///
    /// @param pointId  TempoMapPoint.id, of a row the model holds.
    /// @return The point.
    TempoMapPointInfo pointInfo(std::int64_t pointId) const {
        const native::TempoMapPointRow& row =
            model_->canonical.tempoMapPoints.at(static_cast<std::size_t>(pointId));
        return TempoMapPointInfo{.id = pointId,
            .position = row.position,
            .startTempo = row.startTempo,
            .hold = row.hold};
    }

    const NativeModel* model_;                                   ///< The model; not owned.
    std::unordered_map<std::int64_t, std::int64_t> rootOfScore_; ///< Score to its root map.
    std::unordered_map<std::int64_t, std::int64_t> headOfMap_;   ///< Map to its first point.
    std::unordered_map<std::int64_t, std::int64_t> nextOfPoint_; ///< Point to the next one.
};

} // namespace

std::unique_ptr<TimingSource> makeNativeTimingSource(const native::NativeModel& model) {
    return std::make_unique<NativeTimingSource>(model);
}

} // namespace rainbohz::rendering_engine::transform
