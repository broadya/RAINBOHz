#ifndef AUDIOTYPES_H
#define AUDIOTYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

/*
--------------------------------------
Defines various contstants, limits, type equivalents that are useful during additive synthesis
calculations.

Defines all of the simple struct types used in additive synthesis calculations, and includes asserts
for their invariants. These are intentionally not held in classes with encapsulation because they
have a role simply to pass around structured data and not to perform operations on that data.

The types are decoupled from the classes that can render audio based on them. This is to allow for
distributed processing of the types, where the proess of specifying a synthesis "compute job" will
be independent from actually performing that "compute job".

The types are immutable.

The main types build up in a hierarchy as follows -

PaxelSpecification | A single paxel that can be of any duration.

MultiPaxelSpecification | A vector of PaxelSpecification that are all of the same duration, but each
repesents some subdivision of that duration without overlaps.

PartialSpecification | A vector of MultiPaxelSpecification that describes the evolution of a single
partial.

MultiPartialSpecification | A vector of PartialSpecification that describes some bundle of partials
that are to be rendered together.

The other types describe the envelopes that specify a partial.

Envelope | A generic specification of an envelope following the same approach as sclang.

FrequencyEnvelope | An envelope representing evolution of the frequency of a partial.

AmplitudeEnvelope | An envelope representing evolution of the amplitude of a partial.

PhaseCoordinate | A point in the evolution of a partial where a certain phase must be achieved, or
where specifically the "natural" phase must be used. Natural phase is an important concept in the
RAINBOHz additive synthesis approach; it means "the phase that the partial it would naturally have
at this point, if unmodified".

PhaseCoordinates | A vector of PhaseCoordinate representing all points in evolution of the partial
where the phase is defined (or at least is explicitly at the "natural" phase). These coordinates
also define the begin and the end of a partial.

PartialEnvelopes | An aggregation of FrequencyEnvelope, AmplitudeEnvelope and PhaseCoordinates. It
fully specifies a partial, but does not place it at a particular starting point in a composition.
--------------------------------------
*/

