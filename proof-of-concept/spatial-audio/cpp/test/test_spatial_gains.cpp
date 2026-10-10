// SPDX-License-Identifier: Apache-2.0

// Property tests for the channel modes, ported from the Python oracle's
// tests/test_spatial.py. The oracle comparison itself is in test_oracle.cpp.

#include "rainbohz/spatial_poc/spatial_gains.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

namespace rainbohz::spatial_poc {
namespace {

constexpr double kTol = 1e-12;
constexpr double kPi = std::numbers::pi;

Radians deg(double degrees) {
    return Radians{degrees * kPi / 180.0};
}

SpatialPosition at(double azimuthDeg, double elevationDeg, double metres) {
    return {.azimuth = deg(azimuthDeg), .elevation = deg(elevationDeg), .distance = Metres{metres}};
}

std::vector<double> gains(ChannelMode mode, double azimuthDeg, double elevationDeg = 0.0) {
    std::vector<double> out;
    for (const LinearGain g : directionGains(mode, deg(azimuthDeg), deg(elevationDeg)).channels()) {
        out.push_back(g.value());
    }
    return out;
}

void expectGains(const std::vector<double>& actual, const std::vector<double>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t c = 0; c < actual.size(); ++c) {
        EXPECT_NEAR(actual[c], expected[c], kTol) << "channel " << c;
    }
}

struct Angles {
    Radians azimuth;
    Radians elevation;
};

// Uniform on the sphere: elevation from asin of a uniform z. Fixed seed.
std::vector<Angles> randomDirections(std::size_t n) {
    // A fixed seed is the point: the tests must be reproducible.
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
    static std::mt19937_64 rng{20261004};
    std::uniform_real_distribution<double> az{-kPi, kPi};
    std::uniform_real_distribution<double> z{-1.0, 1.0};
    std::vector<Angles> out;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = az(rng);
        out.push_back({.azimuth = Radians{a}, .elevation = Radians{std::asin(z(rng))}});
    }
    return out;
}

std::vector<double> gains(ChannelMode mode, const Angles& angles) {
    std::vector<double> out;
    for (const LinearGain g : directionGains(mode, angles.azimuth, angles.elevation).channels()) {
        out.push_back(g.value());
    }
    return out;
}

// ---- channel modes ---------------------------------------------------------

TEST(ChannelMode, FromIntAcceptsEveryDefinedMode) {
    for (std::int8_t v = 0; v <= 7; ++v) {
        EXPECT_EQ(static_cast<std::int8_t>(channelModeFromInt(v)), v);
    }
}

TEST(ChannelMode, FromIntRejectsUndefinedValues) {
    EXPECT_THROW(channelModeFromInt(8), std::invalid_argument);
    EXPECT_THROW(channelModeFromInt(-1), std::invalid_argument);
}

TEST(ChannelMode, Labels) {
    EXPECT_EQ(channelLabels(ChannelMode::Mono)[0], "M");
    EXPECT_EQ(channelLabels(ChannelMode::StereoFromMidSide)[0], "L");
    EXPECT_EQ(channelLabels(ChannelMode::MidSide)[1], "S");
    EXPECT_EQ(channelLabels(ChannelMode::BlumleinClassic)[0], "L");
    EXPECT_EQ(channelLabels(ChannelMode::BlumleinRotated)[0], "M");
    EXPECT_EQ(channelLabels(ChannelMode::FoaAFormat)[3], "BRU");
    EXPECT_EQ(channelLabels(ChannelMode::FoaBFormat)[1], "Y");
}

TEST(ChannelMode, GainsHaveOneEntryPerChannel) {
    EXPECT_EQ(channelCount(ChannelMode::Mono), 1U);
    EXPECT_EQ(channelCount(ChannelMode::FoaBFormat), 4U);
    for (std::int8_t v = 0; v <= 7; ++v) {
        const ChannelMode mode = channelModeFromInt(v);
        EXPECT_EQ(directionGains(mode, Radians{0.3}, Radians{0.2}).count, channelCount(mode));
    }
}

// ---- geometry and distance -------------------------------------------------

