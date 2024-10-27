#ifndef ENVELOPETYPES_H
#define ENVELOPETYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <vector>

#include "audio_helpers.h"
#include "audio_types.h"

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

/*
--------------------------------------
Logical envelope, closer to concepts in the mind of the composer or sound designer.
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

/*
--------------------------------------
Physical envelope, closer to the rendering engine.
--------------------------------------

- Curves mapped to a set of linear approximations.

- Frequency envelope and phase coordinates merged into a single concept of cycle coordinates.

- Amplitude envelope expressed in absolute time, not in relative time.

- Time expressed in samples not in seconds.

*/

/// @brief A point (coordinate) in a physical envelope
struct PhysicalEnvelopePoint {
   public:
    PhysicalEnvelopePoint(const uint32_t sampleNumber, const double value)
        : sampleNumber(sampleNumber), value(value) {
        // No invariants.
        // sampleNumber is unsigned, and 0 is allowed.
        // Negative values are allowed.
    }

    const uint32_t sampleNumber;
    double value;  // intentionally not const to allow efficient manipulation
};

/// @brief A generic physical envelope, expressed in terms of coordinates.
/// Values are floting point from -1.0 to 1.0.
/// PhysicalEnvlope always has at least two points (begin and end)
/// All PhysicalEnvelopes for the same partial have the same begin and end.
struct PhysicalEnvelope {
   public:
    PhysicalEnvelope(const std::vector<PhysicalEnvelopePoint>& coords) : coords(coords) {
        // Invariants
        // Points are coordinates describing the complete shape of the partial.
        // At a minimum they must have a start and an end coordinate.
        assert(coords.size() > 1);
    }

    const std::vector<PhysicalEnvelopePoint> coords;
};

/// @brief An envelope to be applied to the cycles of a partial.
/// Values are in radians.
/// This is a concept similar to the concept of a phase accumulator, but it does not "wrap around".
/// Because of this, its "rate of change" corresponds to frequency, and it also allows for exact
/// phase coordinates to be achieved.
struct PhysicalCycleEnvelope : public PhysicalEnvelope {
    PhysicalCycleEnvelope(const std::vector<PhysicalEnvelopePoint>& physicalEnvelopePoints)
        : PhysicalEnvelope(physicalEnvelopePoints) {
        // Invariants are handled by the superclass.
    }
};

/// @brief An envelope to be applied to the amplitude of a partial.
/// Amplitudes must remain in the range [-1.0,1.0].
/// It is allowed to use negative values, which correspond to phase inversion.
struct PhysicalAmplitudeEnvelope : public PhysicalEnvelope {
    PhysicalAmplitudeEnvelope(const std::vector<PhysicalEnvelopePoint>& physicalEnvelopePoints)
        : PhysicalEnvelope(physicalEnvelopePoints) {
        // Invariants are handled by the superclass.
    }
};

/// @brief A means to specify a partial in terms of envelopes, closer to the composer / sound
/// designer's view of the partial.
struct PhysicalPartialEnvelopes {
   public:
    PhysicalPartialEnvelopes(const PhysicalAmplitudeEnvelope& physicalAmplitudeEnvelope,
                             const PhysicalCycleEnvelope& physicalCycleEnvelope,
                             const uint32_t startSample)
        : physicalAmplitudeEnvelope(physicalAmplitudeEnvelope),
          physicalCycleEnvelope(physicalCycleEnvelope),
          startSample(startSample),
          endSample(startSample + physicalCycleEnvelope.coords.back().sampleNumber) {
        // Invariants
        // The two envelopes must start and end at the same point.
        // These envelopes are at absolute positions in the piece, so they do not always start at
        // sample zero.
        assert(physicalAmplitudeEnvelope.coords.front().sampleNumber ==
               physicalCycleEnvelope.coords.front().sampleNumber);
        assert(physicalAmplitudeEnvelope.coords.back().sampleNumber ==
               physicalCycleEnvelope.coords.back().sampleNumber);
    }

    const PhysicalAmplitudeEnvelope physicalAmplitudeEnvelope; /**< The amplitude envelope */
    const PhysicalCycleEnvelope physicalCycleEnvelope;         /**< The cycle envelope */
    const uint32_t startSample; /**< The first sample in the envelope ("front") */
    const uint32_t endSample;   /**< The last sample in the envelope ("back") */
};

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H