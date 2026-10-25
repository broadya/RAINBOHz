// SPDX-License-Identifier: Apache-2.0

// The native engine-db backend against the Ladybug one (ADR 0028).
//
// Every example score hzlc compiles is loaded into both backends from the
// same tables, and every stage is run on each. What crosses the store
// boundary must be equal: each unit the transform and precomputation write,
// their reports, the census after each stage, everything the gather reads
// for every frame, every answer of the timing queries, and the rendered
// frames, byte for byte. A stage that depended on an order the Ladybug
// queries do not state, or a native read that differed from its query, shows
// here at the stage where it happens.
//
// Lives with the transform tests because it names the transform's store,
// which reads musical time.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/frame_gather.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/native_store.hpp"
#include "rainbohz/rendering_engine/native_table_loader.hpp"
#include "rainbohz/rendering_engine/precomputation_store.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/table_loader.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"
#include "rainbohz/rendering_engine/transform/transform_store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace re = rainbohz::rendering_engine;
namespace tr = rainbohz::rendering_engine::transform;

// ---------------------------------------------------------------------------
// Stores that record what they are asked to write
// ---------------------------------------------------------------------------

/// A TransformStore that forwards every call and keeps a copy of each unit.
class RecordingTransformStore final : public tr::TransformStore {
public:
    explicit RecordingTransformStore(tr::TransformStore& inner) : inner_{&inner} {}

    std::vector<re::EngineConfigRecord> readEngineConfigs() override {
        return inner_->readEngineConfigs();
    }
    tr::TransformInputs readInputs() override { return inner_->readInputs(); }
    std::vector<std::optional<std::string>> writePartials(
        std::span<const tr::TransformedPartial> units) override {
        partials.insert(partials.end(), units.begin(), units.end());
        return inner_->writePartials(units);
    }
    std::vector<std::optional<std::string>> writeSources(
        std::span<const tr::TransformedSource> units) override {
        sources.insert(sources.end(), units.begin(), units.end());
        return inner_->writeSources(units);
    }

    std::vector<tr::TransformedPartial> partials; ///< Every partial unit, in order.
    std::vector<tr::TransformedSource> sources;   ///< Every source unit, in order.

private:
    tr::TransformStore* inner_;
};

/// A precomputed unit, with the spatial ids it views copied out.
struct RecordedPartial {
    re::PrecomputedPartial unit;                      ///< The unit, its views cleared.
    std::array<std::vector<std::int64_t>, 3> spatial; ///< The spatial ids it viewed.
};

/// A PrecomputationStore that forwards every call and keeps a copy of each
/// unit.
class RecordingPrecomputationStore final : public re::PrecomputationStore {
public:
    explicit RecordingPrecomputationStore(re::PrecomputationStore& inner) : inner_{&inner} {}

    std::vector<re::EngineConfigRecord> readEngineConfigs() override {
        return inner_->readEngineConfigs();
    }
    re::PrecomputationInputs readInputs() override { return inner_->readInputs(); }
    std::vector<std::optional<std::string>> writePartials(
        std::span<const re::PrecomputedPartial> units) override {
        for (const re::PrecomputedPartial& unit : units) {
            RecordedPartial copy{.unit = unit, .spatial = {}};
            for (std::size_t k = 0; k < 3; ++k) {
                copy.spatial.at(k).assign(
                    unit.spatialPointIds.at(k).begin(), unit.spatialPointIds.at(k).end());
                copy.unit.spatialPointIds.at(k) = {};
            }
            partials.push_back(std::move(copy));
        }
        return inner_->writePartials(units);
    }

    std::vector<RecordedPartial> partials; ///< Every unit, in order.

private:
    re::PrecomputationStore* inner_;
};

// ---------------------------------------------------------------------------
// Field-by-field comparison
// ---------------------------------------------------------------------------

void expectEqual(const tr::TransformedPartial& a, const tr::TransformedPartial& b) {
    EXPECT_EQ(a.partialId, b.partialId);
    EXPECT_EQ(a.staleFrequency, b.staleFrequency);
    EXPECT_EQ(a.staleAmplitude, b.staleAmplitude);
    EXPECT_EQ(a.stalePhase, b.stalePhase);
    EXPECT_EQ(a.frequency, b.frequency);
    EXPECT_EQ(a.amplitude, b.amplitude);
    EXPECT_EQ(a.phase, b.phase);
}

