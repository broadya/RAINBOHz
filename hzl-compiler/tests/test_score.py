# SPDX-License-Identifier: Apache-2.0

import pytest
from conftest import BASE, TreeBuilder, hzl

from hzl_compiler.canonical.model import Model
from hzl_compiler.errors import HzlError
from hzl_compiler.lower.score import EngineOptions, lower_score
from hzl_compiler.ref import Ref

ENV = hzl(
    "envelope",
    "envelopes/env",
    """
    initial_partial_state_set: {namespace: state_sets, name: start}
    envelope_stages:
      - envelope_stage:
          duration: "2:0"
          envelope_transforms:
            - envelope_transform:
                transformation: {namespace: transformations, name: update}
                source_partial_state_set: {namespace: state_sets, name: end}
    """,
)


def compile_(
    tree: TreeBuilder,
    files: dict[str, str] | None = None,
    options: EngineOptions = EngineOptions(),
) -> Model:
    return lower_score(
        tree({**BASE, "envelopes/env": ENV, **(files or {})}), Ref("scores", "score"), options
    )


def test_placements_add_up_in_ticks_and_rows_are_linked(tree: TreeBuilder) -> None:
    model = compile_(tree, options=EngineOptions(44100, 1))
    assert model.rows["EngineConfig"][0].properties() == {"sampleRate": 44100, "channelMode": 1}
    assert model.rows["Score"][0].properties()["name"] == "scores/score"
    assert model.rows["TempoMap"][0].properties()["name"] == "tempo_maps/main"
    assert [p.properties()["name"] for p in model.rows["Partial"]] == [
        "state_sets#a",
        "state_sets#b",
    ]
    # entity at 1:0, source +0:480, envelope +1:0 -> 2400 ticks; a stage of 2:0 -> 4320
    point_at = [
        e
        for e in model.edges
        if e.rel == "POINT_AT" and e.src.table == "PartialSetMusicalTimelinePoint"
    ]
    assert [e.properties["sync_point"] for e in point_at] == [2400, 4320]
    assert all(e.properties["offset_seconds"] == 0.0 for e in point_at)
    assert model.count("PartialStateSet") == 2 and model.count("PartialState") == 4
    assert model.rows["PartialState"][2].properties() == {"frequency": 220.0, "amplitude": -144.0}


def test_phase_kinds_become_phase_and_phase_free_run(tree: TreeBuilder) -> None:
    start = BASE["state_sets/start"].replace(
        "amplitude: -6.0, labels: [lead", "amplitude: -6.0, phase: -90, labels: [lead"
    )
    end = BASE["state_sets/end"].replace(
        "{identifier: b, frequency: 330.0, amplitude: -144.0}",
        "{identifier: b, frequency: 330.0, amplitude: -144.0, phase: free}",
    )
    model = compile_(tree, {"state_sets/start": start, "state_sets/end": end})
    assert [s.properties() for s in model.rows["PartialState"]] == [
        {"frequency": 440.0, "amplitude": -6.0, "phase": -90.0, "phase_free_run": False},
        {"frequency": 660.0, "amplitude": -6.0},
        {"frequency": 220.0, "amplitude": -144.0},
        {"frequency": 330.0, "amplitude": -144.0, "phase_free_run": True},
    ]


def test_labels_are_shared_rows_linked_from_partials_and_score(tree: TreeBuilder) -> None:
    model = compile_(tree)
    assert sorted(label.properties()["name"] for label in model.rows["Label"]) == [
        "demo",
        "lead",
        "pad",
        "x",
    ]
    has_label = {
        (e.src.table, e.src.index, model.rows["Label"][e.dst.index].properties()["name"])
        for e in model.edges
        if e.rel == "HAS_LABEL"
    }
    assert {
        ("Score", 0, "demo"),
        ("Partial", 0, "lead"),
        ("Partial", 0, "x"),
        ("Partial", 1, "pad"),
    } == has_label


def test_an_envelope_is_placed_once(tree: TreeBuilder) -> None:
    twice = (
        BASE["sources/source"]
        + '    - envelope: {namespace: envelopes, name: env}\n      offset: "4:0"\n'
    )
    with pytest.raises(
        HzlError,
        match=r"envelopes\[1\]: envelope envelopes/env is already placed at .*envelopes\[0\]",
    ):
        compile_(tree, {"sources/source": twice})


def test_tempo_map_rules_are_reported_in_hzl_terms(tree: TreeBuilder) -> None:
    main = BASE["tempo_maps/main"]
    with pytest.raises(HzlError, match="the last point must hold"):
        compile_(tree, {"tempo_maps/main": main.replace("hold: true", "hold: false")})
    with pytest.raises(HzlError, match="first point must be at position 0:0"):
        compile_(tree, {"tempo_maps/main": main.replace('position: "0:0"', 'position: "1:0"')})
    with pytest.raises(HzlError, match=r"tempo is BPM in \[1, 1000\]"):
        compile_(tree, {"tempo_maps/main": main.replace("tempo: 120.0", "tempo: 2000.0")})
    two = main.replace(
        '{position: "0:0", tempo: 120.0, hold: true}',
        (
            '{position: "0:0", tempo: 120.0, hold: false}\n'
            '    - tempo_map_point: {position: "0:0", tempo: 60.0, hold: true}'
        ),
    )
    with pytest.raises(HzlError, match="positions must strictly increase"):
        compile_(tree, {"tempo_maps/main": two})


