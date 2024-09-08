#ifndef PARTIAL_GENERATOR_H
#define PARTIAL_GENERATOR_H

#include <cstdint>
#include <string>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class MultiPaxelGenerator
 * @brief Generates a floating point vector with the length of a single paxel, based on a
 * specification of a vector of contiguous paxels that is called a multipaxel. Phase is expressed in
 * radians.
 */
class PartialGenerator {
   public:
    /// @brief Constructs a Partial ready to create an audio fragment based on a set of sequential
    /// multipaxel specifications.
    /// @param multiPaxelSpecification A specification of the multipaxels to be used.
    /// @param labels A vector of labels to associate with this partial
    PartialGenerator(const PartialSpecification& partialSpecification,
                     const std::vector<std::string>& labels);

    /// @brief Constructs a Partial based on an envelope and phase coordinate specification.
    /// @param partialEnvelopers A specification of the partial.
    /// @param labels A vector of labels to associate with this partial
    PartialGenerator(const PartialEnvelopes& partialEnvelopes,
                     const std::vector<std::string>& labels);

    /// @brief Generate to floating point audio for the entire partial and return it in
    /// a vector
    /// @return A vector of floating point values representing an audio signal that will smoothly
    /// start and end (but neither start nor end sample are guaranteed to be zero) and will be free
    /// from discontinuties with multipaxel specifications within reasonable boundaries.
    std::vector<SamplePaxelFP> generatePartial();

   private:
    PartialSpecification mapEnvelopesToPaxels(const PartialEnvelopes& partialEnvelopes);

    const PartialSpecification partialSpecification_;
    const std::vector<std::string> labels_;
};

}  // namespace RAINBOHz

#endif  // PARTIAL_GENERATOR_H
