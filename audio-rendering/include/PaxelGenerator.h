#ifndef PAXEL_GENERATOR_H
#define PAXEL_GENERATOR_H

#include <cstdint>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class PaxelGenerator
 * @brief Render a integer vector representing a single paxel.
 * This class performs the core audio rendering logic that is used to build up the hierarchy of
 * other classes.
 */
class PaxelGenerator {
   public:
    /// @brief Constructs a PaxelGenerator ready to render a paxel with a fixed specification.
    /// @param paxelSpecification A specification of the paxel.
    PaxelGenerator(const PaxelSpecification& paxelSpecification);

    /// @brief Render audio and return it in a vector of integer samples.
    /// @return A vector of signed integer values representing the audio signal.
    std::vector<SamplePaxelInt> renderAudio();

   private:
    const PaxelSpecification paxelSpecification_;
};

}  // namespace RAINBOHz

#endif  // PAXEL_GENERATOR_H
