#ifndef MULTIPARTIAL_GENERATOR_H
#define MULTIPARTIAL_GENERATOR_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "audio_types.h"

namespace RAINBOHz {

/**
 * @class MultiPartialGenerator
 * @brief Container for a set of partials. Can generates an integer vector with the length of the
 * longest partial it contains.
 */
class MultiPartialGenerator {
   public:
    /// @brief Constructs a MultiPartialGenerator ready to render audio based on a set of partial
    /// specifications. This is useful when a partials are grouped together, typically because they
    /// represent a concept that the composer / sound desinger has in mind.
    /// @param multiPartialSpecification A specification of the partials to be used.
    /// @param labels A vector of labels to association with this MultiPartial.
    MultiPartialGenerator(const MultiPartialSpecification& multiPartialSpecification,
                          const std::set<std::string>& labels);

    /// @brief Render to integer audio samples with the duration of the longest partial and return
    /// it in a vector
    /// @return A vector of integer samples representing the sum of the partials.
    std::vector<SamplePaxelInt> renderAudio();

   private:
    const MultiPartialSpecification multiPartialSpecification_;
    const std::set<std::string> labels_;
};

}  // namespace RAINBOHz

#endif  // MULTIPARTIAL_GENERATOR_H
