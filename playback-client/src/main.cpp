#include "rainbohz/playback_client/audio_sink.hpp"
#include "rainbohz/playback_client/buffer_dir.hpp"
#include "rainbohz/playback_client/log.hpp"
#include "rainbohz/playback_client/object_fetch.hpp"
#include "rainbohz/playback_client/options.hpp"
#include "rainbohz/playback_client/playback_api.hpp"
#include "rainbohz/playback_client/playback_session.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

/// @file
/// @brief The playback-client entry point.
///
/// Parses the command line, plans the range with the server, makes the
/// buffer directory and the audio output, plays the session, and removes the
/// buffer unless --keep-buffer was given. Exit status: 0 when every loop was
/// played, 1 on any failure or interruption, 2 on a usage error.
///
/// SIGINT and SIGTERM only set an atomic flag, which is all a signal handler
/// may safely do; the session polls it and stops in order.

namespace {

namespace pc = rainbohz::playback_client;

// Set by the signal handler; polled by the session. A signal handler can
// reach nothing but a global, and may touch nothing but a lock-free atomic.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> gInterrupted{false};

static_assert(std::atomic<bool>::is_always_lock_free, "the signal handler needs a lock-free flag");

/// The SIGINT and SIGTERM handler: asks the session to stop.
///
/// @param signal  The signal; unused.
extern "C" void onSignal(int signal) {
    (void)signal;
    gInterrupted.store(true);
}

/// The process exit status for a runtime failure or interruption.
constexpr int kFailure = 1;

/// Installs onSignal() for SIGINT and SIGTERM.
///
/// @note Side effect: changes the process's signal dispositions; logs a
///       warning for a signal whose handler cannot be installed, which then
///       keeps its default (terminating) action.
void installSignalHandlers() {
    for (const int signal : {SIGINT, SIGTERM}) {
        if (std::signal(signal, onSignal) == SIG_ERR) {
            pc::logLine(
                pc::LogLevel::Warn, "cannot handle signal " + std::to_string(signal) +
                                        "; it will end the client without cleaning up the buffer");
        }
    }
}

/// Plays the session the options describe, and cleans up after it.
///
/// @param options  The validated command line.
/// @return 0 when every loop was played; kFailure otherwise.
/// @note Side effects: REST calls, the buffer directory (made and, unless
///       --keep-buffer, removed), audio or --output-wav, and the log.
int runClient(const pc::Options& options) {
    pc::setDebugEnabled(options.debug);
    pc::setThreadName("main");
    installSignalHandlers();

    std::optional<pc::BufferDir> buffer;
    int status = kFailure;
    try {
        const std::unique_ptr<pc::PlaybackApi> api = pc::makeHttpPlaybackApi(options.serverUrl);
        const pc::SessionPlan plan = pc::planSession(*api, options.start, options.end);

        buffer.emplace(options.bufferRoot);
        pc::logLine(pc::LogLevel::Info, "buffer: " + buffer->path().string());

        const std::unique_ptr<pc::AudioSink> sink =
            options.outputWav.has_value()
                ? pc::makeWavFileSink(*options.outputWav, plan.engine.sampleRate)
                : pc::makeDeviceSink(plan.engine.sampleRate);
        const std::unique_ptr<pc::ObjectFetcher> fetcher = pc::makeObjectFetcher();

        const pc::SessionResult result =
            pc::playSession(*api, *fetcher, *sink, plan, *buffer, options.loops, gInterrupted);
        if (result.error.has_value()) {
            pc::logLine(pc::LogLevel::Error, *result.error);
        } else if (result.interrupted) {
            pc::logLine(pc::LogLevel::Info, "stopped after " + std::to_string(result.loopsPlayed) +
                                                " of " + std::to_string(options.loops) +
                                                " loop(s)");
        } else {
            pc::logLine(pc::LogLevel::Info,
                "played " + std::to_string(result.loopsPlayed) + " loop(s)" +
                    (options.outputWav.has_value() ? " to " + options.outputWav->string() : ""));
            status = 0;
        }
    } catch (const std::exception& error) {
        pc::logLine(pc::LogLevel::Error, error.what());
    }

    if (buffer.has_value()) {
        if (options.keepBuffer) {
            pc::logLine(pc::LogLevel::Info, "kept buffer " + buffer->path().string());
        } else {
            try {
                buffer->remove();
                pc::logLine(pc::LogLevel::Debug, "removed buffer " + buffer->path().string());
            } catch (const std::exception& error) {
                pc::logLine(pc::LogLevel::Error, error.what());
                status = kFailure;
            }
        }
    }
    return status;
}

} // namespace

/// Runs playback-client.
///
/// Any exception that escapes is reported on standard error as one line and
/// turned into kFailure, so the process never ends by std::terminate().
///
/// @param argc  Argument count.
/// @param argv  Arguments; see options.hpp.
/// @return The exit status: 0, 1 or 2, as above.
int main(int argc, char** argv) {
    try {
        const pc::ParseResult parsed = pc::parseCommandLine(argc, argv);
        if (!parsed.options.has_value()) {
            return parsed.exitCode;
        }
        return runClient(*parsed.options);
    } catch (const std::exception& error) {
        std::cerr << "playback-client: " << error.what() << '\n';
        return kFailure;
    }
}