namespace RAINBOHz {

/// @brief Possible types of a single audio sample
enum class AudioSampleType { kPaxelFP, kPaxelInt, kPaxelBundleInt, kFullRange, kScaled };

using SamplePaxelFP = float;           // FP32 audio for individual paxel compute
using SamplePaxelInt = int32_t;        // 24-bit audio stored in 32-bit signed int
using SamplePaxelBundleInt = int32_t;  // 32-bit audio stored in 32-bit signed int
using SampleLabelFullRange = int64_t;  // 64-bit audio stored in 64-bit signed int
using SampleLabelScaled = int32_t;     // 24-bit audio stored in 32-bit signed int

// π, 2π and similar are values that will be needed throughout phase calculations.
// Note that M_PI does not have constexpr semantics (it's a macro); one reason to define it here.
// Also, strange as it might seem, M_PI is in fact not guaranteed to be present in <cmath>
// according to the C++ standard.
constexpr double PI = 3.14159265358979323846;
constexpr double ZERO_PI = 0.0;
constexpr double TWO_PI = 2.0 * PI;
constexpr double HALF_PI = 0.5 * PI;          // Useful for test cases
constexpr double ONE_AND_HALF_PI = 1.5 * PI;  // Useful for test cases
constexpr double NATURAL_PHASE =
    -999999999;  // Rogue value for use in phase coordinates where natural value is desired

// To be used when scaling from FP to INT.
// Maximum value for a 24 bit sample.
constexpr int32_t kMaxSamplePaxelInt = 0x7FFFFF;
// Actual scaling will take place using user defined (or perhaps autoscale)
// attenuation / bit shift value.
constexpr int32_t kMaxSamplePaxelBundleInt = INT32_MAX;
constexpr int64_t kMaxSampleLabelFullRange = 0xFFFFFFFFFFFFFFFF;

constexpr uint16_t kPaxelBytesPerSample = 3;
constexpr uint16_t kPaxelBytesPerSampleMem = 4;
constexpr uint16_t kPaxelBundleBytesPerSample = 4;
constexpr uint16_t kLabelBytesPerSample = 8;
constexpr uint16_t kLabelScaledBytesPerSample = 3;
constexpr uint16_t kLabelScaledBytesPerSampleMem = 3;

constexpr uint16_t kPaxelBitDepth = kPaxelBytesPerSample * 8;

// Likely to move to a user configurable value early in the project
constexpr uint32_t kSampleRate = 96000;  // 96kHz

// Limits on audio frequencies
constexpr double kMinAudioFrequency = 20;
constexpr double kMaxAudioFrequency = 20000;

/// @brief Simple, immutable type representing the parameters of a paxel.
struct PaxelSpecification {
   public:
    PaxelSpecification(double startFrequency, double endFrequency, double startAmplitude,
                       double endAmplitude, double startPhase, double endPhase,
                       uint32_t durationSamples, uint32_t startSample, uint32_t endSample)
        : startFrequency(startFrequency),
          endFrequency(endFrequency),
          startAmplitude(startAmplitude),
          endAmplitude(endAmplitude),
          startPhase(startPhase),
          endPhase(endPhase),
          durationSamples(durationSamples),
          startSample(startSample),
          endSample(endSample) {
        // Invariants
        assert(durationSamples > 0);
        assert(startSample >= 0 && startSample <= durationSamples);
        assert(endSample >= startSample &&
               endSample <= durationSamples);  // A one sample paxel is valid
        assert(startAmplitude >= -1.0 && startAmplitude <= 1.0);
        assert(endAmplitude >= -1.0 && endAmplitude <= 1.0);
        assert(startPhase >= 0.0 && startPhase <= TWO_PI);
        assert(endPhase >= 0.0 && endPhase <= TWO_PI);

        // Interesting question if frequencies can go outside of audio range.
        // Current thinking is that they can - but that no signal should be generated when they do,
        // especially for high frequencies. It can be interesting when a transformation takes place
        // that a partial reaches over the limits, then moves back into the audio frequency range.
        // This is expected to be something sound designers could exploit creatively - but it does
        // require special features in the generation code that are not yet present.
        assert(startFrequency > 0);
        assert(endFrequency > 0);
    }

    const double startFrequency;    /**< Start frequency of the paxel, Hz */
    const double endFrequency;      /**< End frequency of the paxel, Hz */
    const double startAmplitude;    /**< Start amplitude of the paxel in the range [-1.0,1.0] */
    const double endAmplitude;      /**< End amplitude of the paxel in the range [-1.0,1.0] */
    const double startPhase;        /**< Start phase of the paxel in the range [0,2π] */
    const double endPhase;          /**< End phase of the paxel in the range [0,2π] */
    const uint32_t durationSamples; /**< Total duration of the paxel in samples */
    const uint32_t startSample;     /**< Sample number at which to start rendering (from zero) */
    const uint32_t endSample;       /**< Sample number at which to end rendering (from zero) */
};

/// @brief A MultiPaxel represents an immutable single complete paxel in terms of a vector of paxels
/// that are intended to be merged into a single paxel within a single partial. It is an invariant
/// that the paxels are in time order. It is an invariant that the paxels do not overlap. It is not
/// an invariant that the paxels cover the entire paxel - there may be gaps at either or both ends
/// which will be rendered to zero value samples, which may occur at the srart or end of a partial.
/// All of the paxels must be of the same length. The paxels must also have exactly aligned
/// frequency, amplitude and phase value where they join.
struct MultiPaxelSpecification {
   public:
    MultiPaxelSpecification(const std::vector<PaxelSpecification>& paxels) : paxels(paxels) {
        // Invariants
        assert(paxels.size() > 0);
        // Lambda function with a capture by reference [&] to check that there are no overlaps
        // between the paxels.
        assert([&]() {
            // Check that there are no overlaps.
            // Note that invariants of PaxelSpecification itself already provide some protections.
            // If there is only one paxel in the vector then it is inherent that there are no
            // discontinuities, hence safe to start loop from 1.
            for (int i = 1; i < paxels.size(); ++i) {
                if (paxels[i].durationSamples != paxels[i - 1].durationSamples) {
                    return false;  // Fail if there is a length mismatch
                }
                if (paxels[i].startSample != (paxels[i - 1].endSample + 1) ||
                    paxels[i].startFrequency != paxels[i - 1].endFrequency ||
                    paxels[i].startPhase != paxels[i - 1].endPhase ||
                    paxels[i].startAmplitude != paxels[i - 1].endAmplitude) {
                    return false;  // Fail if the transition between paxels would generate a
                                   // discontinuity.
                }
            }
            return true;  // If there are no discontinuities, pass the assert
        }());
    }

