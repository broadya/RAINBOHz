#include "gtest/gtest.h"
#include "SineWaveGenerator.h"

using namespace RAINBOHz;

TEST(SineWaveGeneratorTest, GeneratesCorrectWave) {
    SineWaveGenerator generator(440.0, 1.0, 1.0, 44100);
    auto wave = generator.generateWave();
    EXPECT_EQ(wave.size(), 44100);
    // Add more assertions as needed
}