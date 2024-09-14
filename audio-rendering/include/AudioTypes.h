#ifndef AUDIOTYPES_H
#define AUDIOTYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace RAINBOHz {

using SamplePaxelFP = float;           // FP32 audio for individual paxel compute
using SamplePaxelInt = int32_t;        // 24-bit audio stored in 32-bit int
using SamplePaxelGroupInt = int32_t;   // 32-bit audio stored in 32-bit int
using SampleLabelFullRange = int64_t;  // 64-bit audio stored in 64-bit int
using SampleLabelScaled = int32_t;     // 24-bit audio stored in 32-bit int;

// π and 2π are values that will be needed throughout in phase calculations
// Note that M_PI does not have constexpr semantics (it's a macro); one reason to define it here.
// Also, strange as it might seem, M_PI is in fact not guaranteed to be present in <cmath>
// according to the C++ standard.
constexpr double PI = 3.14159265358979323846;
constexpr double ZERO_PI = 0.0;
constexpr double TWO_PI = 2.0 * PI;
constexpr double HALF_PI = 0.5 * PI;          // Useful for test cases
constexpr double ONE_AND_HALF_PI = 1.5 * PI;  // Useful for test cases

// To be used when scaling from FP to INT. 23 due to possibility of negative values.
constexpr uint32_t kMaxSamplePaxelInt = 1 << 23;

constexpr uint32_t kMaxSamplePaxelGroupInt = 1 << 31;
constexpr uint64_t kMaxSampleLabelFullRange = 1LL << 63;

// Actual scaling will take place using user defined (or perhaps autoscale)
// attenuation / bit shift value.
constexpr uint64_t kMaxSampleLabelScaled = 1LL << 23;

constexpr uint16_t kPaxelBytesPerSample = 3;
constexpr uint16_t kPaxelBytesPerSampleMem = 4;
constexpr uint16_t kPaxelGroupBytesPerSample = 4;
constexpr uint16_t kLabelBytesPerSample = 8;
constexpr uint16_t kLabelScaledBytesPerSample = 3;
constexpr uint16_t kLabelScaledBytesPerSampleMem = 3;

constexpr uint16_t kPaxelBitDepth = kPaxelBytesPerSample * 8;

// Likely to move to a user configurable value early in the project
constexpr uint32_t kSampleRate = 96000;

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
        assert(startAmplitude >= 0.0 && startAmplitude <= 1.0);
        assert(endAmplitude >= 0.0 && endAmplitude <= 1.0);
        assert(startPhase >= 0.0 && startPhase <= TWO_PI);
        assert(endPhase >= 0.0 && endPhase <= TWO_PI);

        // Interesting question if frequencies can go outside of audio range.
        // Current thinking is that they can - but that no signal should be generated when they do,
        // especially for high frequencies. It can be interesting when a transformation takes place
        // that a partial reaches over the limits, then moves back into the audio frequency range.
        // This is expected to be something sound designers could exploit creatively - but it does
        // require special features in the generation code.
        assert(startFrequency > 0);
        assert(endFrequency > 0);
    }

    const double startFrequency;
    const double endFrequency;
    const double startAmplitude;
    const double endAmplitude;
    const double startPhase;
    const double endPhase;
    const uint32_t durationSamples;
    const uint32_t startSample;
    const uint32_t endSample;
};

/// @brief A MultiPaxel represents an immutable single complete paxel in terms of a vector of paxels
/// that are intended to be merged into a single paxel within a single partial. It is an invariant
/// that the paxels are in time order. It is an invariant that the paxels do not overlap. It is not
/// an invariant that the paxels cover the entire paxel - there may be gaps at either or both ends
/// which will be rendered to zero value samples - but no overlaps. All of the paxels must be of the
/// same length. The paxels must also have exactly aligned frequency, amplitude and phase value
/// where they join.
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
            uint32_t paxelDurationSamples = paxels[0].durationSamples;
            uint32_t endSample = paxels[0].endSample + 1;
            double endFrequency = paxels[0].endFrequency;
            double endPhase = paxels[0].endPhase;
            double endAmplitude = paxels[0].endAmplitude;
            // If there is only one paxel in the vector then it is inherent that there are no
            // discontinuities.
            for (int i = 1; i < paxels.size(); ++i) {
                if (paxels[i].durationSamples != paxelDurationSamples) {
                    return false;  // Fail if there is a length mismatch
                }
                if (paxels[i].startSample != endSample ||
                    paxels[i].startFrequency != endFrequency || paxels[i].startPhase != endPhase ||
                    paxels[i].startAmplitude != endAmplitude) {
                    return false;  // Fail if the transition between paxels would generate a
                                   // discontinuity.
                }
                endSample = paxels[i].endSample + 1;
            }
            return true;  // If there are no discontinuities, pass the assert
        }());
    }

    const std::vector<PaxelSpecification> paxels;
};

