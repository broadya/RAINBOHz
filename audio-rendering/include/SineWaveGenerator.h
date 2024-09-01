#ifndef SINE_WAVE_GENERATOR_H
#define SINE_WAVE_GENERATOR_H

#include <cstdint>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class SineWaveGenerator
 * @brief Generates a floating point vector representing a sine wave.
 * This class can be seen as a precursor to development of the paxel logic.
 *
 * @note This class is a simplified example for instructional purposes.
 */
class SineWaveGenerator {
   public:
    /// @brief Constructs a SineWaveGenerator ready to create a sine wave with fixed parameters.
    /// @param frequency The frequency of the sine wave in Hz.
    /// @param amplitude The amplitude of the sine wave as a floating point value between 0.0
    /// and 1.0.
    /// @param duration The duration of the sine wave in seconds.
    /// @param sampleRate The sample rate to represent the sine wave in Hz.
    SineWaveGenerator(double frequency, double amplitude, double duration,
                      uint32_t sampleRate = 44100);

    /// @brief Generate te actual sine wave and return it in a vector
    /// @return A vector of floating point values represneting the wave.
    std::vector<SamplePaxelFP> generateWave();

   private:
    double frequency_;
    double amplitude_;
    double duration_;
    uint32_t sampleRate_;
    double phaseAccumulator_;
    double phaseIncrement_;
};

}  // namespace RAINBOHz

#endif  // SINE_WAVE_GENERATOR_H
