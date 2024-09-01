#ifndef PAXEL_GENERATOR_H
#define PAXEL_GENERATOR_H

#include <cstdint>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class PaxelGenerator
 * @brief Generates a floating point vector representing a single paxel.
 * Phase is expressed in radians.
 * This class can be seen as a precursor to development of the more complex, parallelised paxel
 * logic.
 */
class PaxelGenerator {
   public:
    /// @brief Constructs a PaxelGenerator ready to create a paxel with fixed parameters.
    /// @param startFrequency
    /// @param endFrequency
    /// @param startAmplitude
    /// @param endAmplitude
    /// @param startPhase
    /// @param endPhase
    /// @param durationSamples
    /// @param sampleRate
    PaxelGenerator(double startFrequency, double endFrequency, double startAmplitude,
                   double endAmplitude, double startPhase, double endPhase,
                   uint32_t durationSamples, uint32_t sampleRate);

    /// @brief Generate te actual sine wave and return it in a vector
    /// @return A vector of floating point values represneting the wave.
    std::vector<SamplePaxelFP> generatePaxel();

   private:
    double startFrequency_{0.0};
    double endFrequency_{0.0};
    double startAmplitude_{0.0};
    double endAmplitude_{0.0};
    double startPhase_{0.0};
    double endPhase_{0.0};
    double durationSamples_{0.0};
    uint32_t sampleRate_{0};
    double phaseAccumulator_{0.0};
    double phaseIncrement_{0.0};
    double phaseIncrementRate_{0.0};
    double amplitude_{0.0};
    double amplitudeIncrement_{0.0};
};

}  // namespace RAINBOHz

#endif  // PAXEL_GENERATOR_H
