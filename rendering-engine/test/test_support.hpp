// SPDX-License-Identifier: Apache-2.0

#pragma once

// Helpers shared by the rendering tests: reading back a WAV file by hand, and
// building a SoundSource with its spatial timelines.
//
// The reader parses the RIFF chunks itself and checks them as it goes, so
// that the container the engine writes is verified rather than trusted.

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/sample_value.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rainbohz::rendering_engine::test {

/// A WAV file read back: its header fields and its samples.
struct WavContents {
    /// fmt chunk: format tag, channels, sample rate, bits per sample.
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;

    /// The size of the fmt chunk's body, in bytes.
    std::uint32_t fmtChunkSize = 0;

    /// The chunk ids in file order, e.g. {"fmt ", "data"}.
    std::vector<std::string> chunks;

    /// The samples, interleaved as in the file.
    std::vector<SampleValue> samples;
};

/// Reads a little-endian unsigned integer of `N` bytes from `bytes` at `at`.
///
/// @tparam N     Number of bytes: 2 or 4.
/// @param bytes  The file.
/// @param at     Offset of the first byte.
/// @return The value.
template <std::size_t N>
std::uint32_t readLittleEndian(const std::vector<unsigned char>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < N; ++i) {
        value |= static_cast<std::uint32_t>(bytes.at(at + i)) << (8 * i);
    }
    return value;
}

/// Reads the fields of a fmt chunk into `wav`.
///
/// @param bytes     The file.
/// @param body      Offset of the chunk's body.
/// @param size      The chunk's size.
/// @param[out] wav  Receives the fields.
inline void readFmtChunk(const std::vector<unsigned char>& bytes, std::size_t body,
    std::uint32_t size, WavContents& wav) {
    wav.fmtChunkSize = size;
    wav.formatTag = static_cast<std::uint16_t>(readLittleEndian<2>(bytes, body));
    wav.channels = static_cast<std::uint16_t>(readLittleEndian<2>(bytes, body + 2));
    wav.sampleRate = readLittleEndian<4>(bytes, body + 4);
    wav.byteRate = readLittleEndian<4>(bytes, body + 8);
    wav.blockAlign = static_cast<std::uint16_t>(readLittleEndian<2>(bytes, body + 12));
    wav.bitsPerSample = static_cast<std::uint16_t>(readLittleEndian<2>(bytes, body + 14));
}

/// Reads the samples of a data chunk into `wav`.
///
/// @param bytes     The file.
/// @param body      Offset of the chunk's body.
/// @param size      The chunk's size.
/// @param[out] wav  Receives the samples.
inline void readDataChunk(const std::vector<unsigned char>& bytes, std::size_t body,
    std::uint32_t size, WavContents& wav) {
    wav.samples.resize(size / sizeof(SampleValue));
    if (!wav.samples.empty()) {
        std::memcpy(wav.samples.data(), &bytes[body], wav.samples.size() * sizeof(SampleValue));
    }
}

/// Walks the chunks of a RIFF/WAVE file from offset 12, reading the fmt and
/// data chunks into `wav`.
///
/// @param bytes     The whole file.
/// @param file      Its path, for messages.
/// @param[out] wav  Receives the chunk ids, the fmt fields and the samples.
/// @return False, after failing the test, if a chunk overruns the file.
inline bool readChunks(
    const std::vector<unsigned char>& bytes, const std::filesystem::path& file, WavContents& wav) {
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::string id(bytes.begin() + static_cast<std::ptrdiff_t>(at),
            bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4);
        const std::uint32_t size = readLittleEndian<4>(bytes, at + 4);
        const std::size_t body = at + 8;
        if (body + size > bytes.size()) {
            ADD_FAILURE() << "chunk " << id << " of " << size << " bytes overruns " << file;
            return false;
        }
        wav.chunks.push_back(id);
        if (id == "fmt ") {
            readFmtChunk(bytes, body, size, wav);
        } else if (id == "data") {
            readDataChunk(bytes, body, size, wav);
        }
        at = body + size + (size % 2); // chunks are word-aligned
    }
    return true;
}

/// Checks the RIFF header: the tags, and a size matching the file.
///
/// @param bytes  The whole file, at least 12 bytes.
inline void expectRiffHeader(const std::vector<unsigned char>& bytes) {
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
    EXPECT_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), "WAVE");
    EXPECT_EQ(readLittleEndian<4>(bytes, 4) + 8, bytes.size()) << "RIFF size";
}

/// Checks that both required chunks were seen.
///
/// @param wav  The contents after readChunks().
inline void expectFmtAndData(const WavContents& wav) {
    EXPECT_NE(std::ranges::find(wav.chunks, "fmt "), wav.chunks.end()) << "no fmt chunk";
    EXPECT_NE(std::ranges::find(wav.chunks, "data"), wav.chunks.end()) << "no data chunk";
}

