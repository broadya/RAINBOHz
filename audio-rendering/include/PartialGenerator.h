#ifndef PARTIAL_GENERATOR_H
#define PARTIAL_GENERATOR_H

#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "AudioTypes.h"

namespace RAINBOHz {

/**
 * @class MultiPaxelGenerator
 * @brief Generates a vector of audio samples with the length of a single paxel, based on a
 * specification of a vector of contiguous paxels that is called a multipaxel. Phase is expressed in
 * radians.
 */
class PartialGenerator {
   public:
    /// @brief Constructs a Partial ready to create an audio fragment based on a set of sequential
    /// multipaxel specifications.
    /// @param multiPaxelSpecification A specification of the multipaxels to be used.
    /// @param labels A set of labels to associate with this partial
    PartialGenerator(const PartialSpecification& partialSpecification,
                     const std::set<std::string>& labels);

    /// @brief Constructs a Partial based on an envelope and phase coordinate specification.
    /// @param partialEnvelopers A specification of the partial.
    /// @param labels A set of labels to associate with this partial
    /// @param paxelDurationSamples The size of the standard paxel grid in terms of samples
    /// @param offsetSamples Offset of the standard paxel grid relative to the envelopes
    PartialGenerator(const PartialEnvelopes& partialEnvelopes, const std::set<std::string>& labels,
                     uint32_t paxelDurationSamples, uint32_t offsetSamples);

    /// @brief Generate to samples for the entire partial and return it in
    /// a vector
    /// @return A vector of values representing an audio signal that will smoothly
    /// start and end (but neither start nor end sample are guaranteed to be zero) and will be free
    /// from discontinuties with multipaxel specifications within reasonable boundaries.
    std::vector<SamplePaxelInt> generatePartial();

    /// @brief Obtain the complete specification of the partial.
    /// @return The specification of the partial, expressed as a time ordered vector of
    /// MultiPaxelSpecification objects.
    PartialSpecification getPartialSpecification();

    /// @brief Obtain the set of labels for the partial.
    /// @return A set of std::string corresponding to all labels associated with the partial.
    std::set<std::string> getLabels();

   private:
    /// @brief Generates a partial based on a specificaiton in terms of envelopers.
    /// @param partialEnvelopes
    /// @param paxelDurationSamples
    /// @param offsetSamples The offset in samples relative to the "grid" of paxelDurationSamples.
    /// This "pushes" the grid "to the right" by a certain number of samples, and adds space before
    /// the first phase coordinate to remain aligned on whole paxel boundaries.
    /// @return A specification in terms of partials (a vector of MultiPaxels)
    PartialSpecification mapEnvelopesToPaxels(const PartialEnvelopes& partialEnvelopes,
                                              uint32_t paxelDurationSamples,
                                              uint32_t offsetSamples);

    /// @brief A very simple struct allowing for manipulation of paxel specification. Not acceptable
    /// as a means to generate a paxel directly (because invariants are not guaranteed), but can be
    /// used to generate a valid PaxelSpecification once fully populated.
    struct UnrestrictedPaxelSpecification {
       public:
        PaxelSpecification generatePaxelSpecification() {
            // Validate that all values are set
            assert(startFrequency != kHasNoValueDouble);
            assert(endFrequency != kHasNoValueDouble);
            assert(startAmplitude != kHasNoValueDouble);
            assert(endAmplitude != kHasNoValueDouble);
            assert(startPhase != kHasNoValueDouble);
            assert(endPhase != kHasNoValueDouble);
            assert(durationSamples != kHasNoValueInt);
            assert(startSample != kHasNoValueInt);
            assert(endSample != kHasNoValueInt);

            return PaxelSpecification{startFrequency,  endFrequency, startAmplitude,
                                      endAmplitude,    startPhase,   endPhase,
                                      durationSamples, startSample,  endSample};
        }

        // Define the == operator for equality comparisons
        bool operator==(const UnrestrictedPaxelSpecification& other) const {
            return startFrequency == other.startFrequency && endFrequency == other.endFrequency &&
                   startAmplitude == other.startAmplitude && endAmplitude == other.endAmplitude &&
                   startPhase == other.startPhase && endPhase == other.endPhase &&
                   durationSamples == other.durationSamples && startSample == other.startSample;
        }

        // Convenient rogue value that is conveniently out of range for all paxel parameters
        static constexpr double kHasNoValueDouble = -10.0;
        static constexpr uint32_t kHasNoValueInt = 0xFFFFFFFF;

        // Initialize with a rogue value to ensure early fail if not set.
        double startFrequency{kHasNoValueDouble};
        double endFrequency{kHasNoValueDouble};
        double startAmplitude{kHasNoValueDouble};
        double endAmplitude{kHasNoValueDouble};
        double startPhase{kHasNoValueDouble};
        double endPhase{kHasNoValueDouble};
        uint32_t durationSamples{kHasNoValueInt};
        uint32_t startSample{kHasNoValueInt};
        uint32_t endSample{kHasNoValueInt};
    };

    // A specific struct that is useful during partial generation based on
    struct PaxelInPartial {
        PaxelInPartial(uint32_t positionInPartial)
            : positionInPartial(positionInPartial),
              paxel(std::make_shared<UnrestrictedPaxelSpecification>()) {}

        // Define the < operator so that time ordering is maintained.
        bool operator<(const PaxelInPartial& other) const {
            return positionInPartial < other.positionInPartial;
        }

        // Define the == operator for equality comparisons
        bool operator==(const PaxelInPartial& other) const {
            return (*paxel) == (*(other.paxel)) && positionInPartial == other.positionInPartial;
        }

        // Additional member that is used to track the position of the paxel within a partial.
        // This is necessary only when paxels of different lengths are used in low-level
        // calculations, which is the purpose of this struct.
        uint32_t positionInPartial{0};

        //
        std::shared_ptr<UnrestrictedPaxelSpecification> paxel;
    };

    const PartialSpecification partialSpecification_;
    const std::set<std::string> labels_;
};

}  // namespace RAINBOHz

#endif  // PARTIAL_GENERATOR_H