TEST(Geometry, AmbixAxes) {
    const Direction front = unitVector(deg(0.0), deg(0.0));
    const Direction left = unitVector(deg(90.0), deg(0.0));
    const Direction up = unitVector(deg(0.0), deg(90.0));
    EXPECT_NEAR(front.x, 1.0, kTol);
    EXPECT_NEAR(left.y, 1.0, kTol); // +azimuth is left
    EXPECT_NEAR(up.z, 1.0, kTol);
    EXPECT_NEAR(left.x, 0.0, kTol);
    EXPECT_NEAR(up.x, 0.0, kTol);
}

TEST(Distance, GainIsInverseDistance) {
    EXPECT_EQ(distanceGain(Metres{1.0}).value(), 1.0);
    EXPECT_DOUBLE_EQ(distanceGain(Metres{2.0}).value(), 0.5);
    EXPECT_DOUBLE_EQ(distanceGain(Metres{4.0}).value(), 0.25);
}

TEST(Distance, DoublingCosts6_02dB) {
    for (const double r : {1.0, 2.0, 3.7, 100.0}) {
        const double drop = 20.0 * std::log10(distanceGain(Metres{r}).value() /
                                              distanceGain(Metres{2.0 * r}).value());
        EXPECT_NEAR(drop, 6.0206, 1e-4);
    }
}

TEST(Distance, DoesNotChangeDirectionGains) {
    const SpatialPosition near = at(37.0, 12.0, 1.0);
    const SpatialPosition far = at(37.0, 12.0, 300.0);
    for (const ChannelMode mode : {ChannelMode::StereoFromMidSide, ChannelMode::FoaAFormat}) {
        const ChannelGains a = positionGains(mode, near);
        const ChannelGains b = positionGains(mode, far);
        for (std::size_t c = 0; c < a.count; ++c) {
            EXPECT_NEAR(a.gains[c].value(), b.gains[c].value() * 300.0, kTol);
        }
    }
}

TEST(Distance, PositionGainsApplyDistance) {
    const ChannelGains g = positionGains(ChannelMode::Mono, at(123.0, -40.0, 4.0));
    EXPECT_DOUBLE_EQ(g.gains[0].value(), 0.25);

    const double s = std::sin(30.0 * kPi / 180.0);
    const ChannelGains lr = positionGains(ChannelMode::StereoFromMidSide, at(30.0, 0.0, 2.0));
    EXPECT_NEAR(lr.gains[0].value(), 0.5 * (1.0 + s) / 2.0, kTol);
    EXPECT_NEAR(lr.gains[1].value(), 0.5 * (1.0 - s) / 2.0, kTol);
}

TEST(CheckPosition, AcceptsTheAmbisonicsRanges) {
    EXPECT_NO_THROW(checkPosition(at(0.0, 0.0, 1.0)));
    EXPECT_NO_THROW(checkPosition(at(37.0, -12.0, 250.0)));
    // The bounds themselves are in range.
    EXPECT_NO_THROW(checkPosition(
        {.azimuth = Radians{kPi}, .elevation = Radians{kPi / 2.0}, .distance = Metres{16384.0}}));
    EXPECT_NO_THROW(checkPosition(
        {.azimuth = Radians{-kPi}, .elevation = Radians{-kPi / 2.0}, .distance = Metres{1.0}}));
}

TEST(CheckPosition, RejectsAzimuthOutsidePlusMinusPi) {
    // No wrapping: 270° is not accepted as −90°.
    EXPECT_THROW(checkPosition(at(270.0, 0.0, 1.0)), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(-180.5, 0.0, 1.0)), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(720.0, 0.0, 1.0)), std::invalid_argument);
    EXPECT_THROW(checkPosition({.azimuth = Radians{std::nextafter(kPi, 4.0)},
                     .elevation = Radians{0.0},
                     .distance = Metres{1.0}}),
        std::invalid_argument);
}

TEST(CheckPosition, RejectsElevationOutsidePlusMinusHalfPi) {
    EXPECT_THROW(checkPosition(at(0.0, 90.5, 1.0)), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(0.0, -135.0, 1.0)), std::invalid_argument);
}

