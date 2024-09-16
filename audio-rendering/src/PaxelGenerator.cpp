#include "PaxelGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "AudioHelpers.h"

using namespace RAINBOHz;

PaxelGenerator::PaxelGenerator(const PaxelSpecification& paxelSpecification)
    : paxelSpecification_(paxelSpecification) {
    // Invariants are already defined in the PaxelSpecification struct
}

std::vector<SamplePaxelFP> PaxelGenerator::generatePaxel() {
    // Compute actual audio portion, paxels may have silence at begin / end to allow for envlope
    // points. Add one due to the fencepost problem.
    uint32_t audioDurationSamples =
        1 + paxelSpecification_.endSample - paxelSpecification_.startSample;

    // Calculate rates, note that here the calculation is based on the start time and end time of
    // the paxel so it starts from the begin time of the first sample and ends on the end time of
    // the last sample.
    // We are dealing in phase in terms of an accumulation for cycles. These phase calculations do
    // not "wrap around" on 2π. This is intentional because it leads on to the rate at which
    // phase needs to change on each sample.
    double f1PhaseIncrement =
        (TWO_PI * paxelSpecification_.startFrequency) / static_cast<double>(kSampleRate);
    double f1PhaseEnd = paxelSpecification_.startPhase + f1PhaseIncrement * audioDurationSamples;

    double f2PhaseIncrement =
        (TWO_PI * paxelSpecification_.endFrequency) / static_cast<double>(kSampleRate);
    double f2PhaseEnd = paxelSpecification_.startPhase + f2PhaseIncrement * audioDurationSamples;

    // This is where the phase accumulation would end "naturally" if there were no concept of an
    // end phase target.
    double naturalPhaseEnd = (f1PhaseEnd + f2PhaseEnd) / 2.0;

    // However, phase and frequency are of course not orthogonal, so we need to perform compensation
    // of max half a cycle (π) in order to end the paxel at the end phase target.
    // This is the small amout to add to the phase in order to hit the end phase target.
    double phaseCompensation = coherenceCompensation(naturalPhaseEnd, paxelSpecification_.endPhase);

    // The phase compensation needs to be multiplied by 2 as the inverse of the mean calculation
    // that led to the natural phase end calculation, where there was a division by 2.
    double compensatedf2PhaseEnd = f2PhaseEnd + (phaseCompensation * 2.0);
    double compensatedf2PhaseIncrement =
        (compensatedf2PhaseEnd - paxelSpecification_.startPhase) / audioDurationSamples;

    // Set generation parameters. Now the calculation is based on the centre point in time for each
    // sample. This is the reason for dividing by durationSamples + 1 and adding the
    // phaseIncrementRate_ / 2. The value for phase (and also for frequency and amplitude)
    // for a particular sample is the mean of the phase at the start time of the sample and the
    // phase at the end time of the sample.

    // The starting rate of phase change
    double phaseIncrement = f1PhaseIncrement;

    // The second deviation, rate of rate of phase change.
    double phaseIncrementRate =
        (compensatedf2PhaseIncrement - f1PhaseIncrement) / (audioDurationSamples + 1);

    // Initial value for phase is, perhaps surprisingly, not startPhase_ because this is not the
    // mean value for the first sample.
    double phaseAccumulator = paxelSpecification_.startPhase + (phaseIncrementRate / 2.0);

    // The calculation for amplitude is just a straight interpolation, although again the mean of
    // the sample must be taken into account.
    double amplitudeIncrement =
        (paxelSpecification_.endAmplitude - paxelSpecification_.startAmplitude) /
        (audioDurationSamples + 1);
    double amplitude = paxelSpecification_.startAmplitude + (amplitudeIncrement / 2.0);

    std::vector<SamplePaxelFP> samples(paxelSpecification_.durationSamples);

    std::fill(samples.begin(), samples.begin() + paxelSpecification_.startSample, 0.0);

    for (size_t i = paxelSpecification_.startSample; i <= paxelSpecification_.endSample; ++i) {
        samples[i] = static_cast<SamplePaxelFP>(amplitude * sin(phaseAccumulator));
        assert((samples[i] >= -1.0) && (samples[i] <= 1.0));
        phaseIncrement += phaseIncrementRate;
        phaseAccumulator += phaseIncrement;
        amplitude += amplitudeIncrement;

        // Keep phase accumulator within 0 to 2π
        phaseAccumulator = std::fmod(phaseAccumulator, TWO_PI);
    }

    // The +1 here is because of the half-open semantics of std::fill
    // samples.end() points to one after the final sample according to iterator semantics.
    std::fill(samples.begin() + paxelSpecification_.endSample + 1, samples.end(), 0.0);

    // Postconditions
    assert(samples.size() > 0);

    return samples;
}