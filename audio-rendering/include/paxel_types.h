#ifndef PAXELTYPES_H
#define PAXELTYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <list>
#include <numeric>
#include <vector>

#include "audio_helpers.h"
#include "audio_types.h"

/*
--------------------------------------
Describes all of the types that are used in processing paxels.

Paxels have a fixed number of samples.

For each sample, there is a known phase that constantly accumulates instead of cycling back to zero.
This captures both the frequency and phase information. For each sample, there is a known amplitude
that may be positive of negative.

A (series of) paxels can be generated from a specification, given by a PhysicalEnvelope or
PhysicalPartialEnvelope. These specifications allow for simple interpolation of the start and end
values of a paxel.
--------------------------------------
*/

namespace RAINBOHz {

constexpr uint32_t kSamplesPerPaxel = kSampleRate;

struct PaxelSampleSpecification {
    PaxelSampleSpecification(double cycleAccumulator, double amplitude)
        : cycleAccumulator(cycleAccumulator), amplitude(amplitude) {
        // Invariants
        assert(cycleAccumulator >= 0);
        assert(amplitude >= -1 && amplitude <= 1);
    }

    const double cycleAccumulator;
    const double amplitude;
};

struct PaxelSpecification {
    PaxelSpecification() { paxelSampleSpecifications.reserve(kSamplesPerPaxel); }

    std::vector<PaxelSampleSpecification> paxelSampleSpecifications;
};

}  // namespace RAINBOHz

#endif  // PAXELTYPES_H