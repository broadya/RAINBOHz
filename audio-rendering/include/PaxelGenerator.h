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
    /// @param paxelSpecification A specification of the paxel.
    PaxelGenerator(const PaxelSpecification& paxelSpecification);

    /// @brief Generate to floating point audio and return it in a vector
    /// @return A vector of floating point values represneting the wave.
    std::vector<SamplePaxelFP> generatePaxel();

   private:
    const PaxelSpecification paxelSpecification_;
};

}  // namespace RAINBOHz

#endif  // PAXEL_GENERATOR_H
