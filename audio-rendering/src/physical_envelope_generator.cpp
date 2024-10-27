#include "physical_envelope_generator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <string>

#include "audio_helpers.h"
#include "envelope_types.h"

/*
--------------------------------------
PhysicalEnvelopeGenerator

This class is concerned with taking an abstract, or logical, definition of the partial envelope and
converting it to a physical definition.

In the physical definition, the following takes place -

- Times are expressed in terms of samples.
- Frequency and phase are merged, in order to precalculate the total number of cycles at any point
in time. Interpolating between these "cycle accumulator" values during audio generation results in
changes to frequency and phase in a uniform way. The interpolation, taking into account the
different types of phase coordinates is the main processing performed in this class.

Time is simply approximated to samples by rounding, accoring to the secondsToSamples helper
function. Time at 0.0sec does correspond to an exact sample boundary (the start of the first
sample), but all other times are at some arbitrary point that depends on the sample rate.

This problem is inherent in quantised, sample-based audio. Ultimately, every envelope needs to be
placed on a quantised timeline. Some modifications may be necessary later in the project to allow
sound desingers and composers to be sure that they do not obtain gaps or overlaps.
--------------------------------------
*/

using namespace RAINBOHz;

void PhysicalEnvelopeGenerator::initialize(const PartialEnvelopes& partialEnvelopes,
                                           const double startTimeSeconds) {
    // These values will be modified.
    // They are modified to clean them up (because sclang is weak on invariants), and in a future
    // version will also be modified to add curves.
    frequencyTimes_ = partialEnvelopes.frequencyEnvelope.timesSeconds;
    frequencyLevels_ = partialEnvelopes.frequencyEnvelope.levels;
    amplitudeTimes_ = partialEnvelopes.amplitudeEnvelope.timesSeconds;
    amplitudeLevels_ = partialEnvelopes.amplitudeEnvelope.levels;

    // Phase coordinates, however, are not modified during processing by this class.
    phaseCoordinates_ = &partialEnvelopes.phaseCoordinates;

    endTimeSeconds_ = partialEnvelopes.phaseCoordinates.coordinates.back().timeSeconds;
    startSample_ = secondsToSamples(startTimeSeconds);

    // TODO - Handle curves at some point. Interpolate into linear sections by binary splitting down
    // to some (to be defined) limit. Actually relatively simply if done at this point but an
    // unnecessary complication in current early stage.
}

void PhysicalEnvelopeGenerator::trimPartialEnvelope(std::vector<double>& levels,
                                                    std::vector<double>& times) {
    // Clean up envelope, compared to valid envelopes in sclang.
    // In sclang, it is allowed to have additional times in the time vector.
    // These are redundant, so removing them first.
    // Note the >= instead of >. This is intentional due to how envelopes work in sclang.
    // The times define the "gaps" between levels.
    while (times.size() >= levels.size()) {
        times.pop_back();
    }

    // It is also possible for there to be redundant level elements in sclang envelopes.
    // These must also be removed.
    while (levels.size() > (times.size() + 1)) {
        levels.pop_back();
    }

    // Check that frequency levels / times are now logical.
    assert(levels.size() >= 1);
    assert((times.size() + 1) == levels.size());

    double envelopeTotalTime = std::accumulate(times.begin(), times.end(), 0.0);

    // Three possibilities -
    // 1. The envelope is shorter than the phase coordinate limits, so need to add a final
    // point.
    // 2. The envelope is the same length as the phase coordinate limits (no processing required).
    // 3. The envelope is longer than the phase coordinate limits, need to truncate.
    //
    // In all cases, be careful later with handling the final point due to rounding error
    // possibilities.

    if (envelopeTotalTime != endTimeSeconds_) {
        if (envelopeTotalTime < endTimeSeconds_) {
            times.push_back(endTimeSeconds_ - envelopeTotalTime);
            levels.push_back(levels.back());
        } else {
            // Ensure that the final envelope stage crosses the end time boundary
            while ((envelopeTotalTime - times.back()) > endTimeSeconds_) {
                envelopeTotalTime -= times.back();
                times.pop_back();
                levels.pop_back();

                assert(levels.size() >= 1);
                assert(times.size() >= 1);
            }

            // Now calculate the fractional part
            double timeEnvelopeBeforeEnd = envelopeTotalTime - times.back();
            double timeLastEnvelopeSegment = endTimeSeconds_ - timeEnvelopeBeforeEnd;
            double levelBack = levels.back();
            double levelOneBeforeBack = *++levels.rbegin();

            double interpolatedLevel =
                levelOneBeforeBack +
                (levelBack - levelOneBeforeBack) * (timeLastEnvelopeSegment / times.back());

            levels.pop_back();
            times.pop_back();

            levels.push_back(interpolatedLevel);
            times.push_back(timeLastEnvelopeSegment);
        }
    }

    // Postconditions
    // Check that frequency levels / times are now logical.
    assert(levels.size() >= 1);
    assert((times.size() + 1) == levels.size());
}

