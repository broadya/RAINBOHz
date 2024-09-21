#include "MultiPartialGenerator.h"
#include "PartialGenerator.h"
#include "gtest/gtest.h"

using namespace RAINBOHz;

TEST(MultiPartialGeneratorTest, Envelope1Paxel) {
    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope1{{0.4}, {}, {}};
    FrequencyEnvelope frequencyEnvelope1{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates1{{ZERO_PI, 0.0}, {ZERO_PI, 1.0}};
    PartialEnvelopes partialEnvelopes1{amplitudeEnvelope1, frequencyEnvelope1, phaseCoordinates1};
    PartialGenerator partialGenerator1{partialEnvelopes1, {"label_1"}, kSampleRate, 0};
    PartialSpecification partialSpecification1 = partialGenerator1.getPartialSpecification();

    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope2{{0.4}, {}, {}};
    FrequencyEnvelope frequencyEnvelope2{{1500}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates2{{ZERO_PI, 0.0}, {ZERO_PI, 1.0}};
    PartialEnvelopes partialEnvelopes2{amplitudeEnvelope2, frequencyEnvelope2, phaseCoordinates2};
    PartialGenerator partialGenerator2{partialEnvelopes2, {"label_1"}, kSampleRate, 0};
    PartialSpecification partialSpecification2 = partialGenerator2.getPartialSpecification();

    MultiPartialSpecification multiPartialSpecification{
        {partialSpecification1, partialSpecification2}};

    EXPECT_EQ(multiPartialSpecification.partials.size(), 2);
}
