import pytest

from conftest import BASE, hzl
from hzl_compiler.errors import HzlError
from hzl_compiler.hzl.common import FREE_RUN
from hzl_compiler.hzl.envelope import Curve, load_envelope
from hzl_compiler.lower.state_sets import StateSetResolver
from hzl_compiler.lower.timeline import lower_envelope
from hzl_compiler.ref import Ref


def envelope(stages: str) -> str:
    return hzl(
        "envelope",
        "envelopes/env",
        "initial_partial_state_set: {namespace: state_sets, name: start}\nenvelope_stages:\n" + stages,
        labels="[env]",
    )


UPDATE_END = """\
  - envelope_stage:
      duration: "2:0"
      curve: s_curve
      curve_amount: 0.5
      amplitude_curve: step
      envelope_transforms:
        - envelope_transform:
            transformation: {namespace: transformations, name: update}
            source_partial_state_set: {namespace: state_sets, name: end}
"""

MIDDLE = hzl(
    "partial_state_set",
    "state_sets/middle",
    """
    partial_states:
      - partial_state: {identifier: a, amplitude: -12.0}
      - partial_state: {identifier: b, frequency: 330.0}
      - partial_state: {identifier: c, frequency: 1.0, amplitude: -1.0}
    """,
)

FILTERED = """\
  - envelope_stage:
      duration: "1:0"
      offset_seconds: 0.25
      envelope_transforms:
        - envelope_transform:
            filter: "^lea"
            transformation: {namespace: transformations, name: update}
            source_partial_state_set: {namespace: state_sets, name: middle}
        - envelope_transform:
            filter: "pad"
            transformation: {namespace: transformations, name: update}
            source_partial_state_set: {namespace: state_sets, name: middle}
"""


def lower(tree, files):
    t = tree({**BASE, **files})
    return lower_envelope(load_envelope(t, Ref("envelopes", "env")), t, StateSetResolver(t), 960)


def test_initial_set_gives_partials_and_first_point(tree):
    tl = lower(tree, {"envelopes/env": envelope(UPDATE_END)})
    assert tl.name == "envelopes/env" and tl.labels == ("env",)
    assert [(p.identifier, p.name, p.labels) for p in tl.partials] == [
        ("a", "state_sets#a", ("lead", "x")),
        ("b", "state_sets#b", ("pad",)),
    ]
    assert tl.points[0].offset_ticks == 0 and tl.points[0].offset_seconds is None
    assert tl.points[0].states["a"].frequency == 440.0
    assert tl.points[1].offset_ticks == 1920
    assert tl.points[1].states["b"].amplitude == -144.0


def test_a_stage_curve_sits_on_the_point_the_stage_starts_from(tree):
    tl = lower(tree, {"envelopes/env": envelope(UPDATE_END)})
    assert tl.points[0].frequency_curve.type is Curve.S_CURVE and tl.points[0].frequency_curve.amount == 0.5
    assert tl.points[0].amplitude_curve.type is Curve.STEP and tl.points[0].amplitude_curve.amount == 0.5
    assert tl.points[-1].frequency_curve.type is Curve.LINEAR and tl.points[-1].amplitude_curve.amount == 0.0


def test_update_matches_by_identifier_within_the_filter_and_keeps_only_present_values(tree):
    tl = lower(tree, {"state_sets/middle": MIDDLE, "envelopes/env": envelope(FILTERED + UPDATE_END)})
    point = tl.points[1]
    assert point.offset_ticks == 960 and point.offset_seconds == 0.25
    assert point.states["a"].frequency is None and point.states["a"].amplitude == -12.0
    assert point.states["b"].frequency == 330.0 and point.states["b"].amplitude is None
    assert "c" not in point.states  # not a partial of this set: ignored
    assert tl.points[2].offset_ticks == 960 + 1920


def test_filter_selects_by_any_label(tree):
    only_a = FILTERED.replace('filter: "^lea"', 'filter: "nothing"').replace('filter: "pad"', 'filter: "^x$"')
    tl = lower(tree, {"state_sets/middle": MIDDLE, "envelopes/env": envelope(only_a + UPDATE_END)})
    assert list(tl.points[1].states) == ["a"]


