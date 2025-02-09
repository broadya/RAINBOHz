#ifndef ENVELOPETYPES_H
#define ENVELOPETYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <list>
#include <numeric>
#include <ranges>
#include <vector>

#include "audio_helpers.h"
#include "audio_types.h"

/*
--------------------------------------
Describes all of the types that are used in processing envelopes.

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

/*
--------------------------------------
Logical envelope, closer to concepts in the mind of the composer or sound designer.

The logical envelopes are based on the envelope concept from sclang.
--------------------------------------
*/

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
          timesSamples(initConvertSecondsToSamples(times)),
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
    static std::vector<uint32_t> initConvertSecondsToSamples(
        const std::vector<double>& convertTimesSeconds) {
        std::vector<uint32_t> convertTimesSamples(convertTimesSeconds.size());
        std::ranges::transform(convertTimesSeconds, convertTimesSamples.begin(), [](double val) {
            return secondsToSamples(val);  // Scale and convert to uint32_t
        });
        return convertTimesSamples;
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

    const AmplitudeEnvelope amplitudeEnvelope; /**< The amplitude envelope of the partial */
    const FrequencyEnvelope frequencyEnvelope; /**< The frequency envelope of the partial */
    const PhaseCoordinates phaseCoordinates;   /**< The phase coordinates of the partial */
};

/*
--------------------------------------
Physical envelope, closer to the rendering engine.
--------------------------------------

- Curves mapped to a set of linear approximations. (TODO!)

- Frequency envelope and phase coordinates merged into a single concept of cycle coordinates.

- Amplitude envelope expressed in absolute time, not in relative time.

- Time expressed in samples not in seconds.

- The frequency is the normalised frequency, "cycles per sample"

*/

struct PhysicalFrequencyCoordinate {
   public:
    /// @brief Constructor that also normalises the frequency.
    /// @param frequencyHz Frequency in Hz.
    /// @param timeSeconds Time in seconds
    PhysicalFrequencyCoordinate(double frequencyHz, double timeSeconds)
        : frequency(normalizeFrequency(frequencyHz)), timeSamples(secondsToSamples(timeSeconds)) {
        // Preconditions
        assert(timeSeconds >= 0);
        // Invariants
        assert(frequency > 0);
    }

    PhysicalFrequencyCoordinate(double frequency, uint32_t timeSamples)
        : frequency(frequency), timeSamples(timeSamples) {
        // Invariants
        assert(frequency > 0);
    }

    const double frequency;      // normalised
    const uint32_t timeSamples;  // relative to the start of the envelope
};

inline double frequencyRate(PhysicalFrequencyCoordinate coord1,
                            PhysicalFrequencyCoordinate coord2) {
    return (coord2.frequency - coord1.frequency) / (coord2.timeSamples - coord1.timeSamples);
}

struct PhysicalAmplitudeCoordinate {
    PhysicalAmplitudeCoordinate(double amplitude, double timeSeconds)
        : amplitude(amplitude), timeSamples(secondsToSamples(timeSeconds)) {
        // Preconditions
        assert(timeSeconds >= 0);
        // Invariants
        assert(amplitude >= -1.0 && amplitude <= 1.0);
    }

    PhysicalAmplitudeCoordinate(double amplitude, uint32_t timeSamples)
        : amplitude(amplitude), timeSamples(timeSamples) {
        // Invariants
        assert(amplitude >= -1.0 && amplitude <= 1.0);
    }

    const double amplitude;
    const uint32_t timeSamples;  // relative to the start of the envelope
};

inline double amplitudeRate(PhysicalAmplitudeCoordinate coord1,
                            PhysicalAmplitudeCoordinate coord2) {
    return (coord2.amplitude - coord1.amplitude) / (coord2.timeSamples - coord1.timeSamples);
}

struct PhysicalPhaseCoordinate {
    PhysicalPhaseCoordinate(double phase, double timeSeconds, bool natural)
        : phase(phase), timeSamples(secondsToSamples(timeSeconds)), natural(natural) {
        // Preconditions
        assert(timeSeconds >= 0);
        // Invariants
        assert(phase >= ZERO_PI && phase <= TWO_PI);
    }

    PhysicalPhaseCoordinate(double phase, uint32_t timeSamples, bool natural)
        : phase(phase), timeSamples(timeSamples), natural(natural) {
        // Invariants
        assert(phase >= ZERO_PI && phase <= TWO_PI);
    }

