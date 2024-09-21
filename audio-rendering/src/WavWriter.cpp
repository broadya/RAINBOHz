#include "WavWriter.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace RAINBOHz;

WavWriter::WavWriter(uint32_t sampleRate, uint16_t channels)
    : sampleRate_(sampleRate), channels_(channels) {
    // Invariants
    assert(sampleRate_ > 0);
    assert(channels_ > 0);
}

void WavWriter::writeUint32(std::ofstream& outFile, uint32_t value) {
    outFile.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WavWriter::writeUint16(std::ofstream& outFile, uint16_t value) {
    outFile.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void WavWriter::writeUint8Vector(std::ofstream& outFile, const std::vector<uint8_t>& values) {
    outFile.write(reinterpret_cast<const char*>(values.data()), values.size());
}

bool WavWriter::writeToFile(const std::string& filename, const std::vector<int32_t>& samples,
                            const AudioSampleType audioSampleType) {
    return writeToFile(filename, convertSamplesToPCM(samples, audioSampleType));
}

bool WavWriter::writeToFile(const std::string& filename,
                            const std::vector<SamplePaxelFP>& samples) {
    return writeToFile(filename, convertSamplesToPCM(samples));
}

bool WavWriter::writeToFile(const std::string& filename, const std::vector<uint8_t>& pcmData) {
    // Preconditions
    assert(pcmData.size() > 0);
    assert(filename.length() > 0);

    // Inherently part of the WAV file format
    constexpr uint32_t fmtChunkSize = 16;
    constexpr uint16_t audioFormat = 1;  // PCM

    // Variable for this file
    const uint32_t dataChunkSize = static_cast<uint32_t>(pcmData.size());
    const uint32_t riffChunkSize = 4 + (8 + fmtChunkSize) + (8 + dataChunkSize);
    const uint32_t byteRate = sampleRate_ * channels_ * kPaxelBytesPerSample;
    const uint16_t blockAlign = channels_ * kPaxelBytesPerSample;

    try {
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile.is_open()) {
            return false;
        }

        // Write RIFF header
        outFile.write("RIFF", 4);
        writeUint32(outFile, riffChunkSize);
        outFile.write("WAVE", 4);

        // Write fmt subchunk
        outFile.write("fmt ", 4);
        writeUint32(outFile, fmtChunkSize);
        writeUint16(outFile, audioFormat);
        writeUint16(outFile, channels_);
        writeUint32(outFile, sampleRate_);
        writeUint32(outFile, byteRate);
        writeUint16(outFile, blockAlign);
        writeUint16(outFile, kPaxelBitDepth);

        // Write data subchunk
        outFile.write("data", 4);
        writeUint32(outFile, dataChunkSize);
        writeUint8Vector(outFile, pcmData);

        outFile.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return false;  // Or rethrow the exception if it needs to be handled by the caller
    }
}

std::vector<uint8_t> WavWriter::convertSamplesToPCM(const std::vector<int32_t>& samples,
                                                    const AudioSampleType audioSampleType) {
    size_t totalSamples = samples.size();
    assert(totalSamples > 0);
    std::vector<uint8_t> pcmData(totalSamples * kPaxelBytesPerSample);

    switch (audioSampleType) {
        case AudioSampleType::kPaxelInt:
            for (size_t i = 0; i < totalSamples; ++i) {
                assert(samples[i] >= -kMaxSamplePaxelInt && samples[i] <= kMaxSamplePaxelInt);
                std::memcpy(&pcmData[i * kPaxelBytesPerSample], &samples[i], kPaxelBytesPerSample);
            }
            break;
        case AudioSampleType::kPaxelBundleInt:
            for (size_t i = 0; i < totalSamples; ++i) {
                // Scale 32-bit audio to 24-bit audio
                //                SamplePaxelInt scaledSample = samples[i] / 0x10;
                SamplePaxelInt scaledSample = samples[i] / 4;
                assert(scaledSample >= -kMaxSamplePaxelInt && scaledSample <= kMaxSamplePaxelInt);
                std::memcpy(&pcmData[i * kPaxelBytesPerSample], &scaledSample,
                            kPaxelBytesPerSample);
            }
            break;
    }

    assert(pcmData.size() == (samples.size() * kPaxelBytesPerSample));
    return pcmData;
}

std::vector<uint8_t> WavWriter::convertSamplesToPCM(const std::vector<SamplePaxelFP>& samples) {
    size_t totalSamples = samples.size();
    std::vector<uint8_t> pcmData(totalSamples * kPaxelBytesPerSample);

    for (size_t i = 0; i < totalSamples; ++i) {
        SamplePaxelInt sample = static_cast<SamplePaxelInt>(samples[i] * kMaxSamplePaxelInt);
        assert((sample >= 0 && samples[i] >= 0.0) || (sample <= 0 && samples[i] <= 0.0));
        std::memcpy(&pcmData[i * kPaxelBytesPerSample], &sample, kPaxelBytesPerSample);
    }

    assert(pcmData.size() == (samples.size() * kPaxelBytesPerSample));
    return pcmData;
}
