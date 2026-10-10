// SPDX-License-Identifier: Apache-2.0

#pragma once

// The client's log, written to standard error.
//
// One line per message, `HH:MM:SS.mmm [level] [thread] message`, with local
// wall-clock time to the millisecond. The thread is a short name each thread
// sets for itself once (`main`, `network`, `playback`), so that the worker
// threads' lines can be told apart when they interleave. Lines are written whole,
// under a lock, so they never interleave mid-line.
//
// `debug` lines appear only once setDebugEnabled(true) has been called, which
// the command line's --debug does. `info`, `warn` and `error` always appear.
//
// Standard output is left for nothing: the client has no data output, and a
// script reading its exit status needs nothing more.

#include <cstdint>
#include <string>
#include <string_view>

namespace rainbohz::playback_client {

/// How important a log line is.
enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error };

/// Turns `debug` lines on or off for the whole process.
///
/// @param enabled  True to write debug lines.
/// @note Thread-safe; the setting is an atomic and takes effect for lines
///       logged after the call.
void setDebugEnabled(bool enabled) noexcept;

/// Whether `debug` lines are written.
///
/// Lets a caller skip building a costly debug message.
///
/// @return The last value given to setDebugEnabled(); false before any call.
bool debugEnabled() noexcept;

/// Names the calling thread in its log lines.
///
/// @param name  A short name; copied. A thread that never calls this is
///              logged as `?`.
/// @note Thread-local: it affects only the calling thread.
void setThreadName(std::string_view name);

/// Writes one log line to standard error, under the calling thread's name.
///
/// @param level    The line's level. A `Debug` line is dropped unless debug
///                 is enabled.
/// @param message  The message, without a trailing newline.
/// @note Side effect: writes to stderr and flushes it. Thread-safe. Not
///       real-time safe (locks and does I/O): never call it from the audio
///       callback.
void logLine(LogLevel level, std::string_view message);

/// Writes a number with a fixed number of decimals, for log messages.
///
/// @param value     The number.
/// @param decimals  Digits after the point; 0 or more.
/// @return The text, e.g. `0.125` for (0.125, 3). Independent of the locale.
std::string formatFixed(double value, int decimals);

} // namespace rainbohz::playback_client
