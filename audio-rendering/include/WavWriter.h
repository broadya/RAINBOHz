#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <cstdint>
#include <string>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class WavWrite
 * @brief Converts a floating point vector of samples to a fixed point WAV file.
 *
 * @note This class is a simplified example for instructional purposes.
 */
class WavWriter {
   public:
    /// @brief Constructor
    /// @param sampleRate The sample rate for the WAV file in Hz.
    /// @param channels The number of audio channels for the WAV file.
    WavWriter(uint32_t sampleRate = 44100, uint16_t channels = 1);

    /// @brief Generate a WAV file based on a vector of floating point samples.
    /// @param filename The filename of the file to write to. Overwrites any existing file.
    /// @param samples The vector containng the samples.
    /// @return
    bool writeToFile(const std::string& filename, const std::vector<SamplePaxelFP>& samples);

   private:
    uint32_t sampleRate_;
    uint16_t channels_;

    /// @brief Converts a vector of floating point samples to a vector of fixed point samples.
    /// @param samples The vector containing the samples in floating point form.
    /// @return A vector containig the samples in fixed point form.
    std::vector<uint8_t> convertSamplesToPCM(const std::vector<SamplePaxelFP>& samples);

    /// @brief Utility functon to write a 32 bit unsigned int to a file as binary data.
    /// @param outFile The file to write to.
    /// @param value The value to write.
    inline void writeUint32(std::ofstream& outFile, uint32_t value);

    /// @brief Utility function to write a 16 bit unsigned int to a file as binary data.
    /// @param outFile The file to write to.
    /// @param value The value to write.
    inline void writeUint16(std::ofstream& outFile, uint16_t value);

    /// @brief Utiltiy function to write a vector of bytes to a file as binary data.
    /// @param outFile The file to write to.
    /// @param values A vector containing the bytes to write.
    inline void writeUint8Vector(std::ofstream& outFile, const std::vector<uint8_t>& values);
};

}  // namespace RAINBOHz

#endif  // WAV_WRITER_H