def test_ramps_are_linear_in_physical_time_unless_the_score_says_otherwise(
    tree: TreeBuilder,
) -> None:
    score = BASE["scores/score"]
    default = compile_(tree)
    assert default.rows["Score"][0].properties()["tempo_ramp_physical_time"] is True
    tempo_map = "tempo_map: {namespace: tempo_maps, name: main}"
    for spelling, expected in (("physical_time", True), ("musical_position", False)):
        chosen = score.replace(tempo_map, f"{tempo_map}\n  tempo_ramp: {spelling}")
        model = compile_(tree, {"scores/score": chosen})
        assert model.rows["Score"][0].properties()["tempo_ramp_physical_time"] is expected


def test_an_unknown_tempo_ramp_is_refused(tree: TreeBuilder) -> None:
    tempo_map = "tempo_map: {namespace: tempo_maps, name: main}"
    bad = BASE["scores/score"].replace(tempo_map, f"{tempo_map}\n  tempo_ramp: linear")
    shapes = "the shapes are: 'physical_time', 'musical_position'"
    with pytest.raises(HzlError, match=f"unknown tempo_ramp 'linear'; {shapes}"):
        compile_(tree, {"scores/score": bad})


def test_ticks_beyond_the_beat_are_refused_where_written(tree: TreeBuilder) -> None:
    bad = BASE["entities/entity"].replace('offset: "0:480"', 'offset: "0:960"')
    with pytest.raises(HzlError, match=r"sound_sources\[0\].offset: 0:960 has 960 ticks"):
        compile_(tree, {"entities/entity": bad})


def test_a_position_on_a_source_is_refused_in_favour_of_a_trajectory(tree: TreeBuilder) -> None:
    with pytest.raises(
        HzlError, match="sound_source.position: a source's position is its trajectory"
    ):
        compile_(tree, {"sources/source": BASE["sources/source"] + "  position: {azimuth: 0.0}\n"})


def test_engine_options_are_checked() -> None:
    with pytest.raises(ValueError):
        EngineOptions(sample_rate=0)
    with pytest.raises(ValueError):
        EngineOptions(channel_mode=8)
    assert EngineOptions(channel_mode=7).channel_mode == 7


VOICE = hzl(
    "tempo_map",
    "tempo_maps/voice",
    """
    ticks_per_beat: 4
    points:
      - tempo_map_point: {position: "0:0", tempo: 90.0, hold: true}
    synchronization_points:
      - synchronization_point:
          position: "0:0"
          parent: {namespace: tempo_maps, name: main}
          parent_position: "8:0"
    """,
    layer="1: voices",
)


def point_ats(model: Model, table: str) -> list[tuple[int, int]]:
    """(TempoMap index, sync_point) of every POINT_AT from `table`, in order."""
    return [
        (e.dst.index, e.properties["sync_point"])
        for e in model.edges
        if e.rel == "POINT_AT" and e.src.table == table
    ]


def test_a_placement_counts_in_its_map_s_ticks(tree: TreeBuilder) -> None:
    score = BASE["scores/score"].replace(
        'position: "1:0"', 'tempo_map: {namespace: tempo_maps, name: voice}\n      position: "1:0"'
    )
    with pytest.raises(HzlError, match="0:480 has 480 ticks but the tempo map has 4") as raised:
        compile_(tree, {"tempo_maps/voice": VOICE, "scores/score": score})
    assert raised.value.where.path.endswith("sound_sources[0].offset")
    entity = BASE["entities/entity"].replace('offset: "0:480"', 'offset: "0:2"')
    model = compile_(
        tree, {"tempo_maps/voice": VOICE, "scores/score": score, "entities/entity": entity}
    )
    # entity 1:0 + source 0:2 + envelope 1:0 = 10 ticks of voice; the stage of 2:0 ends at 18
    assert point_ats(model, "PartialSetMusicalTimelinePoint") == [(1, 10), (1, 18)]
    assert point_ats(model, "SpatialPositionMusicalTimelinePoint") == [(1, 6)]


def test_an_envelope_on_its_own_map_starts_at_an_absolute_position(tree: TreeBuilder) -> None:
    source = BASE["sources/source"].replace(
        'offset: "1:0"', 'tempo_map: {namespace: tempo_maps, name: voice}\n      position: "3:1"'
    )
    model = compile_(tree, {"tempo_maps/voice": VOICE, "sources/source": source})
    # 3:1 on voice from its 0:0, whatever the entity and source placements; the
    # stage counts on voice from there. The source stays on main at 1:480.
    assert point_ats(model, "PartialSetMusicalTimelinePoint") == [(1, 13), (1, 21)]
    assert point_ats(model, "SpatialPositionMusicalTimelinePoint") == [(0, 1440)]


def test_an_override_and_an_offset_do_not_mix(tree: TreeBuilder) -> None:
    both = BASE["sources/source"].replace(
        'offset: "1:0"', 'tempo_map: {namespace: tempo_maps, name: voice}\n      offset: "1:0"'
    )
    with pytest.raises(HzlError, match="starts at an absolute 'position' on that map"):
        compile_(tree, {"tempo_maps/voice": VOICE, "sources/source": both})
    neither = BASE["sources/source"].replace(
        'offset: "1:0"', "tempo_map: {namespace: tempo_maps, name: voice}"
    )
    with pytest.raises(HzlError, match="missing required key 'position'"):
        compile_(tree, {"tempo_maps/voice": VOICE, "sources/source": neither})
    alone = BASE["sources/source"].replace('offset: "1:0"', 'position: "1:0"')
    with pytest.raises(HzlError, match="without 'tempo_map', write 'offset'"):
        compile_(tree, {"sources/source": alone})


def test_a_placement_s_map_must_be_a_tempo_map(tree: TreeBuilder) -> None:
    source = BASE["sources/source"].replace(
        'offset: "1:0"',
        'tempo_map: {namespace: transformations, name: update}\n      position: "0:0"',
    )
    with pytest.raises(HzlError, match="is a partial_state_set_transformation, but a tempo_map"):
        compile_(tree, {"sources/source": source})
