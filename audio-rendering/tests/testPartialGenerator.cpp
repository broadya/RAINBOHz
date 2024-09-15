#include "PartialGenerator.h"
#include "gtest/gtest.h"

using namespace RAINBOHz;

TEST(PartialGeneratorTest, Envelope1) {
    AmplitudeEnvelope amplitudeEnvelope{{0.1}, {}, {}};
    FrequencyEnvelope frequencyEnvelope{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates{{ZERO_PI, 0.0}, {ZERO_PI, 1.0}};

    PartialEnvelopes partialEnvelopes{amplitudeEnvelope, frequencyEnvelope, phaseCoordinates};
    PartialGenerator partialGenerator{partialEnvelopes, {"label_1"}, kSampleRate, 0};

    PartialSpecification partialSpecification = partialGenerator.getPartialSpecification();

    EXPECT_EQ(partialSpecification.multiPaxels.size(), 1);
}