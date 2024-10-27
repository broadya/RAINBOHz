#ifndef PHYSICAL_ENVELOPE_GENERATOR_H
#define PHYSICAL_ENVELOPE_GENERATOR_H

#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio_types.h"
#include "envelope_types.h"

namespace RAINBOHz {

/**
 * @class PhysicalEnvelopeGenerator
 * @brief Generates the physical representation of the envelopes based on the partial specification
 * envelope. Note that this class is not thread-safe. Create an instance independetly for each
 * generation if parallel operation is required.
 * TODO consider moving to a factory pattern.
 */
class PhysicalEnvelopeGenerator {
   public:
    /// @brief Transforms from a logical representation of the partial envelopes to a physical
    /// representation, closer to the audio rendering logic.
    /// @param partialEnvelopes Specification of the partial envelopes.
    /// @param startTimeSeconds The start time of the envelopes in the piece.
    /// @return The envelopes in their physical representation.
    PhysicalPartialEnvelopes generate(const PartialEnvelopes& partialEnvelopes,
                                      const double startTimeSeconds);

   private:
    /// @brief Sets up a PhysicalEnvelopeGenerator, ready to perform its conversion calculations.
    /// @param partialEnvelopes A specification of the envelope, in the "logical" form.
    /// @param startSample The position of this envelope in the context of the piece, expressed in
    /// time.
    void initialize(const PartialEnvelopes& partialEnvelopes, const double startTimeSeconds);

    /// @brief Clean up envelopes compared to allowed states in sclang, and trim to the end of the
    /// envelope as defined by the final controlled phase point.
    /// @param levels A vector of level values
    /// @param times A vector of time values
    void trimPartialEnvelope(std::vector<double>& levels, std::vector<double>& times);

    /// @brief Based on both the frequency envelope and the phase coordinates, create a "skeleton"
    /// of the target envelope. This has points corresponding to all frequency points and all
    /// controlled phase coordinates. Conversion takes place between frequencies and cycle
    /// accumulator. Where controlled phase points are present, they are merely placeholders when
    /// this function completes. Note that this function does adjust the cycle accumulator based on
    /// phase values that are specified for any controlled phase point, including the first and last
    /// points.
    void populateCycleEnvelope();

    /// @brief Calculates the "natural" cycle accumulation values at the controlled phase points.
    /// @param physicalEnvelopeIterator An iterator into the physical envelope correspondig to a
    /// controlled phase point.
    /// @return The interpolated cycle accumulation value.
    double interpolateControlledPhasePoint(
        std::vector<PhysicalEnvelopePoint>::iterator physicalEnvelopeIterator);

    /// @brief Once the envelope already has all the natural cycle accumulator values, make all of
    /// the compensations for the controlled phase points.
    void interpolateControlledPhasePoints();

    /// @brief Convert the amplitude envelope from the logical form to the physical form.
    void populateAmplitudeEnvelope();

    std::vector<double> frequencyTimes_;
    std::vector<double> frequencyLevels_;
    std::vector<double> amplitudeTimes_;
    std::vector<double> amplitudeLevels_;
    const PhaseCoordinates* phaseCoordinates_;
    double endTimeSeconds_;
    uint32_t startSample_;
    uint32_t endSample_;

    const double UNDEFINED_PHASE = -999999998;  // Rogue value for use in phase coordinates where
                                                // the value is still to be calculated.

    std::vector<PhysicalEnvelopePoint> physicalCycleEnvelope;
    // This vector of iterators is a set of "pointers" into physicalCycleEnvelope.
    // It corresponds one-to-one with the set of PhaseCoordinate in the initial envelope
    // specification.
    std::vector<std::vector<PhysicalEnvelopePoint>::iterator> controlledPhaseIterators;

    std::vector<PhysicalEnvelopePoint> physicalAmplitudeEnvelope;
};

}  // namespace RAINBOHz

#endif  // PARTIAL_GENERATOR_H