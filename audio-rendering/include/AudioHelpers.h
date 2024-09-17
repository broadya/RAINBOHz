#ifndef AUDIO_HELPERS_H
#define AUDIO_HELPERS_H

#include <cassert>
#include <cmath>

#include "AudioTypes.h"

namespace RAINBOHz {

/// @brief Modulus phase operation, take a phase value and shift it into the range [0,2π)
/// @param phase Input phase
/// @return Output phase
inline double phaseMod(double phase) {
    double phaseModResult = std::fmod(phase, TWO_PI);
    if (phaseModResult < 0) {
        phaseModResult += TWO_PI;
    }
    return phaseModResult;
}

/// @brief Calculate the smallest compensation value to add to sourcePhase to achieve phase
/// coherence with the targetPhase. Both values may have values > 2π.
/// @param sourcePhase The phase of the signal that requires compensation.
/// @param targetPhase The required phase value.
/// @return A value
inline double coherenceCompensation(double sourcePhase, double targetPhase) {
    // Avoid floating point errors in calculation where the values really are equal
    if (sourcePhase == targetPhase) return 0.0;

    // Ensure the phases are within the correct range [0, 2π)
    sourcePhase = phaseMod(sourcePhase);
    targetPhase = phaseMod(targetPhase);

    // Calculate the difference
    double difference = targetPhase - sourcePhase;

    // Wrap the difference into the range [-π,π]
    if (difference > PI) {
        difference -= TWO_PI;
    } else if (difference < -PI) {
        difference += TWO_PI;
    }

    assert((difference >= -PI) && (difference <= PI));
    return difference;
}

/// @brief Calculate the "natural" phase at the end of an audio fragment that makes a linear
/// frequency transition inside a given number of samples and where the start phase is known.
/// @param startPhase The start phase, in radians. A value in the range [0,2π]
/// @param startFrequency The start frequency, in Hz.
/// @param endFrequency The end frequence, in Hz.
/// @param durationSamples The duration of the audio fragment in samples (sample rate is an
/// application constant)
/// @param onlyIncompleteCycles If true, limits the phase returned to a value in the range [0,2π).
/// If false, returns the full natural phase that also provides an indication of the number of
/// complete cycles (multiples of 2π)
/// @return The end phase, in radians. If onlyIncompleteCycles is true, this is limited to a value
/// in the range [0,2π)
inline double naturalPhase(double startPhase, double startFrequency, double endFrequency,
                           uint32_t durationSamples, bool onlyIncompleteCycles) {
    // Preconditions
    // Start phase must be within the allowed range for a paxel boundary
    assert(startPhase >= ZERO_PI && startPhase <= TWO_PI);
    // There must be at least one sample present. It is allowed to have a single sample.
    assert(durationSamples > 0);
    // Frequency values must not be negative or zero
    assert(startFrequency > 0);
    assert(endFrequency > 0);

    // Calculate rates, note that here the calculation is based on the start time and end time of
    // the paxel so it starts from the begin time of the first sample and ends on the end time of
    // the last sample. Even one sample has a duration (1/kSampleRate seconds), it is not a point in
    // time.
    // We are dealing in phase in terms of an accumulation for cycles. These phase
    // calculations do not "wrap around" on 2π. This is intentional because it can be required to
    // calculate the rate at which phase needs to change on each sample.
    double f1PhaseIncrement = (TWO_PI * startFrequency) / static_cast<double>(kSampleRate);
    double f1PhaseEnd = startPhase + f1PhaseIncrement * durationSamples;

    double f2PhaseIncrement = (TWO_PI * endFrequency) / static_cast<double>(kSampleRate);
    double f2PhaseEnd = startPhase + f2PhaseIncrement * durationSamples;

    // This is where the phase accumulation would end "naturally" if there were no concept of an
    // end phase target.
    double fullPhaseEnd = (f1PhaseEnd + f2PhaseEnd) / 2.0;
    double onlyIncompletePhaseEnd = phaseMod(fullPhaseEnd);

    // Postconditions
    // Incomplete cycles must be within the allowed range.
    assert(onlyIncompletePhaseEnd >= ZERO_PI && onlyIncompletePhaseEnd < TWO_PI);
    // Check that natural phase end is within the possible limit.
    assert(fullPhaseEnd >= onlyIncompletePhaseEnd);

    return onlyIncompleteCycles ? onlyIncompletePhaseEnd : fullPhaseEnd;
}

}  // namespace RAINBOHz

#endif  // AUDIO_HELPERS_H
