#include "rainbohz/spatial_poc/spatial_gains.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace rainbohz::spatial_poc {

namespace {

/// Nearest distance in the rendering model, metres (engine-schema.cypher).
constexpr double kMinDistanceMetres = 1.0;
/// Furthest distance in the rendering model, metres (engine-schema.cypher).
constexpr double kMaxDistanceMetres = 16384.0;

// First-order shape parameter `a` in a + (1 − a)·cos θ.

/// Pressure: gain 1 in every direction.
constexpr double kOmni = 1.0;
/// Half pressure, half gradient: null at the rear.
constexpr double kCardioid = 0.5;
/// Pressure gradient: nulls at the sides, rear lobe reversed.
constexpr double kFigure8 = 0.0;

constexpr double kInvSqrt2 = std::numbers::sqrt2 / 2.0;
constexpr double kInvSqrt3 = std::numbers::inv_sqrt3;

// Microphone axes. Written as exact unit vectors rather than computed from
// angles, so that they can be constexpr; the oracle computes them from angles,
// and the two agree to rounding.
constexpr Direction kFront{.x = 1.0, .y = 0.0, .z = 0.0};
constexpr Direction kLeft{.x = 0.0, .y = 1.0, .z = 0.0};
constexpr Direction kUp{.x = 0.0, .y = 0.0, .z = 1.0};
constexpr Direction kFrontLeft{.x = kInvSqrt2, .y = kInvSqrt2, .z = 0.0};   // azimuth +45°
constexpr Direction kFrontRight{.x = kInvSqrt2, .y = -kInvSqrt2, .z = 0.0}; // azimuth −45°

// The tetrahedron's vertices: azimuths ±45° and ±135°, elevations
// ±asin(1/√3) ≈ ±35.26°, which is the cube's corners (±1, ±1, ±1)/√3.
constexpr Direction kFlu{.x = kInvSqrt3, .y = kInvSqrt3, .z = kInvSqrt3};
constexpr Direction kFrd{.x = kInvSqrt3, .y = -kInvSqrt3, .z = -kInvSqrt3};
constexpr Direction kBld{.x = -kInvSqrt3, .y = kInvSqrt3, .z = -kInvSqrt3};
constexpr Direction kBru{.x = -kInvSqrt3, .y = -kInvSqrt3, .z = kInvSqrt3};

/// A perfect first-order microphone.
struct Mic {
    /// Shape parameter in [0, 1]: kOmni, kCardioid, kFigure8 or between.
    double a = kOmni;
    /// Unit vector the pattern points along; irrelevant for omni.
    Direction axis = kFront;
};

constexpr Mic kOmniMic{.a = kOmni, .axis = kFront};

/// A linear decode: one row per output channel, one column per microphone.
using DecodeMatrix = std::array<std::array<double, kMaxChannels>, kMaxChannels>;

/// A set of coincident microphones and an optional linear decode. Without a
/// decode the outputs are the microphones themselves.
struct MicArray {
    /// The microphones; only the first `micCount` are used.
    std::array<Mic, kMaxChannels> mics{};
    /// Number of microphones.
    std::size_t micCount = 0;
    /// Whether the outputs are `decode` applied to the microphones.
    bool decoded = false;
    /// The decode, when `decoded`.
    DecodeMatrix decode{};
};

// Omni M and figure-8 S to L/R: L = (M + S)/2, R = (M − S)/2. A constant-
// amplitude pan law (L + R = M): hard left is 0 dB in L only; centre is −6 dB
// in each.
constexpr DecodeMatrix kMidSideToLeftRight{{
    {0.5, 0.5, 0.0, 0.0},
    {0.5, -0.5, 0.0, 0.0},
}};

constexpr MicArray kMonoArray{.mics = {kOmniMic}, .micCount = 1};

// Omni M and a figure-8 S whose positive lobe faces left: S = cos(el)·sin(az).
constexpr MicArray kMidSideArray{
    .mics = {kOmniMic, Mic{.a = kFigure8, .axis = kLeft}},
    .micCount = 2,
};

constexpr MicArray kStereoFromMidSideArray{
    .mics = kMidSideArray.mics,
    .micCount = 2,
    .decoded = true,
    .decode = kMidSideToLeftRight,
};

constexpr MicArray kXYArray{
    .mics = {Mic{.a = kCardioid, .axis = kFrontLeft}, Mic{.a = kCardioid, .axis = kFrontRight}},
    .micCount = 2,
};

constexpr MicArray kBlumleinClassicArray{
    .mics = {Mic{.a = kFigure8, .axis = kFrontLeft}, Mic{.a = kFigure8, .axis = kFrontRight}},
    .micCount = 2,
};

constexpr MicArray kBlumleinRotatedArray{
    .mics = {Mic{.a = kFigure8, .axis = kFront}, Mic{.a = kFigure8, .axis = kLeft}},
    .micCount = 2,
};

constexpr MicArray kAFormatArray{
    .mics = {Mic{.a = kCardioid, .axis = kFlu}, Mic{.a = kCardioid, .axis = kFrd},
        Mic{.a = kCardioid, .axis = kBld}, Mic{.a = kCardioid, .axis = kBru}},
    .micCount = 4,
};

// B-format is encoded directly: an omni and figure-8s along y, z and x, in ACN
// order. Under SN3D, first order is W = 1, Y = y, Z = z, X = x for a unit
// source in direction (x, y, z), which is exactly what these patterns give.
// Decoding the A-format array gives the same result; a test proves it.
constexpr MicArray kBFormatArray{
    .mics = {kOmniMic, Mic{.a = kFigure8, .axis = kLeft}, Mic{.a = kFigure8, .axis = kUp},
        Mic{.a = kFigure8, .axis = kFront}},
    .micCount = 4,
};

/// The microphones and decode that make up a mode.
///
/// @param mode  A defined mode.
/// @return The mode's array, in static storage.
const MicArray& micArray(ChannelMode mode) noexcept {
    switch (mode) {
    case ChannelMode::Mono:
        return kMonoArray;
    case ChannelMode::StereoFromMidSide:
        return kStereoFromMidSideArray;
    case ChannelMode::StereoFromXY:
        return kXYArray;
    case ChannelMode::MidSide:
        return kMidSideArray;
    case ChannelMode::BlumleinClassic:
        return kBlumleinClassicArray;
    case ChannelMode::BlumleinRotated:
        return kBlumleinRotatedArray;
    case ChannelMode::FoaAFormat:
        return kAFormatArray;
    case ChannelMode::FoaBFormat:
        return kBFormatArray;
    }
    return kMonoArray; // unreachable for a defined mode
}

/// The signed gain of a first-order microphone for a source direction.
///
/// @param mic        The microphone.
/// @param direction  Unit vector towards the source.
/// @return a + (1 − a)·cos θ, θ the angle between the axis and `direction`:
///         1 on axis, negative where the polarity is reversed.
constexpr double patternGain(const Mic& mic, const Direction& direction) noexcept {
    const double cosTheta =
        (mic.axis.x * direction.x) + (mic.axis.y * direction.y) + (mic.axis.z * direction.z);
    return mic.a + ((1.0 - mic.a) * cosTheta);
}

/// Whether a value lies in a closed range.
///
/// Written as two comparisons that must both hold, so that NaN, which fails
/// every comparison, is never in range.
///
/// @param value  The value to test.
/// @param lo     Lowest value in range.
/// @param hi     Highest value in range.
/// @return True when lo ≤ value ≤ hi.
constexpr bool inRange(double value, double lo, double hi) noexcept {
    return value >= lo && value <= hi;
}

} // namespace

