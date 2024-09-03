#include "PaxelGenerator.h"

#include <cassert>
#include <cmath>

#include "AudioHelpers.h"

using namespace RAINBOHz;

PaxelGenerator::PaxelGenerator(double startFrequency, double endFrequency, double startAmplitude,
                               double endAmplitude, double startPhase, double endPhase,
                               uint32_t durationSamples, uint32_t sampleRate)
    : startFrequency_(startFrequency),
      endFrequency_(endFrequency),
      startAmplitude_(startAmplitude),
      endAmplitude_(endAmplitude),
      startPhase_(startPhase),
      endPhase_(endPhase),
      durationSamples_(durationSamples),
      sampleRate_(sampleRate) {
    // Invariants
    assert(durationSamples_ > 0);

    // Calculate rates, note that here the calculation is based on the start time and end time of
    // the paxel so it starts from the begin time of the first sample and ends on the end time of
    // the last sample.
    // We are dealing in phase in terms of an accumulation for cycles. These phase calculations do
    // not "wrap around" on 2π. This is intentional because it leads on to the rate at which
    // phase needs to change on each sample.
    double f1PhaseIncrement = (TWO_PI * startFrequency_) / static_cast<double>(sampleRate_);
    double f1PhaseEnd = startPhase_ + f1PhaseIncrement * durationSamples_;

    double f2PhaseIncrement = (TWO_PI * endFrequency_) / static_cast<double>(sampleRate_);
    double f2PhaseEnd = startPhase_ + f2PhaseIncrement * durationSamples_;

    // This is where the phase accumulation would end "naturally" if there were no concept of an
    // end phase target.
    double naturalPhaseEnd = (f1PhaseEnd + f2PhaseEnd) / 2.0;

    // However, phase and frequency are of course not orthogonal, so we need to perform compensation
    // of max half a cycle (π) in order to end the paxel at the end phase target.
    // This is the small amout to add to the phase in order to hit the end phase target.
    double phaseCompensation = coherenceCompensation(naturalPhaseEnd, endPhase_);

    // The phase compensation needs to be multiplied by 2 as the inverse of the mean calculation
    // that led to the natural phase end calculation, where there was a division by 2.
    double compensatedf2PhaseEnd = f2PhaseEnd + (phaseCompensation * 2.0);
    double compensatedf2PhaseIncrement = (compensatedf2PhaseEnd - startPhase_) / durationSamples_;

    // Set generation parameters. Now the calculation is based on the centre point in time for each
    // sample. This is the reason for dividing by durationSamples + 1 and adding the
    // phaseIncrementRate_ / 2. The value for phase (and also for frequency and amplitude)
    // for a particular sample is the mean of the phase at the start time of the sample and the
    // phase at the end time of the sample.

    // The starting rate of phase change
    phaseIncrement_ = f1PhaseIncrement;

    // The second deviation, rate of rate of phase change.
    phaseIncrementRate_ = (compensatedf2PhaseIncrement - f1PhaseIncrement) / (durationSamples_ + 1);

    // Initial value for phase is, perhaps surprisingly, not startPhase_ because this is not the
    // mean value for the first sample.
    phaseAccumulator_ = startPhase_ + (phaseIncrementRate_ / 2.0);

    // The calculation for amplitude is just a straight interpolation, although again the mean of
    // the sample must be taken into account.
    amplitudeIncrement_ = (endAmplitude_ - startAmplitude_) / (durationSamples_ + 1);
    amplitude_ = startAmplitude_ + (amplitudeIncrement_ / 2.0);

    // Postconditions
}

std::vector<SamplePaxelFP> PaxelGenerator::generatePaxel() {
    std::vector<SamplePaxelFP> samples(durationSamples_);

    for (size_t i = 0; i < durationSamples_; ++i) {
        samples[i] = static_cast<SamplePaxelFP>(amplitude_ * sin(phaseAccumulator_));
        assert((samples[i] >= -1.0) && (samples[i] <= 1.0));
        phaseIncrement_ += phaseIncrementRate_;
        phaseAccumulator_ += phaseIncrement_;
        amplitude_ += amplitudeIncrement_;

        // Keep phase accumulator within 0 to 2π
        phaseAccumulator_ = std::fmod(phaseAccumulator_, TWO_PI);
    }

    // Postconditions
    assert(samples.size() > 0);

    return samples;
}