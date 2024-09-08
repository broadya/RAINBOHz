#include "PartialGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

#include "AudioHelpers.h"
#include "MultiPaxelGenerator.h"

using namespace RAINBOHz;

PartialGenerator::PartialGenerator(const PartialSpecification& partialSpecification,
                                   const std::vector<std::string>& labels)
    : partialSpecification_(partialSpecification), labels_(labels) {
    // Invariants
    // It is allowed to have zero labels, but if they are present none may be empty
    assert(std::none_of(labels_.begin(), labels_.end(),
                        [](const std::string& str) { return str.empty(); }));
}

PartialGenerator::PartialGenerator(const PartialEnvelopes& partialEnvelopes,
                                   const std::vector<std::string>& labels)
    : partialSpecification_(mapEnvelopesToPaxels(partialEnvelopes)), labels_(labels) {
    // Invariants
    // It is allowed to have zero labels, but if they are present none may be empty
    assert(std::none_of(labels_.begin(), labels_.end(),
                        [](const std::string& str) { return str.empty(); }));
}

PartialSpecification PartialGenerator::mapEnvelopesToPaxels(
    const PartialEnvelopes& partialEnvelopes) {
    // TODO - Logic to generate a PartialSpecification, instead of it being directly provided
    // The big one :-)

    /* General idea...

    1. Analyse all of the specifications and based on that decide how many paxels there will
    be. You can just reuse the classic Paxel struct, or a non const variant of it.

    For amplitude -

    By definition, every envelope point will lie on a paxel boundary.
    Calculate gradient (first derivative) for each envelope point.
    Just by interpolation, calculate the other amplitudes.

    For frequency -

    By definition, every envelope point will lie on a paxel boundary.
    Calculate gradient (first derivative) for each envelope point.
    Just by interpolation, calculate the other frequencies.

    OK, now for the big trick - phase.

    Using the extrapolation technique, calculate the natural phase at every paxel boundary. You will
    need to add up each natural phase value. Gnarly but possible. Then, at every fixed phase point,
    calculate the actual phase using the extrapolation technique already used. Then calculate the
    necessary phase offset. Then by interpolation calculate the actual phase target on every paxel
    boundary.

    Then, once you've done everything, gather up some of the paxels into multipaxels as required.

    The put that vector of multipaxels into the member variable.

    */
}

std::vector<SamplePaxelFP> PartialGenerator::generatePartial() {
    // Preconditions
    assert(partialSpecification_.multiPaxels.size() > 0);

    std::vector<SamplePaxelFP> result;
    result.reserve(partialSpecification_.multiPaxels.front().paxels.front().durationSamples *
                   partialSpecification_.multiPaxels.size());

    // Check for allowed range
    assert(result.capacity() > 0);

    for (int i = 0; i < partialSpecification_.multiPaxels.size(); ++i) {
        MultiPaxelGenerator multiPaxelGeneratorI{partialSpecification_.multiPaxels[i]};
        std::vector<SamplePaxelFP> resultI = multiPaxelGeneratorI.generatePaxel();
        result.insert(result.end(), resultI.begin(), resultI.end());

        // Check that the result does correctly fill to the end;
        assert((i < (partialSpecification_.multiPaxels.size() - 1)) ||
               (result[result.size() - 1] == resultI[resultI.size() - 1]));
    }

    return result;
}