void PhysicalEnvelopeGenerator::populateCycleEnvelope() {
    auto phaseCoordinatesIterator{phaseCoordinates_->coordinates.begin()};
    auto frequencyLevelsIterator{frequencyLevels_.begin()};
    auto frequencyTimesIterator{frequencyTimes_.begin()};
    double frequencyTimeAccumulator{0.0};
    double frequencyCycleAccumulator{0.0};

    while (frequencyLevelsIterator != frequencyLevels_.end()) {
        if (frequencyTimeAccumulator > phaseCoordinatesIterator->timeSeconds) {
            PhysicalEnvelopePoint controlledPhasePoint{
                secondsToSamples(phaseCoordinatesIterator->timeSeconds), UNDEFINED_PHASE};
            physicalCycleEnvelope.push_back(controlledPhasePoint);
            controlledPhaseIterators.push_back(--physicalCycleEnvelope.end());
            ++phaseCoordinatesIterator;
        } else {
            PhysicalEnvelopePoint naturalPhasePoint{secondsToSamples(frequencyTimeAccumulator),
                                                    frequencyCycleAccumulator};
            physicalCycleEnvelope.push_back(naturalPhasePoint);
            if (frequencyTimeAccumulator == phaseCoordinatesIterator->timeSeconds) {
                controlledPhaseIterators.push_back(--physicalCycleEnvelope.end());
                ++phaseCoordinatesIterator;
            }

            if (frequencyLevelsIterator == frequencyLevels_.end()) {
                // Could be some cases here where the FP maths does not exactly align. Needs a
                // compensating calc here to ensure that final phase position is exactly aligned to
                // final point in physicalCycleEnvelope.
                // It is a postcondition that the final element in controlledPhaseIterators is an
                // iterator that points to the final element in physicalCycleEnvelope.
                if (phaseCoordinatesIterator != phaseCoordinates_->coordinates.end()) {
                    controlledPhaseIterators.push_back(--physicalCycleEnvelope.end());
                    // This can only happen if the FP calculations just missed the final point.
                    assert(phaseCoordinatesIterator++ == phaseCoordinates_->coordinates.end());
                } else {
                    // Just to be sure that the final point does align with the controlled phase
                    // point.
                    controlledPhaseIterators.pop_back();
                    controlledPhaseIterators.push_back(--physicalCycleEnvelope.end());
                }
            } else {
                // Update accumulators and iterators for the next loop, if necessary.
                double previousTimeAccumulator{frequencyTimeAccumulator};
                double previousFrequency{*frequencyLevelsIterator};
                frequencyTimeAccumulator += *frequencyTimesIterator;
                if (frequencyTimesIterator != frequencyTimes_.end()) {
                    ++frequencyTimesIterator;
                }
                ++frequencyLevelsIterator;
                double envelopeStageTime{frequencyTimeAccumulator - previousTimeAccumulator};
                // The more natural way to write this would be to multiply by TWO_PI (the number of
                // radians in a cycle) and then divide by two (for the mean). This is just a way to
                // write this formula cancelling the 2s.
                frequencyCycleAccumulator +=
                    (previousFrequency + *frequencyLevelsIterator) * envelopeStageTime * PI;
            }
        }
    }

    // Postconditions
    // The first point in physicalCycleEnvelope corresponds to the first point in
    // controlledPhaseIterators;
    assert((controlledPhaseIterators.front()) == physicalCycleEnvelope.begin());
    // The last point in physicalCycleEnvelope corresponds to the last point in
    // controlledPhaseIterators;
    assert((controlledPhaseIterators.back()) == --physicalCycleEnvelope.end());
    // The size of controlledPhaseIterators and phaseCoordinates_ is the same
    assert(controlledPhaseIterators.size() == phaseCoordinates_->coordinates.size());
    // The start and end phase are both defined. Due to the trimming process, there
    // is always a defined intial target frequency at the final sample.
    assert(physicalCycleEnvelope.begin()->value != UNDEFINED_PHASE);
    assert((--physicalCycleEnvelope.end())->value != UNDEFINED_PHASE);
}

