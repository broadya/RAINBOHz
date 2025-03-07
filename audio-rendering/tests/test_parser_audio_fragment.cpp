#include <iostream>

#include "gtest/gtest.h"
#include "parser_audio_fragment.h"
#include "paxel_generator.h"
#include "physical_envelope_generator.h"
#include "wav_writer.h"

using namespace RAINBOHz;

inline std::string TEST_FILES_DIR =
    "/Users/alanbroady/Development/RAINBOHz/audio-rendering/tests/test_data";

TEST(AudioFragmentParserTest, ThreeStageEnvelope) {
    // Path to the YAML file (adjust the path as necessary for your test environment)
    std::string yamlFilename = TEST_FILES_DIR + "/three_stage_envelope.yaml";

    // Create the parser and parse the YAML file.
    AudioFragmentParser parser(yamlFilename);
    AudioFragment fragment = parser.parse();

    // Basic checks:
    EXPECT_EQ(fragment.start_time, 0.0);
    // Expect exactly one partial.
    ASSERT_EQ(fragment.partials.size(), 1u);

    const AudioFragment::Partial& partial = fragment.partials[0];

    // Check AmplitudeEnvelope: expecting levels [0.4, 0.5, 0.1] and times [1.0, 2.0]
    EXPECT_EQ(partial.envelopes.amplitudeEnvelope.levels.size(), 3u);
    EXPECT_EQ(partial.envelopes.amplitudeEnvelope.timesSeconds.size(), 2u);
    EXPECT_DOUBLE_EQ(partial.envelopes.amplitudeEnvelope.levels[0], 0.4);
    EXPECT_DOUBLE_EQ(partial.envelopes.amplitudeEnvelope.levels[1], 0.5);
    EXPECT_DOUBLE_EQ(partial.envelopes.amplitudeEnvelope.levels[2], 0.1);
    EXPECT_DOUBLE_EQ(partial.envelopes.amplitudeEnvelope.timesSeconds[0], 1.0);
    EXPECT_DOUBLE_EQ(partial.envelopes.amplitudeEnvelope.timesSeconds[1], 2.0);

    // Check FrequencyEnvelope: expecting levels [1000, 2000] and times [1.5]
    EXPECT_EQ(partial.envelopes.frequencyEnvelope.levels.size(), 2u);
    EXPECT_EQ(partial.envelopes.frequencyEnvelope.timesSeconds.size(), 1u);
    EXPECT_DOUBLE_EQ(partial.envelopes.frequencyEnvelope.levels[0], 1000);
    EXPECT_DOUBLE_EQ(partial.envelopes.frequencyEnvelope.levels[1], 2000);
    EXPECT_DOUBLE_EQ(partial.envelopes.frequencyEnvelope.timesSeconds[0], 1.5);

    // Check PhaseCoordinates: expecting two coordinates at times 0.0 and 5.5 with phases 0.0 (i.e.
    // ZERO_PI)
    ASSERT_GE(partial.envelopes.phaseCoordinates.coordinates.size(), 2u);
    EXPECT_DOUBLE_EQ(partial.envelopes.phaseCoordinates.coordinates.front().timeSeconds, 0.0);
    EXPECT_DOUBLE_EQ(partial.envelopes.phaseCoordinates.coordinates.front().value, 0.0);
    EXPECT_DOUBLE_EQ(partial.envelopes.phaseCoordinates.coordinates.back().timeSeconds, 5.5);
    EXPECT_DOUBLE_EQ(partial.envelopes.phaseCoordinates.coordinates.back().value, 0.0);

    PartialEnvelopes partialEnvelopes1{partial.envelopes.amplitudeEnvelope,
                                       partial.envelopes.frequencyEnvelope,
                                       partial.envelopes.phaseCoordinates};

    PhysicalEnvelopeGenerator physicalEnvelopeGenerator{partialEnvelopes1, fragment.start_time};

    PhysicalPartialEnvelope physicalPartialEnvelope{physicalEnvelopeGenerator.generate()};

    PaxelGenerator paxelGenerator{physicalPartialEnvelope};

    std::vector<SamplePaxelInt> samples{paxelGenerator.renderAudio()};

    WavWriter wavWriter{};
    wavWriter.writeToFile("yaml-ThreeStageEnvelope.wav", samples, AudioSampleType::kPaxelInt);

    EXPECT_EQ(physicalPartialEnvelope.firstPaxelIndex, 0);
}