Direction unitVector(Radians azimuth, Radians elevation) noexcept {
    const double cosElevation = std::cos(elevation.value());
    return {.x = cosElevation * std::cos(azimuth.value()),
        .y = cosElevation * std::sin(azimuth.value()),
        .z = std::sin(elevation.value())};
}

ChannelGains directionGains(ChannelMode mode, Radians azimuth, Radians elevation) noexcept {
    const MicArray& array = micArray(mode);
    const Direction direction = unitVector(azimuth, elevation);

    std::array<double, kMaxChannels> raw{};
    for (std::size_t m = 0; m < array.micCount; ++m) {
        raw[m] = patternGain(array.mics[m], direction);
    }

    ChannelGains result;
    result.count = channelCount(mode);
    for (std::size_t c = 0; c < result.count; ++c) {
        if (!array.decoded) {
            result.gains[c] = LinearGain{raw[c]};
            continue;
        }
        double sum = 0.0;
        for (std::size_t m = 0; m < array.micCount; ++m) {
            sum += array.decode[c][m] * raw[m];
        }
        result.gains[c] = LinearGain{sum};
    }
    return result;
}

LinearGain distanceGain(Metres distance) noexcept {
    return LinearGain{1.0 / distance.value()};
}

ChannelGains positionGains(ChannelMode mode, const SpatialPosition& position) noexcept {
    ChannelGains result = directionGains(mode, position.azimuth, position.elevation);
    const LinearGain attenuation = distanceGain(position.distance);
    for (std::size_t c = 0; c < result.count; ++c) {
        result.gains[c] = result.gains[c] * attenuation;
    }
    return result;
}

void checkPosition(const SpatialPosition& position) {
    const double azimuth = position.azimuth.value();
    if (!inRange(azimuth, -std::numbers::pi, std::numbers::pi)) {
        throw std::invalid_argument(
            "azimuth must be in [-pi, pi] rad, got " + std::to_string(azimuth));
    }
    const double elevation = position.elevation.value();
    if (!inRange(elevation, -std::numbers::pi / 2.0, std::numbers::pi / 2.0)) {
        throw std::invalid_argument(
            "elevation must be in [-pi/2, pi/2] rad, got " + std::to_string(elevation));
    }
    const double metres = position.distance.value();
    if (!inRange(metres, kMinDistanceMetres, kMaxDistanceMetres)) {
        throw std::invalid_argument(
            "distance must be in [1, 16384] m, got " + std::to_string(metres));
    }
}

} // namespace rainbohz::spatial_poc
