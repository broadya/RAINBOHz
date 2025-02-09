#ifndef PAXEL_GENERATOR_H
#define PAXEL_GENERATOR_H

#include <cstdint>
#include <vector>

#include "audio_types.h"
#include "envelope_types.h"
#include "paxel_types.h"

namespace RAINBOHz {

/**
 * @class PaxelGenerator
 * @brief Based on a physical envelope, extracts a given paxel for a piece and is then able to
 * render audio for that paxel.
 */
class PaxelGenerator {
   public:
    /// @brief Constructs a PaxelGenerator ready to render a paxel with a fixed specification.
    /// @param physicalPartialEnvelope A specification of the paxel.
    PaxelGenerator(const PhysicalPartialEnvelope& physicalPartialEnvelope);

    /// @brief Render audio and return it in a vector of integer samples.
    /// @return A vector of signed integer values representing the audio signal.
    std::vector<SamplePaxelInt> renderAudio();

   private:
    std::vector<SamplePaxelInt> renderSinglePaxelAudio(
        const std::vector<PhysicalEnvelopePoint>& coords);
    const PhysicalPartialEnvelope& physicalPartialEnvelope;
};

}  // namespace RAINBOHz

#endif  // PAXEL_GENERATOR_H
