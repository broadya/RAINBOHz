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
    -999999999;  // Rogue value for use in phase coordinates whre natural value is desired

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

/// @brief Possible types of envelope curve, intended to be semantically equivalent to sclang.
/// \warning Envelope curves are not yet implemented!
enum class EnvelopeCurveType { lin, exp, sine, welch, step, numeric };

/// @brief Represents a single point on an envelope curve.
/// \warning Envelope curves are not yet implemented!
struct EnvelopeCurvePoint {
   public:
    EnvelopeCurvePoint(EnvelopeCurveType envelopeCurveType)
        : envelopeCurveType(envelopeCurveType), numericValue(0) {};
    EnvelopeCurvePoint(double numericValue)
        : envelopeCurveType(EnvelopeCurveType::numeric), numericValue(numericValue) {};
    const EnvelopeCurveType
        envelopeCurveType; /**< The type of curve to use at this envelope point  */
    //  numericValue is ignored if the curve type is not numeric
    const double numericValue; /**< The value for the curve to use at this envelope point  */
};

/// @brief A generic envelope, based on and intended to have the same semantics as, the Env
/// structure in sclang.
/// \todo Curves are not yet implemented and may only be implemented much later in this project.
struct Envelope {
   public:
    Envelope(const std::vector<double>& levels, const std::vector<double>& times,
             const std::vector<EnvelopeCurvePoint>& curves)
        : levels(levels),
          timesSeconds(times),
          timesSamples(secondsToSamples(times)),
          curves(curves)

    {
        // Invariants
        // There must be at least one level - note that it is allowed to have just one point, which
        // is like a constant value.
        assert(levels.size() >= 1);
        // It is allowed to have extra time elements, they are ignored. This is to emulate sclang.
        // It is allowed to have zero time elements. This represents a constant value.
        assert(timesSeconds.size() >= levels.size() - 1);
        // Time values must not be negative
        assert(std::none_of(timesSeconds.begin(), timesSeconds.end(),
                            [](const double& time) { return time < 0.0; }));
        // Times in seconds and in samples must be similar
        assert(timesSeconds.size() == timesSamples.size());

        // Curves are optional, so there are no invariants for curves.
    }

    const std::vector<double> levels;             /**< Levels in order of the steps  */
    const std::vector<double> timesSeconds;       /**< Durations in order of the steps (secs) */
    const std::vector<uint32_t> timesSamples;     /**< Durations in order of the steps (samples) */
    const std::vector<EnvelopeCurvePoint> curves; /**< Curves to apply to each step  */

   private:
    std::vector<uint32_t> secondsToSamples(const std::vector<double>& timesSeconds) {
        std::vector<uint32_t> timesSamples(timesSeconds.size());
        double scaleFactor = kSampleRate;
        std::transform(
            timesSeconds.begin(), timesSeconds.end(), timesSamples.begin(),
            [scaleFactor](double val) {
                return static_cast<uint32_t>(val * scaleFactor);  // Scale and convert to uint32_t
            });
        return timesSamples;
    }
};

/// @brief An envelope to be applied to the frequency of a partial.
struct FrequencyEnvelope : public Envelope {
    FrequencyEnvelope(const std::vector<double>& levels, const std::vector<double>& times,
                      const std::vector<EnvelopeCurvePoint>& curves)
        : Envelope(levels, times, curves) {
        // Invariants
        // The levels of a frequency envelope must remain positive
        // Currently not applying stricter rules as extreme values may be creatively interesting
        assert(std::all_of(levels.begin(), levels.end(),
                           [](const double& level) { return level > 0.0; }));
    }
};

/// @brief An envelope to be applied to the amplitude of a partial.
/// Amplitudes must remain in the range [-1.0,1.0].
/// It is allowed to use negative values, which correspond to phase inversion.
struct AmplitudeEnvelope : public Envelope {
    AmplitudeEnvelope(const std::vector<double>& levels, const std::vector<double>& times,
                      const std::vector<EnvelopeCurvePoint>& curves)
        : Envelope(levels, times, curves) {
        // Invariants
        // The levels of an amplitude envelope must remain within the range [-1.0,1.0].
        // Negative levels are unusual but are allowed and correspond to phase reversal.
        // This also emulates behaviour in sclang.
        assert(std::all_of(levels.begin(), levels.end(),
                           [](const double& level) { return (level >= -1.0 && level <= 1.0); }));
    }
};

