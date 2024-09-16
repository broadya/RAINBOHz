#include "PartialGenerator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <string>

#include "AudioHelpers.h"
#include "MultiPaxelGenerator.h"

using namespace RAINBOHz;

PartialGenerator::PartialGenerator(const PartialSpecification& partialSpecification,
                                   const std::set<std::string>& labels)
    : partialSpecification_(partialSpecification), labels_(labels) {
    // Invariants
    // It is allowed to have zero labels, but if they are present none may be empty
    assert(std::none_of(labels_.begin(), labels_.end(),
                        [](const std::string& str) { return str.empty(); }));
}

PartialGenerator::PartialGenerator(const PartialEnvelopes& partialEnvelopes,
                                   const std::set<std::string>& labels,
                                   uint32_t paxelDurationSamples, uint32_t offsetSamples)
    : labels_(labels),
      partialSpecification_(
          mapEnvelopesToPaxels(partialEnvelopes, paxelDurationSamples, offsetSamples)) {
    // Invariants
    // It is allowed to have zero labels, but if they are present none may be empty
    assert(std::none_of(labels_.begin(), labels_.end(),
                        [](const std::string& str) { return str.empty(); }));
}

PartialSpecification PartialGenerator::mapEnvelopesToPaxels(
    // ------------------------------------------
    // This is a multi-layered calculation that is at the heart of the
    // synthesis concept. Currently this code is not optimised and it is clear that this method must
    // be refactored into smaller modules.
    // ------------------------------------------

    const PartialEnvelopes& partialEnvelopes, uint32_t paxelDurationSamples,
    uint32_t offsetSamples) {
    // Preconditions
    // Invariants or partialEnvelope are set witin PartialEnvelope istels.
    // In principle it is allowed to have a paxel duration of one sample.
    assert(paxelDurationSamples > 0);
    assert(offsetSamples < paxelDurationSamples);

    // To perform all of the envelope calculations, we need to first determine where all the paxel
    // boundaries will lie. There are the regular boundaries casued by the duration of a "standard"
    // paxel and there are also additional, shorter paxels caused by envelope points. At the very
    // beginning and end there can also be shorter paxels casued by the start and end times. The
    // ultimate start and end times are defined by the phase coordinates.

    std::set<PaxelInPartial> prePaxels;

    // First insert the phase coordinates because these also determine the end time of the partial.
    // Adding the phase value is necessary to allow calculation of the "natural" phase end points in
    // the first pass that processes the frequency envelope.
    for (const auto& phaseCoordinate : partialEnvelopes.phaseCoordinates.coordinates) {
        PaxelInPartial phasePaxel{phaseCoordinate.timeSamples};
        phasePaxel.paxel->startPhase = phaseCoordinate.value;
        prePaxels.insert(phasePaxel);
    }

    uint32_t endTime = (*prePaxels.rbegin()).positionInPartial;

    // Every envelope point corresponds, by definition to a paxel boundary.
    // The UnrestrictedPaxelSpecification is a struct used within this class to gradually build up a
    // full description of all paxels needed to build the partial.

    // Note that if there is an amplitude or frequency envelope point after this end time, it still
    // needs to be taken into account because it may define the envelope slope and optional curve
    // towards the end of the partial.

    // Insert all the times that are defined by the paxel duration. These are at regular intervals.
    uint32_t total{offsetSamples};
    while (total < endTime) {
        // Insert the running total into the set.
        prePaxels.insert(PaxelInPartial{total});
        // Add current value to the running total
        total += paxelDurationSamples;
    }

    // Insert all the times found in the amplitude envelope points
    total = 0;  // Reset the total
    for (const auto& time : partialEnvelopes.amplitudeEnvelope.timesSamples) {
        // Add current value to the running total
        total += time;
        // Insert the running total into the set, if it is within the range of the partial
        // and also add the last envelope point that is beyond the endTime, if it is present.
        prePaxels.insert(PaxelInPartial{total});
        if (total >= endTime) break;
    }

    // Insert all the times found in the frequency envelope points
    total = 0;  // Reset the total
    for (const auto& time : partialEnvelopes.frequencyEnvelope.timesSamples) {
        // Add current value to the running total
        total += time;
        // Insert the running total into the set, if it is within the range of the partial
        // and also add the last envelope point that is beyond the endTime, if it is present.
        prePaxels.insert(PaxelInPartial{total});
        if (total >= endTime) break;
    }

    // The number of paxels needed is the number of boundary points - 1
    assert(prePaxels.size() > 1);

    // The unrestricted paxels are not (yet) of fixed duration, but it is known that they can be
    // merged into multipaxels that will have boudaries of the correct fixed duration because these
    // points have been added to the set of boundary times.
    //
    // Intentionally starting the loop at index 1, because we need to take each boundary point and
    // the previous point to calculate the limits on the paxels.

    // It's a two pass process.
    // Pass 1 - Set all amplitude and frequency envelope points, and interpolate points in-between,
    // set the natural phase values taking into account the start phase.

    // Iterators and time counters into the various envelopes and coordinates
    // Amplitude state
    auto amplitudeTimeIterator = partialEnvelopes.amplitudeEnvelope.timesSamples.begin();
    auto amplitudeLevelIterator = partialEnvelopes.amplitudeEnvelope.levels.begin();
    uint32_t amplitudeTimeTotal{0};

    double previousAmplitudeLevel = *amplitudeLevelIterator++;
    uint32_t previousAmplitudeTimeSamples{0};  // All partials must begin on time zero
    // The first point to search for (intentional ++ here, envelopes are not coordinates, first time
    // point is implicit but first level point is explicit)
    uint32_t currentAmplitudeTimeSamples{endTime + 1};
    if (partialEnvelopes.amplitudeEnvelope.timesSamples.size() > 0) {
        currentAmplitudeTimeSamples = *amplitudeTimeIterator++;
    }

    // Frequency state
    auto frequencyTimeIterator = partialEnvelopes.frequencyEnvelope.timesSamples.begin();
    auto frequencyLevelIterator = partialEnvelopes.frequencyEnvelope.levels.begin();
    uint32_t frequencyTimeTotal{0};

    double previousFrequencyLevel = *frequencyLevelIterator++;
    uint32_t previousFrequencyTimeSamples{0};  // All partials must begin on time zero
    // The first point to search for (intentional ++ here, envelopes are not coordinates, first time
    // point is implicit but first level point is explicit)
    uint32_t currentFrequencyTimeSamples{endTime + 1};
    if (partialEnvelopes.frequencyEnvelope.timesSamples.size() > 0) {
        currentFrequencyTimeSamples = *frequencyTimeIterator++;
    }

    // This is an iterator into the set of PaxelInPartial
    auto timePointIterator = prePaxels.begin();

    // ------------------------------------------
    // Pass 1 - Process the Amplitude and Frequency Envelopes
    // ------------------------------------------
    while (timePointIterator != prePaxels.end()) {
        PaxelInPartial paxelInPartial = *timePointIterator;
        ++timePointIterator;
        // Calculate the duration of the current paxel.
        // Because the calculations work "backwards", this means that all durations are defined for
        // the inner loop.
        if (timePointIterator != prePaxels.end()) {
            paxelInPartial.paxel->durationSamples =
                timePointIterator->positionInPartial - paxelInPartial.positionInPartial;
        } else {
            paxelInPartial.paxel->durationSamples = paxelDurationSamples;
        }

        UnrestrictedPaxelSpecification& timePointPaxel = *paxelInPartial.paxel;

        // ------------------------------------------
        // Amplitude envelope matching and processing
        // ------------------------------------------
        if (amplitudeTimeIterator == partialEnvelopes.amplitudeEnvelope.timesSamples.end()) {
            timePointPaxel.startAmplitude = previousAmplitudeLevel;
            timePointPaxel.endAmplitude = previousAmplitudeLevel;
        } else if (currentAmplitudeTimeSamples == paxelInPartial.positionInPartial) {
            // Search to find where in the set the paxel corresponding the previous amplitude
            // envelope point is.
            auto innerTimePointIterator = std::find_if(
                prePaxels.begin(), prePaxels.end(),
                [previousAmplitudeTimeSamples](const PaxelInPartial& paxelSpecification) {
                    return paxelSpecification.positionInPartial == previousAmplitudeTimeSamples;
                });

            // Validate conditions at this point in processing
            assert((*innerTimePointIterator).positionInPartial == previousAmplitudeTimeSamples);
            assert((*timePointIterator).positionInPartial > previousAmplitudeTimeSamples);

            // The paxel corresponding the last envelope point
            UnrestrictedPaxelSpecification& innerTimePointPaxel = *(innerTimePointIterator->paxel);

            // Fill in known information
            innerTimePointPaxel.startAmplitude = previousAmplitudeLevel;

            // Now we can calculate the gradient.
            // This calculation is not about the mean value in a sample, it is about the
            // boundary values. It is about the "fenceposts".
            double amplitudeGradient =
                (*amplitudeLevelIterator - innerTimePointPaxel.startAmplitude) /
                (currentAmplitudeTimeSamples - previousAmplitudeTimeSamples);

            // Now loop through the set and write the boundary values, noting that the start
            // boudary parameters are always the same as the corresponding end boundary parameters.
            //
            // Note that here we are dealing with the fenceposts - the specification of paxels, as
            // opposed to dealing with the actual amplitude of real samples, where we need to take
            // the mean value of the time period of the sample in question.
            do {
                double boundaryAmplitude = innerTimePointPaxel.startAmplitude +
                                           innerTimePointPaxel.durationSamples * amplitudeGradient;

                innerTimePointPaxel.endAmplitude = boundaryAmplitude;
                ++innerTimePointIterator;
                innerTimePointPaxel = *(innerTimePointIterator->paxel);
                innerTimePointPaxel.startAmplitude = boundaryAmplitude;
            } while ((*innerTimePointIterator).positionInPartial != currentAmplitudeTimeSamples);

            // Ensure that rounding errors do not affect the final boundary point
            --innerTimePointIterator;
            innerTimePointIterator->paxel->endAmplitude = *amplitudeLevelIterator;
            timePointPaxel.startAmplitude = *amplitudeLevelIterator;

            // Move on to the next point in the amplitude envelope
            // Envelope time is relative, so need to sum here
            previousAmplitudeTimeSamples = currentAmplitudeTimeSamples;
            currentAmplitudeTimeSamples += *amplitudeTimeIterator;
            previousAmplitudeLevel = *amplitudeLevelIterator;
            ++amplitudeLevelIterator;
            ++amplitudeTimeIterator;
        }

        // ------------------------------------------
        // Frequency envelope matching and processing
        // ------------------------------------------
        if (frequencyTimeIterator == partialEnvelopes.frequencyEnvelope.timesSamples.end()) {
            // If the envelope has ended (or never started), it is still necessary to set the
            // frequency values and calculate the natural pahse.
            timePointPaxel.startFrequency = previousFrequencyLevel;
            timePointPaxel.endFrequency = previousFrequencyLevel;
            double naturalBoundaryPhase =
                naturalPhase(timePointPaxel.startPhase, timePointPaxel.startFrequency,
                             timePointPaxel.endFrequency, timePointPaxel.durationSamples, true);
            timePointPaxel.endPhase = naturalBoundaryPhase;

            // At the boundary, need to set the start phase of the next paxel, if it is available.
            if (timePointIterator != prePaxels.end()) {
                UnrestrictedPaxelSpecification& nextPointPaxel = *(timePointIterator->paxel);
                // Only set the startPhase if it has not been specified by a phase coordinate.
                if (nextPointPaxel.startPhase == nextPointPaxel.kHasNoValueDouble) {
                    nextPointPaxel.startPhase = naturalBoundaryPhase;
                }
            }
        } else if (currentFrequencyTimeSamples == paxelInPartial.positionInPartial) {
            // Search to find where in the set the paxel corresponding the previous frequency
            // envelope point is.
            auto innerTimePointIterator = std::find_if(
                prePaxels.begin(), prePaxels.end(),
                [previousFrequencyTimeSamples](const PaxelInPartial& paxelSpecification) {
                    return paxelSpecification.positionInPartial == previousFrequencyTimeSamples;
                });

            // Validate conditions at this point in processing
            assert((*innerTimePointIterator).positionInPartial == previousFrequencyTimeSamples);
            assert((*timePointIterator).positionInPartial > previousFrequencyTimeSamples);

            // The paxel corresponding the last envelope point
            UnrestrictedPaxelSpecification& innerTimePointPaxel = *(innerTimePointIterator->paxel);

            // Fill in known information
            innerTimePointPaxel.startFrequency = previousFrequencyLevel;

            // Now we can calculate the gradient.
            // This calculation is not about the mean value in a sample, it is about the
            // boundary values. It is about the "fenceposts".
            double frequencyGradient =
                (*frequencyLevelIterator - innerTimePointPaxel.startFrequency) /
                (currentFrequencyTimeSamples - previousFrequencyTimeSamples);

            // Now loop through the set and write the boundary values, noting that the start
            // boudary parameters are always the same as the corresponding end boundary parameters.
            //
            // Note that here we are dealing with the fenceposts - the specification of paxels, as
            // opposed to dealing with the actual frequency of real samples, where we need to take
            // the mean value of the time period of the sample in question.
            do {
                double boundaryFrequency = innerTimePointPaxel.startFrequency +
                                           innerTimePointPaxel.durationSamples * frequencyGradient;
                innerTimePointPaxel.endFrequency = boundaryFrequency;
                double naturalBoundaryPhase = naturalPhase(
                    innerTimePointPaxel.startPhase, innerTimePointPaxel.startFrequency,
                    innerTimePointPaxel.endFrequency, innerTimePointPaxel.durationSamples, true);
                innerTimePointPaxel.endPhase = naturalBoundaryPhase;
                ++innerTimePointIterator;
                innerTimePointPaxel = *(innerTimePointIterator->paxel);
                innerTimePointPaxel.startFrequency = boundaryFrequency;

                // Only set the startPhase if it has not been specified by a phase coordinate.
                if (innerTimePointPaxel.startPhase == innerTimePointPaxel.kHasNoValueDouble) {
                    innerTimePointPaxel.startPhase = naturalBoundaryPhase;
                }
            } while ((*innerTimePointIterator).positionInPartial != currentFrequencyTimeSamples);

            // Ensure that rounding errors do not affect the final boundary point
            --innerTimePointIterator;
            (*(*innerTimePointIterator).paxel).endFrequency = *frequencyLevelIterator;
            timePointPaxel.startFrequency = *frequencyLevelIterator;

            // Move on to the next point in the frequency envelope
            // Envelope time is relative, so need to sum here
            previousFrequencyTimeSamples = currentFrequencyTimeSamples;
            currentFrequencyTimeSamples += *frequencyTimeIterator;
            previousFrequencyLevel = *frequencyLevelIterator;
            ++frequencyLevelIterator;
            ++frequencyTimeIterator;
        }
    }

    // ------------------------------------------
    // Pass 2 - Phase Correction
    // ------------------------------------------

    // There must always be two phase coordinates. These specify the stat and end of the partial.
    assert(partialEnvelopes.phaseCoordinates.coordinates.size() >= 2);
    // Initial phase is guaranteed to be already set.
    assert(prePaxels.begin()->paxel->startPhase ==
           partialEnvelopes.phaseCoordinates.coordinates.begin()->value);

    auto currentPhaseIterator = partialEnvelopes.phaseCoordinates.coordinates.begin();
    // Intentionally x++ not ++x. We want the old value as the previous iterator.
    auto previousPhaseIterator = currentPhaseIterator++;

    // Every valid partial must consist of at least one paxel.
    assert(prePaxels.size() > 0);
    auto previousPaxelIterator = prePaxels.begin();  // First paxel must be first phase point

    while (currentPhaseIterator != partialEnvelopes.phaseCoordinates.coordinates.end()) {
        double previousPhase = (*previousPhaseIterator).value;
        double currentPhase = (*currentPhaseIterator).value;
        uint32_t previousTimeSamples = previousPhaseIterator->timeSamples;
        uint32_t currentTimeSamples = currentPhaseIterator->timeSamples;

        // Locate the paxel that corresponds to the current phase coordinate
        // Provided that the intiial population of the paxel set was correct, this is guaranteed to
        // be present.
        auto currentPaxelIterator =
            std::find_if(prePaxels.begin(), prePaxels.end(),
                         [currentTimeSamples](const PaxelInPartial& paxelSpecification) {
                             return paxelSpecification.positionInPartial == currentTimeSamples;
                         });

        // At this paxel, there is expected to be a discontinuity between the endPhase of the
        // previous paxel and the start phase of the "current" paxel. The startPhase of the current
        // paxel must already be correct according to the phase coordinates. This was set during
        // the initialisation of the paxels and used by the frequency envelope processing to
        // calculate the other "natural" phase values. We now use these "natural" phase values to
        // calulate the required phase shifts.
        assert(currentPaxelIterator->paxel->startPhase == currentPhase);

        // Go back to the previous paxel. This will hold the "natural" phase that emerged at the
        // boundary.
        --currentPaxelIterator;
        double naturalBoundaryPhase = currentPaxelIterator->paxel->endPhase;
        double phaseShift = coherenceCompensation(naturalBoundaryPhase, currentPhase);
        uint32_t timeShiftSamples = (currentTimeSamples - previousTimeSamples);
        uint32_t cumulativeTimeSamples{0};

        while (previousPaxelIterator != currentPaxelIterator) {
            cumulativeTimeSamples += previousPaxelIterator->paxel->durationSamples;
            // Add fraction of the overall phase shift, proportional to the total time of the shift.
            double paxelPhaseShift = (phaseShift * cumulativeTimeSamples) / timeShiftSamples;
            double boundaryPhase =
                std::fmod(previousPaxelIterator->paxel->endPhase + paxelPhaseShift, TWO_PI);
            previousPaxelIterator->paxel->endPhase = boundaryPhase;
            ++previousPaxelIterator;
            previousPaxelIterator->paxel->startPhase = boundaryPhase;
        }

        // The final boundary position is set exactly based on the envelope values to reduce any
        // inaccuracies that could arise from rounding errors in the floating point calculations.
        // This way there can be no rounding calculations where envelopes are simple and time
        // periods are short.
        currentPaxelIterator->paxel->endPhase = currentPhase;
        // Note that we went backwards by one element already, this is the compensation for that.
        ++currentPaxelIterator;
        // This must already be set (and has been asserted before), but just to be certain.
        assert(currentPaxelIterator->paxel->startPhase == currentPhase);

        // This is the "normal" iteration forwards in the phase coordinates that allow the next
        // iteration of the loop.
        ++previousPhaseIterator;
        ++currentPhaseIterator;
    }

    // ------------------------------------------
    // Pass 3 - Convert to correctly offset and combined MultiPaxels
    // ------------------------------------------
    // Every valid partial must consist of at least one paxel.
    assert(prePaxels.size() > 0);
    // Duration and offset must be sensible values. In particular, the maximum offset is
    // paxelDurationSamples - 1.
    assert(paxelDurationSamples > 0);
    assert(offsetSamples < paxelDurationSamples);

    auto paxelIterator = prePaxels.begin();
    uint32_t paxelStartTime{0};
    uint32_t paxelEndTime{paxelDurationSamples};
    // inverseOffset is the gap between the start of the first paxel and the first sample with
    // audio. It is important for alignment during optimisation of partial summing.
    uint32_t inverseOffset = (paxelDurationSamples - offsetSamples) % paxelDurationSamples;

    // Data structures to hold the results of the transformation.
    std::vector<MultiPaxelSpecification> completeMultiPaxelVector;
    std::vector<PaxelSpecification> currentMultiPaxel;
    do {
        PaxelInPartial currentPrePaxel = *paxelIterator;
        currentPrePaxel.positionInPartial += inverseOffset;
        uint32_t positionInPaxel = currentPrePaxel.positionInPartial % paxelDurationSamples;
        currentPrePaxel.paxel->startSample = positionInPaxel;
        // Fencepost makes the -1 necessary. If duration is 1 sample, then startSample == endSample;
        currentPrePaxel.paxel->endSample =
            positionInPaxel + currentPrePaxel.paxel->durationSamples - 1;
        currentPrePaxel.paxel->durationSamples = paxelDurationSamples;

        PaxelSpecification currentPaxelSpecification{
            currentPrePaxel.paxel->generatePaxelSpecification()};
        currentMultiPaxel.push_back(currentPaxelSpecification);

        ++paxelIterator;
        if (paxelIterator->positionInPartial == paxelEndTime) {
            assert(currentMultiPaxel.size() > 0);
            // Note that the invariants of a MultiPaxel are assert tested in this constructor.
            MultiPaxelSpecification currentMultiPaxelSpecification{currentMultiPaxel};
            completeMultiPaxelVector.push_back(currentMultiPaxelSpecification);
            paxelStartTime += paxelDurationSamples;
            paxelEndTime += paxelDurationSamples;
            currentMultiPaxel.clear();  // Ready to start with the next MultiPaxel;
        }
        // Note that there is always at least one extra paxel that is used for computation purposes
        // only. These extra paxels are used to store the final phase target and to allow for
        // envelopes that extend over the end of the partial.
    } while (paxelIterator->positionInPartial != endTime);

    PartialSpecification completePartialSpecificaion{completeMultiPaxelVector};
    return completePartialSpecificaion;
}

std::vector<SamplePaxelFP> PartialGenerator::generatePartial() {
    // Preconditions
    assert(partialSpecification_.multiPaxels.size() > 0);

    std::vector<SamplePaxelFP> result;
    result.reserve(partialSpecification_.multiPaxels.front().paxels.front().durationSamples *
                   partialSpecification_.multiPaxels.size());

    // Check for allowed range
    assert(result.capacity() > 0);

    for (int i = 0; i < partialSpecification_.multiPaxels.size(); ++i) {
        MultiPaxelGenerator multiPaxelGeneratorI{partialSpecification_.multiPaxels[i]};
        std::vector<SamplePaxelFP> resultI = multiPaxelGeneratorI.generatePaxel();
        result.insert(result.end(), resultI.begin(), resultI.end());

        // Check that the result does correctly fill to the end;
        assert((i < (partialSpecification_.multiPaxels.size() - 1)) ||
               (result[result.size() - 1] == resultI[resultI.size() - 1]));
    }

    return result;
}

PartialSpecification PartialGenerator::getPartialSpecification() { return partialSpecification_; }

std::set<std::string> PartialGenerator::getLabels() { return labels_; }
