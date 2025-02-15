#include "audio_helpers.h"
#include "gtest/gtest.h"

using namespace RAINBOHz;

TEST(AudioHelpers, ComputeFrequencyRate) {
    double startCycleAccumulator{0.0};
    double startFrequency{0.0654498469497873};
    double endCycleAccumulator{8377.0};
    uint32_t samplesSinceStart{96000};

    double a{startFrequency * samplesSinceStart};
    double b{endCycleAccumulator - startCycleAccumulator - a};
    double c{2 * b};
    uint32_t d{samplesSinceStart * samplesSinceStart};
    double e{c / d};

    double computedFrequencyRate = computeFrequencyRate(startCycleAccumulator, startFrequency,
                                                        endCycleAccumulator, samplesSinceStart);

    EXPECT_EQ(computedFrequencyRate, 0.0);
}