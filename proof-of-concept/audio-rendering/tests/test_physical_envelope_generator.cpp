#include "gtest/gtest.h"
#include "physical_envelope_generator.h"

using namespace RAINBOHz;

TEST(PhysicalEnvelopeGeneratorTest, MinimalEnvelope) {
    // A one second envelope with the paxel duration equal to the sample rate for one paxel in the
    // result.
    AmplitudeEnvelope amplitudeEnvelope1{{0.4}, {}, {}};
    FrequencyEnvelope frequencyEnvelope1{{1000}, {}, {}};
    std::vector<PhaseCoordinate> phaseCoordinates1{{0.0, ZERO_PI}, {1.0, ZERO_PI}};
    PartialEnvelopes partialEnvelopes1{amplitudeEnvelope1, frequencyEnvelope1, phaseCoordinates1};

    PhysicalEnvelopeGenerator physicalEnvelopeGenerator{partialEnvelopes1, 0.0};

    PhysicalPartialEnvelope physicalPartialEnvelope{physicalEnvelopeGenerator.generate()};

    EXPECT_EQ(physicalPartialEnvelope.firstPaxelIndex, 0);
}