double PhysicalEnvelopeGenerator::interpolateControlledPhasePoint(
    std::vector<PhysicalEnvelopePoint>::iterator physicalEnvelopeIterator) {
    auto startPhysicalEnvelopePoint{*physicalEnvelopeIterator};

    // Scan backwards to find nearest defined point.
    auto backwardsScan{physicalEnvelopeIterator};
    PhysicalEnvelopePoint* backwardsScanPhysicalEnvelopePoint{&*backwardsScan};
    do {
        --backwardsScan;
        backwardsScanPhysicalEnvelopePoint = &*backwardsScan;
    } while (backwardsScanPhysicalEnvelopePoint->value == UNDEFINED_PHASE);

    // Scan forwards to find first defined point.
    auto forwardsScan{physicalEnvelopeIterator};
    PhysicalEnvelopePoint* forwardsScanPhysicalEnvelopePoint{&*forwardsScan};
    do {
        ++forwardsScan;
        forwardsScanPhysicalEnvelopePoint = &*forwardsScan;
    } while (forwardsScanPhysicalEnvelopePoint->value == UNDEFINED_PHASE);

    double timeFraction = static_cast<double>(startPhysicalEnvelopePoint.sampleNumber -
                                              backwardsScanPhysicalEnvelopePoint->sampleNumber) /
                          (forwardsScanPhysicalEnvelopePoint->sampleNumber -
                           backwardsScanPhysicalEnvelopePoint->sampleNumber);

    double naturalCycles = forwardsScanPhysicalEnvelopePoint->value +
                           timeFraction * (forwardsScanPhysicalEnvelopePoint->value -
                                           backwardsScanPhysicalEnvelopePoint->value);

    // Postconditions
    // Range checks on naturalCycles
    assert(naturalCycles > 0.0);
    assert(naturalCycles >= backwardsScanPhysicalEnvelopePoint->value);
    assert(naturalCycles <= forwardsScanPhysicalEnvelopePoint->value);

    return naturalCycles;
}

void PhysicalEnvelopeGenerator::interpolateControlledPhasePoints() {
    // Preconditions
    // The first point in physicalCycleEnvelope corresponds to the first point in
    // controlledPhaseIterators;
    assert((controlledPhaseIterators.front()) == physicalCycleEnvelope.begin());
    // The last point in physicalCycleEnvelope corresponds to the last point in
    // controlledPhaseIterators;
    assert((controlledPhaseIterators.back()) == --physicalCycleEnvelope.end());
    // The size of controlledPhaseIterators and phaseCoordinates_ is the same
    assert(controlledPhaseIterators.size() == phaseCoordinates_->coordinates.size());
    // The start and end phase are both defined. Due to the trimming process, there
    // is always a defined intial target frequency at the final sample.
    assert(physicalCycleEnvelope.begin()->value != UNDEFINED_PHASE);
    assert((--physicalCycleEnvelope.end())->value != UNDEFINED_PHASE);

    // Every phase coordinate point corresponds to an iterator point in controlledPhaseIterators
    //
    // Two pass process.
    // Pass 1 - Calculate all the natural values for the controlled phase points.
    // Pass 2 - Compensate for the target phase (where relevant) for the controlled phase points.
    //
    // Performed in two passes due to the cumulative nature of adding the phase compensations.
    // Otherwise calculation woudl be complex where two or more controlled phase points are
    // adjacent.

    // Pass 1.
    // - Calculate the natural cycle value.

    auto controlledPhaseIterIterator{++controlledPhaseIterators.begin()};
    auto endControlledPhaseIterIterator{controlledPhaseIterators.end()};

    while (controlledPhaseIterIterator != endControlledPhaseIterIterator) {
        auto& physicalEnvelopePoint{**controlledPhaseIterIterator};

        if (physicalEnvelopePoint.value == UNDEFINED_PHASE) {
            // Natural phase point not yet established, which will be the usual case.
            // It only doesn't happen when a phase coordinate and a frequency envelope point are
            // aligned. UNDEFINED_PHASE is not allowed for either the first or the last point, which
            // is why simple interpolation is possible.
            assert(controlledPhaseIterIterator != --controlledPhaseIterators.end());
            physicalEnvelopePoint.value =
                interpolateControlledPhasePoint(*controlledPhaseIterIterator);
        }

        ++controlledPhaseIterIterator;
    }

    // Pass 2.
    // - Calculate the required phase offset.
    // - Interpolate phase shift for all envelope points from the previous controlled phase point.

    auto phaseCoordinatesIterator{phaseCoordinates_->coordinates.begin()};
    auto controlledPhaseIterIteratorPrevious{controlledPhaseIterators.begin()};
    assert((*controlledPhaseIterIteratorPrevious)->value != UNDEFINED_PHASE);
    controlledPhaseIterIterator = ++controlledPhaseIterIteratorPrevious;
    double cumulativePhaseShift{0.0};

    // Handle any phase shift required from the first controlled phase point.
    if (!phaseCoordinatesIterator->natural) {
        // TODO - May the first controlled phase point be negative?
        // Currently don't see any reason why not, but needs to be considered in detail.
        cumulativePhaseShift += phaseCoordinatesIterator->value;
        (*controlledPhaseIterIteratorPrevious)->value = cumulativePhaseShift;
    }
    // Now move on with the rest of the controlled phase points.
    ++phaseCoordinatesIterator;

    while (controlledPhaseIterIterator != endControlledPhaseIterIterator) {
        auto& physicalEnvelopePoint{**controlledPhaseIterIterator};
        double proportionalShift{0.0};

        if (!phaseCoordinatesIterator->natural) {
            proportionalShift =
                coherenceCompensation(physicalEnvelopePoint.value, phaseCoordinatesIterator->value);
        }

        if (cumulativePhaseShift != 0.0 || proportionalShift != 0.0) {
            uint32_t timePrevious = (*controlledPhaseIterIteratorPrevious)->sampleNumber;
            uint32_t timeDelta = phaseCoordinatesIterator->timeSamples - timePrevious;
            double phaseShiftPerSample = proportionalShift / timeDelta;

            // Don't iterate through the controlledPhaseIterators vector itself, but get a copy of
            // the iterator into physicalCycleEnvelope
            auto interpolationIterator = *controlledPhaseIterIteratorPrevious;

            // Don't modify the first element, this is already a controlled phase point that has
            // been processed.
            ++interpolationIterator;

            while (interpolationIterator != *controlledPhaseIterIterator) {
                timeDelta = interpolationIterator->sampleNumber - timePrevious;
                double phaseShift = timeDelta * phaseShiftPerSample;
                interpolationIterator->value += (phaseShift + cumulativePhaseShift);
                ++interpolationIterator;
            }

            // Ensure that the final point is exactly correct, removing some floating-point errors.
            cumulativePhaseShift += proportionalShift;
            (*controlledPhaseIterIterator)->value += cumulativePhaseShift;
        }

        // The three iterators are in lock-step.
        ++phaseCoordinatesIterator;
        ++controlledPhaseIterIteratorPrevious;
        ++controlledPhaseIterIterator;
    }
}

