#include "physical_envelope_generator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <string>

#include "audio_helpers.h"
#include "envelope_types.h"
#include "paxel_types.h"

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

- Amplitude and Frequency envelopes are merged.

- Time is simply approximated to samples by rounding, accoring to the secondsToSamples helper
function. Time at 0.0sec does correspond to an exact sample boundary (the start of the first
sample), but all other times are at some arbitrary point that depends on the sample rate.

This problem is inherent in quantised, sample-based audio. Ultimately, every envelope needs to be
placed on a quantised timeline. Some modifications may be necessary later in the project to allow
sound desingers and composers to be sure that they do not obtain gaps or overlaps.
--------------------------------------
*/

using namespace RAINBOHz;

PhysicalEnvelopeGenerator::PhysicalEnvelopeGenerator(const PartialEnvelopes& partialEnvelopes,
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

void PhysicalEnvelopeGenerator::populatePhysicalAmplitudeCoords(
    const std::vector<double>& amplitudeLevels, const std::vector<double>& amplitudeTimes,
    std::list<PhysicalAmplitudeCoordinate>& physicalAmplitudeCoords, uint32_t finalSample) {
    // Preconditons
    // It is also a general precondition that trimPartialEnvelope has been applied.
    // Due to this, the final time value in the time vector is redundant, if finalSample is known.
    assert(amplitudeLevels.size() == (amplitudeTimes.size() + 1));
    assert(amplitudeLevels.size() > 0);
    assert(physicalAmplitudeCoords.size() == 0);

    double timeAccumulator{0.0};
    auto levelEnvelopeStageIterator{amplitudeLevels.begin()};
    auto timeEnvelopeStageIterator{amplitudeTimes.begin()};

    while (levelEnvelopeStageIterator != amplitudeLevels.end()) {
        if (timeEnvelopeStageIterator != amplitudeTimes.end()) {
            physicalAmplitudeCoords.push_back(
                PhysicalAmplitudeCoordinate{*levelEnvelopeStageIterator, timeAccumulator});
            timeAccumulator += *timeEnvelopeStageIterator;
            ++timeEnvelopeStageIterator;
        } else {
            physicalAmplitudeCoords.push_back(
                PhysicalAmplitudeCoordinate{*levelEnvelopeStageIterator, finalSample});
        }
        ++levelEnvelopeStageIterator;
    }

    // Postconditions
    assert(physicalAmplitudeCoords.size() == amplitudeLevels.size());
    assert(physicalAmplitudeCoords.back().timeSamples == finalSample);
}

void PhysicalEnvelopeGenerator::populatePhysicalFrequencyCoords(
    const std::vector<double>& frequencyLevels, const std::vector<double>& frequencyTimes,
    std::list<PhysicalFrequencyCoordinate>& physicalFrequencyCoords, uint32_t finalSample) {
    // Preconditons
    // It is also a general precondition that trimPartialEnvelope has been applied.
    // Due to this, the final time value in the time vector is redundant, if finalSample is known.
    assert(frequencyLevels.size() == (frequencyTimes.size() + 1));
    assert(frequencyLevels.size() > 0);
    assert(physicalFrequencyCoords.size() == 0);

    double timeAccumulator{0.0};
    auto levelEnvelopeStageIterator{frequencyLevels.begin()};
    auto timeEnvelopeStageIterator{frequencyTimes.begin()};

    // Be careful when populating because the physical envelope deals in normalized frequency.
    while (levelEnvelopeStageIterator != frequencyLevels.end()) {
        if (timeEnvelopeStageIterator != frequencyTimes.end()) {
            // This constructor converts to normalized, with time in seconds.
            physicalFrequencyCoords.push_back(
                PhysicalFrequencyCoordinate{*levelEnvelopeStageIterator, timeAccumulator});
            timeAccumulator += *timeEnvelopeStageIterator;
            ++timeEnvelopeStageIterator;
        } else {
            // This constructor does not convert to normalized, hence the need to normalize here.
            physicalFrequencyCoords.push_back(PhysicalFrequencyCoordinate{
                normalizeFrequency(*levelEnvelopeStageIterator), finalSample});
        }
        ++levelEnvelopeStageIterator;
    }

    // Postconditions
    assert(physicalFrequencyCoords.size() == frequencyLevels.size());
    assert(physicalFrequencyCoords.back().timeSamples == finalSample);
}

void PhysicalEnvelopeGenerator::populatePhysicalPhaseCoords(
    const PhaseCoordinates& phaseCoordinates,
    std::list<PhysicalPhaseCoordinate>& physicalPhaseCoords) {
    auto phaseCoordinateIterator{phaseCoordinates.coordinates.begin()};

    while (phaseCoordinateIterator != phaseCoordinates.coordinates.end()) {
        PhysicalPhaseCoordinate phaseCoordinate{*phaseCoordinateIterator};
        physicalPhaseCoords.push_back(phaseCoordinate);
        ++phaseCoordinateIterator;
    }
}

void PhysicalEnvelopeGenerator::populateEnvelope() {
    // Preconditions
    // All envelopes must start and finish on the same smaple.
    assert(physicalFrequencyCoords_.front().timeSamples ==
           physicalAmplitudeCoords_.front().timeSamples);
    assert(physicalAmplitudeCoords_.front().timeSamples ==
           physicalPhaseCoords_.front().timeSamples);
    assert(physicalFrequencyCoords_.back().timeSamples ==
           physicalAmplitudeCoords_.back().timeSamples);
    assert(physicalAmplitudeCoords_.back().timeSamples == physicalPhaseCoords_.back().timeSamples);

    auto nextFrequencyCoordinatesIterator{physicalFrequencyCoords_.begin()};
    auto currentFrequencyCoordinatesIterator{nextFrequencyCoordinatesIterator++};
    auto nextAmplitudeCoordinatesIterator{physicalAmplitudeCoords_.begin()};
    auto currentAmplitudeCoordinatesIterator{nextAmplitudeCoordinatesIterator++};
    auto phaseCoordinatesIterator{++physicalPhaseCoords_.begin()};

    uint32_t currentTimeSamples{0};
    uint32_t endTimeSamples{physicalPhaseCoords_.back().timeSamples};
    double cycleAccumulator{0.0};  // If initial phase is not zero, this is corrected later
    double lastFreqPointCycleAccumulator{0.0};

    double currentFrequencyRate{
        frequencyRate(*currentFrequencyCoordinatesIterator, *nextFrequencyCoordinatesIterator)};
    double currentAmplitudeRate{
        amplitudeRate(*currentAmplitudeCoordinatesIterator, *nextAmplitudeCoordinatesIterator)};

    coords.push_back(PhysicalEnvelopePoint{
        currentTimeSamples, cycleAccumulator, currentFrequencyCoordinatesIterator->frequency,
        currentFrequencyRate, currentAmplitudeCoordinatesIterator->amplitude,
        currentAmplitudeRate});
    controlledPhaseIterators.push_back(--coords.end());

    while (currentTimeSamples < endTimeSamples) {
        uint32_t nextTimeSamples{std::min({nextFrequencyCoordinatesIterator->timeSamples,
                                           nextAmplitudeCoordinatesIterator->timeSamples,
                                           phaseCoordinatesIterator->timeSamples})};

        uint32_t elapsedFrequencyTimeSamples =
            nextTimeSamples - currentFrequencyCoordinatesIterator->timeSamples;
        double currentFrequency = currentFrequencyCoordinatesIterator->frequency +
                                  currentFrequencyRate * elapsedFrequencyTimeSamples;

        // Accumulator value can be derived using integral calculus.
        // Cycles at t = ½frequencyRate * t² + frequency₀ * t + cycles at t₀ (for the current
        // envelope stage)
        cycleAccumulator = computeCycleAccumulator(
            lastFreqPointCycleAccumulator, currentFrequencyCoordinatesIterator->frequency,
            currentFrequencyRate, elapsedFrequencyTimeSamples);

        uint32_t elapsedAmplitudeTimeSamples =
            nextTimeSamples - currentAmplitudeCoordinatesIterator->timeSamples;
        double currentAmplitude = currentAmplitudeCoordinatesIterator->amplitude +
                                  currentAmplitudeRate * elapsedAmplitudeTimeSamples;

        if (nextTimeSamples == nextFrequencyCoordinatesIterator->timeSamples) {
            // Avoid posible rounding errors
            currentFrequency = nextFrequencyCoordinatesIterator->frequency;
            // Another way to calculate the accumulator if you have the start and finish frequency.
            // This again can avoid rounding errors / inconsistencies.
            cycleAccumulator = computeCycleAccumulatorToExactEnd(
                lastFreqPointCycleAccumulator, currentFrequencyCoordinatesIterator->frequency,
                currentFrequency, elapsedFrequencyTimeSamples);

            lastFreqPointCycleAccumulator = cycleAccumulator;

            if (nextTimeSamples != endTimeSamples) {
                currentFrequencyCoordinatesIterator = nextFrequencyCoordinatesIterator++;
                currentFrequencyRate = frequencyRate(*currentFrequencyCoordinatesIterator,
                                                     *nextFrequencyCoordinatesIterator);
            }
        }

        if (nextTimeSamples == nextAmplitudeCoordinatesIterator->timeSamples) {
            // Avoid posible rounding errors
            currentAmplitude = nextAmplitudeCoordinatesIterator->amplitude;
            if (nextTimeSamples != endTimeSamples) {
                currentAmplitudeCoordinatesIterator = nextAmplitudeCoordinatesIterator++;
                currentAmplitudeRate = amplitudeRate(*currentAmplitudeCoordinatesIterator,
                                                     *nextAmplitudeCoordinatesIterator);
            }
        }

        currentTimeSamples = nextTimeSamples;

        coords.push_back(PhysicalEnvelopePoint{currentTimeSamples, cycleAccumulator,
                                               currentFrequency, currentFrequencyRate,
                                               currentAmplitude, currentAmplitudeRate});

        if (currentTimeSamples == phaseCoordinatesIterator->timeSamples) {
            controlledPhaseIterators.push_back(--coords.end());
            if (currentTimeSamples != endTimeSamples) {
                ++phaseCoordinatesIterator;
            }
        }
    }

    endSample_ = coords.back().timeSamples + startSample_;

    // Postconditions
    assert(coords.size() > 1);
    assert(coords.front().timeSamples == 0);
    assert(coords.back().timeSamples == physicalAmplitudeCoords_.back().timeSamples);
}

void PhysicalEnvelopeGenerator::interpolateControlledPhasePoints() {
    // Preconditions
    // The first point in physicalCycleEnvelope corresponds to the first point in
    // controlledPhaseIterators;
    assert((**controlledPhaseIterators.begin()) == *coords.begin());
    // The last point in physicalCycleEnvelope corresponds to the last point in
    // controlledPhaseIterators;
    assert((**--controlledPhaseIterators.end()) == *--coords.end());
    // The size of controlledPhaseIterators and phaseCoordinates_ is the same
    assert(controlledPhaseIterators.size() == phaseCoordinates_->coordinates.size());
    assert(controlledPhaseIterators.size() == physicalPhaseCoords_.size());

    // Every phase coordinate point corresponds to an iterator point in controlledPhaseIterators
    // Compensate for the target phase (where relevant) for the controlled phase points.
    //
    // Calculate the required phase offset.
    // Interpolate phase shift for all envelope points from the previous controlled phase point.

    auto phaseCoordinatesIterator{physicalPhaseCoords_.begin()};

    auto controlledPhaseIterIterator{controlledPhaseIterators.begin()};
    auto controlledPhaseIterIteratorPrevious{controlledPhaseIterIterator++};
    double cumulativePhaseShift{0.0};

    // Handle any phase shift required from the first controlled phase point.
    if (!phaseCoordinatesIterator->natural) {
        // TODO - May the first controlled phase point be negative?
        // Currently don't see any reason why not, but needs to be considered in detail.
        cumulativePhaseShift += phaseCoordinatesIterator->phase;
        (*controlledPhaseIterIteratorPrevious)->cycleAccumulator = cumulativePhaseShift;
    }
    // Now move on with the rest of the controlled phase points.
    ++phaseCoordinatesIterator;

    while (controlledPhaseIterIterator != controlledPhaseIterators.end()) {
        auto& physicalEnvelopePoint{**controlledPhaseIterIterator};
        double proportionalShift{0.0};

        // Calculate the phase shift for the current point, if it is not a natural phase point.
        if (!phaseCoordinatesIterator->natural) {
            proportionalShift =
                coherenceCompensation(physicalEnvelopePoint.cycleAccumulator + cumulativePhaseShift,
                                      phaseCoordinatesIterator->phase);
        }

        if (cumulativePhaseShift != 0.0 || proportionalShift != 0.0) {
            uint32_t timePrevious = (*controlledPhaseIterIteratorPrevious)->timeSamples;
            uint32_t timeDelta = phaseCoordinatesIterator->timeSamples - timePrevious;

            // This is over the total calculation, which can be spread over multiple points, just
            // depends how many natural points there are between the fixed phase points.
            // This gives a way to calulate how much phase shift to add to each envelope stage by
            // multiplying this by the time period of the stage.
            double phaseShiftPerSample = proportionalShift / timeDelta;

            // Don't iterate through the controlledPhaseIterators vector itself, but get a copy of
            // the iterator into physicalCycleEnvelope
            auto interpolationIterator{*controlledPhaseIterIteratorPrevious};

            // Go through all the points between two controlled phase points. This might be just two
            // controlled phase points if there are no natural points between them (hence a do loop,
            // not a while loop - it's always necessary to make a compensation for every controlled
            // phase point, other than the first point).
            do {
                auto& interpolationPoint{*interpolationIterator};
                ++interpolationIterator;
                uint32_t timeDeltaInner = interpolationIterator->timeSamples - timePrevious;
                double phaseShift = timeDeltaInner * phaseShiftPerSample;
                interpolationIterator->cycleAccumulator += (phaseShift + cumulativePhaseShift);
                interpolationPoint.frequencyRate = computeFrequencyRate(
                    interpolationPoint.cycleAccumulator, interpolationPoint.frequency,
                    interpolationIterator->cycleAccumulator,
                    interpolationIterator->timeSamples - interpolationPoint.timeSamples);
            } while (interpolationIterator != *controlledPhaseIterIterator);

            cumulativePhaseShift += proportionalShift;
        }

        // The three iterators are in lock-step.
        ++phaseCoordinatesIterator;
        ++controlledPhaseIterIteratorPrevious;
        ++controlledPhaseIterIterator;
    }
}

std::vector<std::vector<PhysicalEnvelopePoint>> PhysicalEnvelopeGenerator::divideIntoPaxelGrid() {
    // Calculate the start time, in samples, of the first paxel (nearest paxel boundary prior to
    // startSample_)
    uint32_t firstPaxelStartTimeSamples = startSample_ - (startSample_ % kSamplesPerPaxel);
    uint32_t gridOffset = startSample_ - firstPaxelStartTimeSamples;

    // Store the start paxel index, which is an offset compared to the paxel vector index
    firstPaxelIndex_ = firstPaxelStartTimeSamples / kSamplesPerPaxel;

    // If the start point is undefined, insert an additional coordinate.
    if (startSample_ != firstPaxelStartTimeSamples) {
        PhysicalEnvelopePoint firstSilentPoint{coords.front()};
        firstSilentPoint.timeSamples = 0;  // To be corrected later in the algorithm
        firstSilentPoint.amplitude = 0;
        firstSilentPoint.amplitudeRate = 0;
        firstSilentPoint.frequency = 0;
        firstSilentPoint.frequencyRate = 0;
        coords.push_front(firstSilentPoint);
    }

    // Calcaulate the end time (nearest paxel boundary after the end time)
    uint32_t lastPaxelEndTimeSamples =
        endSample_ - (endSample_ % kSamplesPerPaxel) + (kSamplesPerPaxel - 1);

    // Store the last paxel index, which is an offset compared to the paxel vector index
    lastPaxelIndex_ = lastPaxelEndTimeSamples / kSamplesPerPaxel;

    // If the last point is not the end of the paxel, need to add silence at the end of the paxel
    if (endSample_ != lastPaxelEndTimeSamples) {
        PhysicalEnvelopePoint lastSilentPoint{coords.back()};
        lastSilentPoint.timeSamples++;
        lastSilentPoint.amplitude = 0;
        lastSilentPoint.amplitudeRate = 0;
        lastSilentPoint.frequency = 0;
        lastSilentPoint.frequencyRate = 0;
        coords.push_back(lastSilentPoint);
    }

    std::vector<std::vector<PhysicalEnvelopePoint>> returnPoints;
    returnPoints.reserve(lastPaxelIndex_ - firstPaxelIndex_ + 1);

    // Loop through the resulting list.
    std::size_t currentRelativePaxel{0};
    auto coordsIter{coords.begin()};

    // Each loop iteration corresponds to one paxel due to the inner loop.
    while (coordsIter != coords.end()) {
        auto coordsIterPaxelStart{coordsIter};
        uint32_t pointsInCurrentPaxel{
            0};  // Count to allow efficient instantiation of the vector to hold these points.
        uint32_t currentPaxelEndSample =
            ((currentRelativePaxel + 1) * kSamplesPerPaxel) - gridOffset - 1;

        PhysicalEnvelopePoint* currentPoint{&*coordsIter};
        // Loop through a single paxel

        while (coordsIter != coords.end() && currentPoint->timeSamples < currentPaxelEndSample) {
            ++pointsInCurrentPaxel;
            ++coordsIter;
            if (coordsIter != coords.end()) {
                currentPoint = &*coordsIter;
            }
        }

        // Populate the vector of vectors holding this paxel, now we know how many points it has.
        std::vector<PhysicalEnvelopePoint> currentPaxelVector;
        currentPaxelVector.reserve(pointsInCurrentPaxel);
        uint32_t offset = coordsIterPaxelStart->timeSamples;

        // First paxel is a special case because of the grid offset and the very first point
        // needs special handling.
        if (offset == 0) {
            currentPaxelVector.push_back(
                *coordsIterPaxelStart);  // has already got a timeSamples value of zero.
            ++coordsIterPaxelStart;
            std::transform(coordsIterPaxelStart,
                           std::next(coordsIterPaxelStart, pointsInCurrentPaxel - 1),
                           std::back_inserter(currentPaxelVector),
                           [gridOffset](const PhysicalEnvelopePoint& pep) {
                               return PhysicalEnvelopePoint{pep.timeSamples + gridOffset,
                                                            pep.cycleAccumulator,
                                                            pep.frequency,
                                                            pep.frequencyRate,
                                                            pep.amplitude,
                                                            pep.amplitudeRate};
                           });

        } else {
            std::transform(
                coordsIterPaxelStart, std::next(coordsIterPaxelStart, pointsInCurrentPaxel),
                std::back_inserter(currentPaxelVector), [offset](const PhysicalEnvelopePoint& pep) {
                    return PhysicalEnvelopePoint{
                        pep.timeSamples - offset, pep.cycleAccumulator, pep.frequency,
                        pep.frequencyRate,        pep.amplitude,        pep.amplitudeRate};
                });
        }

        returnPoints.push_back(currentPaxelVector);

        // Is the first point in the next paxel well defined? (usually it won't be)
        // Note that it is a precondition that the very first point of the partial is well defined,
        // that's why it's inserted at the start of this function.
        if ((coordsIter != coords.end()) &&
            (currentPoint->timeSamples != currentPaxelEndSample + 1)) {
            --coordsIter;
            PhysicalEnvelopePoint& previousPoint{*coordsIter};
            ++coordsIter;

            // Insert interpolation point at start of next paxel if necessary
            PhysicalEnvelopePoint interpolationPointPaxelStart =
                interpolate(previousPoint, *currentPoint, currentPaxelEndSample + 1);

            // Note the need to also update coordsIter, because the iterator would otherwise miss
            // the newly inserted point.
            coordsIter = coords.insert(coordsIter, interpolationPointPaxelStart);
            // This is in the "next" paxel, so do not increment pointsInCurrentPaxel.
        }

        ++currentRelativePaxel;
    }

    return returnPoints;
}

PhysicalPartialEnvelope PhysicalEnvelopeGenerator::generate() {
    // Trim the vectors, so that they fit into the phase coords and have stricter invariants.
    trimPartialEnvelope(amplitudeLevels_, amplitudeTimes_);
    trimPartialEnvelope(frequencyLevels_, frequencyTimes_);

    // Convert envelopes to coordinates.
    // This avoids rounding errors on time and simplifies computation logic.
    populatePhysicalPhaseCoords(*phaseCoordinates_, physicalPhaseCoords_);
    populatePhysicalAmplitudeCoords(amplitudeLevels_, amplitudeTimes_, physicalAmplitudeCoords_,
                                    physicalPhaseCoords_.back().timeSamples);
    populatePhysicalFrequencyCoords(frequencyLevels_, frequencyTimes_, physicalFrequencyCoords_,
                                    physicalPhaseCoords_.back().timeSamples);

    // Calculate all the natural phase points in terms of cycles
    // Put this into a special secondary data structure
    // Insert points for the phase coords too.
    // Create a list with a second list of iterators that points into all of the fixed phase
    // points.
    populateEnvelope();

    // Calculate the natural phase values at the controlled phase points.
    interpolateControlledPhasePoints();

    // The first and last samples (a discrete model of time) will, in the general case, not
    // exactly align with the point in time specified in the input envelopes (a continuous model of
    // time - or at least as continuous as a double can represent).
    // This is compensated by weighting the amplitude of those samples.
    // To explain this with an example - suppose the composer / sound designer wants to put two
    // partials exactly next to each other at a precise point in time. This point in time is half
    // way through a sample. Using this technique means that the sample will get half of its
    // amplitude from the first partial and half of its amplitude from the second partial.
    double timePerSample = 1.0 / kSamplesPerPaxel;

    double firstSampleFraction =
        1 - (std::fmod(phaseCoordinates_->coordinates.front().timeSeconds, timePerSample) /
             timePerSample);
    double lastSampleFraction =
        std::fmod(phaseCoordinates_->coordinates.back().timeSeconds, timePerSample) / timePerSample;

    PhysicalPartialEnvelope returnPartialEnvelope{divideIntoPaxelGrid(), firstPaxelIndex_,
                                                  firstSampleFraction, lastSampleFraction};

    return returnPartialEnvelope;
}