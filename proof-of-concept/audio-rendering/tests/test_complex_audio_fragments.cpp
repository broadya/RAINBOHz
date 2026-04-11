#include <algorithm>
#include <cmath>
#include <execution>
#include <iostream>

#include "gtest/gtest.h"
#include "parser_audio_fragment.h"
#include "paxel_generator.h"
#include "physical_envelope_generator.h"
#include "wav_writer.h"

using namespace RAINBOHz;

inline std::string TEST_FILES_DIR =
    "/Users/alanbroady/Development/RAINBOHz/audio-rendering/tests/test_data";

TEST(AudioFragmentParserTest, ComplexEnvelopeTest) {
    // Path to the YAML file (adjust the path as necessary for your test environment)
    std::string yamlFilename = TEST_FILES_DIR + "/two_partials_variant.yaml";
    //    std::string yamlFilename = TEST_FILES_DIR + "/three_stage_envelope.yaml";

    // Create the parser and parse the YAML file.
    AudioFragmentParser parser(yamlFilename);
    AudioFragment fragment = parser.parse();

    std::vector<std::vector<SamplePaxelInt>> partialsAudio{};

    // Generate every partial

    for (const auto& partial : fragment.partials) {
        PartialEnvelopes partialEnvelopes1{partial.envelopes.amplitudeEnvelope,
                                           partial.envelopes.frequencyEnvelope,
                                           partial.envelopes.phaseCoordinates};

        PhysicalEnvelopeGenerator physicalEnvelopeGenerator{partialEnvelopes1, fragment.start_time};

        PhysicalPartialEnvelope physicalPartialEnvelope{physicalEnvelopeGenerator.generate()};

        PaxelGenerator paxelGenerator{physicalPartialEnvelope};

        partialsAudio.push_back(paxelGenerator.renderAudio());
    }

    bool autoNormalize{true};

    // Summ all the partials (with option to auto-normalize, which is handy for testing)

    // Find the maximum length among all inner vectors.
    size_t maxLength = 0;
    for (const auto& partial : partialsAudio) {
        maxLength = std::max(maxLength, partial.size());
    }

    // Reserve a result vector of size maxSize initialized to zero.
    std::vector<SamplePaxelInt> summedAudio(maxLength, 0);

    uint32_t scalingBits{0};
    if (autoNormalize) {
        scalingBits =
            static_cast<uint32_t>(std::ceil(std::log(partialsAudio.size()) / std::log(2)));
    }

    // For each inner vector, add its elements (plus the scaling factor) into result.
    for (const auto& partial : partialsAudio) {
        std::transform(std::execution::par, partial.begin(), partial.end(), summedAudio.begin(),
                       summedAudio.begin(),
                       [scalingBits](const SamplePaxelInt& partialSample,
                                     const SamplePaxelInt& sumToNowSample) {
                           return (sumToNowSample + (partialSample >> scalingBits));
                       });
    }

    WavWriter wavWriter{};
    wavWriter.writeToFile("yaml-ComplexAudio.wav", summedAudio, AudioSampleType::kPaxelInt);
}
