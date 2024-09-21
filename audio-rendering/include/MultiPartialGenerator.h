#ifndef MULTIPARTIAL_GENERATOR_H
#define MULTIPARTIAL_GENERATOR_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class MultiPartialGenerator
 * @brief Container for a set of partials. Can generates an integer vector with the length of the
 * longest partial it contains.
 */
class MultiPartialGenerator {
   public:
    /// @brief Constructs a MultiPaxelGenerator ready to create a paxel based on a set of paxel
    /// specifications. This is useful when a paxel cannot be fully specificed with a single
    /// PaxelSpecification. This is typically because there is one or more envelope points within
    /// the paxel.
    /// @param multiPaxelSpecification A specification of the paxels to be used.
    /// @param labels A vector of labels to association with this MultiPartial.
    MultiPartialGenerator(const MultiPartialSpecification& multiPartialSpecification,
                          const std::set<std::string>& labels);

    /// @brief Generate to floating point audio with the duration of a single paxel and return it in
    /// a vector
    /// @return A vector of floating point values represneting the wave.
    std::vector<SamplePaxelInt> renderAudio();

   private:
    const MultiPartialSpecification multiPartialSpecification_;
    const std::set<std::string> labels_;
};

}  // namespace RAINBOHz

#endif  // MULTIPARTIAL_GENERATOR_H
