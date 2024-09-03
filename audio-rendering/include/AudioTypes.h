#ifndef AUDIOTYPES_H
#define AUDIOTYPES_H

#include <cstdint>

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
constexpr double TWO_PI = 2.0 * PI;

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

constexpr uint16_t kMinAudioFrequencyHz = 20;
constexpr uint16_t kMaxAudioFrequencyHz = 20000;

/// @brief Simple type representing the parameters of a paxel.
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
          firstSample(firstSample),
          endSample(endSample) {}

    const double startFrequency;
    const double endFrequency;
    const double startAmplitude;
    const double endAmplitude;
    const double startPhase;
    const double endPhase;
    const uint32_t durationSamples;
    const uint32_t firstSample;
    const uint32_t endSample;
};

}  // namespace RAINBOHz

#endif  // AUDIOTYPES_H
