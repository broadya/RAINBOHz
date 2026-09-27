// Tests for rendering against the real engine-db schema, in memory.
//
// Each test builds a small score, precomputes it, renders frames into a
// fresh directory, and reads the files back by hand. Numerical agreement
// over whole frames is the oracle's job (test/oracle); these tests cover
// the mechanism: the gather through FrameEntry nodes, validation, the
// per-frame loop, summation and the files.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif
#ifndef RE_TEST_EXAMPLE_STATE
#define RE_TEST_EXAMPLE_STATE ""
#endif
#ifndef RE_TEST_TWO_SETS_STATE
#define RE_TEST_TWO_SETS_STATE ""
#endif

namespace {

using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::frameFileName;
using rainbohz::rendering_engine::FrameOutcome;
using rainbohz::rendering_engine::FrameRequest;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::precomputeAll;
using rainbohz::rendering_engine::renderFrames;
using rainbohz::rendering_engine::RenderingError;
using rainbohz::rendering_engine::RenderingReport;
using rainbohz::rendering_engine::RenderRequest;
using rainbohz::rendering_engine::SampleValue;
using rainbohz::rendering_engine::setFileName;
using rainbohz::rendering_engine::test::freshDirectory;
using rainbohz::rendering_engine::test::readFloatWav;
using rainbohz::rendering_engine::test::WavContents;

/// Formats a double as a Cypher literal that reads back exactly.
///
/// @param value  A finite value.
/// @return The value with a decimal point or exponent, typed DOUBLE.
std::string literal(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << std::showpoint << value;
    return out.str();
}

/// A (time, value) pair of a timeline.
using Point = std::pair<double, double>;

/// Checks that `whole` is the sample-wise sum of `a` and `b`, at a stride.
///
/// @param whole  The frame's samples.
/// @param a      One set's samples, the same length.
/// @param b      The other set's samples, the same length.
void expectSum(const std::vector<SampleValue>& whole, const std::vector<SampleValue>& a,
    const std::vector<SampleValue>& b) {
    for (std::size_t n = 0; n < whole.size(); n += 1001) {
        EXPECT_NEAR(whole[n], a[n] + b[n], 1e-6) << n;
    }
}

/// The first frame error in a report, for an assertion message.
///
/// @param report  A rendering report.
/// @return The first failed frame's number and error; empty when none failed.
std::string firstError(const RenderingReport& report) {
    for (const FrameOutcome& frame : report.frames) {
        if (frame.error.has_value()) {
            return "frame " + std::to_string(frame.frame.value()) + ": " + *frame.error;
        }
    }
    return {};
}

/// A request for the given frames, all partials.
///
/// @param frames  Frame numbers.
/// @return The request.
RenderRequest requestFor(const std::vector<std::int32_t>& frames) {
    RenderRequest request;
    request.file = "test.yaml";
    for (const std::int32_t frame : frames) {
        request.frames.push_back(FrameRequest{.frameNumber = frame, .labels = {}, .line = 0});
    }
    return request;
}

/// An in-memory engine-db with the real schema and a 48 kHz EngineConfig.
class RenderingDb : public ::testing::Test {
protected:
    /// Applies the schema and the config.
    void SetUp() override {
        ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok())
            << "the engine schema did not load";
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    }

    /// Adds a partial set.
    ///
    /// @param name  PartialSet.name.
    void addSet(const std::string& name) {
        db.execute("CREATE (:PartialSet {name: '" + name + "'});");
    }

