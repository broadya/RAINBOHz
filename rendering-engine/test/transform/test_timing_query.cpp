// Tests for the timing queries behind server mode's GET calls.
//
// The parser is tested on its own. Every query is then tested against the
// real engine-db schema, in memory, with small tempo maps built here. No
// transform runs, so a map may have synchronisation edges, which a loaded
// score cannot have yet.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/transform/timing_query.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#ifndef RE_TEST_SCHEMA_FILE
#define RE_TEST_SCHEMA_FILE ""
#endif

namespace {

using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::requireInt64;
using rainbohz::rendering_engine::transform::BeatsTicks;
using rainbohz::rendering_engine::transform::ChannelMode;
using rainbohz::rendering_engine::transform::channelModeName;
using rainbohz::rendering_engine::transform::FramePosition;
using rainbohz::rendering_engine::transform::framePosition;
using rainbohz::rendering_engine::transform::kWalkChunk;
using rainbohz::rendering_engine::transform::parseBeatsTicks;
using rainbohz::rendering_engine::transform::readEngineParameters;
using rainbohz::rendering_engine::transform::readRootTempoMap;
using rainbohz::rendering_engine::transform::readTempoMapChildren;
using rainbohz::rendering_engine::transform::readTempoMapPoints;
using rainbohz::rendering_engine::transform::TempoMapNode;
using rainbohz::rendering_engine::transform::TempoMapPointPage;
using rainbohz::rendering_engine::transform::TimingQueryError;
using Kind = TimingQueryError::Kind;

// ---------------------------------------------------------------------------
// beats:ticks
// ---------------------------------------------------------------------------

TEST(ParseBeatsTicks, ReadsBeatsAndTicks) {
    const std::optional<BeatsTicks> at = parseBeatsTicks("12:480");
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->beats, 12);
    EXPECT_EQ(at->ticks, 480);
}

TEST(ParseBeatsTicks, ABareNumberIsBeats) {
    const std::optional<BeatsTicks> at = parseBeatsTicks("3");
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->beats, 3);
    EXPECT_EQ(at->ticks, 0);
}

TEST(ParseBeatsTicks, RefusesAnythingElse) {
    for (const char* text : {"", ":", "1:", ":1", "-1", "-1:0", "1:-1", "+1", "1.5", "1:2.0",
             "1:2:3", " 1", "1 ", "1e3", "x", "99999999999999999999"}) {
        EXPECT_FALSE(parseBeatsTicks(text).has_value()) << '"' << text << '"';
    }
}

TEST(ChannelModeName, SpellsEachMode) {
    EXPECT_EQ(channelModeName(ChannelMode::Mono), "mono");
    EXPECT_EQ(channelModeName(ChannelMode::Stereo), "stereo");
    EXPECT_EQ(channelModeName(ChannelMode::MidSide), "mid-side");
    EXPECT_EQ(channelModeName(ChannelMode::FoaAFormat), "foa-a-format");
    EXPECT_EQ(channelModeName(ChannelMode::FoaBFormat), "foa-b-format");
}

// ---------------------------------------------------------------------------
// Against engine-db
// ---------------------------------------------------------------------------

/// Expects `call` to throw a TimingQueryError of `kind`.
template <typename Call>
void expectFailure(Kind kind, Call call) {
    try {
        call();
        ADD_FAILURE() << "no TimingQueryError was thrown";
    } catch (const TimingQueryError& e) {
        EXPECT_EQ(e.kind(), kind) << e.what();
    }
}

/// One TempoMapPoint to create.
struct Point {
    std::int64_t position = 0;
    double tempo = 120.0;
    bool hold = true;
};

