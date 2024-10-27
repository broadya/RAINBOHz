#include "multi_paxel_generator.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "audio_helpers.h"
#include "paxel_generator.h"

using namespace RAINBOHz;

MultiPaxelGenerator::MultiPaxelGenerator(const MultiPaxelSpecification& multiPaxelSpecification)
    : multiPaxelSpecification_(multiPaxelSpecification) {
    // Invariants are already defined in the MultiPaxelSpecification struct
}

std::vector<SamplePaxelInt> MultiPaxelGenerator::renderAudio() {
    // Preconditions
    assert(multiPaxelSpecification_.paxels.size() > 0);

    PaxelGenerator paxelGenerator{multiPaxelSpecification_.paxels[0]};
    std::vector<SamplePaxelInt> result = paxelGenerator.renderAudio();

    for (int i = 1; i < multiPaxelSpecification_.paxels.size(); ++i) {
        PaxelGenerator paxelGeneratorI{multiPaxelSpecification_.paxels[i]};
        std::vector<SamplePaxelInt> resultI = paxelGeneratorI.renderAudio();
        std::transform(result.begin(), result.end(), resultI.begin(), result.begin(),
                       [](SamplePaxelInt a, SamplePaxelInt b) { return a + b; });
    }

    return result;
}