    /// Adds a partial with both timelines to a set.
    ///
    /// @param set        Name of an existing PartialSet.
    /// @param name       Partial.name, unique in the test.
    /// @param frequency  (time, radians per sample) points, at least two.
    /// @param amplitude  (time, dB) points, at least two, spanning the same
    ///                   times.
    void addPartial(const std::string& set, const std::string& name,
        const std::vector<Point>& frequency, const std::vector<Point>& amplitude) {
        db.execute("MATCH (s:PartialSet {name: '" + set +
                   "'}) CREATE (s)-[:AGGREGATE_OF_ONE_MANY]->"
                   "(:Partial {name: '" +
                   name + "'});");
        std::string cypher =
            "MATCH (p:Partial {name: '" + name +
            "'}) CREATE (p)-[:HAS_TIMELINE]->(t:FrequencyTimeline) WITH t CREATE (t)";
        for (const auto& [time, frequencyValue] : frequency) {
            cypher += "-[:NEXT_POINT]->(:FrequencyTimelinePoint {time: " + literal(time) +
                      ", frequency: " + literal(frequencyValue) + ", frequency_curve_type: 0})";
        }
        db.execute(cypher + ";");
        cypher = "MATCH (p:Partial {name: '" + name +
                 "'}) CREATE (p)-[:HAS_TIMELINE]->(t:AmplitudeTimeline) WITH t CREATE (t)";
        for (const auto& [time, decibels] : amplitude) {
            cypher += "-[:NEXT_POINT]->(:AmplitudeTimelinePoint {time: " + literal(time) +
                      ", amplitude: " + literal(decibels) + ", amplitude_curve_type: 0})";
        }
        db.execute(cypher + ";");
    }

    /// Precomputes, failing the test if any partial fails.
    void precompute() {
        const auto report = precomputeAll(db);
        ASSERT_TRUE(report.ok()) << report.partials.front().error.value_or("");
    }

    /// Renders `frames` into a fresh directory named after the test.
    ///
    /// @param frames     Frame numbers.
    /// @param perSetWav  Whether to write per-set files.
    /// @return The report.
    RenderingReport render(const std::vector<std::int32_t>& frames, bool perSetWav = false) {
        dir = freshDirectory(::testing::UnitTest::GetInstance()->current_test_info()->name());
        return renderFrames(db, requestFor(frames), dir, perSetWav);
    }

    EngineDb db;
    std::filesystem::path dir;
};

TEST(Rendering, RendersTheWorkedExample) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_EXAMPLE_STATE).ok());
    ASSERT_TRUE(precomputeAll(db).ok());
    const std::filesystem::path dir = freshDirectory("Rendering.RendersTheWorkedExample");

    const RenderingReport report = renderFrames(db, requestFor({3}), dir, false);

    ASSERT_EQ(report.frames.size(), 1U);
    const FrameOutcome& frame = report.frames[0];
    ASSERT_TRUE(frame.ok()) << *frame.error;
    EXPECT_EQ(report.sampleRate.value(), 48000);
    EXPECT_EQ(report.frameSize.value(), 48000);
    EXPECT_EQ(frame.setCount, 1U);
    EXPECT_EQ(frame.partialCount, 1U);
    EXPECT_NEAR(frame.peak, 0.501187, 1e-5);
    EXPECT_EQ(frame.file, dir / "frame-000003.wav");

    const WavContents wav = readFloatWav(frame.file);
    EXPECT_EQ(wav.sampleRate, 48000U);
    EXPECT_EQ(wav.channels, 1U);
    ASSERT_EQ(wav.samples.size(), 48000U);
    EXPECT_NEAR(wav.samples[0], -0.43739, 1e-5);
    EXPECT_NEAR(wav.samples[1], -0.41078, 1e-5);
    EXPECT_NEAR(wav.samples[2], -0.38006, 1e-5);
}

TEST(Rendering, FileNames) {
    EXPECT_EQ(frameFileName(rainbohz::rendering_engine::FrameNumber{3}), "frame-000003.wav");
    EXPECT_EQ(frameFileName(rainbohz::rendering_engine::FrameNumber{1234567}), "frame-1234567.wav");
    EXPECT_EQ(
        setFileName(rainbohz::rendering_engine::FrameNumber{3}, 2), "frame-000003.set-000002.wav");
}