void expectEqual(const tr::TransformedSource& a, const tr::TransformedSource& b) {
    EXPECT_EQ(a.sourceId, b.sourceId);
    EXPECT_EQ(a.stale, b.stale);
    EXPECT_EQ(a.points, b.points);
}

void expectEqual(const tr::TransformReport& a, const tr::TransformReport& b) {
    EXPECT_EQ(a.sampleRate, b.sampleRate);
    ASSERT_EQ(a.tempoMaps.size(), b.tempoMaps.size());
    for (std::size_t i = 0; i < a.tempoMaps.size(); ++i) {
        const auto& x = a.tempoMaps[i];
        const auto& y = b.tempoMaps[i];
        EXPECT_EQ(x.scoreId, y.scoreId);
        EXPECT_EQ(x.tempoMapId, y.tempoMapId);
        EXPECT_EQ(x.name, y.name);
        EXPECT_EQ(x.ticksPerBeat, y.ticksPerBeat);
        EXPECT_EQ(x.pointCount, y.pointCount);
        EXPECT_EQ(x.rampPhysicalTime, y.rampPhysicalTime);
        EXPECT_EQ(x.parentIds, y.parentIds);
        EXPECT_EQ(x.syncPointCount, y.syncPointCount);
    }
    ASSERT_EQ(a.sets.size(), b.sets.size());
    for (std::size_t i = 0; i < a.sets.size(); ++i) {
        EXPECT_EQ(a.sets[i].setId, b.sets[i].setId);
        EXPECT_EQ(a.sets[i].setName, b.sets[i].setName);
        EXPECT_EQ(a.sets[i].error, b.sets[i].error);
        ASSERT_EQ(a.sets[i].partials.size(), b.sets[i].partials.size());
        for (std::size_t j = 0; j < a.sets[i].partials.size(); ++j) {
            const auto& x = a.sets[i].partials[j];
            const auto& y = b.sets[i].partials[j];
            EXPECT_EQ(x.partialId, y.partialId);
            EXPECT_EQ(x.partialName, y.partialName);
            EXPECT_EQ(x.error, y.error);
            EXPECT_EQ(x.frequencyPoints, y.frequencyPoints);
            EXPECT_EQ(x.amplitudePoints, y.amplitudePoints);
            EXPECT_EQ(x.phasePoints, y.phasePoints);
        }
    }
    ASSERT_EQ(a.sources.size(), b.sources.size());
    for (std::size_t i = 0; i < a.sources.size(); ++i) {
        EXPECT_EQ(a.sources[i].sourceId, b.sources[i].sourceId);
        EXPECT_EQ(a.sources[i].sourceName, b.sources[i].sourceName);
        EXPECT_EQ(a.sources[i].error, b.sources[i].error);
        EXPECT_EQ(a.sources[i].points, b.sources[i].points);
    }
}

