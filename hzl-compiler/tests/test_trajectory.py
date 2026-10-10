# SPDX-License-Identifier: Apache-2.0

import pytest
from conftest import BASE, hzl
from test_score import ENV

from hzl_compiler.errors import HzlError
from hzl_compiler.hzl.envelope import Curve
from hzl_compiler.lower.score import lower_score
from hzl_compiler.ref import Ref

ORBIT = hzl(
    "trajectory",
    "trajectories/orbit",
    """
    initial_position: {azimuth: 0.0, elevation: 0.0, distance: 1.0}
    trajectory_stages:
      - trajectory_stage:
          duration: "2:0"
          curve: s_curve
          curve_amount: 0.5
          distance_curve: step
          position: {azimuth: 90}
      - trajectory_stage:
          duration: "0:480"
          offset_seconds: 0.25
          position: {elevation: 45.0, distance: 4.0}
    """,
)

SOURCE_WITH_ORBIT = BASE["sources/source"].replace(
    "envelopes:", "trajectory: {namespace: trajectories, name: orbit}\n  envelopes:"
)


def compile_(tree, files=None):
    return lower_score(
        tree({**BASE, "envelopes/env": ENV, **(files or {})}), Ref("scores", "score")
    )


def position_edges(model, rel):
    return [e for e in model.edges if e.rel == rel and e.src.table.startswith("SpatialPosition")]


def test_a_source_without_a_trajectory_is_ahead_at_one_metre_from_its_start(tree):
    model = compile_(tree)
    assert model.count("SpatialPositionMusicalTimeline") == 1
    assert [s.properties() for s in model.rows["SpatialPositionState"]] == [
        {"azimuth": 0.0, "elevation": 0.0, "distance": 1.0}
    ]
    # entity at 1:0, source +0:480 -> 1440 ticks; before the envelope's 2400
    assert [e.properties for e in position_edges(model, "POINT_AT")] == [
        {"sync_point": 1440, "offset_seconds": 0.0}
    ]
    owner = [
        e
        for e in model.edges
        if e.rel == "HAS_MUSICAL_TIMELINE" and e.dst.table == "SpatialPositionMusicalTimeline"
    ]
    assert [(e.src.table, e.src.index) for e in owner] == [("SoundSource", 0)]


def test_a_trajectory_lowers_to_sparse_states_from_the_source_start(tree):
    model = compile_(tree, {"sources/source": SOURCE_WITH_ORBIT, "trajectories/orbit": ORBIT})
    assert [s.properties() for s in model.rows["SpatialPositionState"]] == [
        {"azimuth": 0.0, "elevation": 0.0, "distance": 1.0},
        {"azimuth": 90.0},
        {"elevation": 45.0, "distance": 4.0},
    ]
    assert [e.properties for e in position_edges(model, "POINT_AT")] == [
        {"sync_point": 1440, "offset_seconds": 0.0},
        {"sync_point": 1440 + 1920, "offset_seconds": 0.0},
        {"sync_point": 1440 + 1920 + 480, "offset_seconds": 0.25},
    ]
    # NEXT_POINT: timeline -> p0 -> p1 -> p2
    chain = [(e.src.table, e.src.index, e.dst.index) for e in position_edges(model, "NEXT_POINT")]
    assert chain == [
        ("SpatialPositionMusicalTimeline", 0, 0),
        ("SpatialPositionMusicalTimelinePoint", 0, 1),
        ("SpatialPositionMusicalTimelinePoint", 1, 2),
    ]


def test_a_stage_curve_lands_on_the_point_the_stage_starts_from(tree):
    model = compile_(tree, {"sources/source": SOURCE_WITH_ORBIT, "trajectories/orbit": ORBIT})
    points = [p.properties() for p in model.rows["SpatialPositionMusicalTimelinePoint"]]
    s_curve, step, linear = Curve.S_CURVE.value, Curve.STEP.value, Curve.LINEAR.value
    assert points[0] == {
        "azimuth_curve_type": s_curve,
        "azimuth_curve_amount": 0.5,
        "elevation_curve_type": s_curve,
        "elevation_curve_amount": 0.5,
        "distance_curve_type": step,
        "distance_curve_amount": 0.5,
    }
    assert points[1]["azimuth_curve_type"] == linear and points[2]["azimuth_curve_type"] == linear


def test_one_trajectory_serves_several_sources(tree):
    entity = (
        BASE["entities/entity"]
        + '    - sound_source: {namespace: sources, name: other}\n      offset: "2:0"\n'
    )
    other = hzl(
        "sound_source",
        "sources/other",
        "trajectory: {namespace: trajectories, name: orbit}\nenvelopes: []",
    )
    model = compile_(
        tree,
        {
            "sources/source": SOURCE_WITH_ORBIT,
            "sources/other": other,
            "entities/entity": entity,
            "trajectories/orbit": ORBIT,
        },
    )
    assert model.count("SpatialPositionMusicalTimeline") == 2
    assert model.count("SpatialPositionState") == 6
    first = [e.properties["sync_point"] for e in position_edges(model, "POINT_AT")][::3]
    assert first == [1440, 960 + 1920]


def test_a_trajectory_without_stages_is_a_fixed_position(tree):
    fixed = hzl(
        "trajectory",
        "trajectories/orbit",
        "initial_position: {azimuth: -30, elevation: 10, distance: 2}",
    )
    model = compile_(tree, {"sources/source": SOURCE_WITH_ORBIT, "trajectories/orbit": fixed})
    assert [s.properties() for s in model.rows["SpatialPositionState"]] == [
        {"azimuth": -30.0, "elevation": 10.0, "distance": 2.0}
    ]


@pytest.mark.parametrize(
    "body, message",
    [
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0}",
            r"initial_position.distance: missing required key 'distance'",
        ),
        (
            "initial_position: {azimuth: 181.0, elevation: 0.0, distance: 1.0}",
            r"azimuth is in degrees and must lie in \[-180, 180\]",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: -91.0, distance: 1.0}",
            r"elevation is in degrees and must lie in \[-90, 90\]",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0, distance: 0.5}",
            r"distance is in metres and must lie in \[1, 16384\]",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0, distance: 1.0, pan: 0.0}",
            r"unknown key 'pan'",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0, distance: 1.0}\n"
            'trajectory_stages:\n  - trajectory_stage: {duration: "1:0", position: {}}',
            r"position: a position gives at least one of azimuth, elevation and distance",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0, distance: 1.0}\n"
            'trajectory_stages:\n  - trajectory_stage: {duration: "1:0"}',
            r"missing required key 'position'",
        ),
        (
            "initial_position: {azimuth: 0.0, elevation: 0.0, distance: 1.0}\n"
            "trajectory_stages:\n"
            '  - trajectory_stage: {duration: "1:0", frequency_curve: step, '
            "position: {azimuth: 1}}",
            r"unknown key 'frequency_curve'",
        ),
    ],
)
def test_bad_trajectories_are_refused_where_written(tree, body, message):
    bad = hzl("trajectory", "trajectories/orbit", body)
    with pytest.raises(HzlError, match=message):
        compile_(tree, {"sources/source": SOURCE_WITH_ORBIT, "trajectories/orbit": bad})


def test_pan_on_a_partial_points_to_the_trajectory(tree):
    start = BASE["state_sets/start"].replace(
        "amplitude: -6.0, labels: [pad]", "amplitude: -6.0, pan: 0.5, labels: [pad]"
    )
    with pytest.raises(
        HzlError, match="pan is not in HzL; write an azimuth in the source's trajectory"
    ):
        compile_(tree, {"state_sets/start": start})
