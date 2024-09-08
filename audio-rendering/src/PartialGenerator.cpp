#include "PartialGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

#include "AudioHelpers.h"
#include "MultiPaxelGenerator.h"

using namespace RAINBOHz;

PartialGenerator::PartialGenerator(const PartialSpecification& partialSpecification,
                                   const std::vector<std::string> labels)
    : partialSpecification_(partialSpecification), labels_(labels) {
    // Invariants
    // It is allowed to have zero labels, but if they are present none may be empty
    assert(std::none_of(labels_.begin(), labels_.end(),
                        [](const std::string& str) { return str.empty(); }));
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