void expectEqual(const RecordedPartial& a, const RecordedPartial& b) {
    const re::PrecomputedPartial& x = a.unit;
    const re::PrecomputedPartial& y = b.unit;
    EXPECT_EQ(x.partialId, y.partialId);
    EXPECT_EQ(x.stale.size(), y.stale.size());
    EXPECT_EQ(x.staleFrameEntries, y.staleFrameEntries);
    EXPECT_EQ(x.staleFrameScope, y.staleFrameScope);
    ASSERT_EQ(x.segments.size(), y.segments.size());
    for (std::size_t i = 0; i < x.segments.size(); ++i) {
        EXPECT_EQ(x.segments[i].startTime, y.segments[i].startTime) << "segment " << i;
        EXPECT_EQ(x.segments[i].endTime, y.segments[i].endTime) << "segment " << i;
        EXPECT_EQ(x.segments[i].startPhase, y.segments[i].startPhase) << "segment " << i;
        EXPECT_EQ(x.segments[i].nominal.startFrequency, y.segments[i].nominal.startFrequency);
        EXPECT_EQ(x.segments[i].nominal.linearGradient, y.segments[i].nominal.linearGradient);
    }
    ASSERT_EQ(x.compensation.size(), y.compensation.size());
    for (std::size_t i = 0; i < x.compensation.size(); ++i) {
        EXPECT_EQ(x.compensation[i].startTime, y.compensation[i].startTime);
        EXPECT_EQ(x.compensation[i].endTime, y.compensation[i].endTime);
        EXPECT_EQ(x.compensation[i].startDelta, y.compensation[i].startDelta);
        EXPECT_EQ(x.compensation[i].endDelta, y.compensation[i].endDelta);
    }
    EXPECT_EQ(x.frames.first, y.frames.first);
    EXPECT_EQ(x.frames.last, y.frames.last);
    EXPECT_EQ(x.amplitudePointIds, y.amplitudePointIds);
    EXPECT_EQ(a.spatial, b.spatial);
    ASSERT_EQ(x.entries.size(), y.entries.size());
    for (std::size_t i = 0; i < x.entries.size(); ++i) {
        EXPECT_EQ(x.entries[i].frame, y.entries[i].frame);
        EXPECT_EQ(x.entries[i].segment, y.entries[i].segment);
        EXPECT_EQ(x.entries[i].point, y.entries[i].point);
        EXPECT_EQ(x.entries[i].compensation, y.entries[i].compensation);
        EXPECT_EQ(x.entries[i].spatial, y.entries[i].spatial);
    }
}

void expectEqual(const re::PrecomputationReport& a, const re::PrecomputationReport& b) {
    EXPECT_EQ(a.frameSize, b.frameSize);
    ASSERT_EQ(a.partials.size(), b.partials.size());
    for (std::size_t i = 0; i < a.partials.size(); ++i) {
        const auto& x = a.partials[i];
        const auto& y = b.partials[i];
        EXPECT_EQ(x.partialId, y.partialId);
        EXPECT_EQ(x.partialName, y.partialName);
        EXPECT_EQ(x.error, y.error);
        EXPECT_EQ(x.segmentCount, y.segmentCount);
        EXPECT_EQ(x.compensationCount, y.compensationCount);
        EXPECT_EQ(x.frames.has_value(), y.frames.has_value());
        if (x.frames.has_value() && y.frames.has_value()) {
            EXPECT_EQ(x.frames->first, y.frames->first);
            EXPECT_EQ(x.frames->last, y.frames->last);
        }
        EXPECT_EQ(x.entryCount, y.entryCount);
    }
}

void expectEqual(const re::GatheredSegment& a, const re::GatheredSegment& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.startTime, b.startTime);
    EXPECT_EQ(a.endTime, b.endTime);
    EXPECT_EQ(a.startPhase, b.startPhase);
    EXPECT_EQ(a.startFrequency, b.startFrequency);
    EXPECT_EQ(a.linearComponent, b.linearComponent);
    EXPECT_EQ(a.linearGradient, b.linearGradient);
    EXPECT_EQ(a.curveComponent, b.curveComponent);
    EXPECT_EQ(a.hasSuccessor, b.hasSuccessor);
}

void expectEqual(const re::GatheredPoint& a, const re::GatheredPoint& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.time, b.time);
    EXPECT_EQ(a.amplitude, b.amplitude);
    EXPECT_EQ(a.curveType, b.curveType);
    EXPECT_EQ(a.hasSuccessor, b.hasSuccessor);
}

void expectEqual(const re::GatheredCompensation& a, const re::GatheredCompensation& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.startTime, b.startTime);
    EXPECT_EQ(a.endTime, b.endTime);
    EXPECT_EQ(a.startDelta, b.startDelta);
    EXPECT_EQ(a.endDelta, b.endDelta);
    EXPECT_EQ(a.hasSuccessor, b.hasSuccessor);
}

void expectEqual(const re::GatheredSpatialPoint& a, const re::GatheredSpatialPoint& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.time, b.time);
    EXPECT_EQ(a.value, b.value);
    EXPECT_EQ(a.curveType, b.curveType);
    EXPECT_EQ(a.hasSuccessor, b.hasSuccessor);
    EXPECT_EQ(a.hasPredecessor, b.hasPredecessor);
}

