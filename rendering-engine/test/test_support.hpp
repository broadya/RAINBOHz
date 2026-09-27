#pragma once

// Helpers shared by the rendering tests: reading back a WAV file by hand.
//
// The reader parses the RIFF chunks itself and checks them as it goes, so
// that the container the engine writes is verified rather than trusted.

#include "rainbohz/rendering_engine/sample_value.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

} // namespace rainbohz::rendering_engine::test