class TimingQueryDb : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(loadCypherFile(db, RE_TEST_SCHEMA_FILE).ok())
            << "the engine schema did not load";
    }

    /// Creates a TempoMap with its points, as one statement.
    ///
    /// @param name          The map's name.
    /// @param ticksPerBeat  Its resolution.
    /// @param points        Its points, in list order.
    /// @param ofScore       When set, the map is this Score's root.
    /// @return The map's id.
    std::int64_t addMap(const std::string& name, std::int64_t ticksPerBeat,
        const std::vector<Point>& points, std::optional<std::int64_t> ofScore = std::nullopt) {
        std::string cypher = "CREATE (m:TempoMap {name: '" + name +
                             "', ticks_per_beat: " + std::to_string(ticksPerBeat) + "})";
        if (!points.empty()) {
            cypher += " CREATE (m)";
            for (const Point& point : points) {
                cypher +=
                    "-[:NEXT_POINT]->(:TempoMapPoint {position: " + std::to_string(point.position) +
                    ", start_tempo: " + std::to_string(point.tempo) +
                    ", hold: " + (point.hold ? "true" : "false") + "})";
            }
        }
        db.execute(cypher + ";");
        const std::int64_t id = requireInt64(
            db.query("MATCH (m:TempoMap {name: '" + name + "'}) RETURN m.id;").at(0).at(0));
        if (ofScore.has_value()) {
            db.execute("MATCH (s:Score {id: " + std::to_string(*ofScore) + "}), (m:TempoMap {id: " +
                       std::to_string(id) + "}) CREATE (s)-[:NEXT_POINT]->(m);");
        }
        return id;
    }

    /// Creates a Score.
    ///
    /// @return Its id.
    std::int64_t addScore(const std::string& name) {
        db.execute("CREATE (:Score {name: '" + name + "'});");
        return requireInt64(
            db.query("MATCH (s:Score {name: '" + name + "'}) RETURN s.id;").at(0).at(0));
    }

    /// Joins a child map to a parent.
    void synchronise(
        std::int64_t child, std::int64_t parent, std::int64_t atParent, std::int64_t atChild) {
        db.execute("MATCH (c:TempoMap {id: " + std::to_string(child) +
                   "}), (p:TempoMap {id: " + std::to_string(parent) +
                   "}) CREATE (c)-[:TEMPO_MAP_SYNCHRONIZATION_POINT {sync_point_parent: " +
                   std::to_string(atParent) + ", sync_point_child: " + std::to_string(atChild) +
                   "}]->(p);");
    }

    /// Points at 0, 10, 20, ... held at 120 BPM.
    static std::vector<Point> evenPoints(int count) {
        std::vector<Point> points;
        points.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            points.push_back(Point{.position = std::int64_t{10} * i});
        }
        return points;
    }

    EngineDb db;
};

TEST_F(TimingQueryDb, TheRootIsTheScoresMapWithItsChildren) {
    const std::int64_t score = addScore("S");
    const std::int64_t root = addMap("root", 960, {Point{}}, score);
    const std::int64_t second = addMap("second", 480, {Point{}});
    const std::int64_t first = addMap("first", 96, {Point{}});
    synchronise(second, root, 3840, 0);
    synchronise(first, root, 0, 96);

    const TempoMapNode node = readRootTempoMap(db);
    EXPECT_EQ(node.map.id, root);
    EXPECT_EQ(node.map.name, "root");
    EXPECT_EQ(node.map.ticksPerBeat, 960);
    ASSERT_EQ(node.children.size(), 2U);
    // In child id order, each with its own edge's sync points.
    EXPECT_EQ(node.children[0].map.id, second);
    EXPECT_EQ(node.children[0].map.ticksPerBeat, 480);
    EXPECT_EQ(node.children[0].syncPointParent, 3840);
    EXPECT_EQ(node.children[0].syncPointChild, 0);
    EXPECT_EQ(node.children[1].map.id, first);
    EXPECT_EQ(node.children[1].syncPointParent, 0);
    EXPECT_EQ(node.children[1].syncPointChild, 96);

    EXPECT_TRUE(readTempoMapChildren(db, first).empty());
}

