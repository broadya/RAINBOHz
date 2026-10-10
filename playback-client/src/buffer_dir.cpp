// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/playback_client/buffer_dir.hpp"

#include <filesystem>
#include <iomanip>
#include <locale>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

/// @file
/// @brief Makes, names and removes a run's buffer directory.
///
/// The run directory's name ends in six random letters and digits.
/// std::filesystem::create_directory reports whether it made the directory
/// or found one already there, which makes the choice of name race-free
/// without mkdtemp, and portable to Windows: a name already taken is simply
/// drawn again.
///
/// @see buffer_dir.hpp

namespace rainbohz::playback_client {

namespace {

/// The prefix of every run directory's name.
constexpr std::string_view kRunPrefix = "rainbohz-playback-";

/// How many names to try before giving up.
constexpr int kNameAttempts = 100;

/// A random suffix for a run directory's name.
///
/// @param random  The generator; advanced.
/// @return Six characters from [a-z0-9].
std::string randomSuffix(std::mt19937_64& random) {
    static constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<std::size_t> pick{0, kAlphabet.size() - 1};
    std::string suffix;
    for (int i = 0; i < 6; ++i) {
        suffix += kAlphabet[pick(random)];
    }
    return suffix;
}

} // namespace

std::string_view laneName(Lane lane) noexcept {
    return lane == Lane::A ? "a" : "b";
}

BufferDir::BufferDir(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        throw BufferDirError{"buffer directory root " + root.string() + " is not a directory"};
    }
    std::mt19937_64 random{std::random_device{}()};
    for (int attempt = 0; attempt < kNameAttempts; ++attempt) {
        std::filesystem::path candidate = root / (std::string{kRunPrefix} + randomSuffix(random));
        if (std::filesystem::create_directory(candidate, error)) {
            path_ = std::move(candidate);
            break;
        }
        if (error) {
            throw BufferDirError{
                "cannot make a buffer directory in " + root.string() + ": " + error.message()};
        }
    }
    if (path_.empty()) {
        throw BufferDirError{"cannot find a free buffer directory name in " + root.string()};
    }
    for (const Lane lane : {Lane::A, Lane::B}) {
        if (!std::filesystem::create_directory(laneDir(lane), error) && error) {
            throw BufferDirError{"cannot make " + laneDir(lane).string() + ": " + error.message()};
        }
    }
}

std::filesystem::path BufferDir::laneDir(Lane lane) const {
    return path_ / ("lane-" + std::string{laneName(lane)});
}

std::filesystem::path BufferDir::framePath(Lane lane, FrameNumber frame) const {
    std::ostringstream name;
    name.imbue(std::locale::classic());
    name << "frame-" << std::setfill('0') << std::setw(6) << frame.value() << ".wav";
    return laneDir(lane) / name.str();
}

std::filesystem::path BufferDir::partPath(Lane lane, FrameNumber frame) const {
    std::filesystem::path part = framePath(lane, frame);
    part += ".part";
    return part;
}

void BufferDir::remove() const {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error) {
        throw BufferDirError{"cannot remove " + path_.string() + ": " + error.message()};
    }
}

} // namespace rainbohz::playback_client
