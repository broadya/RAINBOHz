import math

import numpy as np
import pytest

from spatial_poc.geometry import SourcePosition, distance_gain, unit_vector, wrap_azimuth_deg
from spatial_poc.patterns import CARDIOID, FIGURE_8, HYPERCARDIOID, OMNI, SUPERCARDIOID, first_order
from spatial_poc.presets import (
    PRESETS,
    TETRA_CAPSULES,
    blumlein,
    blumlein_rotated,
    blumlein_rotated_raw,
    foa_a_tetra,
    foa_b_direct,
    foa_b_from_a,
    mono,
    xy,
    ms_theoretical,
    ms_theoretical_raw,
)
from spatial_poc.render import render

RNG = np.random.default_rng(20261004)


def random_positions(n: int) -> list[SourcePosition]:
    # Uniform on the sphere: elevation from asin of a uniform z.
    az = RNG.uniform(-180.0, 180.0, n)
    el = np.degrees(np.arcsin(RNG.uniform(-1.0, 1.0, n)))
    return [SourcePosition(a, e) for a, e in zip(az, el)]


def gains(array, az, el=0.0, r=1.0):
    return array.output_gains(SourcePosition(az, el, r))


# ---- geometry and distance -------------------------------------------------


def test_distance_gain_is_inverse_distance():
    assert distance_gain(1.0) == 1.0
    assert distance_gain(2.0) == pytest.approx(0.5)
    assert distance_gain(4.0) == pytest.approx(0.25)


def test_distance_doubling_costs_6_02_db():
    for r in (1.0, 2.0, 3.7, 100.0):
        drop = 20.0 * math.log10(distance_gain(r) / distance_gain(2.0 * r))
        assert drop == pytest.approx(6.0206, abs=1e-4)


def test_distance_below_1m_rejected():
    with pytest.raises(ValueError):
        distance_gain(0.99)
    with pytest.raises(ValueError):
        SourcePosition(0.0, 0.0, 0.5)


def test_elevation_out_of_range_rejected():
    with pytest.raises(ValueError):
        SourcePosition(0.0, 90.5)


def test_azimuth_wraps():
    assert wrap_azimuth_deg(270.0) == pytest.approx(-90.0)
    assert wrap_azimuth_deg(-180.0) == pytest.approx(180.0)
    assert wrap_azimuth_deg(720.0 + 30.0) == pytest.approx(30.0)
    assert SourcePosition(450.0).azimuth_deg == pytest.approx(90.0)


def test_ambix_axes():
    np.testing.assert_allclose(unit_vector(0.0, 0.0), [1.0, 0.0, 0.0], atol=1e-15)
    np.testing.assert_allclose(unit_vector(90.0, 0.0), [0.0, 1.0, 0.0], atol=1e-15)  # +az is left
    np.testing.assert_allclose(unit_vector(0.0, 90.0), [0.0, 0.0, 1.0], atol=1e-15)


# ---- first-order patterns --------------------------------------------------


@pytest.mark.parametrize(
    "a, on_axis, side, rear",
    [
        (OMNI, 1.0, 1.0, 1.0),
        (CARDIOID, 1.0, 0.5, 0.0),
        (SUPERCARDIOID, 1.0, SUPERCARDIOID, 2.0 * SUPERCARDIOID - 1.0),
        (HYPERCARDIOID, 1.0, 0.25, -0.5),
        (FIGURE_8, 1.0, 0.0, -1.0),
    ],
)
def test_first_order_gains(a, on_axis, side, rear):
    p = first_order(a, 0.0)
    assert p.gain(unit_vector(0.0, 0.0)) == pytest.approx(on_axis)
    assert p.gain(unit_vector(90.0, 0.0)) == pytest.approx(side, abs=1e-12)
    assert p.gain(unit_vector(180.0, 0.0)) == pytest.approx(rear, abs=1e-12)


def test_figure_8_rear_lobe_is_polarity_reversed():
    p = first_order(FIGURE_8, 0.0)
    assert p.gain(unit_vector(150.0, 0.0)) < 0.0
    assert p.gain(unit_vector(30.0, 0.0)) > 0.0


def test_angle_off_axis_includes_elevation():
    p = first_order(CARDIOID, 0.0)
    assert p.angle_deg(unit_vector(0.0, 60.0)) == pytest.approx(60.0)
    assert p.angle_deg(unit_vector(-120.0, 0.0)) == pytest.approx(120.0)


def test_shape_parameter_out_of_range_rejected():
    with pytest.raises(ValueError):
        first_order(1.5)


def test_distance_does_not_change_pattern_gain():
    for array in (ms_theoretical(), foa_a_tetra()):
        near = array.raw_gains(SourcePosition(37.0, 12.0, 1.0), include_distance=False)
        far = array.raw_gains(SourcePosition(37.0, 12.0, 300.0), include_distance=False)
        np.testing.assert_allclose(near, far)