TEST(CheckPosition, RejectsBadDistances) {
    EXPECT_THROW(checkPosition(at(0.0, 0.0, 0.5)), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(0.0, 0.0, std::nextafter(1.0, 0.0))), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(0.0, 0.0, 16385.0)), std::invalid_argument);
    EXPECT_THROW(checkPosition(at(0.0, 0.0, std::nan(""))), std::invalid_argument);
}

TEST(CheckPosition, RejectsNonFiniteAngles) {
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_THROW(
        checkPosition({.azimuth = Radians{inf}, .elevation = deg(0.0), .distance = Metres{1.0}}),
        std::invalid_argument);
    EXPECT_THROW(
        checkPosition(
            {.azimuth = deg(0.0), .elevation = Radians{std::nan("")}, .distance = Metres{1.0}}),
        std::invalid_argument);
}

// ---- continuity --------------------------------------------------------------

TEST(Continuity, AcrossTheRearSeamAzimuthPlusMinusPi) {
    // ±π are the same direction, so the gains agree there.
    for (int m = 0; m < 8; ++m) {
        const auto mode = static_cast<ChannelMode>(m);
        for (const double el : {-60.0, 0.0, 30.0}) {
            expectGains(gains(mode, 180.0, el), gains(mode, -180.0, el));
        }
    }
}

TEST(Continuity, FromPoleToPole) {
    // A 0.5° step in elevation from −90° to 90° never jumps by more than a
    // first-order pattern's slope allows.
    const double step = 0.5;
    const double bound = (step * kPi / 180.0) + 1e-9; // |d/dθ (a + (1 − a)cos θ)| ≤ 1
    for (int m = 0; m < 8; ++m) {
        const auto mode = static_cast<ChannelMode>(m);
        std::vector<double> previous = gains(mode, 20.0, -90.0);
        for (int i = 1; i <= 360; ++i) {
            const std::vector<double> current = gains(mode, 20.0, -90.0 + (i * step));
            for (std::size_t c = 0; c < current.size(); ++c) {
                EXPECT_LE(std::abs(current[c] - previous[c]), bound);
            }
            previous = current;
        }
    }
}

// ---- mono ------------------------------------------------------------------

TEST(Mono, IsOmni) {
    for (const Angles& a : randomDirections(50)) {
        expectGains(gains(ChannelMode::Mono, a), {1.0});
    }
}

// ---- theoretical M/S -------------------------------------------------------

TEST(StereoFromMidSide, HardLeftAndRight) {
    expectGains(gains(ChannelMode::StereoFromMidSide, 90.0), {1.0, 0.0});
    expectGains(gains(ChannelMode::StereoFromMidSide, -90.0), {0.0, 1.0});
}

TEST(StereoFromMidSide, FrontAndBackAreCentred) {
    expectGains(gains(ChannelMode::StereoFromMidSide, 0.0), {0.5, 0.5});
    expectGains(gains(ChannelMode::StereoFromMidSide, 180.0), {0.5, 0.5});
}

TEST(StereoFromMidSide, LeftPlusRightEqualsMid) {
    for (const Angles& a : randomDirections(200)) {
        const std::vector<double> g = gains(ChannelMode::StereoFromMidSide, a);
        EXPECT_NEAR(g[0] + g[1], 1.0, kTol);
    }
}

TEST(MidSide, SShape) {
    auto s = [](double az, double el = 0.0) { return gains(ChannelMode::MidSide, az, el)[1]; };
    EXPECT_NEAR(s(0.0), 0.0, kTol);
    EXPECT_NEAR(s(90.0), 1.0, kTol);
    EXPECT_NEAR(s(180.0), 0.0, kTol);
    EXPECT_NEAR(s(270.0), -1.0, kTol);
}

TEST(MidSide, MIsOmni) {
    for (const Angles& a : randomDirections(50)) {
        EXPECT_NEAR(gains(ChannelMode::MidSide, a)[0], 1.0, kTol);
    }
}

