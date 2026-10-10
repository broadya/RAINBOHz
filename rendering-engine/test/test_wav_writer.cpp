// SPDX-License-Identifier: Apache-2.0

// Tests for the WAV writer.
//
// A written file is read back through dr_wav and compared sample for sample,
// and its raw bytes are checked by hand, so that the container is verified
// rather than trusted (ADR 0009).

#include "rainbohz/rendering_engine/wav_writer.hpp"

#include "test_support.hpp"

#include <dr_wav.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace {

using rainbohz::rendering_engine::SampleRate;
using rainbohz::rendering_engine::SampleValue;
using rainbohz::rendering_engine::WavWriteError;
using rainbohz::rendering_engine::writeFloatWav;
using rainbohz::rendering_engine::test::freshDirectory;
using rainbohz::rendering_engine::test::readFloatWav;
using rainbohz::rendering_engine::test::WavContents;

TEST(WavWriter, WritesAMonoFileThatDrWavReadsBack) {
    const std::filesystem::path dir = freshDirectory("WavWriter.ReadsBack");
    const std::filesystem::path file = dir / "mono.wav";
    const std::vector<SampleValue> samples{0.0F, 0.5F, -0.5F, 1.0F, -1.0F, 0.25F};
    writeFloatWav(file, SampleRate{48000}, 1, samples);

    drwav wav{};
    ASSERT_TRUE(drwav_init_file(&wav, file.string().c_str(), nullptr));
    EXPECT_EQ(wav.channels, 1U);
    EXPECT_EQ(wav.sampleRate, 48000U);
    EXPECT_EQ(wav.bitsPerSample, 32U);
    EXPECT_EQ(wav.translatedFormatTag, DR_WAVE_FORMAT_IEEE_FLOAT);
    EXPECT_EQ(wav.totalPCMFrameCount, samples.size());
    std::vector<float> back(samples.size());
    EXPECT_EQ(drwav_read_pcm_frames_f32(&wav, samples.size(), back.data()), samples.size());
    drwav_uninit(&wav);
    EXPECT_EQ(back, samples);
}

// The raw chunk layout. This pins what dr_wav actually writes for float data:
// a 16-byte fmt chunk with format tag 3 and no fact chunk. The WAVE
// specification asks for an 18-byte fmt chunk and a fact chunk for non-PCM
// data; readers in practice do not require either, and dr_wav does not write
// them (docs/rendering.md section 5). A change in this layout is a change in
// the dependency worth noticing.
TEST(WavWriter, TheContainerIsRiffWaveFloat32) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Container");
    const std::filesystem::path file = dir / "container.wav";
    const std::vector<SampleValue> samples{0.125F, -0.125F, 0.75F};
    writeFloatWav(file, SampleRate{44100}, 1, samples);

    const WavContents wav = readFloatWav(file);
    EXPECT_EQ(wav.chunks, (std::vector<std::string>{"fmt ", "data"}));
    EXPECT_EQ(wav.fmtChunkSize, 16U);
    EXPECT_EQ(wav.formatTag, 3U);
    EXPECT_EQ(wav.channels, 1U);
    EXPECT_EQ(wav.sampleRate, 44100U);
    EXPECT_EQ(wav.byteRate, 44100U * 4U);
    EXPECT_EQ(wav.blockAlign, 4U);
    EXPECT_EQ(wav.bitsPerSample, 32U);
    EXPECT_EQ(wav.samples, samples);
    EXPECT_EQ(std::filesystem::file_size(file), 44U + (4U * samples.size()));
}

TEST(WavWriter, InterleavesChannels) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Stereo");
    const std::filesystem::path file = dir / "stereo.wav";
    // Three frames of two channels: L0 R0 L1 R1 L2 R2.
    const std::vector<SampleValue> samples{0.1F, -0.1F, 0.2F, -0.2F, 0.3F, -0.3F};
    writeFloatWav(file, SampleRate{48000}, 2, samples);

    const WavContents wav = readFloatWav(file);
    EXPECT_EQ(wav.channels, 2U);
    EXPECT_EQ(wav.blockAlign, 8U);
    EXPECT_EQ(wav.byteRate, 48000U * 8U);
    EXPECT_EQ(wav.samples, samples);

    drwav wav2{};
    ASSERT_TRUE(drwav_init_file(&wav2, file.string().c_str(), nullptr));
    EXPECT_EQ(wav2.totalPCMFrameCount, 3U);
    drwav_uninit(&wav2);
}

TEST(WavWriter, AnEmptyBufferIsAValidFileOfNoFrames) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Empty");
    const std::filesystem::path file = dir / "empty.wav";
    writeFloatWav(file, SampleRate{48000}, 1, {});
    const WavContents wav = readFloatWav(file);
    EXPECT_TRUE(wav.samples.empty());
    EXPECT_EQ(wav.formatTag, 3U);
}

TEST(WavWriter, OverwritesAnExistingFile) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Overwrite");
    const std::filesystem::path file = dir / "again.wav";
    writeFloatWav(file, SampleRate{48000}, 1, std::vector<SampleValue>(100, 0.5F));
    writeFloatWav(file, SampleRate{48000}, 1, std::vector<SampleValue>{0.25F});
    const WavContents wav = readFloatWav(file);
    EXPECT_EQ(wav.samples, (std::vector<SampleValue>{0.25F}));
}

TEST(WavWriter, RefusesSamplesThatDoNotDivideIntoChannels) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Divide");
    const std::filesystem::path file = dir / "odd.wav";
    EXPECT_THROW(
        writeFloatWav(file, SampleRate{48000}, 2, std::vector<SampleValue>{0.0F, 0.0F, 0.0F}),
        WavWriteError);
    EXPECT_THROW(
        writeFloatWav(file, SampleRate{48000}, 0, std::vector<SampleValue>{}), WavWriteError);
    EXPECT_FALSE(std::filesystem::exists(file));
}

TEST(WavWriter, AFileThatCannotBeCreatedThrowsAndLeavesNothing) {
    const std::filesystem::path dir = freshDirectory("WavWriter.Unwritable");
    const std::filesystem::path file = dir / "no-such-directory" / "frame.wav";
    EXPECT_THROW(
        writeFloatWav(file, SampleRate{48000}, 1, std::vector<SampleValue>{0.0F}), WavWriteError);
    EXPECT_FALSE(std::filesystem::exists(file));
}

} // namespace