    const std::vector<PaxelSpecification> paxels; /**< The paxels that define this MultiPaxel */
};

/// @brief A PartialSpecification represents the specification of a timeline of an individual
/// partial, expressed as a vector of MultiPaxelSpecification. The vector is a sequence in time
/// order, representing relative time (not absolute time).
/// It is an invariant that there are no gaps and that thhere are no discontinuities during the
/// sequence of paxels. This means that at every multipaxel boundary, the frequency, amplitude and
/// phase must be exactly aligned. It is also an invariant that all of the paxels are the same
/// length. This isn't strictly necessary, but it is related to the overall concept of paxel-based
/// additive synthesis.
struct PartialSpecification {
   public:
    PartialSpecification(const std::vector<MultiPaxelSpecification>& multiPaxels)
        : multiPaxels(multiPaxels) {
        // Invariants
        assert(multiPaxels.size() > 0);

        // Lambda function with a capture by reference [&] to check that invariants that apply to
        // the vector of multipaxel specifications.
        assert([&]() {
            // Check that there are no discontinuities.
            // If there is only one multipaxel in the vector then it is inherent that there
            // are no discontinuities, loop will then not start. Note the final checks for
            // duration to ensure that there is not a multipaxel present that has a gap at
            // the end, with the exception of the last multipaxel and that there is not a
            // multipaxel present with a gap at the start, with the exception of the first
            // multipaxel.
            for (int i = 1; i < multiPaxels.size(); ++i) {
                // Boundary has a back and a front
                PaxelSpecification backPaxelSpecification = multiPaxels[i - 1].paxels.back();
                PaxelSpecification frontPaxelSpecification = multiPaxels[i].paxels.front();

                // Validate that the specification of the back is the same as the specification
                // of the front and that there are no gaps at the boundary.
                if (frontPaxelSpecification.durationSamples !=
                        backPaxelSpecification.durationSamples ||
                    frontPaxelSpecification.startFrequency != backPaxelSpecification.endFrequency ||
                    frontPaxelSpecification.startAmplitude != backPaxelSpecification.endAmplitude ||
                    frontPaxelSpecification.startPhase != backPaxelSpecification.endPhase ||
                    backPaxelSpecification.endSample !=
                        backPaxelSpecification.durationSamples - 1 ||
                    frontPaxelSpecification.startSample != 0) {
                    return false;  // Fail if the transition between multipaxels would generate
                                   // a discontinuity.
                }
            }
            return true;  // If there are no discontinuities, pass the assert
        }());
    }

    const std::vector<MultiPaxelSpecification>
        multiPaxels; /**< The multipaxels that define this partial */
};

/// @brief A set of partials that are related in some way and are to be rendered together.
struct MultiPartialSpecification {
   public:
    MultiPartialSpecification(const std::vector<PartialSpecification>& partials)
        : partials(partials) {
        // Invariants
        assert(partials.size() > 0);

        // All other invariants are handled by PartialSpecification itself.
    }

    const std::vector<PartialSpecification>
        partials; /**< The partials that define this multipartial  */
};

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H