void PhysicalEnvelopeGenerator::populateAmplitudeEnvelope() {
    auto amplitudeTimesIter{amplitudeTimes_.begin()};
    auto amplitudeLevelsIter{amplitudeLevels_.begin()};
    double amplitudeTimeAccumulator{0.0};

    while (amplitudeLevelsIter != amplitudeLevels_.end()) {
        uint32_t timeSamples = secondsToSamples(amplitudeTimeAccumulator);
        PhysicalEnvelopePoint currentPoint{timeSamples, *amplitudeLevelsIter};
        physicalAmplitudeEnvelope.push_back(currentPoint);

        ++amplitudeLevelsIter;
        if (amplitudeTimesIter != amplitudeTimes_.end()) {
            amplitudeTimeAccumulator += *amplitudeTimesIter;
            ++amplitudeTimesIter;
        }
    }
}

PhysicalPartialEnvelopes PhysicalEnvelopeGenerator::generate(
    const PartialEnvelopes& partialEnvelopes, const double startTimeSeconds) {
    // Initialise
    // Copy the envelope vectors into local copies and set other member variable values.
    initialize(partialEnvelopes, startTimeSeconds);

    // Trim the vectors, so that they fit into the phase coords and have stricter invariants.
    trimPartialEnvelope(amplitudeLevels_, amplitudeTimes_);
    trimPartialEnvelope(frequencyLevels_, frequencyTimes_);

    // Calculate all the natural phase points in terms of cycles
    // Put this into a special secondary data structure
    // Insert points for the phase coords too.
    // Create a vector with a second vector of iterators that points into all of the fixed phase
    // points.
    populateCycleEnvelope();

    // Calculate the natural phase values at the controlled phase points.
    interpolateControlledPhasePoints();

    // Amplitude transformation is much simpler.
    populateAmplitudeEnvelope();

    // Build up the return object
    PhysicalCycleEnvelope returnCycleEnvelope(physicalCycleEnvelope);
    PhysicalAmplitudeEnvelope returnAmplitudeEnvelope(physicalAmplitudeEnvelope);

    PhysicalPartialEnvelopes returnPartialEnvelopes(returnAmplitudeEnvelope, returnCycleEnvelope,
                                                    startSample_);

    return returnPartialEnvelopes;
}