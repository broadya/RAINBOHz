#ifndef AUDIO_HELPERS_H
#define AUDIO_HELPERS_H

#include <cassert>
#include <cmath>

#include "audio_types.h"

/*
--------------------------------------
Helper functions for use in additive synthesis calculations.
--------------------------------------
*/

namespace RAINBOHz {

/// @brief Modulus phase operation, take a phase value and shift it into the range [0,2π)
/// @param phase Input phase, any value.
/// @return Output phase, positive value in the range [0,2π).
inline double phaseMod(double phase) {
    double phaseModResult = std::fmod(phase, TWO_PI);
    if (phaseModResult < 0) {
        phaseModResult += TWO_PI;
    }
    return phaseModResult;
}

/// @brief Calculate the smallest compensation value to add to sourcePhase to achieve phase
/// coherence with the targetPhase. Both values may be > 2π.
/// @param sourcePhase The phase of the signal that requires compensation.
/// @param targetPhase The required phase value.
/// @return A value to be added to the source signal to realise the required phase alignment. In the
/// range [-π,π].
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
/// frequency transition within a given number of samples and where the start phase is known.
/// @param startPhase The start phase, in radians. A value in the range [0,2π]
/// @param startFrequency The start frequency, in Hz.
/// @param endFrequency The end frequency, in Hz.
/// @param durationSamples The duration of the audio fragment in samples (sample rate is an
/// application constant)
/// @param onlyIncompleteCycles If true, limits the phase returned to a value in the range [0,2π).
/// If false, returns the full natural phase that also provides an indication of the number of
/// complete cycles (multiples of 2π).
/// @return The end phase, in radians. If onlyIncompleteCycles is true, this is limited to a value
/// in the range [0,2π).
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
    // the last sample. Even one sample has a duration (1/kSampleRate seconds); samples are not
    // points in time. We are dealing in phase in terms of an accumulation for cycles. These phase
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

inline uint32_t secondsToSamples(double timesSeconds) {
    return static_cast<uint32_t>(timesSeconds * kSampleRate);  // Scale and convert to uint32_t
}

inline double samplesToSeconds(uint32_t timeSamples) {
    return (timeSamples / static_cast<double>(kSampleRate));  // Scale and convert to double
}

inline double normalizeFrequency(double frequencyHz) {
    return ((frequencyHz * TWO_PI) / kSampleRate);  // Convert to samples time base
}

/// @brief Interpolates cycle accumulator between two points in the physical coordinate space.
/// @return Interpolated cycle accumulator value.

/// @brief Calulates the cycle accumulator value at an arbitrary time given initial conditions.
/// @param startCycleAccumulator The initial value of the cycle accumulator.
/// @param startFrequency The initial frequency (normalized)
/// @param startFrequencyRate THe initial frequency rate (normalized)
/// @param samplesSinceStart The point in time at which to calculate the accumulator value.
/// @return The value of the cycle accumulator at the time after the initial conditions.
inline double computeCycleAccumulator(double startCycleAccumulator, double startFrequency,
                                      double startFrequencyRate, uint32_t samplesSinceStart) {
    // Cast to all doubles to prevent side effects, especially because 96000 * 96000 is out of range
    // for uint32_t
    double doubleSamplesSinceStart{static_cast<double>(samplesSinceStart)};
    // Accumulator value can be derived using integral calculus.
    // Cycles at t = ½frequencyRate * t² + frequency₀ * t + cycles at t₀ (for the current
    // envelope stage)
    return 0.5 * startFrequencyRate * doubleSamplesSinceStart * doubleSamplesSinceStart +
           startFrequency * doubleSamplesSinceStart + startCycleAccumulator;
}

/// @brief Calculates the cycle accumulator value when the exact end frequency is known (i.e. not
/// interpolating). This is useful when calculating physical envelope points that correspond to
/// exact points in the original frequency envelope.
/// @param startCycleAccumulator The initial value of the cycle accumulator.
/// @param startFrequency The initial frequency (normalized)
/// @param endFrequency THe final frequency rate (normalized)
/// @param samplesBetween The number of samples between the start and end frequencies.
/// @return The value of the cycle accumulator at the end point.
inline double computeCycleAccumulatorToExactEnd(double startCycleAccumulator, double startFrequency,
                                                double endFrequency, uint32_t samplesBetween) {
    // Cast to all doubles to prevent side effects, especially because 96000 * 96000 is out of range
    // for uint32_t
    double doubleSamplesBetween{static_cast<double>(samplesBetween)};

    return startCycleAccumulator + (startFrequency * doubleSamplesBetween) +
           ((endFrequency - startFrequency) * doubleSamplesBetween / 2.0);
}

/// @brief Calculates the (normalized) frequency rate to achieve a given cycle accumulator value and
/// other initial conditions.
/// @param startCycleAccumulator The initial value of the cycle accumulator.
/// @param startFrequency The initial frequency (normalized)
/// @param endCycleAccumulator The final value of the cycle accumulator, the target that requires a
/// certain rate.
/// @param samplesSinceStart The point in time at which the final cycle accumulator value must be
/// reached.
/// @return The normalized frequency rate at the interpolated point
inline double computeFrequencyRate(double startCycleAccumulator, double startFrequency,
                                   double endCycleAccumulator, uint32_t samplesSinceStart) {
    // Cast to all doubles to prevent side effects, especially because 96000 * 96000 is out of range
    // for uint32_t
    double doubleSamplesSinceStart{static_cast<double>(samplesSinceStart)};
    // Frequency rate can be derived using integral calculus.
    // Rate at t = 2 * (cycles at t - cycles at t₀ - frequency₀ * t) / t²
    return 2.0 *
           (endCycleAccumulator - startCycleAccumulator -
            startFrequency * doubleSamplesSinceStart) /
           (doubleSamplesSinceStart * doubleSamplesSinceStart);
}

}  // namespace RAINBOHz

#endif  // AUDIO_HELPERS_H