    PhysicalPhaseCoordinate(PhaseCoordinate phaseCoordinate)
        : phase(phaseCoordinate.value),
          timeSamples(secondsToSamples(phaseCoordinate.timeSeconds)),
          natural(phaseCoordinate.natural) {
        // Preconditions
        assert(phaseCoordinate.timeSeconds >= 0);
        // Invariants
        assert(phase >= ZERO_PI && phase <= TWO_PI);
    }

    double phase;
    bool natural;
    uint32_t timeSamples;  // relative to the start of the envelope
};

/// @brief A point (coordinate) in a physical envelope
struct PhysicalEnvelopePoint {
   public:
    PhysicalEnvelopePoint(const uint32_t timeSamples, const double cycleAccumulator,
                          const double frequency, const double frequencyRate,
                          const double amplitude, const double amplitudeRate)
        : timeSamples(timeSamples),
          cycleAccumulator(cycleAccumulator),
          frequency(frequency),
          frequencyRate(frequencyRate),
          amplitude(amplitude),
          amplitudeRate(amplitudeRate) {
        // invariants
        assert(frequency >= 0);  // Zero value is theoretically well defined, and is useful for
                                 // silent sections of paxels
        assert(cycleAccumulator >= 0);  // ? perhaps negative cycles are useful in some contexts
        assert(amplitude >= -1.0 && amplitude <= 1.0);
    }

    uint32_t timeSamples;
    // Values are intentionally not const to allow manipulation
    // Units of time are samples not seconds
    double cycleAccumulator; /**< Total cycles until this point (including fractional part -
                                determines phase) */
    double frequency;        /**< The normalised frequency of the partial at this point */
    double frequencyRate;    /**< The rate of change of frequency for the next envelope stage, df/dt
                                normalised */
    double amplitude;        /**< The amplitude of the partial at this point */
    double amplitudeRate;    /**< The rate of change of amplitude for the next envelope stage da/dt
                                normalised */
    auto operator<=>(const PhysicalEnvelopePoint&) const = default;
};

/// @brief Factory that creates a PhysicalEnvelopePoint between two existing PhysicalEnvelopePoints.
/// @param pointA The PhysicalEnvelopePoint that is earlier in time.
/// @param pointB The PhsuicalEnveloperPoint that is later in time,
/// @param samplesSincePointA The number of samples between the two (zero means at pointA)
/// @return A PhysicalEnveloperPoint that is interpolated between the two points.
inline PhysicalEnvelopePoint interpolate(const PhysicalEnvelopePoint& pointA,
                                         const PhysicalEnvelopePoint& pointB,
                                         uint32_t timeSamples) {
    // Preconditions
    assert(timeSamples >= pointA.timeSamples);
    assert(timeSamples <= pointB.timeSamples);

    double ratio = static_cast<double>(timeSamples - pointA.timeSamples) /
                   (pointB.timeSamples - pointA.timeSamples);

    return PhysicalEnvelopePoint{
        timeSamples,
        computeCycleAccumulator(pointA.cycleAccumulator, pointA.frequency, pointA.frequencyRate,
                                timeSamples - pointA.timeSamples),
        ratio * (pointB.frequency - pointA.frequency) + pointA.frequency,
        pointA.frequencyRate,
        ratio * (pointB.amplitude - pointA.amplitude) + pointA.amplitude,
        pointA.amplitudeRate};
};

/// @brief A means to specify a partial in terms of a single envelope, closer to the rendering
/// process.
struct PhysicalPartialEnvelope {
   public:
    PhysicalPartialEnvelope(const std::vector<std::vector<PhysicalEnvelopePoint>>& paxelPoints,
                            uint32_t firstPaxelIndex, double firstSampleFraction,
                            double lastSampleFraction)
        : paxelPoints(paxelPoints),
          firstPaxelIndex(firstPaxelIndex),
          firstSampleFraction(firstSampleFraction),
          lastSampleFraction(lastSampleFraction) {
        // Invariants to be defined.
    }

    // Data structure holding the coordinates mapped to paxels
    std::vector<std::vector<PhysicalEnvelopePoint>> paxelPoints;
    const uint32_t firstPaxelIndex;
    const double firstSampleFraction;
    const double lastSampleFraction;
};

}  // namespace RAINBOHz

#endif  // ENVELOPETYPES_H