/// Reads a RIFF/WAVE file of 32-bit IEEE float samples by hand.
///
/// Fails the current test, with a message, on anything unexpected in the
/// container: a missing RIFF or WAVE tag, a RIFF size that does not match the
/// file, a chunk that overruns the file, or a missing fmt or data chunk.
///
/// @param file  The file to read.
/// @return Its contents. Partially filled if the test was failed.
inline WavContents readFloatWav(const std::filesystem::path& file) {
    WavContents wav;
    std::ifstream in{file, std::ios::binary};
    EXPECT_TRUE(in.is_open()) << "cannot open " << file;
    const std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    if (bytes.size() < 12) {
        ADD_FAILURE() << file << " is too short to be a WAV file";
        return wav;
    }
    expectRiffHeader(bytes);
    if (readChunks(bytes, file, wav)) {
        expectFmtAndData(wav);
    }
    return wav;
}

/// A fresh, empty directory under the system temporary directory, unique to
/// the calling test.
///
/// @param name  A name for the directory, e.g. the test's.
/// @return The directory, created empty; any earlier one of that name is
///         removed first.
inline std::filesystem::path freshDirectory(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("rendering_engine_tests-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// A (time, value) point of a spatial timeline: samples, and radians or
/// metres.
using SpatialTestPoint = std::pair<double, double>;

/// Where a test's source is when the test does not care: straight ahead,
/// level, 1 m away, at which the mono gain is exactly 1, from 0 to well past
/// any test's score. One function per timeline.
///
/// @return The timeline's two points.
inline std::vector<SpatialTestPoint> stillAzimuth() {
    return {{0.0, 0.0}, {1.0e12, 0.0}};
}

/// See stillAzimuth().
///
/// @return The timeline's two points.
inline std::vector<SpatialTestPoint> stillElevation() {
    return {{0.0, 0.0}, {1.0e12, 0.0}};
}

/// See stillAzimuth().
///
/// @return The timeline's two points.
inline std::vector<SpatialTestPoint> stillDistance() {
    return {{0.0, 1.0}, {1.0e12, 1.0}};
}

/// Formats a double so that Cypher reads it back exactly, as a DOUBLE.
///
/// @param value  A finite value.
/// @return The value with 17 significant digits and a decimal point.
inline std::string spatialLiteral(double value) {
    std::ostringstream out;
    out.precision(17);
    out << std::showpoint << value;
    return out.str();
}

/// Gives an existing SoundSource its three spatial timelines, each with
/// linear curves.
///
/// @param[in,out] db  Database to write.
/// @param name        SoundSource.name of an existing source, unique in the
///                    test.
/// @param azimuth     (time, radians) points, linked in order. Empty for no
///                    timeline at all.
/// @param elevation   (time, radians) points; empty for no timeline.
/// @param distance    (time, metres) points; empty for no timeline.
inline void addSpatialTimelines(EngineDb& db, const std::string& name,
    const std::vector<SpatialTestPoint>& azimuth = stillAzimuth(),
    const std::vector<SpatialTestPoint>& elevation = stillElevation(),
    const std::vector<SpatialTestPoint>& distance = stillDistance()) {
    const auto timeline = [&](const std::string& kind, const std::string& column,
                              const std::vector<SpatialTestPoint>& points) {
        if (points.empty()) {
            return;
        }
        std::string cypher = "MATCH (s:SoundSource {name: '";
        cypher += name + "'}) CREATE (s)-[:HAS_PHYSICAL_TIMELINE]->(t:";
        cypher += kind + "PhysicalTimeline) WITH t CREATE (t)";
        for (const auto& [time, value] : points) {
            cypher += "-[:NEXT_POINT]->(:" + kind + "PhysicalTimelinePoint {time: ";
            cypher += spatialLiteral(time) + ", " + column + ": ";
            cypher += spatialLiteral(value) + ", " + column + "_curve_type: 0})";
        }
        db.execute(cypher + ";");
    };
    timeline("Azimuth", "azimuth", azimuth);
    timeline("Elevation", "elevation", elevation);
    timeline("Distance", "distance", distance);
}

/// Creates a SoundSource with its three spatial timelines, as
/// addSpatialTimelines() gives them.
///
/// @param[in,out] db  Database to write.
/// @param name        SoundSource.name, unique in the test.
/// @param azimuth     (time, radians) points; empty for no timeline.
/// @param elevation   (time, radians) points; empty for no timeline.
/// @param distance    (time, metres) points; empty for no timeline.
inline void addSoundSource(EngineDb& db, const std::string& name,
    const std::vector<SpatialTestPoint>& azimuth = stillAzimuth(),
    const std::vector<SpatialTestPoint>& elevation = stillElevation(),
    const std::vector<SpatialTestPoint>& distance = stillDistance()) {
    db.execute("CREATE (:SoundSource {name: '" + name + "'});");
    addSpatialTimelines(db, name, azimuth, elevation, distance);
}

} // namespace rainbohz::rendering_engine::test