# ---- theoretical M/S --------------------------------------------------------


def test_ms_theoretical_hard_left_and_right():
    np.testing.assert_allclose(gains(ms_theoretical(), 90.0), [1.0, 0.0], atol=1e-12)
    np.testing.assert_allclose(gains(ms_theoretical(), -90.0), [0.0, 1.0], atol=1e-12)


def test_ms_theoretical_front_and_back_are_centred():
    np.testing.assert_allclose(gains(ms_theoretical(), 0.0), [0.5, 0.5], atol=1e-12)
    np.testing.assert_allclose(gains(ms_theoretical(), 180.0), [0.5, 0.5], atol=1e-12)


def test_ms_theoretical_s_shape():
    raw = ms_theoretical_raw()
    s = lambda az, el=0.0: gains(raw, az, el)[1]  # noqa: E731
    assert s(0.0) == pytest.approx(0.0, abs=1e-12)
    assert s(90.0) == pytest.approx(1.0)
    assert s(180.0) == pytest.approx(0.0, abs=1e-12)
    assert s(270.0) == pytest.approx(-1.0)


def test_ms_theoretical_m_is_omni():
    for p in random_positions(50):
        assert ms_theoretical_raw().output_gains(p)[0] == pytest.approx(1.0)


def test_ms_theoretical_l_plus_r_equals_m():
    array = ms_theoretical()
    for p in random_positions(200):
        left, right = array.output_gains(p)
        assert left + right == pytest.approx(1.0)


def test_ms_theoretical_s_fades_to_zero_at_the_poles():
    raw = ms_theoretical_raw()
    previous = None
    for el in np.linspace(0.0, 90.0, 91):
        s = gains(raw, 90.0, el)[1]
        assert s == pytest.approx(math.cos(math.radians(el)), abs=1e-12)
        if previous is not None:
            assert s <= previous + 1e-12
        previous = s
    assert gains(raw, 90.0, 90.0)[1] == pytest.approx(0.0, abs=1e-12)
    assert gains(raw, -90.0, -90.0)[1] == pytest.approx(0.0, abs=1e-12)


def test_ms_theoretical_s_cones_about_y_axis():
    # Equal S gain wherever the angle to the +y axis is equal, whatever the
    # direction around that axis.
    raw = ms_theoretical_raw()
    for p in random_positions(100):
        cos_to_y = p.direction[1]
        assert raw.output_gains(p)[1] == pytest.approx(cos_to_y)


def test_ms_theoretical_is_continuous_everywhere():
    # On a 0.5° grid, no neighbouring gain differs by more than the 0.5° slope
    # bound of a first-order pattern: no clicks at the rear or overhead.
    array = ms_theoretical()
    step = 0.5
    bound = math.radians(step) * 0.5 + 1e-9  # |d/dθ (0.5·sin)| ≤ 0.5
    azimuths = np.arange(-180.0, 180.0 + step, step)
    for el in np.arange(-90.0, 90.0 + step, 5.0):
        row = np.array([gains(array, az, el) for az in azimuths])
        assert np.max(np.abs(np.diff(row, axis=0))) <= bound
    for az in np.arange(-180.0, 180.0, 15.0):
        col = np.array([gains(array, az, el) for el in np.arange(-90.0, 90.0 + step, step)])
        assert np.max(np.abs(np.diff(col, axis=0))) <= bound


# ---- X/Y ----------------------------------------------------------------------


def test_xy_cardioids_at_45():
    c45 = 0.5 + 0.5 * math.cos(math.radians(45.0))
    c135 = 0.5 + 0.5 * math.cos(math.radians(135.0))
    np.testing.assert_allclose(gains(xy(), 0.0), [c45, c45])  # centre: −1.38 dB each
    np.testing.assert_allclose(gains(xy(), 45.0), [1.0, 0.5])  # on the L axis; R is 90° off
    np.testing.assert_allclose(gains(xy(), -45.0), [0.5, 1.0])
    np.testing.assert_allclose(gains(xy(), 180.0), [c135, c135])
    np.testing.assert_allclose(gains(xy(), -135.0), [0.0, 0.5], atol=1e-12)  # L's null; R is 90° off


def test_xy_cardioids_never_reverse_polarity():
    for p in random_positions(500):
        assert np.all(xy().output_gains(p) >= -1e-12)


def test_xy_is_left_right_symmetric():
    for p in random_positions(100):
        mirror = SourcePosition(-p.azimuth_deg, p.elevation_deg)
        np.testing.assert_allclose(xy().output_gains(p), xy().output_gains(mirror)[::-1], atol=1e-12)


def test_xy_overhead_is_centred():
    # Straight up both cardioids are 90° off axis: 0.5 each, whatever the azimuth.
    for az in range(-180, 180, 30):
        np.testing.assert_allclose(gains(xy(), az, 90.0), [0.5, 0.5], atol=1e-12)


