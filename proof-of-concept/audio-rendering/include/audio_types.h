#ifndef AUDIOTYPES_H
#define AUDIOTYPES_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numbers>
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
constexpr double PI = std::numbers::pi;
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

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H