// Frames k and k+1 rendered in two separate runs join without a phase
// discontinuity: both agree with the closed form at their meeting point.
TEST_F(RenderingDb, FramesRenderedSeparatelyJoin) {
    addSet("S");
    // A glide, so that the phase at the join is not a multiple of anything.
    addPartial("S", "glide", {{0.0, 0.05}, {480000.0, 0.15}}, {{0.0, -6.0}, {480000.0, -6.0}});
    precompute();

    const RenderingReport three = render({3});
    ASSERT_TRUE(three.ok()) << *three.frames[0].error;
    const std::vector<SampleValue> a = readFloatWav(three.frames[0].file).samples;
    const RenderingReport four = renderFrames(db, requestFor({4}), dir, false);
    ASSERT_TRUE(four.ok()) << *four.frames[0].error;
    const std::vector<SampleValue> b = readFloatWav(four.frames[0].file).samples;

    const double g = 0.1 / 480000.0;
    const auto expected = [&](double t) {
        return 0.501187 * std::sin((0.05 * t) + (0.5 * g * t * t));
    };
    EXPECT_NEAR(a[47999], expected(191999.0), 2e-6);
    EXPECT_NEAR(b[0], expected(192000.0), 2e-6);
    EXPECT_NEAR(b[1], expected(192001.0), 2e-6);
}

TEST_F(RenderingDb, AFrameWithNothingInScopeIsAFullLengthSilentFile) {
    addSet("S");
    addPartial("S", "short", {{0.0, 0.1}, {1000.0, 0.1}}, {{0.0, -6.0}, {1000.0, -6.0}});
    precompute();

    // Frame 5 has no Frame node at all; frame 0 is the only one in scope.
    const RenderingReport report = render({5});

    ASSERT_TRUE(report.ok()) << *report.frames[0].error;
    EXPECT_EQ(report.frames[0].setCount, 0U);
    EXPECT_EQ(report.frames[0].partialCount, 0U);
    EXPECT_EQ(report.frames[0].peak, 0.0F);
    const WavContents wav = readFloatWav(report.frames[0].file);
    ASSERT_EQ(wav.samples.size(), 48000U);
    EXPECT_TRUE(std::ranges::all_of(wav.samples, [](SampleValue s) { return s == 0.0F; }));
}

TEST_F(RenderingDb, APartialThatFailsValidationFailsItsFramesAndNoOthers) {
    addSet("S");
    addPartial("S", "good", {{0.0, 0.1}, {200000.0, 0.1}}, {{0.0, -6.0}, {200000.0, -6.0}});
    addPartial("S", "bad", {{0.0, 0.1}, {60000.0, 0.1}}, {{0.0, -6.0}, {60000.0, -6.0}});
    precompute();
    // Break the bad partial's only segment after precomputation: a curve
    // component, which this version refuses.
    db.execute("MATCH (:Partial {name: 'bad'})-[:HAS_TIMELINE]->(:PartialSegmentTimeline)"
               "-[:NEXT_POINT]->(:PartialIntegrableSegment)-[:SEGMENT_TO_FREQUENCY_NOMINAL]->"
               "(n:PartialSegmentNominalFrequency) SET n.curve_component = true;");

    const RenderingReport report = render({0, 1, 2});

    ASSERT_EQ(report.frames.size(), 3U);
    EXPECT_FALSE(report.frames[0].ok());
    EXPECT_FALSE(report.frames[1].ok());
    EXPECT_TRUE(report.frames[2].ok()) << *report.frames[2].error;
    EXPECT_EQ(report.failureCount(), 2U);

    // Reported once, in the first frame it failed in, with the reason.
    ASSERT_EQ(report.frames[0].newFailures.size(), 1U);
    EXPECT_EQ(report.frames[0].newFailures[0].partialName, "bad");
    EXPECT_NE(report.frames[0].newFailures[0].reason.find("curve"), std::string::npos);
    EXPECT_TRUE(report.frames[1].newFailures.empty());
    EXPECT_EQ(report.failedPartialCount(), 1U);
    EXPECT_NE(report.frames[1].error->find("bad"), std::string::npos);

    // A failed frame has no file; the others' remain.
    EXPECT_FALSE(std::filesystem::exists(dir / "frame-000000.wav"));
    EXPECT_FALSE(std::filesystem::exists(dir / "frame-000001.wav"));
    EXPECT_TRUE(std::filesystem::exists(dir / "frame-000002.wav"));
}