TEST(MidSide, SFadesToZeroAtThePoles) {
    double previous = 2.0;
    for (int el = 0; el <= 90; ++el) {
        const double s = gains(ChannelMode::MidSide, 90.0, el)[1];
        EXPECT_NEAR(s, std::cos(el * kPi / 180.0), kTol);
        EXPECT_LE(s, previous + kTol);
        previous = s;
    }
    EXPECT_NEAR(gains(ChannelMode::MidSide, -90.0, -90.0)[1], 0.0, kTol);
}

TEST(MidSide, SConesAboutYAxis) {
    for (const Angles& a : randomDirections(100)) {
        EXPECT_NEAR(gains(ChannelMode::MidSide, a)[1], unitVector(a.azimuth, a.elevation).y, kTol);
    }
}

TEST(MidSide, OverheadIsMidOnly) {
    expectGains(gains(ChannelMode::MidSide, 0.0, 90.0), {1.0, 0.0});
}

// ---- X/Y -------------------------------------------------------------------

TEST(StereoFromXY, CardioidsAt45) {
    const double c45 = 0.5 + (0.5 * std::cos(kPi / 4.0));
    const double c135 = 0.5 + (0.5 * std::cos(3.0 * kPi / 4.0));
    expectGains(gains(ChannelMode::StereoFromXY, 0.0), {c45, c45}); // −1.38 dB each
    EXPECT_NEAR(20.0 * std::log10(c45), -1.38, 0.005);
    expectGains(gains(ChannelMode::StereoFromXY, 45.0), {1.0, 0.5});
    expectGains(gains(ChannelMode::StereoFromXY, -45.0), {0.5, 1.0});
    expectGains(gains(ChannelMode::StereoFromXY, 180.0), {c135, c135});
    expectGains(gains(ChannelMode::StereoFromXY, -135.0), {0.0, 0.5});
}

TEST(StereoFromXY, CardioidsNeverReversePolarity) {
    for (const Angles& a : randomDirections(500)) {
        for (const double g : gains(ChannelMode::StereoFromXY, a)) {
            EXPECT_GE(g, -kTol);
        }
    }
}

TEST(StereoFromXY, IsLeftRightSymmetric) {
    for (const Angles& a : randomDirections(100)) {
        const std::vector<double> g = gains(ChannelMode::StereoFromXY, a);
        const std::vector<double> mirror = gains(ChannelMode::StereoFromXY,
            {.azimuth = Radians{-a.azimuth.value()}, .elevation = a.elevation});
        EXPECT_NEAR(g[0], mirror[1], kTol);
        EXPECT_NEAR(g[1], mirror[0], kTol);
    }
}

TEST(StereoFromXY, OverheadIsCentred) {
    for (int az = -180; az < 180; az += 30) {
        expectGains(gains(ChannelMode::StereoFromXY, az, 90.0), {0.5, 0.5});
    }
}

// ---- Blumlein --------------------------------------------------------------

TEST(BlumleinClassic, StageIsPlusMinus45) {
    expectGains(gains(ChannelMode::BlumleinClassic, 45.0), {1.0, 0.0});
    expectGains(gains(ChannelMode::BlumleinClassic, -45.0), {0.0, 1.0});
    const std::vector<double> g = gains(ChannelMode::BlumleinClassic, 90.0);
    EXPECT_NEAR(g[0], -g[1], kTol); // 90° is antiphase, not hard left
}

TEST(BlumleinClassic, RearIsMirroredAndInverted) {
    for (int step = -18; step <= 18; ++step) {
        const double phi = 5.0 * step;
        const std::vector<double> front = gains(ChannelMode::BlumleinClassic, phi);
        const std::vector<double> rear = gains(ChannelMode::BlumleinClassic, 180.0 - phi);
        EXPECT_NEAR(rear[0], -front[1], kTol);
        EXPECT_NEAR(rear[1], -front[0], kTol);
    }
}

TEST(BlumleinRotated, IsFigure8MidAndSide) {
    expectGains(gains(ChannelMode::BlumleinRotated, 0.0), {1.0, 0.0});
    expectGains(gains(ChannelMode::BlumleinRotated, 90.0), {0.0, 1.0});
    expectGains(gains(ChannelMode::BlumleinRotated, 180.0), {-1.0, 0.0});
}