def test_later_transform_overrides_earlier_property_by_property(tree):
    louder = MIDDLE.replace("state_sets/middle", "state_sets/louder").replace("name: middle", "name: louder")
    louder = louder.replace("amplitude: -12.0", "amplitude: -3.0").replace("frequency: 330.0", "frequency: 331.0")
    # first transform applies `louder` to everything, second `middle`: middle wins where it sets a value
    stage = FILTERED.replace('filter: "^lea"', 'filter: "."').replace('filter: "pad"', 'filter: "."')
    stage = stage.replace("name: middle}", "name: louder}", 1)
    tl = lower(tree, {"state_sets/middle": MIDDLE, "state_sets/louder": louder, "envelopes/env": envelope(stage + UPDATE_END)})
    assert tl.points[1].states["a"].amplitude == -12.0
    assert tl.points[1].states["b"].frequency == 330.0


def test_phase_is_carried_and_a_later_transform_overrides_it_only_where_given(tree):
    start = BASE["state_sets/start"].replace("amplitude: -6.0, labels: [lead", "amplitude: -6.0, phase: 45, labels: [lead")
    target = MIDDLE.replace("state_sets/middle", "state_sets/target").replace("name: middle", "name: target").replace("{identifier: a, amplitude: -12.0}", "{identifier: a, phase: 90}")
    target = target.replace("{identifier: b, frequency: 330.0}", "{identifier: b, phase: 10}")
    free = MIDDLE.replace("{identifier: b, frequency: 330.0}", "{identifier: b, phase: free}")
    # first transform gives `target` to everything, second `middle` (a: no phase, b: free)
    stage = FILTERED.replace('filter: "^lea"', 'filter: "."').replace('filter: "pad"', 'filter: "."')
    stage = stage.replace("name: middle}", "name: target}", 1)
    tl = lower(tree, {"state_sets/start": start, "state_sets/middle": free, "state_sets/target": target, "envelopes/env": envelope(stage + UPDATE_END)})
    assert [tl.points[0].states[i].phase for i in "ab"] == [45.0, None]
    assert tl.points[1].states["a"].phase == 90.0
    assert tl.points[1].states["b"].phase is FREE_RUN
    assert [tl.points[2].states[i].phase for i in "ab"] == [None, None]


def test_every_partial_needs_both_values_at_the_last_point(tree):
    stage = FILTERED.replace('filter: "^lea"', 'filter: "."').replace('filter: "pad"', 'filter: "."')
    with pytest.raises(HzlError, match=r"envelope_stages\[0\].envelope_stage: partial 'a' has no frequency at the last stage of envelopes/env"):
        lower(tree, {"state_sets/middle": MIDDLE, "envelopes/env": envelope(stage)})


def test_every_partial_needs_both_values_at_the_start(tree):
    start = BASE["state_sets/start"].replace("frequency: 660.0, ", "")
    with pytest.raises(HzlError, match="partial 'b' of state_sets/start has no frequency"):
        lower(tree, {"state_sets/start": start, "envelopes/env": envelope(UPDATE_END)})


def test_only_update_is_allowed_in_a_stage(tree):
    with pytest.raises(HzlError, match="only 'Update' is in this version"):
        lower(tree, {"envelopes/env": envelope(UPDATE_END.replace("name: update", "name: product"))})


def test_an_envelope_needs_a_stage(tree):
    with pytest.raises(HzlError, match="at least one stage"):
        lower(tree, {"envelopes/env": envelope("  []\n")})


def test_bad_curve_and_filter_are_reported(tree):
    with pytest.raises(HzlError, match="unknown curve 'wobbly'"):
        lower(tree, {"envelopes/env": envelope(UPDATE_END.replace("s_curve", "wobbly"))})
    bad_filter = UPDATE_END.replace("            transformation: {", '            filter: "("\n            transformation: {')
    with pytest.raises(HzlError, match="not a valid regular expression"):
        lower(tree, {"envelopes/env": envelope(bad_filter)})


def test_a_duration_is_exact_beats_and_ticks(tree):
    with pytest.raises(HzlError, match=r'envelope_stages\[0\].envelope_stage.duration: expected musical time'):
        lower(tree, {"envelopes/env": envelope(UPDATE_END.replace('duration: "2:0"', "duration: 1.5"))})
