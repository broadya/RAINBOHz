#ifndef PHYSICAL_ENVELOPE_GENERATOR_H
#define PHYSICAL_ENVELOPE_GENERATOR_H

#include <cstdint>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include "audio_types.h"
#include "envelope_types.h"

namespace RAINBOHz {

/**
 * @class PhysicalEnvelopeGenerator
 * @brief Generates the physical representation of the envelopes based on the partial specification
 * envelope. Note that this class is not thread-safe. Create an instance independently for each
 * generation if parallel operation is required.
 * TODO consider moving to a factory pattern.
 */
class PhysicalEnvelopeGenerator {
   public:
    /// @brief Sets up a PhysicalEnvelopeGenerator, ready to perform its conversion calculations.
    /// @param partialEnvelopes A specification of the envelope, in the "logical" form.
    /// @param startSample The position of this envelope in the context of the piece, expressed in
    /// time.
    PhysicalEnvelopeGenerator(const PartialEnvelopes& partialEnvelopes,
                              const double startTimeSeconds);

    /// @brief Transforms from a logical representation of the partial envelopes to a physical
    /// representation, closer to the audio rendering logic.
    /// @param partialEnvelopes Specification of the partial envelopes.
    /// @param startTimeSeconds The start time of the envelopes in the piece.
    /// @return The envelopes in their physical representation.
    PhysicalPartialEnvelope generate();

   private:
    /// @brief Clean up envelopes compared to allowed states in sclang, and trim to the end of the
    /// envelope as defined by the final controlled phase point.
    /// @param levels A vector of level values
    /// @param times A vector of time values
    void trimPartialEnvelope(std::vector<double>& levels, std::vector<double>& times);

    void populatePhysicalAmplitudeCoords(
        const std::vector<double>& amplitudeLevels, const std::vector<double>& amplitudeTimes,
        std::list<PhysicalAmplitudeCoordinate>& physicalAmplitudeCoords, uint32_t finalSample);

    void populatePhysicalFrequencyCoords(
        const std::vector<double>& frequencyLevels, const std::vector<double>& frequencyTimes,
        std::list<PhysicalFrequencyCoordinate>& physicalFrequencyCoords, uint32_t finalSample);

    void populatePhysicalPhaseCoords(const PhaseCoordinates& phaseCoordinates,
                                     std::list<PhysicalPhaseCoordinate>& physicalPhaseCoords);

    /// @brief Based on both the frequency envelope, the amplitude envelope and the phase
    /// coordinates, create a "skeleton" of the target envelope. This has points corresponding to
    /// all frequency points, all amplitude points and all controlled phase coordinates. Conversion
    /// takes place between normalised frequencies and cycle accumulator. Where controlled phase
    /// points are present, they are merely placeholders for phase when this function completes.
    /// Note that this function does adjust the cycle accumulator based on phase values that are
    /// specified for any controlled phase point, including the first and last points.
    void populateEnvelope();

    /// @brief Once the envelope already has all the natural cycle accumulator values, make all of
    /// the compensations for the controlled phase points.
    void interpolateControlledPhasePoints();

    /// @brief Split the full envelope up over the paxel grid. Populates startPaxelIndex_.
    /// @return  The return value is a vector of vectors. The outer vector has an index in units of
    /// paxels, relative to the startPaxelIndex. THe inner vector is a series of
    /// PhysicalEvelopePoints that desribe, independently, that paxel relative to zero as the start
    /// sample of that paxel. The time values in this data structure are relative to the start of
    /// each paxel. This allows each paxel to be generated independently.
    std::vector<std::vector<PhysicalEnvelopePoint>> divideIntoPaxelGrid();

    std::vector<double> frequencyTimes_;
    std::vector<double> frequencyLevels_;
    std::vector<double> amplitudeTimes_;
    std::vector<double> amplitudeLevels_;
    const PhaseCoordinates* phaseCoordinates_;

    std::list<PhysicalFrequencyCoordinate> physicalFrequencyCoords_;
    std::list<PhysicalAmplitudeCoordinate> physicalAmplitudeCoords_;
    std::list<PhysicalPhaseCoordinate> physicalPhaseCoords_;

    double endTimeSeconds_;
    uint32_t startSample_;
    uint32_t endSample_;
    uint32_t firstPaxelIndex_;  // Absolute index of the first paxel, in the entire piece.
    uint32_t lastPaxelIndex_;   // Absolute index of the last paxel, in the entire piece.

    // The coordinates of the envelope. These coordinates control frequency, phase and amplitude.
    std::list<PhysicalEnvelopePoint> coords;

    // This vector of iterators is a set of "pointers" into physicalCycleEnvelope.
    // It corresponds one-to-one with the set of PhaseCoordinate in the initial envelope
    // specification.
    std::list<std::list<PhysicalEnvelopePoint>::iterator> controlledPhaseIterators;
};

}  // namespace RAINBOHz

#endif  // PHYSICAL_ENVELOPE_GENERATOR_H