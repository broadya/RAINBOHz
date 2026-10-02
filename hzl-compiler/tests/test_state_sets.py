import pytest

from conftest import BASE, hzl
from hzl_compiler.errors import HzlError, Where
from hzl_compiler.lower.state_sets import StateSetResolver, evaluate_expression
from hzl_compiler.ref import Ref

W = Where(None, "")

SCALE = hzl(
    "scale",
    "scales/c",
    """
    notes:
      - note: C4
        frequency: 261.63
      - note: G4
        frequency: 392.0
        amplitude: -3.0
    """,
    labels="[major]",
)

HARMONICS = hzl(
    "harmonic_series",
    "series/h",
    """
    base_frequency: 2.0
    number_of_harmonics: 3
    frequency_expression: "harmonic_index"
    amplitude_expression: "-20*log10(harmonic_index)"
    """,
    labels="[harmonic]",
)

PRODUCT = hzl(
    "virtual_partial_state_set",
    "virtual/v",
    """
    transformation: {namespace: transformations, name: product}
    partial_state_sets:
      - partial_state_set: {namespace: scales, name: c}
      - partial_state_set: {namespace: series, name: h}
    """,
    labels="[virtual]",
)


def resolve(tree, files, ref):
    return StateSetResolver(tree({**BASE, **files})).resolve(Ref(*ref.split("/")), W)


def test_scale_lowers_to_one_partial_per_note(tree):
    s = resolve(tree, {"scales/c": SCALE}, "scales/c")
    assert [(p.identifier, p.labels, p.frequency, p.amplitude) for p in s.states] == [
        ("0", ("C4",), 261.63, None),
        ("1", ("G4",), 392.0, -3.0),
    ]
    assert s.header.namespace == "scales"


def test_harmonic_series_evaluates_expressions(tree):
    s = resolve(tree, {"series/h": HARMONICS}, "series/h")
    assert [p.identifier for p in s.states] == ["1", "2", "3"]
    assert [p.labels for p in s.states] == [("harmonic 1",), ("harmonic 2",), ("harmonic 3",)]
    assert [p.frequency for p in s.states] == [2.0, 4.0, 6.0]
    assert s.states[0].amplitude == 0.0
    assert s.states[1].amplitude == pytest.approx(-20 * 0.3010299956639812)


def test_expression_errors_are_reported():
    with pytest.raises(HzlError, match="does not parse"):
        evaluate_expression("1 +", 1, W)
    with pytest.raises(HzlError, match="unknown names: n"):
        evaluate_expression("2*n", 1, W)
    with pytest.raises(HzlError, match="real number"):
        evaluate_expression("sqrt(-harmonic_index)", 1, W)


def test_harmonic_series_out_of_range_amplitude_is_an_error(tree):
    loud = HARMONICS.replace('"-20*log10(harmonic_index)"', '"6*harmonic_index"')
    with pytest.raises(HzlError, match=r"amplitude_expression: amplitude is in dB"):
        resolve(tree, {"series/h": loud}, "series/h")


def test_constant_and_expression_together_are_refused(tree):
    with pytest.raises(HzlError, match="either 'frequency' or 'frequency_expression'"):
        resolve(tree, {"series/h": HARMONICS + "  frequency: 100.0\n"}, "series/h")


def test_flattened_cartesian_product(tree):
    s = resolve(tree, {"scales/c": SCALE, "series/h": HARMONICS, "virtual/v": PRODUCT}, "virtual/v")
    assert len(s.states) == 6
    first, *_, last = s.states
    assert first.identifier == "0*1" and first.labels == ("C4", "harmonic 1")
    assert first.frequency == pytest.approx(261.63 * 2.0)
    assert first.amplitude == 0.0  # the scale gives none: the series' value is taken
    assert last.identifier == "1*3"
    assert last.frequency == pytest.approx(392.0 * 6.0)
    assert last.amplitude == pytest.approx(-3.0 + -20 * 0.47712125471966244)  # dB add
    assert s.header.labels == ("virtual", "product", "major", "harmonic")
    assert s.header.namespace == "virtual"


def test_product_amplitude_is_floored_at_silence(tree):
    quiet = SCALE.replace("amplitude: -3.0", "amplitude: -140.0")
    s = resolve(tree, {"scales/c": quiet, "series/h": HARMONICS, "virtual/v": PRODUCT}, "virtual/v")
    assert s.states[-1].amplitude == -144.0


def test_product_needs_two_sources_and_the_product_transformation(tree):
    three = PRODUCT + "    - partial_state_set: {namespace: scales, name: c}\n"
    with pytest.raises(HzlError, match="exactly two state sets, got 3"):
        resolve(tree, {"scales/c": SCALE, "series/h": HARMONICS, "virtual/v": three}, "virtual/v")
    wrong = PRODUCT.replace("name: product", "name: update")
    with pytest.raises(HzlError, match="applies 'Flattened Cartesian Product'"):
        resolve(tree, {"scales/c": SCALE, "series/h": HARMONICS, "virtual/v": wrong}, "virtual/v")


def test_reference_cycles_are_refused(tree):
    loop = PRODUCT.replace("{namespace: series, name: h}", "{namespace: virtual, name: v}")
    with pytest.raises(HzlError, match="cycle: virtual/v -> virtual/v"):
        resolve(tree, {"scales/c": SCALE, "virtual/v": loop}, "virtual/v")


def test_a_non_state_set_reference_is_refused(tree):
    with pytest.raises(HzlError, match="tempo_maps/main is a tempo_map; a state set is needed"):
        resolve(tree, {}, "tempo_maps/main")


def test_phase_and_spatial_properties_are_not_in_this_version(tree):
    p = hzl("partial_state_set", "state_sets/p", "partial_states:\n  - partial_state: {identifier: a, frequency: 1.0, phase: 0.0}")
    with pytest.raises(HzlError, match="phase is not in this version"):
        resolve(tree, {"state_sets/p": p}, "state_sets/p")


def test_duplicate_identifiers_are_refused(tree):
    d = hzl("partial_state_set", "state_sets/d", "partial_states:\n  - partial_state: {identifier: a}\n  - partial_state: {identifier: a}")
    with pytest.raises(HzlError, match=r"partial_states\[1\].partial_state.identifier: identifier 'a' is already used"):
        resolve(tree, {"state_sets/d": d}, "state_sets/d")


def test_amplitude_range_is_checked_where_written(tree):
    loud = hzl("partial_state_set", "state_sets/l", "partial_states:\n  - partial_state: {identifier: a, amplitude: 3.0}")
    with pytest.raises(HzlError, match=r"partial_states\[0\].partial_state.amplitude: amplitude is in dB"):
        resolve(tree, {"state_sets/l": loud}, "state_sets/l")
