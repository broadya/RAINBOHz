#include "rainbohz/playback_client/log.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

/// @file
/// @brief Writes the client's log lines to standard error.
///
/// @see log.hpp

namespace rainbohz::playback_client {

namespace {

/// Whether debug lines are written. Process-wide, set from --debug.
///
/// A function-local static, so that it is initialised on first use.
///
/// @return The flag, valid for the whole program.
std::atomic<bool>& debugFlag() noexcept {
    static std::atomic<bool> flag{false};
    return flag;
}

/// The calling thread's name, set by setThreadName().
///
/// @return This thread's name, `?` until set. Valid for the thread's life.
std::string& threadName() {
    thread_local std::string name = "?";
    return name;
}

/// Serialises whole lines, so that the threads' lines never interleave.
///
/// @return The mutex, valid for the whole program.
std::mutex& logMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

/// The name a level is written with.
///
/// @param level  The level.
/// @return Its lower-case name, padded to five characters. A string literal.
std::string_view levelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info ";
    case LogLevel::Warn:
        return "warn ";
    case LogLevel::Error:
        return "error";
    }
    return "?    ";
}

/// The current local time, `HH:MM:SS.mmm`.
///
/// @return The time of day, to the millisecond.
std::string timeOfDay() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setfill('0') << std::setw(2) << local.tm_hour << ':' << std::setw(2)
         << local.tm_min << ':' << std::setw(2) << local.tm_sec << '.' << std::setw(3) << millis;
    return text.str();
}

} // namespace

void setDebugEnabled(bool enabled) noexcept {
    debugFlag().store(enabled);
}

bool debugEnabled() noexcept {
    return debugFlag().load();
}

void setThreadName(std::string_view name) {
    threadName() = std::string{name};
}

void logLine(LogLevel level, std::string_view message) {
    if (level == LogLevel::Debug && !debugEnabled()) {
        return;
    }
    std::string line = timeOfDay();
    line += " [";
    line += levelName(level);
    line += "] [";
    line += threadName();
    line += "] ";
    line += message;
    line += '\n';

    const std::scoped_lock lock{logMutex()};
    std::cerr << line << std::flush;
}

std::string formatFixed(double value, int decimals) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(decimals) << value;
    return text.str();
}

} // namespace rainbohz::playback_client