TEST_F(TimingQueryDb, TheRootNeedsExactlyOneScoreWithAMap) {
    expectFailure(Kind::NotFound, [&] { readRootTempoMap(db); });
    const std::int64_t score = addScore("S");
    expectFailure(Kind::NotFound, [&] { readRootTempoMap(db); });
    addMap("root", 960, {Point{}}, score);
    EXPECT_NO_THROW(readRootTempoMap(db));
    addScore("T");
    expectFailure(Kind::Unprocessable, [&] { readRootTempoMap(db); });
}

TEST_F(TimingQueryDb, AnUnknownMapIsNotFound) {
    expectFailure(Kind::NotFound, [&] { readTempoMapChildren(db, 42); });
    expectFailure(Kind::NotFound, [&] { readTempoMapPoints(db, 42, std::nullopt, 10); });
    expectFailure(Kind::NotFound, [&] { framePosition(db, 42, BeatsTicks{}); });
}

TEST_F(TimingQueryDb, PagesWalkTheWholeListAcrossChunks) {
    // More points than several walk chunks, and not a multiple of one.
    const int count = (3 * kWalkChunk) + 7;
    const std::int64_t map = addMap("long", 960, evenPoints(count));

    std::vector<std::int64_t> seen;
    std::optional<std::int64_t> after;
    int pages = 0;
    for (bool more = true; more; ++pages) {
        const TempoMapPointPage page = readTempoMapPoints(db, map, after, 10);
        EXPECT_EQ(page.map.id, map);
        for (const auto& point : page.points) {
            seen.push_back(*point.position);
        }
        after = page.nextAfter;
        more = after.has_value();
    }

    ASSERT_EQ(seen.size(), static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(seen[static_cast<std::size_t>(i)], 10 * i);
    }
    EXPECT_EQ(pages, (count + 9) / 10);
}

TEST_F(TimingQueryDb, APageThatEndsTheListHasNoNext) {
    const std::int64_t map = addMap("five", 960, evenPoints(5));
    EXPECT_FALSE(readTempoMapPoints(db, map, std::nullopt, 5).nextAfter.has_value());
    EXPECT_EQ(readTempoMapPoints(db, map, std::nullopt, 4).nextAfter, 30);
    EXPECT_EQ(readTempoMapPoints(db, map, std::nullopt, 1000).points.size(), 5U);
}

TEST_F(TimingQueryDb, TheCursorIsExclusiveAndNeedNotBeAPosition) {
    const std::int64_t map = addMap("five", 960, evenPoints(5));
    TempoMapPointPage page = readTempoMapPoints(db, map, 20, 10);
    ASSERT_EQ(page.points.size(), 2U);
    EXPECT_EQ(page.points[0].position, 30);

    page = readTempoMapPoints(db, map, 25, 10);
    ASSERT_EQ(page.points.size(), 2U);
    EXPECT_EQ(page.points[0].position, 30);

    page = readTempoMapPoints(db, map, 40, 10);
    EXPECT_TRUE(page.points.empty());
    EXPECT_FALSE(page.nextAfter.has_value());
}

TEST_F(TimingQueryDb, AMapWithNoPointsHasAnEmptyPage) {
    const std::int64_t map = addMap("empty", 960, {});
    const TempoMapPointPage page = readTempoMapPoints(db, map, std::nullopt, 10);
    EXPECT_TRUE(page.points.empty());
    EXPECT_FALSE(page.nextAfter.has_value());
}

TEST_F(TimingQueryDb, PositionsMustOrderTheList) {
    const std::int64_t map = addMap(
        "unordered", 960, {Point{.position = 0}, Point{.position = 20}, Point{.position = 10}});
    expectFailure(Kind::Unprocessable, [&] { readTempoMapPoints(db, map, std::nullopt, 10); });

    const std::int64_t unplaced = addMap("unplaced", 960, evenPoints(2));
    db.execute("MATCH (m:TempoMap {id: " + std::to_string(unplaced) +
               "})-[:NEXT_POINT]->(:TempoMapPoint)-[:NEXT_POINT]->(p:TempoMapPoint) "
               "SET p.position = NULL;");
    expectFailure(Kind::Unprocessable, [&] { readTempoMapPoints(db, unplaced, std::nullopt, 10); });
}