// Long lists: the entry leads into the middle, and the walk covers the
// frame and agrees with the closed form.
TEST_F(RenderingDb, EntriesLeadIntoTheMiddleOfLongLists) {
    db.execute("MATCH (c:EngineConfig) SET c.frameSize = 1000;");
    addSet("S");
    std::vector<Point> frequency;
    std::vector<Point> amplitude;
    for (int i = 0; i <= 100; ++i) {
        frequency.emplace_back(1000.0 * i, 0.1 + (0.01 * (i % 5)));
        amplitude.emplace_back(1000.0 * i, (i % 2 == 0) ? -6.0 : -12.0);
    }
    addPartial("S", "long", frequency, amplitude);
    precompute();

    const RenderingReport report = render({50});

    ASSERT_TRUE(report.ok()) << *report.frames[0].error;
    const std::vector<SampleValue> out = readFloatWav(report.frames[0].file).samples;
    ASSERT_EQ(out.size(), 1000U);

    // Closed form at t = 50000 + n: phase is the sum over the 50 pieces
    // before, plus the piece within; amplitude ramps -6 -> -12 dB over the
    // frame (piece 50 goes from -6 at 50000 to -12 at 51000).
    double phaseAtStart = 0.0;
    for (int i = 0; i < 50; ++i) {
        const double f0 = 0.1 + (0.01 * (i % 5));
        const double f1 = 0.1 + (0.01 * ((i + 1) % 5));
        phaseAtStart += 0.5 * (f0 + f1) * 1000.0;
    }
    const double f0 = 0.1 + (0.01 * (50 % 5));
    const double f1 = 0.1 + (0.01 * (51 % 5));
    const double g = (f1 - f0) / 1000.0;
    for (const std::size_t n : {0U, 1U, 499U, 999U}) {
        const auto tau = static_cast<double>(n);
        const double phase = phaseAtStart + (f0 * tau) + (0.5 * g * tau * tau);
        const double decibels = -6.0 - (0.006 * tau);
        EXPECT_NEAR(out[n], std::sin(phase) * std::pow(10.0, decibels / 20.0), 2e-6) << n;
    }
}

TEST_F(RenderingDb, AStaleEntryIsDetected) {
    db.execute("MATCH (c:EngineConfig) SET c.frameSize = 1000;");
    addSet("S");
    addPartial("S", "p", {{0.0, 0.1}, {1000.0, 0.2}, {2000.0, 0.1}, {3000.0, 0.1}},
        {{0.0, -6.0}, {3000.0, -6.0}});
    precompute();
    // Re-point frame 2's entry at the first segment, which does not cover
    // 2000.
    db.execute("MATCH (:Frame {frame_number: 2})-[:FRAME_ENTRY]->(e:FrameEntry)"
               "-[r:FRAME_ENTRY_TIMELINE]->(:PartialIntegrableSegment) DELETE r;");
    db.execute("MATCH (:Frame {frame_number: 2})-[:FRAME_ENTRY]->(e:FrameEntry), "
               "(s:PartialIntegrableSegment) WHERE s.start_time = 0.0 "
               "CREATE (e)-[:FRAME_ENTRY_TIMELINE]->(s);");

    const RenderingReport report = render({1, 2});

    ASSERT_EQ(report.frames.size(), 2U);
    EXPECT_TRUE(report.frames[0].ok()) << *report.frames[0].error;
    ASSERT_FALSE(report.frames[1].ok());
    ASSERT_EQ(report.frames[1].newFailures.size(), 1U);
    EXPECT_NE(report.frames[1].newFailures[0].reason.find("stale"), std::string::npos);
}

TEST_F(RenderingDb, AnEntryWithoutAnAmplitudePointIsRefused) {
    addSet("S");
    addPartial("S", "p", {{0.0, 0.1}, {1000.0, 0.1}}, {{0.0, -6.0}, {1000.0, -6.0}});
    precompute();
    db.execute("MATCH (:FrameEntry)-[r:FRAME_ENTRY_TIMELINE]->(:AmplitudeTimelinePoint) DELETE r;");

    const RenderingReport report = render({0});
    ASSERT_FALSE(report.frames[0].ok());
    EXPECT_NE(report.frames[0].newFailures[0].reason.find("0 amplitude point"), std::string::npos);
}

