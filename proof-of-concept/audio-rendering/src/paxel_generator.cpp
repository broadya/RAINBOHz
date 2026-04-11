#include "paxel_generator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>  // to be able to use memcpy
#include <execution>
#include <iostream>

#include "audio_helpers.h"
#include "envelope_types.h"
#include "paxel_types.h"

using namespace RAINBOHz;

PaxelGenerator::PaxelGenerator(const PhysicalPartialEnvelope& physicalPartialEnvelope)
    : physicalPartialEnvelope(physicalPartialEnvelope) {}

PaxelSpecification precomputePaxel(const std::vector<PhysicalEnvelopePoint>& coords) {
    PaxelSpecification returnPaxel{};
    auto coordsIter = coords.begin();

    while (coordsIter != coords.end()) {
        uint32_t fillToSampleIndex{0};
        const PhysicalEnvelopePoint& currentStage{*coordsIter};
        ++coordsIter;
        if (coordsIter != coords.end()) {
            fillToSampleIndex = coordsIter->timeSamples;
        } else {
            fillToSampleIndex = kSamplesPerPaxel;
        }

        for (uint32_t sampleIndex = 0; sampleIndex < (fillToSampleIndex - currentStage.timeSamples);
             sampleIndex++) {
            double amplitude = currentStage.amplitude + currentStage.amplitudeRate * sampleIndex;
            double cycleAccumulator =
                computeCycleAccumulator(currentStage.cycleAccumulator, currentStage.frequency,
                                        currentStage.frequencyRate, sampleIndex);
            returnPaxel.paxelSampleSpecifications.push_back(
                PaxelSampleSpecification{cycleAccumulator, amplitude});
        }
    }

    return returnPaxel;
}

std::vector<SamplePaxelInt> PaxelGenerator::renderSinglePaxelAudio(
    const std::vector<PhysicalEnvelopePoint>& coords) {
    PaxelSpecification paxelSpecification = precomputePaxel(coords);

    // Compute the audio for a single paxel.
    std::vector<SamplePaxelInt> samples;
    samples.resize(kSamplesPerPaxel);  // looks wasteul, but allows parallel sine calculation (on
                                       // compilers with parallel support)

    // Generate samples
    // This had std::execution::par on other C++ compilers. On macOS with clang this is not
    // supported.
    // TODO: Decide how to address this issue
    std::transform(paxelSpecification.paxelSampleSpecifications.begin(),
                   paxelSpecification.paxelSampleSpecifications.end(), samples.begin(),
                   [](const PaxelSampleSpecification& in) {
                       return (std::sin(in.cycleAccumulator) * in.amplitude) * kMaxSamplePaxelInt;
                   });

    return samples;
}

std::vector<SamplePaxelInt> PaxelGenerator::renderAudio() {
    // Compute an entire partial built up from paxels.
    // This is useful in testing and early development, but this will be replaced in the target
    // rendering system by the build process or some other higher level component.
    std::vector<SamplePaxelInt> samples;
    samples.resize(kSamplesPerPaxel * physicalPartialEnvelope.paxelPoints.size());

    auto paxelIterator = physicalPartialEnvelope.paxelPoints.begin();
    uint32_t paxelCount{0};

    while (paxelIterator != physicalPartialEnvelope.paxelPoints.end()) {
        auto paxelSamples = renderSinglePaxelAudio(*paxelIterator);
        std::memcpy(samples.data() + kSamplesPerPaxel * paxelCount, paxelSamples.data(),
                    kSamplesPerPaxel * sizeof(SamplePaxelInt));
        ++paxelCount;
        ++paxelIterator;
    }

    return samples;
}