TEST_F(TimingQueryDb, AListThatLoopsIsRefused) {
    const std::int64_t map = addMap("loop", 960, evenPoints(3));
    db.execute("MATCH (m:TempoMap {id: " + std::to_string(map) +
               "})-[:NEXT_POINT]->(head:TempoMapPoint)-[:NEXT_POINT]->(:TempoMapPoint)"
               "-[:NEXT_POINT]->(last:TempoMapPoint) CREATE (last)-[:NEXT_POINT]->(head);");
    expectFailure(Kind::Unprocessable, [&] { readTempoMapPoints(db, map, std::nullopt, 10); });
}

TEST_F(TimingQueryDb, EngineParametersResolveTheFrameSize) {
    db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    auto parameters = readEngineParameters(db);
    EXPECT_EQ(parameters.sampleRate.value(), 48000);
    EXPECT_EQ(parameters.frameSize.value(), 48000);
    EXPECT_EQ(parameters.channelMode, ChannelMode::Mono);

    db.execute("MATCH (c:EngineConfig) SET c.frameSize = 1024, c.channelMode = 4;");
    parameters = readEngineParameters(db);
    EXPECT_EQ(parameters.frameSize.value(), 1024);
    EXPECT_EQ(parameters.channelMode, ChannelMode::FoaBFormat);

    db.execute("MATCH (c:EngineConfig) SET c.channelMode = 2;");
    EXPECT_EQ(readEngineParameters(db).channelMode, ChannelMode::MidSide);
}

TEST_F(TimingQueryDb, UnusableEngineParametersAreRefused) {
    expectFailure(Kind::Unprocessable, [&] { readEngineParameters(db); });
    db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 5});");
    expectFailure(Kind::Unprocessable, [&] { readEngineParameters(db); });
    db.execute("MATCH (c:EngineConfig) SET c.channelMode = 1, c.sampleRate = NULL;");
    expectFailure(Kind::Unprocessable, [&] { readEngineParameters(db); });
    db.execute("MATCH (c:EngineConfig) SET c.sampleRate = 48000, c.frameSize = 0;");
    expectFailure(Kind::Unprocessable, [&] { readEngineParameters(db); });
    db.execute("MATCH (c:EngineConfig) SET c.frameSize = NULL;");
    EXPECT_NO_THROW(readEngineParameters(db));
    db.execute("CREATE (:EngineConfig {sampleRate: 44100, channelMode: 0});");
    expectFailure(Kind::Unprocessable, [&] { readEngineParameters(db); });
}

/// A score whose map makes a tick a sample: 60 BPM, as many ticks per beat as
/// samples per second.
class SampleTicks : public TimingQueryDb {
protected:
    void SetUp() override {
        TimingQueryDb::SetUp();
        db.execute("CREATE (:EngineConfig {sampleRate: 48000, frameSize: 1000, channelMode: 0});");
        map = addMap("samples", 48000, {Point{.position = 0, .tempo = 60.0}}, addScore("S"));
    }

    std::int64_t map = 0;
};

TEST_F(SampleTicks, AHeldTempoGivesTheExactSample) {
    // Positions whose seconds are exact binary fractions, so no rounding
    // can intervene: 2.5 s is sample 120000, frame 120 of 1000 samples.
    FramePosition at = framePosition(db, map, BeatsTicks{.beats = 2, .ticks = 24000});
    EXPECT_EQ(at.frame.value(), 120);
    EXPECT_EQ(at.offset.value(), 0);

    at = framePosition(db, map, BeatsTicks{.beats = 0, .ticks = 750});
    EXPECT_EQ(at.frame.value(), 0);
    EXPECT_EQ(at.offset.value(), 750);

    at = framePosition(db, map, BeatsTicks{});
    EXPECT_EQ(at.frame.value(), 0);
    EXPECT_EQ(at.offset.value(), 0);
}

