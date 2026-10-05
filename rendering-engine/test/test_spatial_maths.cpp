// Tests for the spatial maths: linear interpolation of a source's position,
// with the azimuth along the shorter arc and wrapped into [-π, π]
// (docs/rendering.md section 8.5).

#include "rainbohz/rendering_engine/spatial_maths.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace {

using rainbohz::rendering_engine::AngleGradient;
using rainbohz::rendering_engine::Azimuth;
using rainbohz::rendering_engine::azimuthAt;
using rainbohz::rendering_engine::azimuthGradient;
using rainbohz::rendering_engine::distanceAt;
using rainbohz::rendering_engine::DistanceGradient;
using rainbohz::rendering_engine::distanceGradient;
using rainbohz::rendering_engine::Elevation;
using rainbohz::rendering_engine::elevationAt;
using rainbohz::rendering_engine::elevationGradient;
using rainbohz::rendering_engine::Metres;
using rainbohz::rendering_engine::NominalPosition;
using rainbohz::rendering_engine::positionAt;
using rainbohz::rendering_engine::SampleDuration;
using rainbohz::rendering_engine::SpatialPosition;
using rainbohz::rendering_engine::raw::azimuthChange;
using rainbohz::rendering_engine::raw::wrapAzimuth;

constexpr double kPi = std::numbers::pi;
constexpr double kTol = 1e-12;

TEST(SpatialMaths, AzimuthChangeTakesTheShorterArc) {
    EXPECT_NEAR(azimuthChange(0.0, 1.0), 1.0, kTol);
    EXPECT_NEAR(azimuthChange(1.0, -1.0), -2.0, kTol);
    // From 3 to -3 rad the short way is anticlockwise, through π, not
    // clockwise through the front.
    EXPECT_NEAR(azimuthChange(3.0, -3.0), (2.0 * kPi) - 6.0, kTol);
    EXPECT_NEAR(azimuthChange(-3.0, 3.0), 6.0 - (2.0 * kPi), kTol);
}

TEST(SpatialMaths, AHalfTurnEitherWayIsTakenAnticlockwise) {
    EXPECT_DOUBLE_EQ(azimuthChange(0.0, kPi), kPi);
    EXPECT_DOUBLE_EQ(azimuthChange(kPi / 2.0, -kPi / 2.0), kPi);
    EXPECT_DOUBLE_EQ(azimuthChange(-kPi / 2.0, kPi / 2.0), kPi);
    EXPECT_DOUBLE_EQ(azimuthChange(kPi, 0.0), kPi);
}

TEST(SpatialMaths, WrapBringsAnAzimuthBackIntoRange) {
    EXPECT_DOUBLE_EQ(wrapAzimuth(0.5), 0.5);
    EXPECT_DOUBLE_EQ(wrapAzimuth(kPi), kPi);
    EXPECT_DOUBLE_EQ(wrapAzimuth(-kPi), -kPi);
    EXPECT_NEAR(wrapAzimuth(3.5), 3.5 - (2.0 * kPi), kTol);
    EXPECT_NEAR(wrapAzimuth(-3.5), (2.0 * kPi) - 3.5, kTol);
}

TEST(SpatialMaths, AzimuthInterpolatesAcrossTheBack) {
    const AngleGradient g = azimuthGradient(Azimuth{3.0}, Azimuth{-3.0}, SampleDuration{100.0});
    EXPECT_NEAR(g.value(), ((2.0 * kPi) - 6.0) / 100.0, kTol);
    EXPECT_DOUBLE_EQ(azimuthAt(Azimuth{3.0}, g, SampleDuration{0.0}).value(), 3.0);
    // Halfway, exactly behind.
    EXPECT_NEAR(std::abs(azimuthAt(Azimuth{3.0}, g, SampleDuration{50.0}).value()), kPi, 1e-12);
    // Past π the value is wrapped, and arrives at the end point.
    const double late = azimuthAt(Azimuth{3.0}, g, SampleDuration{75.0}).value();
    EXPECT_LT(late, 0.0);
    EXPECT_GE(late, -kPi);
    EXPECT_NEAR(azimuthAt(Azimuth{3.0}, g, SampleDuration{100.0}).value(), -3.0, kTol);
}

TEST(SpatialMaths, ElevationAndDistanceAreStraightLines) {
    const AngleGradient ge =
        elevationGradient(Elevation{-0.5}, Elevation{0.5}, SampleDuration{1000.0});
    EXPECT_DOUBLE_EQ(ge.value(), 0.001);
    EXPECT_DOUBLE_EQ(elevationAt(Elevation{-0.5}, ge, SampleDuration{250.0}).value(), -0.25);

    const DistanceGradient gd = distanceGradient(Metres{1.0}, Metres{9.0}, SampleDuration{8.0});
    EXPECT_DOUBLE_EQ(gd.value(), 1.0);
    EXPECT_DOUBLE_EQ(distanceAt(Metres{1.0}, gd, SampleDuration{2.5}).value(), 3.5);
}

TEST(SpatialMaths, PositionAtCombinesTheThree) {
    const NominalPosition nominal{.startAzimuth = Azimuth{kPi - 0.1},
        .azimuthGradient = AngleGradient{0.01},
        .startElevation = Elevation{0.2},
        .elevationGradient = AngleGradient{-0.01},
        .startDistance = Metres{2.0},
        .distanceGradient = DistanceGradient{0.5}};
    const SpatialPosition p = positionAt(nominal, SampleDuration{20.0});
    EXPECT_NEAR(p.azimuth.value(), -kPi + 0.1, kTol);
    EXPECT_NEAR(p.elevation.value(), 0.0, kTol);
    EXPECT_DOUBLE_EQ(p.distance.value(), 12.0);
}

} // namespace
