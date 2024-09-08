#ifndef MULTIPAXEL_GENERATOR_H
#define MULTIPAXEL_GENERATOR_H

#include <cstdint>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class MultiPaxelGenerator
 * @brief Generates a floating point vector with the length of a single paxel, based on a
 * specification of a vector of contiguous paxels that is called a multipaxel. Phase is expressed in
 * radians.
 */
class MultiPaxelGenerator {
   public:
    /// @brief Constructs a MultiPaxelGenerator ready to create a paxel based on a set of paxel
    /// specifications. This is useful when a paxel cannot be fully specificed with a single
    /// PaxelSpecification. This is typically because there is one or more envelope points within
    /// the paxel.
    /// @param multiPaxelSpecification A specification of the paxels to be used.
    MultiPaxelGenerator(const MultiPaxelSpecification& multiPaxelSpecification);

    /// @brief Generate to floating point audio with the duration of a single paxel and return it in
    /// a vector
    /// @return A vector of floating point values represneting the wave.
    std::vector<SamplePaxelFP> generatePaxel();

   private:
    const MultiPaxelSpecification multiPaxelSpecification_;
};

}  // namespace RAINBOHz

#endif  // MULTIPAXEL_GENERATOR_H