TEST_F(SampleTicks, TicksMustBeBelowTheResolution) {
    expectFailure(
        Kind::BadRequest, [&] { framePosition(db, map, BeatsTicks{.beats = 0, .ticks = 48000}); });
    expectFailure(Kind::BadRequest,
        [&] { framePosition(db, map, BeatsTicks{.beats = std::int64_t{1} << 62, .ticks = 0}); });
}

TEST_F(SampleTicks, APositionBeyondTheLastFrameIsRefused) {
    // 10^12 seconds is far beyond INT32 frames of 1000 samples.
    expectFailure(Kind::Unprocessable,
        [&] { framePosition(db, map, BeatsTicks{.beats = 1'000'000'000'000, .ticks = 0}); });
}

TEST_F(SampleTicks, OnlyARootMapConverts) {
    const std::int64_t child = addMap("child", 960, {Point{}});
    synchronise(child, map, 0, 0);
    expectFailure(Kind::Unprocessable, [&] { framePosition(db, child, BeatsTicks{}); });
}

TEST_F(TimingQueryDb, APositionBetweenSamplesGivesTheOneBefore) {
    // Two ticks per sample: tick 1 is half a sample in, tick 3 one and a half.
    db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    const std::int64_t map =
        addMap("half", 96000, {Point{.position = 0, .tempo = 60.0}}, addScore("S"));
    EXPECT_EQ(framePosition(db, map, BeatsTicks{.beats = 0, .ticks = 1}).offset.value(), 0);
    EXPECT_EQ(framePosition(db, map, BeatsTicks{.beats = 0, .ticks = 3}).offset.value(), 1);
    // A whole second in is the first sample of frame 1, a frame being one
    // second when frameSize is NULL.
    const FramePosition second = framePosition(db, map, BeatsTicks{.beats = 1, .ticks = 0});
    EXPECT_EQ(second.frame.value(), 1);
    EXPECT_EQ(second.offset.value(), 0);
}

TEST_F(TimingQueryDb, ARampFollowsTheTempoMap) {
    // tempo-map-score's map: 120 BPM ramping to 60 over four beats of 960
    // ticks. One beat in, the tempo is 105, and the seconds elapsed are
    // 60 · 4 · ln(105 / 120) / (60 − 120) (docs/transform.md section 6.1).
    db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    const std::int64_t map = addMap("ramp", 960,
        {Point{.position = 0, .tempo = 120.0, .hold = false},
            Point{.position = 3840, .tempo = 60.0, .hold = true}},
        addScore("S"));
    const double seconds = 60.0 * 4.0 * std::log(105.0 / 120.0) / (60.0 - 120.0);
    const auto expected = static_cast<std::int32_t>(std::floor(seconds * 48000.0));

    const FramePosition at = framePosition(db, map, BeatsTicks{.beats = 1, .ticks = 0});
    EXPECT_EQ(at.frame.value(), 0);
    EXPECT_EQ(at.offset.value(), expected);

    // Beat 6 is 4 ln 2 seconds for the ramp plus two held beats at 60 BPM.
    const double later = (4.0 * std::numbers::ln2) + 2.0;
    const auto laterSample = static_cast<std::int64_t>(std::floor(later * 48000.0));
    const FramePosition six = framePosition(db, map, BeatsTicks{.beats = 6, .ticks = 0});
    EXPECT_EQ(six.frame.value(), laterSample / 48000);
    EXPECT_EQ(six.offset.value(), laterSample % 48000);
}

TEST_F(TimingQueryDb, AnInvalidMapDoesNotConvert) {
    db.execute("CREATE (:EngineConfig {sampleRate: 48000, channelMode: 0});");
    // The last point of a valid map holds.
    const std::int64_t map = addMap("ramp-to-nowhere", 960,
        {Point{.position = 0, .tempo = 120.0, .hold = false}}, addScore("S"));
    expectFailure(Kind::Unprocessable, [&] { framePosition(db, map, BeatsTicks{}); });
}

} // namespace