TEST_F(RenderingDb, PerSetFilesSumToTheFrame) {
    addSet("A");
    addSet("B");
    addPartial("A", "a", {{0.0, 0.1}, {100000.0, 0.1}}, {{0.0, -6.0}, {100000.0, -6.0}});
    addPartial("B", "b", {{0.0, 0.3}, {100000.0, 0.3}}, {{0.0, -12.0}, {100000.0, -12.0}});
    addPartial("B", "b2", {{20000.0, 0.2}, {100000.0, 0.2}}, {{20000.0, -20.0}, {100000.0, -20.0}});
    precompute();

    const RenderingReport report = render({1}, true);

    ASSERT_TRUE(report.ok()) << *report.frames[0].error;
    const FrameOutcome& frame = report.frames[0];
    EXPECT_EQ(frame.setCount, 2U);
    EXPECT_EQ(frame.partialCount, 3U);
    ASSERT_EQ(frame.setFiles.size(), 2U);
    EXPECT_EQ(frame.setFiles[0].second.filename(), "frame-000001.set-000000.wav");
    EXPECT_EQ(frame.setFiles[1].second.filename(), "frame-000001.set-000001.wav");

    const std::vector<SampleValue> whole = readFloatWav(frame.file).samples;
    const std::vector<SampleValue> a = readFloatWav(frame.setFiles[0].second).samples;
    const std::vector<SampleValue> b = readFloatWav(frame.setFiles[1].second).samples;
    ASSERT_EQ(a.size(), 48000U);
    expectSum(whole, a, b);
    EXPECT_NEAR(*std::ranges::max_element(a), 0.501187, 1e-4);
}

TEST_F(RenderingDb, NoPerSetFilesByDefault) {
    addSet("S");
    addPartial("S", "p", {{0.0, 0.1}, {100000.0, 0.1}}, {{0.0, -6.0}, {100000.0, -6.0}});
    precompute();
    const RenderingReport report = render({0});
    ASSERT_TRUE(report.ok());
    EXPECT_TRUE(report.frames[0].setFiles.empty());
    EXPECT_FALSE(std::filesystem::exists(dir / "frame-000000.set-000000.wav"));
}

TEST_F(RenderingDb, ChannelModeOtherThanMonoIsFatal) {
    db.execute("MATCH (c:EngineConfig) SET c.channelMode = 1;");
    EXPECT_THROW(render({0}), RenderingError);
}

TEST_F(RenderingDb, NoEngineConfigIsFatal) {
    db.execute("MATCH (c:EngineConfig) DELETE c;");
    EXPECT_THROW(render({0}), RenderingError);
}

TEST_F(RenderingDb, ANullSampleRateIsFatal) {
    db.execute("MATCH (c:EngineConfig) SET c.sampleRate = NULL, c.frameSize = 1000;");
    EXPECT_THROW(render({0}), RenderingError);
}

TEST(Rendering, RendersTheTwoPartialSetsScore) {
    EngineDb db;
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok());
    ASSERT_TRUE(loadCypherFile(db, RE_TEST_TWO_SETS_STATE).ok());
    ASSERT_TRUE(precomputeAll(db).ok());
    const std::filesystem::path dir = freshDirectory("Rendering.TwoPartialSets");

    const RenderingReport report = renderFrames(db, requestFor({0, 1, 2, 3, 4, 6, 11}), dir, false);

    ASSERT_TRUE(report.ok()) << firstError(report);
    // Frame 3 holds both sets: A, B (mid-frame start), C and D.
    EXPECT_EQ(report.frames[3].setCount, 2U);
    EXPECT_EQ(report.frames[3].partialCount, 4U);
    // Frame 11 is past every partial.
    EXPECT_EQ(report.frames[6].setCount, 0U);
    EXPECT_EQ(report.frames[6].peak, 0.0F);
}

} // namespace