/// @brief Specifies positions where phase is expected to reach a certain set value.
/// This is not a concept in sclang, and therefore is not intended to provide interoperability.
struct PhaseCoordinate {
   public:
    /// @brief Set a phase coordinate at a certain point in a partial.
    /// @param time The time of the pahse coordinate, expressed in seconds.
    /// @param value The phase, expressed in radians, in the range [0,2π].
    PhaseCoordinate(double time, double value)
        : timeSeconds(time), timeSamples(secondsToSamples(time)), value(value), natural(false) {
        // Invariants
        assert(time >= 0.0);
        assert(value >= ZERO_PI && value <= TWO_PI);
        assert(timeSamples == static_cast<uint32_t>(timeSeconds * kSampleRate));
    }

   public:
    /// @brief Set a phase coordinate indicating "natural" phase - the phase that would be achieved
    /// by simply continuing the cycles to this point. This is useful in order to set a kind of
    /// anchor point from which no control over phase is desired to another point at which control
    /// over phase is desired.
    /// @param time The time, in seconds, of the phase coordinate.
    PhaseCoordinate(double time)
        : timeSeconds(time),
          timeSamples(secondsToSamples(time)),
          value(NATURAL_PHASE),
          natural(true) {
        // Invariants
        assert(time > 0.0);  // natural phase not allowed as first coordinate
        assert(timeSamples == static_cast<uint32_t>(timeSeconds * kSampleRate));
    }

    const double timeSeconds;   /**< Time of the phase coordinate in seconds  */
    const uint32_t timeSamples; /**< Time of the phase coordinate in samples  */
    const double value;         /**< Phase value, if not "natural", in radians  */
    const bool natural;         /**< Flag indicating that "natural" phase is intended  */

   private:
    uint32_t secondsToSamples(double timesSeconds) {
        return static_cast<uint32_t>(timesSeconds * kSampleRate);  // Scale and convert to uint32_t
    }
};

/// @brief PhaseCoordinates also define the limits on the phase of the partial. The start phase
/// coordinate must be on time zero and the final phase coordinate corresponds to the end of the
/// partial. It is a requirement to explicitly specify the phase at the start of the partial. The
/// end coordinate may specify "natural" phase. The coordinates must be provided in time order. Note
/// that times here represent coordinates, they are not relative to the previous coordinate like in
/// Envelopes.
/// Note that phase coordinates do not really define an envelope as such, it may be
/// more useful to think of them as "target values".

struct PhaseCoordinates {
   public:
    PhaseCoordinates(const std::vector<PhaseCoordinate>& coordinates) : coordinates(coordinates) {
        // Invariants
        // At a minimum the start and end phase must be determined.
        assert(coordinates.size() >= 2);

        // The first coordinate must be at time zero. It defines the start of the partial.
        assert(coordinates.front().timeSamples == 0);
        assert(coordinates.front().timeSeconds == 0.0);
        // First coordinate must have a defined phase.
        assert(!coordinates.front().natural);

        // The more complex invariants
        assert([&]() {
            // Check that time coordinates are in ascending order
            return std::adjacent_find(coordinates.begin(), coordinates.end(),
                                      [](const PhaseCoordinate& a, const PhaseCoordinate& b) {
                                          return b.timeSeconds <= a.timeSeconds;
                                      }) == coordinates.end();
        }());
    }

    const std::vector<PhaseCoordinate> coordinates; /**< Vector of the coordinates in time order */
};

/// @brief A means to specify a partial in terms of envelopes, closer to the composer / sound
/// designer's view of the partial.
struct PartialEnvelopes {
   public:
    PartialEnvelopes(const AmplitudeEnvelope& amplitudeEnvelope,
                     const FrequencyEnvelope& frequencyEnvelope,
                     const PhaseCoordinates& phaseCoordinates)
        : amplitudeEnvelope(amplitudeEnvelope),
          frequencyEnvelope(frequencyEnvelope),
          phaseCoordinates(phaseCoordinates) {
        // Invariants are handled by the three structs theselves.
    }

    const AmplitudeEnvelope amplitudeEnvelope; /**< Tha amplitude envelope of the partial */
    const FrequencyEnvelope frequencyEnvelope; /**< Tha frequency envelope of the partial */
    const PhaseCoordinates phaseCoordinates;   /**< Tha phase coordinates of the partial */
};

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H