def test_xy_parameters():
    left, right = gains(xy(half_angle_deg=60.0, a=0.25), 60.0)
    assert left == pytest.approx(1.0)
    assert right == pytest.approx(0.25 + 0.75 * math.cos(math.radians(120.0)))  # hypercardioid rear lobe: < 0
    assert right < 0.0


# ---- Blumlein ----------------------------------------------------------------


def test_blumlein_equals_figure_8_ms():
    a, b = blumlein(), blumlein_rotated()
    for p in random_positions(200):
        np.testing.assert_allclose(a.output_gains(p), b.output_gains(p), atol=1e-12)


def test_blumlein_rotated_raw_is_figure_8_m_and_s():
    np.testing.assert_allclose(gains(blumlein_rotated_raw(), 0.0), [1.0, 0.0], atol=1e-12)
    np.testing.assert_allclose(gains(blumlein_rotated_raw(), 90.0), [0.0, 1.0], atol=1e-12)


def test_blumlein_stage_is_plus_minus_45():
    np.testing.assert_allclose(gains(blumlein(), 45.0), [1.0, 0.0], atol=1e-12)
    np.testing.assert_allclose(gains(blumlein(), -45.0), [0.0, 1.0], atol=1e-12)
    left, right = gains(blumlein(), 90.0)
    assert left == pytest.approx(-right)  # 90° is antiphase, not hard left


def test_blumlein_rear_is_mirrored_and_inverted():
    # A rear source at 180° − φ comes out like the front source at φ with L
    # and R swapped and polarity inverted.
    array = blumlein()
    for phi in np.linspace(-90.0, 90.0, 37):
        front_l, front_r = gains(array, phi)
        rear_l, rear_r = gains(array, 180.0 - phi)
        assert rear_l == pytest.approx(-front_r, abs=1e-12)
        assert rear_r == pytest.approx(-front_l, abs=1e-12)


def test_blumlein_differs_from_theoretical_ms():
    assert not np.allclose(gains(blumlein(), 30.0), gains(ms_theoretical(), 30.0))


# ---- FOA ------------------------------------------------------------------


def test_tetra_cardioids_sum_to_twice_omni():
    a = foa_a_tetra()
    for p in random_positions(200):
        assert np.sum(a.output_gains(p, include_distance=False)) == pytest.approx(2.0)


def test_a_to_b_matches_direct_b_format():
    from_a, direct = foa_b_from_a(), foa_b_direct()
    for p in random_positions(500):
        np.testing.assert_allclose(from_a.output_gains(p), direct.output_gains(p), atol=1e-12)


def test_b_format_is_ambix_sn3d():
    # ACN order W, Y, Z, X; SN3D first order is W = 1 and Y, Z, X the unit-vector components.
    for p in random_positions(50):
        x, y, z = p.direction
        np.testing.assert_allclose(foa_b_direct().output_gains(p), [1.0, y, z, x], atol=1e-12)


def test_b_format_cardinal_directions():
    np.testing.assert_allclose(gains(foa_b_direct(), 0.0), [1, 0, 0, 1], atol=1e-12)
    np.testing.assert_allclose(gains(foa_b_direct(), 90.0), [1, 1, 0, 0], atol=1e-12)
    np.testing.assert_allclose(gains(foa_b_direct(), 0.0, 90.0), [1, 0, 1, 0], atol=1e-12)


def test_tetra_capsule_on_axis_gain_is_one():
    a = foa_a_tetra()
    for i, (_, az, el) in enumerate(TETRA_CAPSULES):
        assert a.output_gains(SourcePosition(az, el), include_distance=False)[i] == pytest.approx(1.0)


# ---- render ---------------------------------------------------------------


@pytest.mark.parametrize("name", list(PRESETS))
def test_render_shape_and_labels(name):
    array = PRESETS[name]()
    signal = np.sin(np.linspace(0.0, 10.0, 64))
    out = render(signal, SourcePosition(20.0, 10.0, 2.0), array)
    assert out.shape == (64, len(array.output_labels))


def test_render_is_gain_times_signal():
    signal = RNG.standard_normal(256)
    source = SourcePosition(30.0, 0.0, 2.0)
    out = render(signal, source, ms_theoretical())
    s = math.sin(math.radians(30.0))
    expected_l = 0.5 * (1.0 + s) / 2.0  # pattern gain times 1/r
    expected_r = 0.5 * (1.0 - s) / 2.0
    np.testing.assert_allclose(out[:, 0], expected_l * signal)
    np.testing.assert_allclose(out[:, 1], expected_r * signal)


def test_render_mono_applies_distance_only():
    signal = RNG.standard_normal(32)
    out = render(signal, SourcePosition(123.0, -40.0, 4.0), mono())
    np.testing.assert_allclose(out[:, 0], 0.25 * signal)
