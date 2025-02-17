#include "gtest/gtest.h"
#include "paxel_generator.h"
#include "physical_envelope_generator.h"
#include "wav_writer.h"

using namespace RAINBOHz;

TEST(PaxelGeneratorTest, ThreeStageEnvelope) {
    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope1{{0.4, 0.5, 0.1}, {1.0, 2.0}, {}};
    FrequencyEnvelope frequencyEnvelope1{{1000, 2000}, {1.5}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates1{{0.0, ZERO_PI}, {5.5, ZERO_PI}};
    PartialEnvelopes partialEnvelopes1{amplitudeEnvelope1, frequencyEnvelope1, phaseCoordinates1};

    PhysicalEnvelopeGenerator physicalEnvelopeGenerator{partialEnvelopes1, 0.0};

    PhysicalPartialEnvelope physicalPartialEnvelope{physicalEnvelopeGenerator.generate()};

    PaxelGenerator paxelGenerator{physicalPartialEnvelope};

    std::vector<SamplePaxelInt> samples{paxelGenerator.renderAudio()};

    WavWriter wavWriter{};
    wavWriter.writeToFile("ThreeStageEnvelope.wav", samples, AudioSampleType::kPaxelInt);

    EXPECT_EQ(physicalPartialEnvelope.firstPaxelIndex, 0);
}

TEST(PaxelGeneratorTest, MinimalEnvelope) {
    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope1{{0.4}, {}, {}};
    FrequencyEnvelope frequencyEnvelope1{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates1{{0.0, ZERO_PI}, {1.0, ZERO_PI}};
    PartialEnvelopes partialEnvelopes1{amplitudeEnvelope1, frequencyEnvelope1, phaseCoordinates1};

    PhysicalEnvelopeGenerator physicalEnvelopeGenerator{partialEnvelopes1, 0.0};

    PhysicalPartialEnvelope physicalPartialEnvelope{physicalEnvelopeGenerator.generate()};

    PaxelGenerator paxelGenerator{physicalPartialEnvelope};

    std::vector<SamplePaxelInt> samples{paxelGenerator.renderAudio()};

    WavWriter wavWriter{};
    wavWriter.writeToFile("MinimalEnvelope.wav", samples, AudioSampleType::kPaxelInt);

    EXPECT_EQ(physicalPartialEnvelope.firstPaxelIndex, 0);
}
