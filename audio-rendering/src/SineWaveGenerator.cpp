#include "SineWaveGenerator.h"

#include <cassert>
#include <cmath>

using namespace RAINBOHz;

SineWaveGenerator::SineWaveGenerator(double frequency, double amplitude, double duration,
                                     uint32_t sampleRate)
    : frequency_(frequency),
      amplitude_(amplitude),
      duration_(duration),
      sampleRate_(sampleRate),
      phaseAccumulator_(0.0) {
    // Invariants
    assert((frequency_ >= kMinAudioFrequency) && (frequency_ <= kMaxAudioFrequency));
    assert((amplitude_ >= 0.0) && (amplitude_ <= 1.0));
    assert(duration_ >= 0.0);
    assert(sampleRate_ > 0);

    phaseIncrement_ = (2.0 * M_PI * frequency_) / static_cast<double>(sampleRate_);

    // Postconditions
    assert(phaseIncrement_ > 0);
}

std::vector<SamplePaxelFP> SineWaveGenerator::generateWave() {
    size_t totalSamples = static_cast<size_t>(duration_ * sampleRate_);
    std::vector<SamplePaxelFP> samples(totalSamples);

    for (size_t i = 0; i < totalSamples; ++i) {
        samples[i] = static_cast<SamplePaxelFP>(amplitude_ * sin(phaseAccumulator_));
        assert((samples[i] >= -1.0) && (samples[i] <= 1.0));
        phaseAccumulator_ += phaseIncrement_;

        // Keep phase accumulator within 0 to 2PI
        if (phaseAccumulator_ >= 2.0 * M_PI) {
            phaseAccumulator_ -= 2.0 * M_PI;
        }
    }

    // Postconditions
    assert(samples.size() > 0);

    return samples;
}