/// @brief A PartialSpecification represents the specification of a timeline of an individual
/// partial, expressed as a vector of MultiPaxelSpecification. The timeline is a sequence in time
/// order, representing relative time (not absolute time). A partial also has a label, which
/// connects it to concepts in the composition; the label is a way to group partials together.
/// It is a constraint that there are no gaps, that the initial phase is 0 or π, that the final
/// phase is 0 or π, and that thhere are no discontinuities during the sequence of paxels. This
/// means that at every multipaxel boundary, the frequency, amplitude and phase must be exactly
/// aligned. There is also a constraint that all of the paxels are the same length. This isn't
/// strictly necessary, but it is related to the overall concept of paxel-based additive synthesis.
struct PartialSpecification {
   public:
    PartialSpecification(const std::vector<MultiPaxelSpecification>& multiPaxels)
        : multiPaxels(multiPaxels) {
        // Invariants
        assert(multiPaxels.size() > 0);

        // Check the phase of the start and end are correct. Note that this should be directly set
        // as parameters, but there is some risk here of floating point errors creating false
        // asserts here.
        assert([&]() {
            double startPhase = multiPaxels.front().paxels.front().startPhase;
            double endPhase = multiPaxels.back().paxels.back().endPhase;
            return ((startPhase == ZERO_PI || startPhase == PI) &&
                    (endPhase == ZERO_PI || endPhase == PI));
        }());

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

    const std::vector<MultiPaxelSpecification> multiPaxels;
};

/// @brief Possible types of envelope curve, intended to be semantically equivalent to sclang
enum class EnvelopeCurveType { lin, exp, sine, welch, step, numeric };

/// @brief Represents a single point on an envelope curve
struct EnvelopeCurvePoint {
   public:
    EnvelopeCurvePoint(EnvelopeCurveType envelopeCurveType)
        : envelopeCurveType(envelopeCurveType), numericValue(0) {};
    EnvelopeCurvePoint(double numericValue)
        : envelopeCurveType(EnvelopeCurveType::numeric), numericValue(numericValue) {};
    const EnvelopeCurveType envelopeCurveType;
    const double numericValue;  // this value is ignored if the curve type is not numeric
};

/// @brief A generic envelope, based on and intended to have the same semantics as, the Env
/// structure in sclang.
/// (TODO) Curves are not yet implemented and may only be implemented much later in this project.
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

    const std::vector<double> levels;
    const std::vector<double> timesSeconds;
    const std::vector<uint32_t> timesSamples;
    const std::vector<EnvelopeCurvePoint> curves;

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

struct AmplitudeEnvelope : public Envelope {
    AmplitudeEnvelope(const std::vector<double>& levels, const std::vector<double>& times,
                      const std::vector<EnvelopeCurvePoint>& curves)
        : Envelope(levels, times, curves) {
        // Invariants
        // The levels of an amplitude envelope must remain with an absolute value between 0 and 1.
        // Negative levels are unusual but are allowed and are expected to cause phase reversal.
        // This also emulates behaviour in sclang.
        assert(std::all_of(levels.begin(), levels.end(),
                           [](const double& level) { return (level >= -1.0 && level <= 1.0); }));
    }
};

/// @brief  Sets the positions where phase is expected to reach a certain set value.
/// This is not a concept in sclang, and therefore is not intended to provide interoperability.
struct PhaseCoordinate {
   public:
    PhaseCoordinate(double value, double time)
        : value(value), timeSeconds(time), timeSamples(secondsToSamples(time)) {
        // Invariants
        assert(!(time < 0.0));
        assert(value >= ZERO_PI && value <= TWO_PI);
        assert(timeSamples == static_cast<uint32_t>(timeSeconds * kSampleRate));
    }

    const double value;
    const double timeSeconds;
    const uint32_t timeSamples;

   private:
    uint32_t secondsToSamples(double timesSeconds) {
        return static_cast<uint32_t>(timesSeconds * kSampleRate);  // Scale and convert to uint32_t
    }
};

// PhaseCoordinates also define the limits on the partial. The start phase coordinate must be on
// time zero and the final phase coordinate corresponds to the end of the partial. This is necessary
// because it is a requirement to specify the phase (0 or π) at both the start and the end of the
// partial. The coordinates must be provided in time order.
// Note that times here represent coordinates, they are not relative to the previous coordinate like
// in Envelopes.
struct PhaseCoordinates {
   public:
    PhaseCoordinates(const std::vector<PhaseCoordinate>& coordinates) : coordinates(coordinates) {
        // Invariants
        // At a minimum the start and end phase must be determined.
        assert(coordinates.size() >= 2);

        // The first coordinate must be at time zero. It defines the start of the partial.
        assert(coordinates.front().timeSamples == 0);
        assert(coordinates.front().timeSeconds == 0.0);

        // Phase at start and end of the partial must be 0 or π
        assert(coordinates.front().value != ZERO_PI || coordinates.front().value != PI);
        assert(coordinates.back().value != ZERO_PI || coordinates.back().value != PI);

        // The more complex invariants
        assert([&]() {
            // Check that time coordinates are in ascending order
            return std::adjacent_find(coordinates.begin(), coordinates.end(),
                                      [](const PhaseCoordinate& a, const PhaseCoordinate& b) {
                                          return b.timeSeconds <= a.timeSeconds;
                                      }) == coordinates.end();
        }());
    }

    const std::vector<PhaseCoordinate> coordinates;
};

/// @brief A way to specify a partial in terms of envelopes, closer to the composer / sound
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

    const AmplitudeEnvelope amplitudeEnvelope;
    const FrequencyEnvelope frequencyEnvelope;
    const PhaseCoordinates phaseCoordinates;
};

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H