TEST(Blumlein, OverheadIsSilentInBothOrientations) {
    for (int az = -180; az < 180; az += 30) {
        expectGains(gains(ChannelMode::BlumleinClassic, az, 90.0), {0.0, 0.0});
        expectGains(gains(ChannelMode::BlumleinRotated, az, -90.0), {0.0, 0.0});
    }
}

TEST(Blumlein, DiffersFromMidSide) {
    // Same channel names as mode 3, different microphones: M is a figure-8, not an omni.
    const std::vector<double> rotated = gains(ChannelMode::BlumleinRotated, 30.0, 20.0);
    const std::vector<double> midSide = gains(ChannelMode::MidSide, 30.0, 20.0);
    EXPECT_GT(std::abs(rotated[0] - midSide[0]), 0.1);
}

TEST(Blumlein, RotatedDecodedToLeftRightEqualsClassic) {
    // Why mode 5 is raw: decoded with L, R = (M ± S)/√2 it would be mode 4.
    for (const Angles& a : randomDirections(200)) {
        const std::vector<double> ms = gains(ChannelMode::BlumleinRotated, a);
        const std::vector<double> lr = gains(ChannelMode::BlumleinClassic, a);
        EXPECT_NEAR((ms[0] + ms[1]) / std::numbers::sqrt2, lr[0], kTol);
        EXPECT_NEAR((ms[0] - ms[1]) / std::numbers::sqrt2, lr[1], kTol);
    }
}

// ---- FOA -------------------------------------------------------------------

TEST(FoaAFormat, CapsuleOnAxisGainIsOne) {
    const double el = std::asin(std::numbers::inv_sqrt3) * 180.0 / kPi;
    const std::array<std::array<double, 2>, 4> capsules{
        {{45.0, el}, {-45.0, -el}, {135.0, -el}, {-135.0, el}}};
    for (std::size_t i = 0; i < capsules.size(); ++i) {
        EXPECT_NEAR(gains(ChannelMode::FoaAFormat, capsules[i][0], capsules[i][1])[i], 1.0, kTol);
    }
}

TEST(FoaAFormat, CardioidsSumToTwiceOmni) {
    for (const Angles& a : randomDirections(200)) {
        double sum = 0.0;
        for (const double g : gains(ChannelMode::FoaAFormat, a)) {
            sum += g;
        }
        EXPECT_NEAR(sum, 2.0, kTol);
    }
}

TEST(FoaBFormat, DecodingAFormatMatchesDirectEncoding) {
    // A-format (FLU, FRD, BLD, BRU) of perfect cardioids to AmbiX SN3D (W, Y, Z, X).
    const double h = std::numbers::sqrt3 / 2.0;
    const std::array<std::array<double, 4>, 4> aToB{{
        {0.5, 0.5, 0.5, 0.5},
        {h, -h, h, -h},
        {h, -h, -h, h},
        {h, h, -h, -h},
    }};
    for (const Angles& a : randomDirections(500)) {
        const std::vector<double> aFormat = gains(ChannelMode::FoaAFormat, a);
        const std::vector<double> bFormat = gains(ChannelMode::FoaBFormat, a);
        for (std::size_t row = 0; row < 4; ++row) {
            double decoded = 0.0;
            for (std::size_t col = 0; col < 4; ++col) {
                decoded += aToB[row][col] * aFormat[col];
            }
            EXPECT_NEAR(decoded, bFormat[row], kTol);
        }
    }
}

TEST(FoaBFormat, IsAmbixSn3d) {
    for (const Angles& a : randomDirections(50)) {
        const Direction d = unitVector(a.azimuth, a.elevation);
        expectGains(gains(ChannelMode::FoaBFormat, a), {1.0, d.y, d.z, d.x});
    }
}

TEST(FoaBFormat, CardinalDirections) {
    expectGains(gains(ChannelMode::FoaBFormat, 0.0), {1.0, 0.0, 0.0, 1.0});
    expectGains(gains(ChannelMode::FoaBFormat, 90.0), {1.0, 1.0, 0.0, 0.0});
    expectGains(gains(ChannelMode::FoaBFormat, 0.0, 90.0), {1.0, 0.0, 1.0, 0.0});
}

} // namespace
} // namespace rainbohz::spatial_poc
