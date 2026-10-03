#include "rainbohz/playback_client/playback_session.hpp"

#include "rainbohz/playback_client/feeder.hpp"
#include "rainbohz/playback_client/lane_board.hpp"
#include "rainbohz/playback_client/log.hpp"
#include "rainbohz/playback_client/network_worker.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

/// @file
/// @brief Plans a session with the server, and runs its threads.
///
/// @see playback_session.hpp

namespace rainbohz::playback_client {

namespace {

/// How often the main thread looks at the interrupt flag and the threads.
constexpr std::chrono::milliseconds kPollInterval{50};

/// Checks a position's ticks against the map's resolution.
///
/// @param which     `start` or `end`, for the diagnostic.
/// @param position  The position.
/// @param map       The map it is read on.
/// @throws SessionError if the map has a resolution and the ticks are not
///         below it. A map without one is left to the server to judge.
void checkTicks(const char* which, const MusicalPosition& position, const TempoMapSummary& map) {
    if (map.ticksPerBeat.has_value() && !validTicks(position, *map.ticksPerBeat)) {
        throw SessionError{std::string{"--"} + which + " " + formatMusicalPosition(position) +
                           ": " + std::to_string(position.ticks) + " ticks is not below the " +
                           "tempo map's " + std::to_string(*map.ticksPerBeat) + " ticks per beat"};
    }
}

} // namespace

SessionPlan planSession(
    PlaybackApi& api, const MusicalPosition& start, const MusicalPosition& end) {
    const EngineParameters engine = api.engineParameters();
    if (engine.channelMode != "mono") {
        throw SessionError{
            "the score's channel mode is " + engine.channelMode + "; this version plays mono only"};
    }
    const TempoMapSummary map = api.rootTempoMap();
    checkTicks("start", start, map);
    checkTicks("end", end, map);

    const FrameOffset first = api.framePosition(map.id, start);
    const FrameOffset last = api.framePosition(map.id, end);
    FramePlan frames;
    try {
        frames = planFrames(first, last, engine.frameSize);
    } catch (const FramePlanError& error) {
        throw SessionError{formatMusicalPosition(start) + " to " + formatMusicalPosition(end) +
                           ": " + error.what()};
    }

    const SampleCount length = frames.length();
    logLine(LogLevel::Info, "score: " + std::to_string(engine.sampleRate.value()) +
                                " Hz, frames of " + std::to_string(engine.frameSize.value()) +
                                " samples; tempo map " + std::to_string(map.id) +
                                (map.name.empty() ? "" : " (" + map.name + ")"));
    logLine(LogLevel::Info,
        "range " + formatMusicalPosition(start) + " to " + formatMusicalPosition(end) + ": frame " +
            std::to_string(first.frame.value()) + " +" + std::to_string(first.offset) +
            " to frame " + std::to_string(last.frame.value()) + " +" + std::to_string(last.offset) +
            ", " + std::to_string(frames.slices.size()) + " frame(s), " +
            std::to_string(length.value()) + " samples (" +
            formatFixed(length.seconds(engine.sampleRate), 3) + " s)");
    return SessionPlan{
        .engine = engine, .tempoMap = map, .start = first, .end = last, .frames = frames};
}

SessionResult playSession(PlaybackApi& api, ObjectFetcher& fetcher, AudioSink& sink,
    const SessionPlan& plan, const BufferDir& buffer, std::int64_t loops,
    const std::atomic<bool>& interrupt) {
    LaneBoard board{plan.frames.slices.size()};
    sink.start();

    const NetworkSettings networkSettings{.fills = loops, .sampleRate = plan.engine.sampleRate};
    const FeederSettings feederSettings{.loops = loops, .sampleRate = plan.engine.sampleRate};

    std::atomic<bool> feederDone{false};
    FeederReport report;
    std::thread network{
        [&] { runNetwork(board, api, fetcher, plan.frames, buffer, networkSettings); }};
    std::thread feeder{[&] {
        report = runFeeder(board, plan.frames, buffer, sink, feederSettings);
        feederDone.store(true);
    }};

    SessionResult result;
    while (!feederDone.load()) {
        if (interrupt.load() && !board.stopped()) {
            logLine(LogLevel::Info, "interrupted; stopping");
            result.interrupted = true;
            board.requestStop();
        }
        if (board.stopped()) {
            // Whatever stopped it, the output must not keep the feeder
            // waiting for room in the ring.
            sink.abort();
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    feeder.join();
    if (!report.finished) {
        // The feeder stopped early, so no more fills are wanted. The network
        // thread may be waiting for a lane, or in a call; stop wakes the
        // first, and the second ends at the call's own timeout.
        board.requestStop();
    }
    network.join();

    // The board's count, not the report's: a feeder that failed returns an
    // empty report, but the loops it played before failing are still counted.
    result.loopsPlayed = board.loopsPlayed();
    result.error = board.error();
    return result;
}

} // namespace rainbohz::playback_client
