#include "PartialGenerator.h"
#include "gtest/gtest.h"

using namespace RAINBOHz;

TEST(PartialGeneratorTest, Envelope2) {
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

TEST(PartialGeneratorTest, Envelope1) {
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