/// Compares two lists of gathered records element by element.
template <typename Record>
void expectEqualLists(const std::vector<Record>& a, const std::vector<Record>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        SCOPED_TRACE("element " + std::to_string(i));
        expectEqual(a[i], b[i]);
    }
}

void expectEqual(const re::GatheredFrame& a, const re::GatheredFrame& b) {
    ASSERT_EQ(a.entries.size(), b.entries.size());
    for (std::size_t i = 0; i < a.entries.size(); ++i) {
        SCOPED_TRACE("entry " + std::to_string(i));
        const auto& x = a.entries[i];
        const auto& y = b.entries[i];
        EXPECT_EQ(x.entryId, y.entryId);
        EXPECT_EQ(x.partialId, y.partialId);
        EXPECT_EQ(x.partialName, y.partialName);
        EXPECT_EQ(x.partialSetIds, y.partialSetIds);
        EXPECT_EQ(x.soundSourceIds, y.soundSourceIds);
        EXPECT_EQ(x.soundSourceName, y.soundSourceName);
        EXPECT_EQ(x.entrySegmentHasPredecessor, y.entrySegmentHasPredecessor);
        EXPECT_EQ(x.failure, y.failure);
        for (std::size_t k = 0; k < 3; ++k) {
            expectEqualLists(x.spatial.at(k), y.spatial.at(k));
        }
        expectEqualLists(x.segments, y.segments);
        expectEqualLists(x.points, y.points);
        expectEqualLists(x.compensation, y.compensation);
    }
    ASSERT_EQ(a.sources.size(), b.sources.size());
    for (auto x = a.sources.begin(), y = b.sources.begin(); x != a.sources.end(); ++x, ++y) {
        SCOPED_TRACE("source " + std::to_string(x->first));
        EXPECT_EQ(x->first, y->first);
        EXPECT_EQ(x->second.description, y->second.description);
        EXPECT_EQ(x->second.failure, y->second.failure);
        for (std::size_t k = 0; k < 3; ++k) {
            expectEqualLists(x->second.points.at(k), y->second.points.at(k));
        }
    }
}

void expectEqual(const tr::TempoMapInfo& a, const tr::TempoMapInfo& b) {
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.name, b.name);
    EXPECT_EQ(a.ticksPerBeat, b.ticksPerBeat);
}

void expectEqual(const std::vector<tr::TempoMapChild>& a, const std::vector<tr::TempoMapChild>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        expectEqual(a[i].map, b[i].map);
        EXPECT_EQ(a[i].syncPointParent, b[i].syncPointParent);
        EXPECT_EQ(a[i].syncPointChild, b[i].syncPointChild);
    }
}

void expectEqual(const tr::TempoMapPointPage& a, const tr::TempoMapPointPage& b) {
    expectEqual(a.map, b.map);
    EXPECT_EQ(a.nextAfter, b.nextAfter);
    ASSERT_EQ(a.points.size(), b.points.size());
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        EXPECT_EQ(a.points[i].id, b.points[i].id);
        EXPECT_EQ(a.points[i].position, b.points[i].position);
        EXPECT_EQ(a.points[i].startTempo, b.points[i].startTempo);
        EXPECT_EQ(a.points[i].hold, b.points[i].hold);
    }
}

void expectEqual(const re::Census& a, const re::Census& b) {
    EXPECT_EQ(a.nodes, b.nodes);
    EXPECT_EQ(a.relationships, b.relationships);
    EXPECT_EQ(a.totalNodes, b.totalNodes);
    EXPECT_EQ(a.totalRelationships, b.totalRelationships);
}

