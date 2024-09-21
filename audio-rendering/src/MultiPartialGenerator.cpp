#include "MultiPartialGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "PartialGenerator.h"

using namespace RAINBOHz;

MultiPartialGenerator::MultiPartialGenerator(
    const MultiPartialSpecification& multiPartialSpecification, const std::set<std::string>& labels)
    : multiPartialSpecification_(multiPartialSpecification), labels_(labels) {
    // Invariants are already defined in the MultiPartialSpecification struct
}

std::vector<SamplePaxelBundleInt> MultiPartialGenerator::renderAudio() {
    // Preconditions
    assert(multiPartialSpecification_.partials.size() > 0);
    const uint32_t samplesPerPaxel = multiPartialSpecification_.partials.begin()
                                         ->multiPaxels.begin()
                                         ->paxels.begin()
                                         ->durationSamples;
    assert(samplesPerPaxel > 0);

    // Find the longest partial.
    // Find the element with the largest 'value' using std::max_element
    auto longestPartialIterator = std::max_element(
        multiPartialSpecification_.partials.begin(), multiPartialSpecification_.partials.end(),
        [samplesPerPaxel](const PartialSpecification& a, const PartialSpecification& b) {
            // Verify that all paxels within the MultiPaxels have the same duration.
            // Invariants of the MultiPaxel mean that only testing the first paxel is sufficient.
            // Those invariants also guarantee that there is at least one paxel present.
            assert(a.multiPaxels.begin()->paxels.begin()->durationSamples == samplesPerPaxel);
            assert(b.multiPaxels.begin()->paxels.begin()->durationSamples == samplesPerPaxel);

            return a.multiPaxels.size() < b.multiPaxels.size();
        });

    uint32_t maxPaxels = longestPartialIterator->multiPaxels.size();
    assert(maxPaxels > 0);
    const uint32_t totalDurationSamples = maxPaxels * samplesPerPaxel;
    assert(totalDurationSamples >= samplesPerPaxel);
    std::vector<SamplePaxelBundleInt> result(totalDurationSamples, 0);

    for (int i = 0; i < multiPartialSpecification_.partials.size(); ++i) {
        PartialGenerator partialGeneratorI{multiPartialSpecification_.partials[i], labels_};
        std::vector<SamplePaxelInt> resultI = partialGeneratorI.generatePartial();
        std::transform(resultI.begin(), resultI.end(), result.begin(), result.begin(),
                       [](SamplePaxelInt a, SamplePaxelBundleInt b) { return a + b; });
    }

    assert(result.size() > 0);
    return result;
}