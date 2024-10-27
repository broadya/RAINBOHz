#ifndef MULTIPAXEL_GENERATOR_H
#define MULTIPAXEL_GENERATOR_H

#include <cstdint>
#include <vector>

#include "audio_types.h"

namespace RAINBOHz {

/**
 * @class MultiPaxelGenerator
 * @brief Generates an integer vector with the length of a single paxel, based on a
 * specification of a vector of contiguous paxels that is called a multipaxel.
 */
class MultiPaxelGenerator {
   public:
    /// @brief Constructs a MultiPaxelGenerator ready to create a paxel based on a set of paxel
    /// specifications. This is useful when a paxel cannot be fully specificed with a single
    /// PaxelSpecification. This is typically because there is at least one envelope point within
    /// the paxel. It is allowed for a MultiPaxel to contain only one paxel, and partials are in
    /// fact composed out of MultiPaxels.
    /// @param multiPaxelSpecification A specification of the paxels to be used.
    MultiPaxelGenerator(const MultiPaxelSpecification& multiPaxelSpecification);

    /// @brief Render to integer audio with the duration of a single paxel and return it in
    /// a vector
    /// @return A vector of integer samples representing a paxel.
    std::vector<SamplePaxelInt> renderAudio();

   private:
    const MultiPaxelSpecification multiPaxelSpecification_;
};

}  // namespace RAINBOHz

#endif  // MULTIPAXEL_GENERATOR_H