/// The whole contents of a file.
std::string contentsOf(const std::filesystem::path& file) {
    std::ifstream stream{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/// A directory under the system temporary directory, removed on destruction.
class TempDirectory {
public:
    TempDirectory() {
        static std::random_device entropy;
        path_ =
            std::filesystem::temp_directory_path() /
            ("rainbohz-equivalence-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// The examples
// ---------------------------------------------------------------------------

/// The compiled examples, as compile_scores.py names them.
constexpr std::array<std::string_view, 5> kExamples{
    "tempo-map-score", "polytempo-shift-score", "polytempo-stretch-score", "spatial-score", "demo"};

class StoreEquivalence : public testing::TestWithParam<std::string_view> {};

TEST_P(StoreEquivalence, EveryStageAgreesAcrossBackends) {
    const std::filesystem::path manifest =
        std::filesystem::path{RE_TEST_COMPILED_SCORES} / GetParam() / "manifest.json";
    ASSERT_TRUE(std::filesystem::exists(manifest))
        << manifest << " is missing. Run the tests through CTest, whose scores.compile test "
        << "compiles the examples, or run rendering-engine/test/compile_scores.py.";

    re::EngineDb db;
    ASSERT_TRUE(re::loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    const re::LoadReport loaded = re::loadTableManifest(db, manifest);
    ASSERT_TRUE(loaded.ok()) << loaded.failures.front().message;
    re::LadybugStore ladybug{db};
    re::NativeStore native{re::native::loadNativeScore(manifest).model};

    {
        SCOPED_TRACE("loaded");
        expectEqual(native.census(), ladybug.census());
        EXPECT_EQ(native.labelNames(), ladybug.labelNames());
    }

    // The transform.
    RecordingTransformStore ladybugTransform{ladybug.transformStore()};
    RecordingTransformStore nativeTransform{native.transformStore()};
    const tr::TransformReport ladybugTransformed = tr::transformAll(ladybugTransform);
    const tr::TransformReport nativeTransformed = tr::transformAll(nativeTransform);
    {
        SCOPED_TRACE("transform");
        expectEqual(nativeTransformed, ladybugTransformed);
        ASSERT_EQ(nativeTransform.partials.size(), ladybugTransform.partials.size());
        for (std::size_t i = 0; i < nativeTransform.partials.size(); ++i) {
            SCOPED_TRACE("partial unit " + std::to_string(i));
            expectEqual(nativeTransform.partials[i], ladybugTransform.partials[i]);
        }
        ASSERT_EQ(nativeTransform.sources.size(), ladybugTransform.sources.size());
        for (std::size_t i = 0; i < nativeTransform.sources.size(); ++i) {
            SCOPED_TRACE("source unit " + std::to_string(i));
            expectEqual(nativeTransform.sources[i], ladybugTransform.sources[i]);
        }
        expectEqual(native.census(), ladybug.census());
    }

    // Precomputation.
    RecordingPrecomputationStore ladybugPrecomputation{ladybug.precomputationStore()};
    RecordingPrecomputationStore nativePrecomputation{native.precomputationStore()};
    const re::PrecomputationReport ladybugPrecomputed = re::precomputeAll(ladybugPrecomputation);
    const re::PrecomputationReport nativePrecomputed = re::precomputeAll(nativePrecomputation);
    {
        SCOPED_TRACE("precomputation");
        ASSERT_TRUE(ladybugPrecomputed.ok());
        expectEqual(nativePrecomputed, ladybugPrecomputed);
        ASSERT_EQ(nativePrecomputation.partials.size(), ladybugPrecomputation.partials.size());
        for (std::size_t i = 0; i < nativePrecomputation.partials.size(); ++i) {
            SCOPED_TRACE("unit " + std::to_string(i));
            expectEqual(nativePrecomputation.partials[i], ladybugPrecomputation.partials[i]);
        }
        expectEqual(native.census(), ladybug.census());
    }

    // The gather, for every frame there is, and one past the end.
    const re::FrameSize size = ladybugPrecomputed.frameSize;
    std::vector<std::int32_t> frames(
        native.model().frames.frames().begin(), native.model().frames.frames().end());
    ASSERT_FALSE(frames.empty());
    frames.push_back(frames.back() + 1);
    for (const std::int32_t frame : frames) {
        SCOPED_TRACE("frame " + std::to_string(frame));
        expectEqual(native.frameSource().gatherFrame(re::FrameNumber{frame}, size),
            ladybug.frameSource().gatherFrame(re::FrameNumber{frame}, size));
    }

    // The timing queries, for every tempo map.
    {
        SCOPED_TRACE("timing");
        const tr::TempoMapNode a = tr::readRootTempoMap(native.timingSource());
        const tr::TempoMapNode b = tr::readRootTempoMap(ladybug.timingSource());
        expectEqual(a.map, b.map);
        EXPECT_EQ(a.tempoRampPhysicalTime, b.tempoRampPhysicalTime);
        expectEqual(a.children, b.children);
        const tr::EngineParameters x = tr::readEngineParameters(native.timingSource());
        const tr::EngineParameters y = tr::readEngineParameters(ladybug.timingSource());
        EXPECT_EQ(x.sampleRate, y.sampleRate);
        EXPECT_EQ(x.frameSize, y.frameSize);
        EXPECT_EQ(x.channelMode, y.channelMode);
        const auto maps = static_cast<std::int64_t>(native.model().canonical.tempoMaps.size());
        for (std::int64_t map = 0; map <= maps; ++map) {
            SCOPED_TRACE("TempoMap " + std::to_string(map));
            if (map == maps) {
                EXPECT_THROW(
                    tr::readTempoMapChildren(native.timingSource(), map), tr::TimingQueryError);
                continue;
            }
            expectEqual(tr::readTempoMapChildren(native.timingSource(), map),
                tr::readTempoMapChildren(ladybug.timingSource(), map));
            for (const std::int32_t limit : {1, 3, 1000}) {
                expectEqual(tr::readTempoMapPoints(native.timingSource(), map, std::nullopt, limit),
                    tr::readTempoMapPoints(ladybug.timingSource(), map, std::nullopt, limit));
            }
            for (const std::int64_t beats : {0, 1, 3, 7}) {
                const tr::BeatsTicks at{.beats = beats, .ticks = 0};
                try {
                    const tr::FramePosition p = tr::framePosition(ladybug.timingSource(), map, at);
                    const tr::FramePosition q = tr::framePosition(native.timingSource(), map, at);
                    EXPECT_EQ(q.frame, p.frame);
                    EXPECT_EQ(q.offset, p.offset);
                } catch (const tr::TimingQueryError& ladybugError) {
                    try {
                        static_cast<void>(tr::framePosition(native.timingSource(), map, at));
                        ADD_FAILURE()
                            << "native answered where Ladybug refused: " << ladybugError.what();
                    } catch (const tr::TimingQueryError& nativeError) {
                        EXPECT_STREQ(nativeError.what(), ladybugError.what());
                        EXPECT_EQ(nativeError.kind(), ladybugError.kind());
                    }
                }
            }
        }
    }

    // Rendering: every frame, byte for byte.
    const TempDirectory out;
    std::filesystem::create_directories(out.path() / "ladybug");
    std::filesystem::create_directories(out.path() / "native");
    re::RenderRequest request;
    for (const std::int32_t frame : frames) {
        request.frames.push_back(re::FrameRequest{.frameNumber = frame, .labels = {}});
    }
    const re::RenderingReport x =
        re::renderFrames(ladybug.frameSource(), request, out.path() / "ladybug", false, 1);
    const re::RenderingReport y =
        re::renderFrames(native.frameSource(), request, out.path() / "native", false, 1);
    ASSERT_EQ(x.frames.size(), y.frames.size());
    for (std::size_t i = 0; i < x.frames.size(); ++i) {
        SCOPED_TRACE("rendered frame " + std::to_string(x.frames[i].frame.value()));
        EXPECT_EQ(y.frames[i].error, x.frames[i].error);
        EXPECT_EQ(y.frames[i].sourceCount, x.frames[i].sourceCount);
        EXPECT_EQ(y.frames[i].setCount, x.frames[i].setCount);
        EXPECT_EQ(y.frames[i].partialCount, x.frames[i].partialCount);
        EXPECT_EQ(y.frames[i].peak, x.frames[i].peak);
        EXPECT_EQ(y.frames[i].newFailures.size(), x.frames[i].newFailures.size());
        EXPECT_EQ(y.frames[i].cappedPhases, x.frames[i].cappedPhases);
        if (!x.frames[i].file.empty()) {
            EXPECT_EQ(contentsOf(y.frames[i].file), contentsOf(x.frames[i].file));
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Examples, StoreEquivalence, testing::ValuesIn(kExamples),
    [](const testing::TestParamInfo<std::string_view>& info) {
        std::string name{info.param};
        std::ranges::replace(name, '-', '_');
        return name;
    });

} // namespace
