#include "PartialGenerator.h"
#include "gtest/gtest.h"

using namespace RAINBOHz;

TEST(PartialGeneratorTest, EnvelopeFrequencyTransitionSimple) {
    // A three second sample, whereby the second multipaxel is split to accomodate the amplitude
    // envelope.
    AmplitudeEnvelope amplitudeEnvelope{{1.0}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000, 2000}, {2.5}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 3.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 3);
    EXPECT_EQ(partialSpecification.multiPaxels[0].paxels.size(), 1);
    EXPECT_EQ(partialSpecification.multiPaxels[1].paxels.size(), 1);
    EXPECT_EQ(partialSpecification.multiPaxels[2].paxels.size(), 2);
}

TEST(PartialGeneratorTest, EvelopeAmplitudeFadeOut) {
    // A three second sample, whereby the second multipaxel is split to accomodate the amplitude
    // envelope.
    AmplitudeEnvelope amplitudeEnvelope{{1.0, 0.0}, {1.5}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 3.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 3);
    EXPECT_EQ(partialSpecification.multiPaxels[0].paxels.size(), 1);
    EXPECT_EQ(partialSpecification.multiPaxels[1].paxels.size(), 2);
    EXPECT_EQ(partialSpecification.multiPaxels[2].paxels.size(), 1);
}

TEST(PartialGeneratorTest, Evelope3Seconds) {
    // A 1/1000th second sample at 1kHz with paxel duration of 1 sample for sample rate / 1000
    // paxels in the result.
    AmplitudeEnvelope amplitudeEnvelope{{0.1}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 3.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 3);
}

TEST(PartialGeneratorTest, Envelope2Seconds) {
    // A 1/1000th second sample at 1kHz with paxel duration of 1 sample for sample rate / 1000
    // paxels in the result.
    AmplitudeEnvelope amplitudeEnvelope{{0.1}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 2.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 2);
}

TEST(PartialGeneratorTest, Envelope1Paxel1Sample) {
    // A 1/1000th second sample at 1kHz with paxel duration of 1 sample for sample rate / 1000
    // paxels in the result.
    AmplitudeEnvelope amplitudeEnvelope{{0.1}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 0.001}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, 1, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), kSampleRate / 1000);
}

TEST(PartialGeneratorTest, Envelope1Paxel) {
    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope{{0.1}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 1.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 1);
}
