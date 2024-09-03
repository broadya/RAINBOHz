#ifndef AUDIO_HELPERS_H
#define AUDIO_HELPERS_H

#include <cassert>
#include <cmath>

#include "AudioTypes.h"

namespace RAINBOHz {

/// @brief Calculate the smallest compensation value to add to sourcePhase to achieve phase
/// coherence with the targetPhase. Both values may have values > 2π.
/// @param sourcePhase The phase of the signal that requires compensation.
/// @param targetPhase The required phase value.
/// @return A value
inline double coherenceCompensation(double sourcePhase, double targetPhase) {
    // Avoid floating point errors in calculation where the values really are equal
    if (sourcePhase == targetPhase) return 0.0;

    // Ensure the phases are within the correct range [0, 2π)
    sourcePhase = std::fmod(sourcePhase, TWO_PI);
    targetPhase = std::fmod(targetPhase, TWO_PI);

    // Calculate the difference
    double difference = targetPhase - sourcePhase;

    // Wrap the difference into the range [-π, π]
    if (difference > PI) {
        difference -= TWO_PI;
    } else if (difference < -PI) {
        difference += TWO_PI;
    }

    assert((difference >= -PI) && (difference <= PI));
    return difference;
}

}  // namespace RAINBOHz

#endif  // AUDIO_HELPERS_H
