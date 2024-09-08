#include "MultiPaxelGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "AudioHelpers.h"
#include "PaxelGenerator.h"

using namespace RAINBOHz;

MultiPaxelGenerator::MultiPaxelGenerator(const MultiPaxelSpecification& multiPaxelSpecification)
    : multiPaxelSpecification_(multiPaxelSpecification) {
    // Invariants are already defined in the MultiPaxelSpecification struct
}

std::vector<SamplePaxelFP> MultiPaxelGenerator::generatePaxel() {
    // Preconditions
    assert(multiPaxelSpecification_.paxels.size() > 0);

    PaxelGenerator paxelGenerator{multiPaxelSpecification_.paxels[0]};
    std::vector<SamplePaxelFP> result = paxelGenerator.generatePaxel();

    for (int i = 1; i < multiPaxelSpecification_.paxels.size(); ++i) {
        PaxelGenerator paxelGeneratorI{multiPaxelSpecification_.paxels[i]};
        std::vector<SamplePaxelFP> resultI = paxelGeneratorI.generatePaxel();
        std::transform(result.begin(), result.end(), resultI.begin(), result.begin(),
                       [](SamplePaxelFP a, SamplePaxelFP b) { return a + b; });
    }

    return result;
}